<!--
SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# Retevis C62

## Build

The matching DSP firmware is bundled in `resources/dsp_firmware.bin`, from
ListenAI SDK commit `2ac605527dcde3e90c814bb4175546cc7fbd36da`.
The build verifies and copies it to `build/zephyr/dsp_firmware.bin`.

Enter the ListenAI SDK environment, fetch the C62 dependencies from OpenRTX, and
build from the repository root:

```sh
lisa zep exec bash
west update openrtx-c62-modules-af openrtx-c62-modules-freertos_shims \
  openrtx-c62-modules-lsf openrtx-c62-modules-urpc
west build -p always -b c62 -d build .
```

For subsequent builds, use `west build -d build`.

## Flash

Close the serial monitor and flash both matching images:

```sh
cskburn -s /dev/ttyUSB0 -C 6 -b 115200 \
  0x000000 build/zephyr/zephyr.bin \
  0x100000 build/zephyr/dsp_firmware.bin
```

UART2 console output uses 115200 baud.

## Provisional fixed TX power controls

`tx_power.c` uses all ten populated frequency records from the newer original
`c62.bin`. TX accepts **136–174 MHz VHF and 400–480 MHz UHF, inclusive**;
frequencies in the gap or outside these hardware bands are rejected, not
clamped. These hardware limits are not legal permission to transmit.
The C62 `hwInfo` VHF minimum is also 136 MHz so UI/core limits allow 136–137 MHz.

The nearest reference frequency within the selected band supplies the duties;
exact-distance ties select the lower frequency. There is no interpolation or
flash access during TX; partitions are unchanged. All four raw stock selector
duties are retained, but only selectors 0/1 are used for TX:

| Reference frequency (Hz) | Selector 0 | Selector 1 | Selector 2 | Selector 3 |
| --- | --- | --- | --- | --- |
| 136125000 | 33% | 67% | 60% | 100% |
| 146125000 | 30% | 57% | 60% | 100% |
| 156125000 | 27% | 64% | 60% | 100% |
| 166125000 | 33% | 73% | 65% | 100% |
| 173975000 | 40% | 74% | 70% | 100% |
| 400125000 | 33% | 75% | 70% | 100% |
| 420125000 | 32% | 74% | 65% | 100% |
| 440125000 | 31% | 74% | 65% | 100% |
| 460125000 | 30% | 76% | 60% | 100% |
| 479975000 | 31% | 75% | 60% | 100% |

Source SHA-256: `c91d007021776da46529b6a9180f4eec801946b2722aed81e293e1567045e09d`.
Active entry: `0x3b9f60`, length `0xd48`, table: `0x3ba3b0`, 20 slots of
36 bytes (ten populated). The older `app.bin` is not the source and already
has different selector-0 values. This is an intentional newer-C62 baseline,
not proof of universal calibration. Compare dumps and hardware measurements
before introducing per-unit flash loading.

Requests use three coarse buckets: `0 < power <= 1000` mW selects selector 0,
`1000 < power <= 2500` selects the rounded-down arithmetic mean of selectors
0/1, and `2500 < power <= 5000` selects selector 1. Zero and values above
5000 mW are rejected. A 100 mW request still selects the provisional 1 W duty.
Invalid power/frequency or TX-disable leaves TX off. PWM driver success is
required before PA enable. TX logs requested power, frequency and duty.
**PWM now requests 100 kHz**, matching the stock trace, for comparison against
the earlier 1 kHz measurements. Duty values are unchanged. Pulse widths are
calculated in nanoseconds to avoid whole-microsecond duty quantization;
actual timer resolution and waveform still require hardware verification.
Watt labels are provisional, not measured output;
verify with a dummy load and power meter. Selector indices are not proven
ascending power levels; retaining selectors 2/3 does not expose extra levels.

### Startup saved-settings diagnostic

`c62_tx_power_dump_flash()` (declared in `tx_power.h`) is read-only and uses
`printk` on the UART2 console at 115200 baud. `radio_init()` calls it during
startup after disabling both external PAs and the TX LED, before MIC settling.
It is not called on PTT and never changes the active TX power table. Flash
device readiness is checked by the diagnostic. Keep settings writers idle
during the scan; it is not an atomic snapshot. Additional manual calls must
run in thread context, not an ISR or while holding the flash driver's locks.

The scanner reads physical flash offsets `0x3b0000..0x3bffff`, not CPU-mapped
addresses. It recognizes `0x5a` headers with LE16 lengths `0xbdc` (10 slots) or
`0xd48` (20 slots), and byte-1 checksum equal to the sum of bytes 4 through
length-1 modulo 256. It selects the last checksum-valid sequential entry;
checksum failures are reported and skipped without discarding earlier data.
Malformed framing, a change from the first recognized entry length, or a
truncated entry stops the scan with a warning; entirely
zero/erased headers end it normally. It never searches guessed alternate
addresses. Flash I/O failure reports the physical offset/error and aborts.

Output includes the selected entry/table (`entry + 0x450`), length, slot layout,
checksum and every populated 36-byte record's absolute offset, frequency in Hz,
and four raw LE16 duties at `+4/+6/+8/+a`. Only entirely zero/erased records are
empty. Unknown frequencies and duties above 100 are marked `SUSPECT`, not used
as calibration. Checksums use a 128-byte buffer, not a sector-sized stack copy.
**The compiled frequency table remains unchanged; flash data never drives TX.**

The device is `DT_CHOSEN(zephyr_flash_controller)` (`&flash` in `c62.dts`),
not the `zephyr_flash` memory node (`&flash0`). The vendor SDK driver binding
and target compilation remain unverified locally. There are no writes, erases
or partition changes. The journal overlaps the existing writable
`storage_partition` (`0x300000..0x3fffff`); this reader does not reserve/protect
it. Preserve this region when flashing or configuring storage.

### No-build checks

The external analysis workspace check (not a repository test) is:

```sh
python3 /home/sa/c62-analysis/verify_full_tx.py
```

It verifies the source dump SHA-256 and active journal checksum, all ten
frequency/four-selector rows, hardware bounds, source integration and a Python
reference model for band edges, nearest-frequency ties and power thresholds.
This is source/data inspection, **not C/C++ execution or hardware validation**.
No repository test files are required. Target compilation and RF measurements
remain pending.
