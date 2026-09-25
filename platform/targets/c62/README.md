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

## Provisional TX power calibration

`select_tx_power()` in `platform/drivers/baseband/radio_C62.cpp` contains the
entire power-selection table. No separate power module, flash reader, startup
settings dump, or frequency-reference lookup remains.

| Band | Up to 1 W | Up to 2.5 W | Up to 5 W |
| --- | --- | --- | --- |
| VHF 136–174 MHz | 30% | 57% | 89% |
| UHF 400–480 MHz | 46% | 76% | 100% |

These are starting estimates from measurements at 145.550 and 433.475 MHz,
with **100 kHz PWM**, not verified wattages across the bands. The source
comment records the measured points and estimation method. VHF low/medium
reuse measured 1.1/2.5 W settings; VHF high is extrapolated. UHF low is
interpolated, medium slightly extrapolated, and high capped at full duty:
linear extrapolation would require 126%, so 5 W is not assured.

Only requests in `(0,1000]`, `(1000,2500]`, `(2500,5000]` mW are accepted.
Each range selects a fixed duty, not continuous power regulation. Frequencies
outside the inclusive hardware bands, including the band gap, are rejected.
TX-disable and PWM error handling remain; PWM must succeed before PA enable.
TX logging reports requested power, frequency, duty and PWM frequency.

Tune the six table values with a dummy load and suitable meter, keeping supply
and modulation conditions consistent. At 100% duty the control stays active
rather than toggling. No compilation or hardware validation of these new
estimates has been performed by the assistant.
