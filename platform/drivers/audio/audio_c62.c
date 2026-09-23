/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <hwconfig.h>
#include <interfaces/radio.h>
#include "audio_stream_c62.h"

/* Board layer: select radio AF and speaker GPIOs, then tell the stream
 * driver which sources and sinks to connect. Sample transport lives there.
 * This lock serializes initialization and route changes; the audio worker
 * never takes it, so route callbacks may wait for the worker safely. */
static K_MUTEX_DEFINE(board_mutex);
static bool initialized;
static bool paths[3][3]; /* Active routes, indexed by source and sink. */
/* Both device tables use one driver; config selects capture or playback. */
static const bool input_config = true;
static const bool output_config = false;

const struct audioDevice outputDevices[] = {
    { NULL, NULL, 0, SINK_MCU },
    { &c62_stream_driver, &output_config, SINK_RTX, SINK_RTX },
    { &c62_stream_driver, &output_config, SINK_SPK, SINK_SPK },
};

const struct audioDevice inputDevices[] = {
    { NULL, NULL, 0, SOURCE_MCU },
    { &c62_stream_driver, &input_config, SOURCE_RTX, SOURCE_RTX },
    { &c62_stream_driver, &input_config, SOURCE_MIC, SOURCE_MIC },
};

/* Called with board_mutex held. */
static void init_audio(void)
{
    if (initialized)
        return;
    gpio_pin_configure_dt(&speaker_enable, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&dtmf_enable, GPIO_OUTPUT_INACTIVE);
    /* DSP audio setup may alter the shared console configuration. */
    c62_restore_console();
    initialized = c62_stream_init() == 0;
    c62_restore_console();
}

/* Keep each physical output enabled while any active route needs it. */
static void update_mux(void)
{
    bool radio = false;
    bool speaker = false;
    for (unsigned int i = 0; i < 3; i++) {
        radio |= paths[SOURCE_RTX][i];
        speaker |= paths[i][SINK_SPK];
    }
    if (radio)
        radio_enableAfOutput();
    else
        radio_disableAfOutput();
    gpio_pin_set_dt(&speaker_enable, speaker);
}

void audio_init(void)
{
    k_mutex_lock(&board_mutex, K_FOREVER);
    init_audio();
    k_mutex_unlock(&board_mutex);
}

void audio_terminate(void)
{
    k_mutex_lock(&board_mutex, K_FOREVER);
    if (initialized) {
        memset(paths, 0, sizeof(paths));
        update_mux();
        c62_stream_terminate();
        initialized = false;
    }
    k_mutex_unlock(&board_mutex);
}

void audio_connect(const enum AudioSource source, const enum AudioSink sink)
{
    if (source > SOURCE_MCU || sink > SINK_MCU)
        return;
    k_mutex_lock(&board_mutex, K_FOREVER);
    init_audio();
    if (initialized && !paths[source][sink]) {
        paths[source][sink] = true;
        c62_stream_set_route(source, sink, true);
        update_mux();
    }
    k_mutex_unlock(&board_mutex);
}

void audio_disconnect(const enum AudioSource source, const enum AudioSink sink)
{
    if (source > SOURCE_MCU || sink > SINK_MCU)
        return;
    k_mutex_lock(&board_mutex, K_FOREVER);
    if (initialized && paths[source][sink]) {
        paths[source][sink] = false;
        c62_stream_set_route(source, sink, false);
        update_mux();
    }
    k_mutex_unlock(&board_mutex);
}

bool audio_checkPathCompatibility(const enum AudioSource p1Source,
                                  const enum AudioSink p1Sink,
                                  const enum AudioSource p2Source,
                                  const enum AudioSink p2Sink)
{
    /* Physical sources and sinks are exclusive. MCU is a logical endpoint,
     * so independent capture/playback streams may share it. */
    return (p1Source != p2Source || p1Source == SOURCE_MCU)
        && (p1Sink != p2Sink || p1Sink == SINK_MCU);
}
