/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <hwconfig.h>
#include <interfaces/delays.h>
#include <interfaces/radio.h>
#include <cstddef>

#include <algorithm>
#include <string>

#include "drivers/baseband/BK4819.h"
#include "radioUtils.h"

/* platform function to control APC */
extern "C" {
void platform_set_tx_power(uint8_t power_percent);
}

static const rtxStatus_t
    *config; // Pointer to data structure with radio configuration

static enum opstatus radioStatus; // Current operating status

static enum opmode radioMode = OPMODE_FM;

/*
 * Flat baseband register fields documented by Rob Riggs, WX9O (Mobilinkd):
 * https://github.com/egzumer/uv-k5-firmware-custom/pull/583
 * M17 RX uses AF9, REG_47 bit 1 and RX gain2=0x3f as described at
 * https://wiki.m17foundation.org/index.php?title=A36Plus
 * Preserve other C62 fields and restore the saved values for FM/TX.
 * REG_40 is a raw calibration value, not a deviation expressed in Hz.
 */
static const struct {
    bk4819_reg_t reg;
    uint16_t mask;
    uint16_t digital;
} digitalProfile[] = {
    { BK4819_REG_19, 0x8000, 0x8000 }, // Disable microphone AGC
    { BK4819_REG_2B, 0x0707, 0x0707 }, // Bypass speech filters and emphasis
    { BK4819_REG_31, 0x000c, 0x0000 }, // Disable compander and VOX
    { BK4819_REG_40,
      BK4819_REG40_TX_DEVIATION_ENABLE | BK4819_REG40_TX_DEVIATION_MASK,
      BK4819_REG40_TX_DEVIATION_ENABLE | CONFIG_C62_M17_DEVIATION },
    // 4.5 kHz RF / 3.75 kHz weak-signal filter, 12.5 kHz channel
    { BK4819_REG_43, 0x7ffc, 0x7808 },
    { BK4819_REG_47, 0x0003, 0x0001 }, // Save bit 1; bypass TX filter
    { BK4819_REG_48, 0x0ff0, CONFIG_C62_M17_RX_GAIN1 << 10 },
    { BK4819_REG_4B, 0x0020, 0x0020 }, // Disable audio limiter
    { BK4819_REG_50, 0x8000, 0x0000 }, // Unmute TX after local beeps
    { BK4819_REG_70, 0x8080, 0x0000 }, // Disable tone generators
    { BK4819_REG_7D, 0x001f, 0x0000 }, // Fixed minimum microphone gain
    { BK4819_REG_7E, 0x803f, 0x0000 }, // DC filters off, RF AGC on in RX
};
static constexpr size_t profileSize = sizeof(digitalProfile)
                                    / sizeof(digitalProfile[0]);
static uint16_t fmProfile[profileSize];

static void apply_digital_profile(bool transmit)
{
    for (size_t i = 0; i < profileSize; ++i) {
        const auto &field = digitalProfile[i];
        uint16_t value = BK4819_readReg(&c62_bk4819, field.reg);
        uint16_t digital = field.digital;
        if (field.reg == BK4819_REG_47)
            digital |= transmit ? (fmProfile[i] & 0x0002) : 0x0002;
        if (field.reg == BK4819_REG_48)
            digital |= transmit ? (fmProfile[i] & 0x03f0) : 0x03f0;
        if (field.reg == BK4819_REG_7E && transmit)
            digital |= 0x8000; // Fixed gain during TX only
        BK4819_writeReg(&c62_bk4819, field.reg,
                        (value & ~field.mask) | digital);
    }
    bk4819_disable_ctdcss(&c62_bk4819);
}

/* The BK4819 GPIOs to control the C62 LNA and PA */
enum {
    GPIO_VHF_RX_LNA = 0,
    GPIO_UHF_RX_LNA,
    GPIO_VHF_TX_PA,
    GPIO_UHF_TX_PA,
    GPIO_ALC_TX_LED
};

/**
 * Calculate DCS parity and compose
 */
static uint32_t cdcss_compose(uint16_t cdcss_code)
{
    uint32_t data = 0;

    data |= (((cdcss_code & 0x01) ^ ((cdcss_code >> 1) & 0x01)
              ^ ((cdcss_code >> 2) & 0x01) ^ ((cdcss_code >> 3) & 0x01)
              ^ ((cdcss_code >> 4) & 0x01) ^ ((cdcss_code >> 7) & 0x01))
             << 12);
    data |= (!(((cdcss_code >> 1) & 0x01) ^ ((cdcss_code >> 2) & 0x01)
               ^ ((cdcss_code >> 3) & 0x01) ^ ((cdcss_code >> 4) & 0x01)
               ^ ((cdcss_code >> 5) & 0x01) ^ ((cdcss_code >> 8) & 0x01))
             << 13);
    data |= (((cdcss_code & 0x01) ^ ((cdcss_code >> 1) & 0x01)
              ^ ((cdcss_code >> 5) & 0x01) ^ ((cdcss_code >> 6) & 0x01)
              ^ ((cdcss_code >> 8) & 0x01))
             << 14);
    data |= (!(((cdcss_code >> 1) & 0x01) ^ ((cdcss_code >> 2) & 0x01)
               ^ ((cdcss_code >> 6) & 0x01) ^ ((cdcss_code >> 7) & 0x01)
               ^ ((cdcss_code >> 8) & 0x01))
             << 15);
    data |= (!((cdcss_code & 0x01) ^ ((cdcss_code >> 1) & 0x01)
               ^ ((cdcss_code >> 4) & 0x01) ^ ((cdcss_code >> 8) & 0x01))
             << 16);
    data |= (!(((cdcss_code >> 1) & 0x01) ^ ((cdcss_code >> 3) & 0x01)
               ^ ((cdcss_code >> 4) & 0x01) ^ ((cdcss_code >> 5) & 0x01)
               ^ ((cdcss_code >> 7) & 0x01))
             << 17);
    data |= (((cdcss_code & 0x01) ^ ((cdcss_code >> 2) & 0x01)
              ^ ((cdcss_code >> 3) & 0x01) ^ ((cdcss_code >> 5) & 0x01)
              ^ ((cdcss_code >> 6) & 0x01) ^ ((cdcss_code >> 7) & 0x01)
              ^ ((cdcss_code >> 8) & 0x01))
             << 18);
    data |= ((((cdcss_code >> 1) & 0x01) ^ ((cdcss_code >> 3) & 0x01)
              ^ ((cdcss_code >> 4) & 0x01) ^ ((cdcss_code >> 6) & 0x01)
              ^ ((cdcss_code >> 7) & 0x01) ^ ((cdcss_code >> 8) & 0x01))
             << 19);
    data |= ((((cdcss_code >> 2) & 0x01) ^ ((cdcss_code >> 4) & 0x01)
              ^ ((cdcss_code >> 5) & 0x01) ^ ((cdcss_code >> 7) & 0x01)
              ^ ((cdcss_code >> 8) & 0x01))
             << 20);
    data |= (!(((cdcss_code >> 3) & 0x01) ^ ((cdcss_code >> 5) & 0x01)
               ^ ((cdcss_code >> 6) & 0x01) ^ ((cdcss_code >> 8) & 0x01))
             << 21);
    data |= (!((cdcss_code & 0x01) ^ ((cdcss_code >> 1) & 0x01)
               ^ ((cdcss_code >> 2) & 0x01) ^ ((cdcss_code >> 3) & 0x01)
               ^ ((cdcss_code >> 6) & 0x01))
             << 22);

    data |= (0x04 << 9);

    data |= cdcss_code;

    return data;
}

void radio_init(const rtxStatus_t *rtxState)
{
    config = rtxState;
    radioStatus = OFF;
    radioMode = OPMODE_FM;

    BK4819_SetAF(&c62_bk4819, 0);

    // OpenRTX handles squelch in every mode; keep hardware squelch open.
    bk4819_set_Squelch(&c62_bk4819, 0, 0, 0x7f, 0x7f, 0xff, 0xff);

    bk4819_gpio_pin_set(&c62_bk4819, GPIO_VHF_RX_LNA,
                        false); // VHF RX LNA
    bk4819_gpio_pin_set(&c62_bk4819, GPIO_UHF_RX_LNA,
                        false); // UHF RX LNA
    bk4819_gpio_pin_set(&c62_bk4819, GPIO_VHF_TX_PA,
                        false); // VHF TX PA
    bk4819_gpio_pin_set(&c62_bk4819, GPIO_UHF_TX_PA,
                        false); // UHF TX PA
    bk4819_gpio_pin_set(&c62_bk4819, GPIO_ALC_TX_LED,
                        false); // ALC / TX LED

    /* MIC bias settles only with the ADC enabled and RX DSP disabled.
     * Keep both external PAs off; this delay is paid at startup, not PTT.
     */
    uint16_t power = BK4819_readReg(&c62_bk4819, BK4819_REG_30);
    BK4819_writeReg(&c62_bk4819, BK4819_REG_30, BK4819_REG30_MIC_ADC_ENABLE);
    delayMs(CONFIG_C62_MIC_BIAS_SETTLE_MS);
    BK4819_writeReg(&c62_bk4819, BK4819_REG_30, power);
}

void radio_terminate()
{
}

void radio_setBandwidth(const uint8_t bandwidth)
{
    if (radioMode != OPMODE_M17)
        bk4819_SetFilterBandwidth(&c62_bk4819, bandwidth);
}

void radio_tuneVcxo(const int16_t vhfOffset, const int16_t uhfOffset)
{
    (void)vhfOffset;
    (void)uhfOffset;
}

void radio_setOpmode(const enum opmode mode)
{
    if (mode == radioMode)
        return;

    /* The core calls this before disabling the old mode. Unkey first. */
    radio_disableRtx();
    radio_disableAfOutput();
    if (radioMode == OPMODE_M17) {
        for (size_t i = 0; i < profileSize; ++i) {
            const auto &field = digitalProfile[i];
            uint16_t value = BK4819_readReg(&c62_bk4819, field.reg);
            BK4819_writeReg(&c62_bk4819, field.reg,
                            (value & ~field.mask) | fmProfile[i]);
        }
    }
    if (mode == OPMODE_M17) {
        for (size_t i = 0; i < profileSize; ++i) {
            const auto &field = digitalProfile[i];
            fmProfile[i] = BK4819_readReg(&c62_bk4819, field.reg) & field.mask;
        }
        apply_digital_profile(false);
    }
    radioMode = mode;
    radio_setBandwidth(config->bandwidth);
}

bool radio_checkRxDigitalSquelch()
{
    return bk4819_get_ctcss(&c62_bk4819);
}

void radio_enableAfOutput()
{
    BK4819_SetAF(&c62_bk4819, radioMode == OPMODE_M17 ? 9 : 1);
}

void radio_disableAfOutput()
{
    BK4819_SetAF(&c62_bk4819, 0);
}

void radio_checkVOX()
{
    return;
    radio_disableRtx();
    if (bk4819_get_vox(&c62_bk4819)) {
        if (radioStatus != TX)
            radio_enableTx();
    } else {
        if (radioStatus != RX)
            radio_enableRx();
    }
}

void radio_setRxFilters(uint32_t freq)
{
    if (freq < 174000000) {
        bk4819_gpio_pin_set(&c62_bk4819, GPIO_VHF_RX_LNA,
                            true); // VHF RX LNA
    } else {
        bk4819_gpio_pin_set(&c62_bk4819, GPIO_UHF_RX_LNA,
                            true); // UHF RX LNA
    }
}

void radio_enableRx()
{
    bk4819_gpio_pin_set(&c62_bk4819, 0, false); // VHF RX LNA
    bk4819_gpio_pin_set(&c62_bk4819, 1, false); // UHF RX LNA
    bk4819_gpio_pin_set(&c62_bk4819, 2, false); // UHF TX PA
    bk4819_gpio_pin_set(&c62_bk4819, 3, false); // UHF TX PA
    bk4819_gpio_pin_set(&c62_bk4819, 4, false); // ALC / TX LED

    radio_setRxFilters(config->rxFrequency);
    bk4819_set_freq(&c62_bk4819, config->rxFrequency);

    if (radioMode == OPMODE_M17)
        apply_digital_profile(false);

    if (radioMode == OPMODE_FM && config->rxToneEn) {
        bk4819_enable_rx_ctcss(&c62_bk4819, config->rxTone);
    }

    bk4819_rx_on(&c62_bk4819);
    radioStatus = RX;
}

void radio_enableTx()
{
    if (config->txDisable == 1 || config->txFrequency < 136000000
        || config->txFrequency > 600000000) {
        radio_disableRtx();
        return;
    }

    bk4819_gpio_pin_set(&c62_bk4819, GPIO_VHF_RX_LNA,
                        false); // VHF RX LNA
    bk4819_gpio_pin_set(&c62_bk4819, GPIO_UHF_RX_LNA,
                        false); // UHF RX LNA
    bk4819_gpio_pin_set(&c62_bk4819, GPIO_VHF_TX_PA,
                        false); // VHF TX PA
    bk4819_gpio_pin_set(&c62_bk4819, GPIO_UHF_TX_PA,
                        false); // UHF TX PA
    bk4819_gpio_pin_set(&c62_bk4819, GPIO_ALC_TX_LED,
                        false); // ALC / TX LED

    bk4819_set_freq(&c62_bk4819, config->txFrequency);

    if (radioMode == OPMODE_M17)
        apply_digital_profile(true);

    if (radioMode == OPMODE_FM && config->txToneEn) {
        bk4819_enable_tx_ctcss(&c62_bk4819, config->txTone);
    }

    if (config->txFrequency < 174000000) {
        bk4819_gpio_pin_set(&c62_bk4819, GPIO_VHF_TX_PA,
                            true); // VHF TX PA
    } else {
        bk4819_gpio_pin_set(&c62_bk4819, GPIO_UHF_TX_PA,
                            true); // UHF TX PA
    }

    bk4819_gpio_pin_set(&c62_bk4819, GPIO_ALC_TX_LED,
                        true); // ALC / TX LED

    // depending on power level set PWM duty cycle for APC voltage control
    // Maybe need table for this instead of crude linear mapping, and also consider frequency dependence of PA efficiency
    platform_set_tx_power(std::min(
        config->txPower * config->txPower * 100 / (5000 * 5000),
    100U)); // crude quadratic mapping of power to duty cycle, max at 5W

    bk4819_tx_on(&c62_bk4819);
    radioStatus = TX;
}

void radio_disableRtx()
{
    bk4819_disable_ctdcss(&c62_bk4819);

    bk4819_gpio_pin_set(&c62_bk4819, GPIO_VHF_RX_LNA,
                        false); // VHF RX LNA
    bk4819_gpio_pin_set(&c62_bk4819, GPIO_UHF_RX_LNA,
                        false); // UHF RX LNA
    bk4819_gpio_pin_set(&c62_bk4819, GPIO_VHF_TX_PA,
                        false); // VHF TX PA
    bk4819_gpio_pin_set(&c62_bk4819, GPIO_UHF_TX_PA,
                        false); // UHF TX PA
    bk4819_gpio_pin_set(&c62_bk4819, GPIO_ALC_TX_LED,
                        false); // ALC / TX LED

    bk4819_rtx_off(&c62_bk4819);
    radioStatus = OFF;
}

void radio_updateConfiguration()
{
    // Set BK4819 PA Gain tuning according to TX power and frequency

    radio_setBandwidth(config->bandwidth);
    if (radioStatus == RX)
        radio_enableRx();
    else if (radioStatus == TX)
        radio_enableTx();
}

rssi_t radio_getRssi()
{
    return bk4819_get_rssi(&c62_bk4819);
}

enum opstatus radio_getStatus()
{
    return radioStatus;
}
