<!--
 - SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 -
 - SPDX-License-Identifier: GPL-3.0-or-later
-->

# Retevis C62

This proof of concept (POC) runs OpenRTX on the ListenAI CSK6011B SoC with a
BK4819 radio IC. FM and M17 transmission have been demonstrated on hardware.
M17 reception works with the C62 receive compensation described below.

## Development status and future work

This POC was developed with heavy LLM assistance and hardware testing.
The code and DSP choices would benefit from further human review, with room
to simplify the implementation and optimize CPU usage, buffering and receive
compensation.

A future version should use the 48 kHz DSP firmware instead of the current
16 kHz firmware. This would remove downsampling from the 48 kHz M17 TX path
and provide higher-rate RX input. Conversion to the modem's 24 kHz RX rate
and Codec2's 8 kHz speech rate would still be needed. Audio transport and RX
compensation will need to be adapted and validated again.

## Build and flash

Run commands from the repository root in the ListenAI Lisa environment:

```bash
lisa zep exec bash
west build -b c62 -d build .
```

Use `west build -p always -b c62 -d build .` for a clean build, including when
moving from an older diagnostic build with cached configuration or overlays.
The firmware image is `build/zephyr/zephyr.hex`.

Alternatively, the Meson wrapper fetches the C62 modules (AF, FreeRTOS shims,
LSF and URPC) with `west update --group-filter +c62` before building:

```bash
meson setup build
meson compile -C build openrtx_c62
```

Use a fresh build directory when changing between direct West and Meson builds.

Flash with the appropriate serial port:

```bash
cskburn -s /dev/ttyUSB0 -C 6 -b 115200 0x000000 build/zephyr/zephyr.hex
```

## Audio transport

The DSP transport uses 16-bit PCM at 16 kHz. Input channels carry microphone
(left) and radio (right) audio; output channels feed the speaker (left) and
radio modulation input (right). The C62 audio bridge converts rates as needed:

| Path | Rate conversion |
| --- | --- |
| M17 RX baseband | 16 → 24 kHz |
| M17 TX baseband | 48 → 16 kHz |
| Codec2 microphone input | 16 → 8 kHz |
| Codec2 speaker output | 8 → 16 kHz |
| FM microphone and speaker paths | 16 kHz, no rate conversion |

## M17 reception

```text
RF → BK4819 → DSP, 16 kHz → resampler, 24 kHz
   → LF compensation → 81-tap equalizer → M17 decoder
   → Codec2, 8 kHz → resampler, 16 kHz → speaker
```

RX uses normal polarity, AF9, REG_47 bit 1 set, and RX gain2 `0x3f`.
RX gain1 provides -6 dB attenuation. Speech filters, emphasis and DC filters
are bypassed; RF AGC stays enabled with the M17 12.5 kHz channel setting.

[`rx_baseband.h`](rx_baseband.h) and [`rx_equalizer.h`](rx_equalizer.h) apply
separate, fixed compensation stages before the shared M17 decoder. They improve
RX decoding, but the source of the underlying response distortion remains
unidentified. These filters apply only to M17 reception.

## M17 transmission

```text
Microphone → DSP, 16 kHz → resampler, 8 kHz → Codec2
   → M17 modulator, 48 kHz → 50% level and polarity inversion
   → resampler, 16 kHz → DAC → BK4819 → RF
```

The current 50% drive and inverted polarity produce decodable M17 TX.
The BK4819 bypasses speech processing and uses fixed gain. Its deviation field
is `0x04d2`; absolute RF deviation still requires calibration. RX compensation
is not applied to TX. PTT controls transmission and returns to RX on release.

## FM and mode changes

FM routes microphone and radio audio through the 16 kHz transport without the
M17 codec or compensation. The driver saves FM register settings on entering
M17 and restores them on return. Hardware squelch stays open in all modes;
OpenRTX handles squelch and audio gating in software.

## Calibration

Options are declared in [`Kconfig.board`](Kconfig.board), with overrides in
[`zephyr.conf`](../../mcu/CSK6011B/zephyr.conf):

| Setting | Current value |
| --- | --- |
| `C62_M17_DEVIATION` | `0x04d2` (register field, not Hz) |
| `C62_TX_BASEBAND_LEVEL_PERCENT` | `50` |
| `C62_TX_BASEBAND_INVERT` | `y` |
| `C62_M17_RX_GAIN1` | `1` (-6 dB) |
| `C62_MIC_BIAS_SETTLE_MS` | `250` ms at startup |

After configuration changes, run `west build -b c62 -d build . --cmake`
inside Lisa.
