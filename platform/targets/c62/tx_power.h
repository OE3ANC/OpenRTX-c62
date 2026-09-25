/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef C62_TX_POWER_H
#define C62_TX_POWER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {

    uint8_t duty_percent;
    bool vhf;
} c62_tx_power_t;

/**
 * @brief Look up provisional TX PWM; no flash access or hardware I/O.
 * Uses all ten frequency/four-selector records from newer c62.bin, 0x3BA3B0.
 * Selects the nearest frequency in the same band; ties choose the lower one.
 * @param frequency_hz TX frequency, inclusive 136..174 or 400..480 MHz.
 * These are hardware limits, not legal permission to transmit.
 * @param power_mw Request in mW: (0,1000] selects raw selector 0,
 * (1000,2500] the floor of the mean of selectors 0/1, (2500,5000] selector 1.
 * Zero and values above 5000 are rejected. Selectors 2/3 are preserved only.
 * Bucket labels are provisional, not calibrated or interpolated RF output.
 * @param result Output control, cleared on failure; must not be NULL.
 * @return True on success; false means TX must remain disabled.
 */
bool c62_tx_power_lookup(uint32_t frequency_hz, uint32_t power_mw,
                         c62_tx_power_t *result);

/**
 * @brief Dump saved flash TX records to printk; read-only startup diagnostic.
 *
 * Finding the saved settings in a firmware dump
 * -------------------------------------------
 * Use a raw flash dump starting at physical flash offset zero, not an update
 * payload. All offsets below are FILE/PHYSICAL FLASH offsets. The observed
 * application CPU alias is 0x18000000 + offset; do not pass that CPU address
 * to flash_read(). The dump must cover the entire journal sector:
 *     [0x3B0000, 0x3C0000) -- 64 KiB.
 * A short dump can still cover this sector; that does not make it a complete
 * backup. These locations/layouts are verified for the compared C62/C22
 * builds, not a guarantee for every future firmware version.
 *
 * Starting at 0x3B0000, walk sequential configuration entries. The journal
 * contains historical copies, so the first table is not necessarily active.
 * Each entry begins with this four-byte header (all multibyte fields LE):
 *
 *   Entry offset   Size   Meaning
 *   +0x00          1      Marker 0x5A
 *   +0x01          1      Sum of payload bytes modulo 256
 *   +0x02          2      Total entry length, INCLUDING this header
 *   +0x04          ...    Payload
 *
 * Checksum: sum(dump[entry + 4 : entry + length]) & 0xFF.
 * This is an 8-bit additive checksum, not a CRC or proof of semantic validity.
 * The two observed layouts are:
 *
 *   Entry length   RF slots   RF table bytes
 *   0x0BDC         10         0x168
 *   0x0D48         20         0x2D0 (ten populated, ten zero in our dump)
 *
 * Require a recognized length and sector bounds before checking its payload.
 * Advance by the recognized entry length, even after a checksum failure, and
 * retain the last checksum-valid entry. Entirely zero/0xFF headers terminate
 * the scan. This diagnostic stops on malformed framing or a length change;
 * it does not guess new offsets or resynchronize inside arbitrary payloads.
 * If nothing validates, report no valid journal rather than invented values.
 *
 * RF record format
 * ----------------
 * The table begins at entry + 0x450 (ENTRY-relative, not payload-relative).
 * Slot i starts at entry + 0x450 + i * 0x24. Each record is 36 bytes:
 *
 *   Record offset  Size   Meaning
 *   +0x00          4      Frequency in Hz, unsigned LE32
 *   +0x04          2      TX selector 0 PWM duty percent, unsigned LE16
 *   +0x06          2      TX selector 1 PWM duty percent, unsigned LE16
 *   +0x08          2      TX selector 2 PWM duty percent, unsigned LE16
 *   +0x0A          2      TX selector 3 PWM duty percent, unsigned LE16
 *   +0x0C..+0x23   24     Other record fields; not decoded here, not padding
 *
 * Expected frequency reference points in the studied dumps:
 * VHF: 136125000, 146125000, 156125000, 166125000, 173975000 Hz.
 * UHF: 400125000, 420125000, 440125000, 460125000, 479975000 Hz.
 * The studied tables store UHF first, then VHF; identify bands by frequency,
 * not assumed slot ordering. Use the layout's slot count, never read twenty
 * slots from a ten-slot layout into unrelated settings. This diagnostic skips
 * wholly zero/erased records and flags unknown frequencies or duties >100.
 * It dumps the newest checksum-valid table even if records are suspicious;
 * it does not silently substitute a different table or apply values to TX.
 *
 * Verified examples (addresses move as settings are saved):
 *   Newer c62.bin: entry 0x3B9F60, table 0x3BA3B0, length 0x0D48.
 *   Older app.bin: entry 0x3B3B4C, table 0x3B3F9C, length 0x0BDC.
 *   C22.bin:       entry 0x3B7698, table 0x3B7AE8, length 0x0BDC.
 * In newer c62.bin, record 0x3BA3F8 contains 440125000 Hz followed by
 * 1F 00 4A 00 41 00 64 00: duties 31, 74, 65, 100 percent.
 * Do not hard-code these active-entry addresses for another unit/save state.
 *
 * Why these are TX controls: stock code selects the nearest frequency record,
 * uses channel byte 0 bits 7:6 to choose one of the four LE16 fields, and feeds
 * that value to a 100 kHz PWM driver. This establishes duty percentages, not
 * watts, low/high labels, or proof of individual factory calibration. Our
 * fixed 1/2.5/5 W labels remain provisional; current OpenRTX PWM stays 1 kHz.
 *
 * Called by radio_init() at startup with both external PAs off. Any additional
 * calls must run in a thread after flash/console initialization, with no
 * concurrent settings writes. Never called by TX lookup or on PTT.
 * Read-only: no flash writes/erases and no changes to the fixed power table.
 */
void c62_tx_power_dump_flash(void);

#ifdef __cplusplus
}
#endif

#endif
