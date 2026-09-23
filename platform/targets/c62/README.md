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
