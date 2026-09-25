/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "tx_power.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>

/* All ten populated records from the newer original c62.bin, saved table
 * 0x3BA3B0: 20 slots of 36 bytes. Source SHA-256:
 * c91d007021776da46529b6a9180f4eec801946b2722aed81e293e1567045e09d
 * Preserve the frequency and all four raw selector duties, not the unrelated
 * record fields. Ascending frequency order makes nearest ties choose lower.
 * Selector indices are NOT proven ascending wattage levels; selector 3 is
 * 100% drive. Only selectors 0/1 feed our provisional power buckets.
 * TODO: keep this fixed baseline until measurements justify per-unit
 * calibration. The older app.bin differs and is not the source.
 */
static const struct tx_power_record {
    uint32_t frequency_hz;
    uint8_t duty_percent[4];
} tx_power_records[] = {
    { 136125000u, { 33, 67, 60, 100 } },
    { 146125000u, { 30, 57, 60, 100 } },
    { 156125000u, { 27, 64, 60, 100 } },
    { 166125000u, { 33, 73, 65, 100 } },
    { 173975000u, { 40, 74, 70, 100 } },
    { 400125000u, { 33, 75, 70, 100 } },
    { 420125000u, { 32, 74, 65, 100 } },
    { 440125000u, { 31, 74, 65, 100 } },
    { 460125000u, { 30, 76, 60, 100 } },
    { 479975000u, { 31, 75, 60, 100 } },
};

bool c62_tx_power_lookup(uint32_t frequency_hz, uint32_t power_mw,
                         c62_tx_power_t *result)
{
    if (!result)
        return false;
    *result = (c62_tx_power_t){ 0 };

    if (power_mw == 0u || power_mw > 5000u)
        return false;

    if (frequency_hz >= 136000000u && frequency_hz <= 174000000u) {
        result->vhf = true;
    } else if (frequency_hz >= 400000000u && frequency_hz <= 480000000u) {
        result->vhf = false;
    } else {
        return false;
    }

    const struct tx_power_record *nearest = NULL;
    uint32_t nearest_distance = UINT32_MAX;
    for (size_t i = 0; i < sizeof(tx_power_records)
                          / sizeof(tx_power_records[0]); ++i) {
        const struct tx_power_record *record = &tx_power_records[i];
        if ((record->frequency_hz < 174000000u) != result->vhf)
            continue;
        uint32_t distance = frequency_hz > record->frequency_hz
                          ? frequency_hz - record->frequency_hz
                          : record->frequency_hz - frequency_hz;
        if (distance < nearest_distance) {
            nearest = record;
            nearest_distance = distance;
        }
    }

    /* Both supported bands have records. Strictly smaller distance above
     * keeps the lower anchor on ties. No frequency/power interpolation.
     * Buckets are provisional, not measured RF output.
     */
    if (power_mw <= 1000u)
        result->duty_percent = nearest->duty_percent[0];
    else if (power_mw <= 2500u)
        result->duty_percent =
            (nearest->duty_percent[0] + nearest->duty_percent[1]) / 2u;
    else
        result->duty_percent = nearest->duty_percent[1];
    return true;
}

/* Physical flash offsets, not CPU addresses or partition-relative offsets. */
#define JOURNAL_START 0x3b0000u
#define JOURNAL_SIZE  0x10000u
#define TABLE_OFFSET  0x450u
#define RECORD_SIZE   36u

static bool dump_read(const struct device *flash, uint32_t offset,
                      void *data, size_t length)
{
    int err = flash_read(flash, offset, data, length);
    if (err) {
        printk("C62: flash read failed at 0x%08x: %d\n",
               (unsigned)offset, err);
        return false;
    }
    return true;
}

static bool dump_empty(const uint8_t *data, size_t length)
{
    bool zero = true;
    bool erased = true;
    for (size_t i = 0; i < length; ++i) {
        zero &= data[i] == 0;
        erased &= data[i] == 0xff;
    }
    return zero || erased;
}

static bool dump_known_frequency(uint32_t frequency)
{
    switch (frequency) {
    case 400125000u: case 420125000u: case 440125000u:
    case 460125000u: case 479975000u: case 136125000u:
    case 146125000u: case 156125000u: case 166125000u:
    case 173975000u:
        return true;
    default:
        return false;
    }
}

void c62_tx_power_dump_flash(void)
{
    /* flash0 is the memory node; flash_controller owns the flash API. */
    const struct device *flash =
        DEVICE_DT_GET(DT_CHOSEN(zephyr_flash_controller));
    if (!device_is_ready(flash)) {
        printk("C62: flash device unavailable\n");
        return;
    }

    uint32_t selected = 0;
    uint16_t selected_length = 0;
    uint16_t journal_length = 0;
    uint8_t selected_checksum = 0;
    uint8_t chunk[128];
    for (uint32_t pos = 0; pos < JOURNAL_SIZE;) {
        uint8_t header[4];
        uint32_t absolute = JOURNAL_START + pos;
        if (JOURNAL_SIZE - pos < sizeof(header)) {
            printk("C62: truncated header at 0x%08x\n", (unsigned)absolute);
            break;
        }
        if (!dump_read(flash, absolute, header, sizeof(header)))
            return;
        if (dump_empty(header, sizeof(header)))
            break;
        uint16_t length = sys_get_le16(header + 2);
        if (header[0] != 0x5a || (length != 0xbdc && length != 0xd48)) {
            printk("C62: malformed header at 0x%08x: %02x %02x %02x %02x; "
                   "scan stopped\n", (unsigned)absolute, header[0], header[1],
                   header[2], header[3]);
            break;
        }
        if (length > JOURNAL_SIZE - pos) {
            printk("C62: truncated entry at 0x%08x length=0x%x\n",
                   (unsigned)absolute, length);
            break;
        }
        if (journal_length && length != journal_length) {
            printk("C62: layout changed at 0x%08x: 0x%x -> 0x%x; "
                   "scan stopped\n", (unsigned)absolute, journal_length,
                   length);
            break;
        }
        journal_length = length;
        uint8_t checksum = 0;
        for (uint32_t offset = 4; offset < length;) {
            size_t count = length - offset;
            if (count > sizeof(chunk))
                count = sizeof(chunk);
            if (!dump_read(flash, absolute + offset, chunk, count))
                return;
            for (size_t i = 0; i < count; ++i)
                checksum += chunk[i];
            offset += count;
        }
        if (checksum == header[1]) {
            selected = absolute;
            selected_length = length;
            selected_checksum = checksum;
        } else {
            printk("C62: checksum mismatch at 0x%08x stored=%02x "
                   "computed=%02x; skipping entry\n", (unsigned)absolute,
                   header[1], checksum);
        }
        pos += length;
    }
    if (!selected_length) {
        printk("C62: no valid journal data\n");
        return;
    }

    unsigned slots = selected_length == 0xbdc ? 10 : 20;
    uint32_t table = selected + TABLE_OFFSET;
    printk("C62: selected entry=0x%08x table=0x%08x length=0x%x "
           "layout=%u slots checksum=%02x valid; diagnostic only\n",
           (unsigned)selected, (unsigned)table, selected_length,
           slots, selected_checksum);
    unsigned populated = 0;
    for (unsigned slot = 0; slot < slots; ++slot) {
        uint8_t record[RECORD_SIZE];
        uint32_t absolute = table + slot * RECORD_SIZE;
        if (!dump_read(flash, absolute, record, sizeof(record)))
            return;
        if (dump_empty(record, sizeof(record)))
            continue;
        uint32_t frequency = sys_get_le32(record);
        uint16_t d0 = sys_get_le16(record + 4);
        uint16_t d1 = sys_get_le16(record + 6);
        uint16_t d2 = sys_get_le16(record + 8);
        uint16_t d3 = sys_get_le16(record + 0xa);
        bool unknown = !dump_known_frequency(frequency);
        bool bad_duty = d0 > 100 || d1 > 100 || d2 > 100 || d3 > 100;
        printk("C62: slot=%u offset=0x%08x frequency=%u Hz "
               "duties[+4,+6,+8,+a]=%u,%u,%u,%u%s%s\n",
               slot, (unsigned)absolute, (unsigned)frequency, d0, d1, d2, d3,
               unknown ? " SUSPECT unknown frequency" : "",
               bad_duty ? " SUSPECT duty >100" : "");
        ++populated;
    }
    printk("C62: %u populated records; fixed TX table unchanged\n", populated);
}
