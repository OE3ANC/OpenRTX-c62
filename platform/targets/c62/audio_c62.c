/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <interfaces/audio.h>
#include <interfaces/radio.h>
#include "AudioSystem.h"
#include "AudioTrack.h"
#include "AudioRecord.h"
#include "audio_resampler.h"

LOG_MODULE_REGISTER(c62_audio, LOG_LEVEL_INF);

#define SAMPLE_RATE 16000
#define QUEUE_SAMPLES 2560 /* 160 ms per endpoint, in application PSRAM. */
#define IO_TIMEOUT_MS 1000
#define WORK_SAMPLES 320
#define AUDIO_STACK_SIZE (8 * 1024)

struct sample_queue {
    int16_t samples[QUEUE_SAMPLES];
    size_t head;
    size_t count;
};

struct audio_stream {
    struct streamCtx *ctx;
    struct c62_resampler converter;
    struct sample_queue queue;
    struct k_sem wake;
    bool input;
    bool busy;
    unsigned int endpoint;
    unsigned int half;
    unsigned int data_half;
    int error;
    int16_t converted[C62_RESAMPLER_MAX_OUTPUT];
    size_t converted_count;
    size_t converted_pos;
    int64_t play_until;
    uint32_t clock_fraction;
};

static K_MUTEX_DEFINE(audio_mutex);
static bool initialized;
static bool terminating;
static bool worker_running;
static bool paths[3][3];
static bool record_active;
static bool track_active[2];
static AudioRecord record;
static AudioTrack tracks[2];
static struct k_thread worker;
K_THREAD_STACK_DEFINE(worker_stack, AUDIO_STACK_SIZE);

/* Separate queues prevent MIC/RX and speaker/TX from stealing each other's data. */
static struct audio_stream inputs[2] __attribute__((section(".psram_section")));
static struct audio_stream outputs[2]
    __attribute__((section(".psram_section")));
static struct sample_queue bypass[2] __attribute__((section(".psram_section")));

static const struct gpio_dt_spec speaker_enable =
    GPIO_DT_SPEC_GET(DT_PATH(gpio_controls, speaker_enable), gpios);
static const bool input_config = true;
static const bool output_config = false;

static void queue_reset(struct sample_queue *queue)
{
    queue->head = queue->count = 0;
}

static int16_t queue_pop(struct sample_queue *queue)
{
    int16_t sample = queue->samples[queue->head];
    queue->head = (queue->head + 1) % QUEUE_SAMPLES;
    queue->count--;
    return sample;
}

static void queue_push(struct sample_queue *queue, int16_t sample)
{
    size_t tail = (queue->head + queue->count) % QUEUE_SAMPLES;
    queue->samples[tail] = sample;
    queue->count++;
}

/* All helpers accessing streams or vendor objects run under audio_mutex. */
static void stream_fail(struct audio_stream *stream, int error)
{
    if (stream->error == 0)
        LOG_ERR("%s endpoint %u: %d", stream->input ? "Input" : "Output",
                stream->endpoint, error);
    stream->error = error;
    if (stream->ctx != NULL)
        stream->ctx->running = 0;
    k_sem_give(&stream->wake);
}

static bool stream_valid(struct audio_stream *stream, struct streamCtx *ctx)
{
    return stream->ctx == ctx && ctx->running && stream->error == 0;
}

static void stream_detach(struct audio_stream *stream)
{
    if (stream->ctx != NULL) {
        stream->ctx->running = 0;
        if (stream->converter.clipped != 0)
            LOG_WRN("Endpoint %u clipped %u resampled samples",
                    stream->endpoint, stream->converter.clipped);
    }
    stream->ctx = NULL;
    queue_reset(&stream->queue);
    k_sem_give(&stream->wake);
}

#ifdef CONFIG_C62_RX_LEVEL_DIAGNOSTICS
static struct {
    uint32_t count;
    uint32_t near_rail;
    int32_t sum;
    int16_t minimum;
    int16_t maximum;
} rx_level;

static void measure_rx_level(int16_t sample)
{
    if (rx_level.count == 0) {
        rx_level.minimum = INT16_MAX;
        rx_level.maximum = INT16_MIN;
        rx_level.sum = 0;
        rx_level.near_rail = 0;
    }
    rx_level.minimum = MIN(rx_level.minimum, sample);
    rx_level.maximum = MAX(rx_level.maximum, sample);
    rx_level.sum += sample;
    if (sample >= 32700 || sample <= -32700)
        rx_level.near_rail++;
    if (++rx_level.count == SAMPLE_RATE) {
        LOG_INF("RX PCM min=%d max=%d mean=%d near_rail=%u/%u",
                rx_level.minimum, rx_level.maximum, rx_level.sum / SAMPLE_RATE,
                rx_level.near_rail, rx_level.count);
        rx_level.count = 0;
    }
}
#endif

/*
 * AudioRecord_read() waits indefinitely and copies whole DSP frames even when
 * the requested size is smaller. Use the same ICStream transport nonblocking,
 * consuming its actual frame size and AudioRecord's channel mapping instead.
 */
static void capture_frame(void)
{
    ICStream *ic = record.mICStream;
    ICStream_Consumer_fetchRemote(ic);
    if (ICStream_Consumer_isEmpty(ic))
        return;

    void *frame;
    int ret = ICStream_Consumer_acquireFrame(ic, &frame);
    if (ret != IC_OK)
        goto error;

    const int16_t *samples = frame;
    size_t channels = ic->share->channelCount;
    size_t count = record.mCblk->frameCount;
    for (unsigned int source = 0; source < 2; source++) {
        struct audio_stream *stream = &inputs[source];
        if (stream->ctx != NULL && stream->ctx->running
            && count > QUEUE_SAMPLES - stream->queue.count) {
            LOG_ERR("Capture overrun on endpoint %u: queued=%u frame=%u "
                    "capacity=%u",
                    source, (unsigned int)stream->queue.count,
                    (unsigned int)count, QUEUE_SAMPLES);
            stream_fail(stream, -EOVERFLOW);
        }

        for (size_t i = 0; i < count; i++) {
            int16_t sample =
                samples[i * channels + record.mChannelOutIdx[source]];
            if (stream->ctx != NULL && stream->ctx->running) {
#ifdef CONFIG_C62_RX_LEVEL_DIAGNOSTICS
                if (source == SOURCE_RTX)
                    measure_rx_level(sample);
#endif
                queue_push(&stream->queue, sample);
            }
            for (unsigned int sink = 0; sink < 2; sink++) {
                if (!paths[source][sink])
                    continue;
                /* Analog monitoring may drop old audio if playback stalls. */
                if (bypass[sink].count == QUEUE_SAMPLES)
                    queue_pop(&bypass[sink]);
                queue_push(&bypass[sink], sample);
            }
        }
        k_sem_give(&stream->wake);
    }

    ret = ICStream_Consumer_releaseFrame(ic, frame);
    if (ret == IC_OK)
        ret = ICStream_Consumer_commitRemote(ic);
    if (ret == IC_OK)
        return;
error:
    for (unsigned int i = 0; i < 2; i++) {
        if (inputs[i].ctx != NULL)
            stream_fail(&inputs[i], -EIO);
    }
}

static void play_bypass(unsigned int sink)
{
    if (bypass[sink].count == 0 || !track_active[sink])
        return;
    AudioTrack_Buffer buffer = { .frameCount = MIN(bypass[sink].count,
                                                   WORK_SAMPLES) };
    if (AudioTrack_obtainBuffer(&tracks[sink], &buffer, 0) != 0)
        return;
    for (size_t i = 0; i < buffer.frameCount; i++)
        buffer.i16[i] = queue_pop(&bypass[sink]);
    AudioTrack_releaseBuffer(&tracks[sink], &buffer);
}

static void audio_worker(void *arg1, void *arg2, void *arg3)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    for (;;) {
        k_mutex_lock(&audio_mutex, K_FOREVER);
        if (!worker_running) {
            k_mutex_unlock(&audio_mutex);
            return;
        }
        if (record_active)
            capture_frame();
        for (unsigned int sink = 0; sink < 2; sink++)
            play_bypass(sink);
        k_mutex_unlock(&audio_mutex);
        k_sleep(K_MSEC(1));
    }
}

static int stream_start(const uint8_t instance, const void *config,
                        struct streamCtx *ctx)
{
    if (instance >= 2 || ctx->buffer == NULL || ctx->bufSize == 0
        || (ctx->bufMode != BUF_LINEAR && ctx->bufMode != BUF_CIRC_DOUBLE)
        || (ctx->bufMode == BUF_CIRC_DOUBLE && ctx->bufSize % 2 != 0))
        return -EINVAL;

    bool input = *(const bool *)config;
    struct audio_stream *stream = input ? &inputs[instance] :
                                          &outputs[instance];
    k_mutex_lock(&audio_mutex, K_FOREVER);
    int ret = 0;
    if (!initialized
        || !(input ? paths[instance][SINK_MCU] : paths[SOURCE_MCU][instance])) {
        ret = -ENODEV;
    } else if (stream->busy || (stream->ctx != NULL && stream->ctx != ctx)) {
        ret = -EBUSY;
    } else if (stream->ctx == ctx && ctx->bufMode == BUF_LINEAR) {
        /* Re-arm linear capture without resetting rational phase/history. */
        ctx->running = stream->error == 0;
        ret = stream->error;
    } else if (!c62_resampler_init(&stream->converter,
                                   input ? SAMPLE_RATE : ctx->sampleRate,
                                   input ? ctx->sampleRate : SAMPLE_RATE)) {
        ret = -EINVAL;
    } else {
        stream->ctx = ctx;
#ifdef CONFIG_C62_RX_LEVEL_DIAGNOSTICS
        if (input && instance == SOURCE_RTX)
            rx_level.count = 0;
#endif
        stream->half = stream->data_half = 0;
        stream->error = 0;
        stream->converted_count = stream->converted_pos = 0;
        stream->play_until = 0;
        stream->clock_fraction = 0;
        queue_reset(&stream->queue);
        k_sem_reset(&stream->wake);
        ctx->priv = stream;
        ctx->running = 1;
        if (!input && !track_active[instance]) {
            AudioTrack_start(&tracks[instance]);
            track_active[instance] = true;
        }
    }
    k_mutex_unlock(&audio_mutex);
    return ret;
}

static int stream_data(struct streamCtx *ctx, stream_sample_t **buffer)
{
    struct audio_stream *stream = ctx->priv;
    int count = -EIO;
    *buffer = NULL;
    k_mutex_lock(&audio_mutex, K_FOREVER);
    if (stream != NULL && stream->ctx == ctx && stream->error == 0) {
        count = ctx->bufSize / (ctx->bufMode == BUF_CIRC_DOUBLE ? 2 : 1);
        *buffer = ctx->buffer + count * stream->data_half;
    }
    k_mutex_unlock(&audio_mutex);
    return count;
}

static int capture_samples(struct audio_stream *stream, struct streamCtx *ctx,
                           size_t count)
{
    size_t filled = 0;
    int64_t deadline = k_uptime_get() + IO_TIMEOUT_MS;
    while (filled < count) {
        k_mutex_lock(&audio_mutex, K_FOREVER);
        if (!stream_valid(stream, ctx)) {
            k_mutex_unlock(&audio_mutex);
            return -EIO;
        }
        while (filled < count) {
            if (stream->converted_pos == stream->converted_count) {
                if (stream->queue.count == 0)
                    break;
                stream->converted_count = c62_resampler_push(
                    &stream->converter, queue_pop(&stream->queue),
                    stream->converted);
                stream->converted_pos = 0;
            }
            while (stream->converted_pos < stream->converted_count
                   && filled < count) {
                int32_t sample = stream->converted[stream->converted_pos++];
#ifdef CONFIG_C62_RX_BASEBAND_INVERT
                /* Invert only software RX baseband, not MIC or FM bypass. */
                if (stream->endpoint == SOURCE_RTX)
                    sample = MIN(-sample, INT16_MAX);
#endif
                ctx->buffer[stream->half * count + filled++] = (int16_t)sample;
            }
        }
        k_mutex_unlock(&audio_mutex);
        if (filled == count)
            return 0;
        if (k_uptime_get() >= deadline)
            return -ETIMEDOUT;
        k_sem_take(&stream->wake, K_MSEC(10));
    }
    return 0;
}

/* Enqueue a caller-owned block, with bounded waits and cancellation checks. */
static int play_samples(struct audio_stream *stream, struct streamCtx *ctx,
                        const int16_t *samples, size_t count)
{
    size_t consumed = 0;
    int64_t deadline = k_uptime_get() + IO_TIMEOUT_MS;
    for (;;) {
        k_mutex_lock(&audio_mutex, K_FOREVER);
        if (!stream_valid(stream, ctx)) {
            k_mutex_unlock(&audio_mutex);
            return -EIO;
        }
        if (consumed == count
            && stream->converted_pos == stream->converted_count) {
            k_mutex_unlock(&audio_mutex);
            return 0;
        }
        AudioTrack *track = &tracks[stream->endpoint];
        AudioTrack_Buffer buffer = { .frameCount = WORK_SAMPLES };
        int ret = AudioTrack_obtainBuffer(track, &buffer, 0);
        size_t written = 0;
        if (ret == 0 && buffer.frameCount != 0) {
            while (written < buffer.frameCount) {
                if (stream->converted_pos == stream->converted_count) {
                    if (consumed == count)
                        break;
                    int32_t sample = samples[consumed++];
                    /* Leave analog FM bypass and speaker audio unchanged. */
                    if (stream->endpoint == SINK_RTX) {
                        sample = sample * CONFIG_C62_TX_BASEBAND_LEVEL_PERCENT
                               / 100;
#ifdef CONFIG_C62_TX_BASEBAND_INVERT
                        /* Use the wide accumulator to handle INT16_MIN. */
                        sample = MIN(-sample, INT16_MAX);
#endif
                    }
                    stream->converted_count = c62_resampler_push(
                        &stream->converter, (int16_t)sample, stream->converted);
                    stream->converted_pos = 0;
                }
                while (stream->converted_pos < stream->converted_count
                       && written < buffer.frameCount)
                    buffer.i16[written++] =
                        stream->converted[stream->converted_pos++];
            }
            buffer.frameCount = written;
            AudioTrack_releaseBuffer(track, &buffer);
        }
        k_mutex_unlock(&audio_mutex);
        if (written != 0)
            deadline = k_uptime_get() + IO_TIMEOUT_MS;
        else if (k_uptime_get() >= deadline)
            return -ETIMEDOUT;
        if (ret != 0)
            k_sem_take(&stream->wake, K_MSEC(1));
    }
}

static int wait_playback(struct audio_stream *stream, struct streamCtx *ctx,
                         int64_t until)
{
    for (;;) {
        k_mutex_lock(&audio_mutex, K_FOREVER);
        bool valid = stream_valid(stream, ctx);
        k_mutex_unlock(&audio_mutex);
        if (!valid)
            return -EIO;
        int64_t remaining = until - k_uptime_get();
        if (remaining <= 0)
            return 0;
        k_sem_take(&stream->wake, K_MSEC(MIN(remaining, 10)));
    }
}

static int stream_sync(struct streamCtx *ctx, uint8_t dirty)
{
    (void)dirty;
    struct audio_stream *stream = ctx->priv;
    k_mutex_lock(&audio_mutex, K_FOREVER);
    if (stream == NULL || !stream_valid(stream, ctx) || stream->busy) {
        k_mutex_unlock(&audio_mutex);
        return -1;
    }
    stream->busy = true;
    size_t count = ctx->bufSize / (ctx->bufMode == BUF_CIRC_DOUBLE ? 2 : 1);
    unsigned int half = stream->half;
    /* Anchor to the sample clock, not to when the producer finishes its work. */
    uint64_t duration = (uint64_t)count * 1000 + stream->clock_fraction;
    int64_t until = stream->play_until != 0 ? stream->play_until :
                                              k_uptime_get();
    until += duration / ctx->sampleRate;
    k_mutex_unlock(&audio_mutex);

    int ret;
    if (stream->input) {
        ret = capture_samples(stream, ctx, count);
    } else {
        /* sync(false) plays the existing half, as a DMA-backed driver would. */
        ret = play_samples(stream, ctx, ctx->buffer + half * count, count);
        if (ret == 0)
            ret = wait_playback(stream, ctx, until);
    }

    k_mutex_lock(&audio_mutex, K_FOREVER);
    if (stream->ctx == ctx) {
        if (ret == 0) {
            stream->data_half = half;
            if (ctx->bufMode == BUF_CIRC_DOUBLE)
                stream->half ^= 1;
            else
                ctx->running = 0;
            if (!stream->input) {
                stream->data_half = stream->half;
                stream->play_until = until;
                stream->clock_fraction = duration % ctx->sampleRate;
            }
        } else {
            stream_fail(stream, ret);
        }
    }
    stream->busy = false;
    k_mutex_unlock(&audio_mutex);
    return ret;
}

/* Flush FIR history and the final partial DSP frame before unkeying. */
static int drain_output(struct audio_stream *stream, struct streamCtx *ctx)
{
    static const int16_t zeros[C62_RESAMPLER_HISTORY] = { 0 };
    int ret = play_samples(stream, ctx, zeros, stream->converter.length);
    if (ret != 0)
        return ret;

    int64_t deadline = k_uptime_get() + IO_TIMEOUT_MS;
    for (;;) {
        k_mutex_lock(&audio_mutex, K_FOREVER);
        if (!stream_valid(stream, ctx)) {
            k_mutex_unlock(&audio_mutex);
            return -EIO;
        }
        AudioTrack *track = &tracks[stream->endpoint];
        if (track->mStreamFrame != NULL) {
            AudioTrack_Buffer buffer = { .frameCount =
                                             track->mStreamFrameAvail };
            if (AudioTrack_obtainBuffer(track, &buffer, 0) == 0) {
                memset(buffer.raw, 0, buffer.size);
                AudioTrack_releaseBuffer(track, &buffer);
            }
        }
        ICStream_Producer_fetchRemote(track->mICStream);
        const ICStreamFifo *fifo = &track->mICStream->share->fifo;
        bool empty = fifo_avail(fifo) == fifo->size;
        /* A consumed frame may still be in the DSP/DAC pipeline. */
        uint32_t latency = track->mLatency + 20;
        k_mutex_unlock(&audio_mutex);
        if (empty)
            return wait_playback(stream, ctx, k_uptime_get() + latency);
        if (k_uptime_get() >= deadline)
            return -ETIMEDOUT;
        k_sem_take(&stream->wake, K_MSEC(1));
    }
}

static void stream_terminate(struct streamCtx *ctx)
{
    k_mutex_lock(&audio_mutex, K_FOREVER);
    struct audio_stream *stream = ctx->priv;
    if (stream != NULL && stream->ctx == ctx) {
        if (!stream->input && track_active[stream->endpoint]) {
            AudioTrack_stop(&tracks[stream->endpoint]);
            AudioTrack_flush(&tracks[stream->endpoint]);
            track_active[stream->endpoint] = false;
        }
        stream_detach(stream);
    }
    k_mutex_unlock(&audio_mutex);
}

static void stream_stop(struct streamCtx *ctx)
{
    k_mutex_lock(&audio_mutex, K_FOREVER);
    struct audio_stream *stream = ctx->priv;
    bool drain = stream != NULL && stream_valid(stream, ctx) && !stream->input
              && !stream->busy;
    if (drain)
        stream->busy = true;
    k_mutex_unlock(&audio_mutex);
    if (drain) {
        int ret = drain_output(stream, ctx);
        k_mutex_lock(&audio_mutex, K_FOREVER);
        if (ret != 0 && stream->ctx == ctx)
            stream_fail(stream, ret);
        stream->busy = false;
        k_mutex_unlock(&audio_mutex);
    }
    stream_terminate(ctx);
}

static const struct audioDriver stream_driver = {
    .start = stream_start,
    .data = stream_data,
    .sync = stream_sync,
    .stop = stream_stop,
    .terminate = stream_terminate,
};

const struct audioDevice outputDevices[] = {
    { NULL, NULL, 0, SINK_MCU },
    { &stream_driver, &output_config, SINK_RTX, SINK_RTX },
    { &stream_driver, &output_config, SINK_SPK, SINK_SPK },
};

const struct audioDevice inputDevices[] = {
    { NULL, NULL, 0, SOURCE_MCU },
    { &stream_driver, &input_config, SOURCE_RTX, SOURCE_RTX },
    { &stream_driver, &input_config, SOURCE_MIC, SOURCE_MIC },
};

void audio_init(void)
{
    k_mutex_lock(&audio_mutex, K_FOREVER);
    if (initialized || terminating) {
        k_mutex_unlock(&audio_mutex);
        return;
    }
    String8 param;
    String8_ctor_char(&param, "ADC_PDM_GAIN_A_LEFT=6;ADC_PDM_GAIN_D_LEFT=20;"
                              "ADC_PDM_GAIN_A_RIGHT=6;ADC_PDM_GAIN_D_RIGHT=20");
    int ret = AudioSystem_setParameters(0, &param);
    String8_dtor(&param);
    if (ret != 0)
        goto fail;
    ret = AudioRecord_ctor(&record, 0, SAMPLE_RATE, PCM_16_BIT,
                           CHANNEL_IN_LEFT | CHANNEL_IN_RIGHT, 0, NULL);
    if (ret != 0)
        goto fail;
    ret = AudioTrack_ctor(&tracks[SINK_SPK], SAMPLE_RATE, PCM_16_BIT,
                          CHANNEL_OUT_FRONT_LEFT, 0, NULL);
    if (ret != 0)
        goto fail_record;
    ret = AudioTrack_ctor(&tracks[SINK_RTX], SAMPLE_RATE, PCM_16_BIT,
                          CHANNEL_OUT_FRONT_RIGHT, 0, NULL);
    if (ret != 0)
        goto fail_speaker;
    for (unsigned int i = 0; i < 2; i++) {
        memset(&inputs[i], 0, sizeof(inputs[i]));
        memset(&outputs[i], 0, sizeof(outputs[i]));
        inputs[i].input = true;
        inputs[i].endpoint = outputs[i].endpoint = i;
        k_sem_init(&inputs[i].wake, 0, 1);
        k_sem_init(&outputs[i].wake, 0, 1);
        queue_reset(&bypass[i]);
    }
    memset(paths, 0, sizeof(paths));
    record_active = false;
    track_active[0] = track_active[1] = false;
    initialized = worker_running = true;
    k_thread_create(&worker, worker_stack, K_THREAD_STACK_SIZEOF(worker_stack),
                    audio_worker, NULL, NULL, NULL, 5, 0, K_NO_WAIT);
    k_thread_name_set(&worker, "c62_audio");
    k_mutex_unlock(&audio_mutex);
    return;
fail_speaker:
    AudioTrack_dtor(&tracks[SINK_SPK]);
fail_record:
    AudioRecord_dtor(&record);
fail:
    LOG_ERR("Audio initialization failed: %d", ret);
    k_mutex_unlock(&audio_mutex);
}

/* Keep vendor device lifetime separate from individual software streams. */
static void update_devices(void)
{
    bool capture = false;
    bool rx = false;
    for (unsigned int sink = 0; sink < 3; sink++) {
        capture |= paths[SOURCE_MIC][sink] || paths[SOURCE_RTX][sink];
        rx |= paths[SOURCE_RTX][sink];
    }
    if (capture != record_active) {
        if (capture)
            AudioRecord_start(&record);
        else
            AudioRecord_stop(&record);
        record_active = capture;
    }
    if (rx)
        radio_enableAfOutput();
    else
        radio_disableAfOutput();

    for (unsigned int sink = 0; sink < 2; sink++) {
        bool playback = false;
        for (unsigned int source = 0; source < 3; source++)
            playback |= paths[source][sink];
        if (playback != track_active[sink]) {
            if (playback) {
                AudioTrack_start(&tracks[sink]);
            } else {
                AudioTrack_stop(&tracks[sink]);
                AudioTrack_flush(&tracks[sink]);
                queue_reset(&bypass[sink]);
            }
            track_active[sink] = playback;
        }
    }
    gpio_pin_set_dt(&speaker_enable, track_active[SINK_SPK]);
}

void audio_connect(const enum AudioSource source, const enum AudioSink sink)
{
    if (source > SOURCE_MCU || sink > SINK_MCU)
        return;
    audio_init();
    k_mutex_lock(&audio_mutex, K_FOREVER);
    if (initialized && !paths[source][sink]) {
        paths[source][sink] = true;
        update_devices();
    }
    k_mutex_unlock(&audio_mutex);
}

void audio_disconnect(const enum AudioSource source, const enum AudioSink sink)
{
    if (source > SOURCE_MCU || sink > SINK_MCU)
        return;
    k_mutex_lock(&audio_mutex, K_FOREVER);
    if (initialized && paths[source][sink]) {
        paths[source][sink] = false;
        if (sink == SINK_MCU && source != SOURCE_MCU)
            stream_detach(&inputs[source]);
        if (source == SOURCE_MCU && sink != SINK_MCU)
            stream_detach(&outputs[sink]);
        if (sink != SINK_MCU)
            queue_reset(&bypass[sink]);
        update_devices();
    }
    k_mutex_unlock(&audio_mutex);
}

void audio_terminate(void)
{
    k_mutex_lock(&audio_mutex, K_FOREVER);
    if (!initialized) {
        k_mutex_unlock(&audio_mutex);
        return;
    }
    worker_running = false;
    terminating = true;
    for (unsigned int i = 0; i < 2; i++) {
        stream_detach(&inputs[i]);
        stream_detach(&outputs[i]);
    }
    memset(paths, 0, sizeof(paths));
    update_devices();
    initialized = false;
    k_mutex_unlock(&audio_mutex);
    k_thread_join(&worker, K_FOREVER);
    /* Let cancelled callers leave before their state can be reinitialised. */
    for (;;) {
        k_mutex_lock(&audio_mutex, K_FOREVER);
        bool busy = inputs[0].busy || inputs[1].busy || outputs[0].busy
                 || outputs[1].busy;
        k_mutex_unlock(&audio_mutex);
        if (!busy)
            break;
        k_sleep(K_MSEC(1));
    }
    AudioRecord_dtor(&record);
    AudioTrack_dtor(&tracks[SINK_SPK]);
    AudioTrack_dtor(&tracks[SINK_RTX]);
    k_mutex_lock(&audio_mutex, K_FOREVER);
    terminating = false;
    k_mutex_unlock(&audio_mutex);
}

bool audio_checkPathCompatibility(const enum AudioSource p1Source,
                                  const enum AudioSink p1Sink,
                                  const enum AudioSource p2Source,
                                  const enum AudioSink p2Sink)
{
    return (p1Source != p2Source || p1Source == SOURCE_MCU)
        && (p1Sink != p2Sink || p1Sink == SINK_MCU);
}
