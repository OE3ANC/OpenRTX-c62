<!--
 - SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 -
 - SPDX-License-Identifier: GPL-3.0-or-later
-->

# Retevis C62

This target is based on the ListenAI CSK6011B SoC.

## Building

For the following commands enter shell with the ListenAI environment

```bash
lisa zep exec bash
````

The C62 is a Zephyr target. It is built via `west` but wrapped through `meson` for convenience:

```bash
rm -rf build
meson setup build
meson compile -C build openrtx_c62
```

The build will automatically run `west update --group-filter +c62` to fetch
C62-specific Zephyr modules (AF, FreeRTOS shims, LSF, URPC) before compiling.

Alternatively, using `west` directly:

```bash
west build -b c62 -d build .
```

## Flashing

> **Warning:** This may brick your device! Use at your own risk!

```bash
cskburn -s /dev/ttyUSB0 -C 6 -b 115200 0x000000 build/zephyr/zephyr.hex
```

## Audio streams

The DSP audio service runs at 16 kHz. FM monitoring and software streams use
the same stereo capture (MIC left, RTX right) and playback (speaker left,
RTX right) transport. Each endpoint has independent buffering in application
PSRAM. Application SRAM remains limited to 256 KiB; bank 4 belongs to the DSP.

The stream bridge supports these conversions:

| Path | Software rate | Conversion |
| --- | --- | --- |
| M17 TX baseband | 48 kHz | 48 to 16 kHz, decimation by 3 |
| M17 RX baseband | 24 kHz | 16 to 24 kHz, interpolation by 3 / decimation by 2 |
| Codec2 microphone | 8 kHz | 16 to 8 kHz, decimation by 2 |
| Codec2 speaker | 8 kHz | 8 to 16 kHz, interpolation by 2 |

Streams may also use 16 kHz directly; conversion between 16 kHz and 24/48 kHz
works in either direction. The FIR converters preserve history and rational
phase between buffers, with unity overall gain. They do not replace the modem's
RRC pulse shaping. Baseband conversion preserves DC through 3.6 kHz and rejects
content at and above 8 kHz. The speech filter preserves frequencies through
3 kHz and rejects content at and above 4 kHz. Coefficient generation parameters
are documented in `audio_resampler.c`.

Output synchronization consumes one software buffer half and paces the caller
at its requested rate. Graceful stop flushes the FIR tail, pads the final DSP
frame with silence and waits for queued playback. Termination cancels pending
work. Capture overflow and stalled I/O fail the stream with a diagnostic;
resampler saturation is counted and reported when the stream closes. There is
no automatic level normalization or deviation compensation.

Hardware validation must check continuous audio, RX decoding and transmitted
signal quality before claiming M17 support.

## BK4819 FM and M17 modes

Selecting M17 applies a flat baseband profile derived from the register behavior
reported by [Rob Riggs, WX9O, Mobilinkd](https://github.com/egzumer/uv-k5-firmware-custom/pull/583).
RX still uses FM demodulation. Speech filters, emphasis, DC filters, microphone
AGC, the audio limiter, companding, VOX and tone generators are disabled. M17
uses a 12.5 kHz channel with the digital narrow RF filter configuration,
regardless of the saved FM bandwidth. RF AGC is enabled on RX and fixed on TX.
Software M17 decoding controls squelch; analog tone settings are ignored.

The driver saves the affected FM register fields before entering M17 and
restores them when leaving it. The current channel's FM bandwidth and squelch
settings are then applied. Muting and unmuting AF output preserves filter
bypass and polarity. RX/TX transitions reapply the digital profile without
replacing the saved FM settings.

The following Kconfig settings support hardware calibration:

- `CONFIG_C62_M17_DEVIATION`: raw BK4819 REG_40 bits 11:0, default `0x04d2`.
  Bit 12 is the modulation enable and is always set separately by the driver.
  This retains the existing C62 starting value, with fixed microphone gain;
  it is **not a calibrated RF deviation in Hz**. Mobilinkd's UV-K6/TNC4 values
  must not be copied without calibrating the C62 DSP-to-radio path.
- `CONFIG_C62_TX_BASEBAND_LEVEL_PERCENT`: scales MCU-to-radio PCM before
  resampling, without affecting FM bypass or speaker audio. The Kconfig
  default is 100%; `zephyr.conf` selects **50% (-6.02 dB)**. Hardware feedback
  confirmed improved outer-symbol separation at this level. Absolute RF
  deviation still requires calibration.
- `CONFIG_C62_TX_BASEBAND_INVERT`: reverses TX baseband polarity, equivalent
  to Module17's TX phase inversion. Enabled in `zephyr.conf` with 50% drive;
  this combination produced decodable M17 TX. Set it to `n` for normal polarity.
  This affects MCU-to-radio streams only, not RX, FM bypass or speaker audio.
- `CONFIG_C62_RX_BASEBAND_INVERT`: reverses received baseband after resampling,
  equivalent to Module17's RX phase inversion. Disabled in `zephyr.conf` for
  the current normal-polarity RX comparison. Microphone audio, FM bypass and
  the working TX path are unchanged.
- `CONFIG_C62_M17_RX_GAIN1`: BK4819 RX audio attenuation in 6 dB steps
  (0 through 3). The current RX diagnostic selects 1 (-6 dB) before the DSP
  ADC. FM restores its original gain, and TX modulation gain is unchanged.
- `CONFIG_C62_RX_LEVEL_DIAGNOSTICS`: enabled for the current RX diagnostic.
  Logs `RX PCM min=... max=... mean=... near_rail=.../16000` once per second
  during software RX capture, before resampling and polarity inversion.
  Compare idle and received-signal levels. Near-rail counts indicate possible
  PCM saturation; zero counts do not rule out analog distortion upstream.
- `CONFIG_C62_MIC_BIAS_SETTLE_MS`: startup settling delay, default 250 ms.
  Only the microphone ADC is enabled during this delay, with RX DSP and the
  external PAs off. Mobilinkd measured 250 ms with 1 uF input coupling; verify
  the appropriate delay for C62. There is no additional settling delay at PTT.

Set overrides in `platform/mcu/CSK6011B/zephyr.conf`. When changing board
Kconfig definitions, refresh the generated board copy using:

```bash
# Inside lisa zep exec bash
west build -b c62 -d build . --cmake
```

Before claiming on-air M17 operation, measure TX deviation and startup drift,
check RX/TX polarity and the analog coupling response, verify RX decoding,
and exercise repeated PTT and FM/M17 switches. The firmware preserves the
existing C62 RX polarity; UV-K6 hardware modifications and polarity settings
are not assumed to apply to C62. Absolute deviation and RX decoding remain
unvalidated.

Hardware feedback confirmed M17 TX decoding with 50% baseband drive and TX
inversion enabled. RX inversion is now disabled for comparison after the
capture timing fix.

### Capture startup timing

On the ListenAI newlib toolchain, `Input endpoint 1: -139` means `EOVERFLOW`
(RX capture queue overrun), not a bad RF level. The queue holds 160 ms at
16 kHz. BK4819 serial transfers use `delayUs()`; this must use `k_busy_wait()`
because `k_usleep(1)` rounds up to scheduler ticks (100 us with the C62
configuration). Sleeping on every serial clock edge can fill the capture
queue during receiver configuration before the modem consumes samples.
Millisecond delays still sleep normally. Overrun diagnostics report queued,
incoming and capacity sample counts; an overrun still fails the stream rather
than silently dropping samples and presenting a discontinuous modem waveform.
