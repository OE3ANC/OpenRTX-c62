/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef C62_AUDIO_STREAM_H
#define C62_AUDIO_STREAM_H

#include <interfaces/audio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed C62 DSP transport. The bool device configuration selects input. */
extern const struct audioDriver c62_stream_driver;

/**
 * Configure the codec and start the DSP audio worker.
 * @return zero on success, a negative error code on failure.
 */
int c62_stream_init(void);

/** Cancel streams, join the worker and release DSP audio resources. */
void c62_stream_terminate(void);

/**
 * Enable or disable DSP audio for a board-selected route.
 * The caller handles GPIOs and radio AF muxing and ensures route compatibility.
 *
 * @param source: audio source.
 * @param sink: audio destination.
 * @param enabled: true to enable, false to disable and cancel its stream.
 */
void c62_stream_set_route(enum AudioSource source, enum AudioSink sink,
                          bool enabled);

#ifdef __cplusplus
}
#endif

#endif /* C62_AUDIO_STREAM_H */
