/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <hwconfig.h>
#include <interfaces/radio.h>
#include "audio_stream_c62.h"

LOG_MODULE_REGISTER(c62_audio, LOG_LEVEL_INF);

/* Stream I/O never takes this board setup/routing lock. */
static K_MUTEX_DEFINE(board_mutex);
static bool initialized;
static bool paths[3][3];
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

/* CP can reprogram shared pinmux and UART registers during audio setup.
 * Reassert the board console configuration without changing IRQ ownership. */
PINCTRL_DT_DEFINE(DT_CHOSEN(zephyr_console));

static void restore_console(void)
{
    const struct device *console = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
    const struct uart_config config = {
        .baudrate = DT_PROP(DT_CHOSEN(zephyr_console), current_speed),
        .parity = UART_CFG_PARITY_NONE,
        .stop_bits = UART_CFG_STOP_BITS_1,
        .data_bits = UART_CFG_DATA_BITS_8,
        .flow_ctrl = UART_CFG_FLOW_CTRL_NONE,
    };
    int pins = pinctrl_apply_state(
        PINCTRL_DT_DEV_CONFIG_GET(DT_CHOSEN(zephyr_console)),
        PINCTRL_STATE_DEFAULT);
    int uart = uart_configure(console, &config);
    if (pins != 0 || uart != 0)
        LOG_ERR("Console configuration failed: pins=%d uart=%d", pins, uart);
}

/* Called with board_mutex held. */
static void init_audio(void)
{
    if (initialized)
        return;
    gpio_pin_configure_dt(&speaker_enable, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&dtmf_enable, GPIO_OUTPUT_INACTIVE);
    restore_console();
    initialized = c62_stream_init() == 0;
    restore_console();
}

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
    return (p1Source != p2Source || p1Source == SOURCE_MCU)
        && (p1Sink != p2Sink || p1Sink == SINK_MCU);
}
