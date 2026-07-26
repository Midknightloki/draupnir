# Toolchain — arduino-cli (headless / agent-driven)

The agent can build and flash directly. This is the fast path; the Arduino IDE GUI
(see `M0_Setup_and_BringUp.md`) is a fallback.

Draupnir has **two board targets**. Waveshare is primary; M5Dial is a supported second target.

---

## Waveshare ESP32-S3 knob (primary)

### The FQBN

```
esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc,FlashMode=qio,FlashSize=16M,PartitionScheme=default_8MB,PSRAM=disabled
```

There is **no knob-specific target** in the ESP32 core — the board is a generic **ESP32S3 Dev
Module** (`esp32:esp32:esp32s3`). The core's Waveshare family covers other products, not this one.

### One-time setup

```
arduino-cli core update-index
arduino-cli core install esp32:esp32     # Espressif's core -- NOT m5stack:esp32
```

The M5Stack core alone is **not** sufficient. It contains only M5Stack boards, so
`board listall` will show nothing suitable and there is no valid FQBN to be found. This is the
single most confusing failure mode on this board; if `board listall` looks empty of S3 dev
modules, the Espressif core is missing.

### Verified hardware facts

Read directly off the chip with `esptool flash-id`, not from a datasheet:

| | |
|---|---|
| Chip | ESP32-S3 (QFN56) rev v0.2 |
| PSRAM | **8 MB embedded** (AP_3v3) — octal |
| Flash | **16 MB**, **quad** (4 data lines) per eFuse, 3.3 V |
| Crystal | 40 MHz |

### Why each option is what it is

| Option | Value | Reason |
|---|---|---|
| `USBMode` | `default` | USB-OTG / **TinyUSB**. Required for USB HID — the entire product. **Not** `hwcdc`. |
| `CDCOnBoot` | `cdc` | Serial diagnostics alongside HID. Without it the board never enumerates as a COM port. |
| `FlashMode` | `qio` | eFuse says quad, not octal. |
| `FlashSize` | `16M` | Confirmed on-chip. |
| `PartitionScheme` | `default_8MB` | See below. |
| `PSRAM` | `disabled` | See below. |

**`PartitionScheme=default_8MB` on a 16 MB board is deliberate.** The firmware stores
`profiles.json` in **LittleFS**, which needs a partition of subtype `spiffs`. Every 16 MB scheme
this board's menu offers is FATFS (`fatflash`, `app3M_fat9M_16MB`) except `esp_sr_16`, which is an
ESP-SR-specific layout. `default_8MB` provides `spiffs, data, spiffs, 0x670000, 0x180000` — 1.5 MB
of LittleFS plus a 3.3 MB app partition, against a sketch of ~970 KB. It fits comfortably inside
16 MB and simply leaves the upper 8 MB unused. This is the same scheme the M5Dial target uses with
LittleFS, so it is proven rather than assumed.

> If the upper 8 MB is ever needed, the fix is `PartitionScheme=custom` with a `partitions.csv` in
> the sketch folder that defines a `spiffs`-subtype region. Do **not** switch to a FATFS scheme —
> `LittleFS.begin()` will fail to find a partition and profiles will not persist.

**`PSRAM=disabled` is a deliberate starting point, not the truthful hardware config.** The board
genuinely has 8 MB of octal PSRAM and `PSRAM=opi` is the eventually-correct value. It is off for
now because (a) the firmware does not use PSRAM at all — LVGL draw buffers are allocated
`MALLOC_CAP_DMA` from internal RAM — and (b) M6 is being flashed to diagnose a *stability*
problem with prior unexplained resets into the ROM bootloader, so adding an untested memory
configuration would confound that diagnosis. A wrong PSRAM mode boot-loops.

Turn it on as its own isolated change once M6 is stable. It is worth doing: it would give the BLE
RX reassembly buffer and the profile `JsonDocument` far more headroom than internal RAM does.

### The USB-C orientation trick — read this before debugging a "wrong chip"

The board carries **two** MCUs — the **ESP32-S3R8** (main, ours) and an **ESP32-U4WDH**
(secondary, 4 MB flash) — behind a **single USB-C port**. A **CH445P 4-SPDT analog switch** routes
the USB-to-UART bridge to one chip or the other, **selected by the Type-C plug orientation.**

Symptoms of the wrong orientation:

| Orientation | Enumerates as | Chip you reach |
|---|---|---|
| Wrong way | VID `0x1A86` PID `0x7523` (WCH CH340 UART bridge) | **ESP32-U4WDH** — an ESP32, 4 MB flash |
| Right way | VID `0x303A` (Espressif native USB) | **ESP32-S3** — ours |

If `esptool` reports `ESP32-U4WDH` / `Detecting chip type... ESP32` / 4 MB flash, **the plug is
upside down.** Unplug, rotate 180°, plug back in. Nothing is wrong with the board.

Always check the VID before assuming which chip you are talking to:

```
arduino-cli board list --format json
```

### Telling run mode from download mode — check `serialNumber`, not the port number

Both modes present VID `0x303A` PID `0x1001`, so PID alone cannot distinguish them. The
`serialNumber` field can:

| `serialNumber` | Mode | Meaning |
|---|---|---|
| `FC012CD1DDD8` (the MAC) | **Running** | Arduino core's TinyUSB CDC sets it from the MAC |
| *empty* | **Download** | ROM USB-Serial/JTAG does not set one |

```
arduino-cli board list --format json
```

The port number also changes between modes (COM10 vs COM11 here), so never reuse a
previously-known port after any mode change.

### After uploading, you must physically replug

**`esptool`'s closing `Hard resetting via RTS pin...` is a no-op on this board** — the same reason
auto-reset does not work. The flash writes and verifies correctly, but the board **stays in
download mode and never runs the new firmware.** Serial capture returns absolutely nothing, which
looks alarmingly like a boot loop.

It is not. **Unplug and replug (correct orientation, without holding BOOT)** and the new firmware
runs. Confirm via the `serialNumber` table above before concluding anything is wrong.

### Download mode (human step)

**Hold BOOT on the ESP32-S3R8, plug in USB-C (correct orientation), release BOOT.**

> **Tip from the field:** mark the S3 side of the USB-C cable (e.g. a dab of green paint) so the
> correct orientation is unambiguous. The two orientations are otherwise indistinguishable by
> feel, and the wrong one silently talks to the other MCU.

Auto-reset does **not** work on this board the way it does on a UART-bridge board. Once the
firmware is running, its native USB is a **TinyUSB CDC** device that does not honour esptool's
DTR/RTS reset, so `esptool` fails with `No serial data received`. That error means "press BOOT",
not "the board is broken".

Note the port **changes between modes** — re-run `board list` after every mode change rather than
reusing a previously-known COM port.

### Compile / upload / monitor

```
FQBN="esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc,FlashMode=qio,FlashSize=16M,PartitionScheme=default_8MB,PSRAM=disabled"

arduino-cli compile --fqbn "$FQBN" firmware/Waveshare_LVGL_Test
arduino-cli upload  -p <PORT> --fqbn "$FQBN" firmware/Waveshare_LVGL_Test
```

Expected build size: ~970 KB (29 %) flash, ~112 KB (34 %) RAM.

**Do not use `arduino-cli monitor` for automated capture** — it treats a non-interactive stdin as
an immediate quit. Use `scripts/serial_capture.ps1` (a .NET `SerialPort` loop) instead.

---

## M5Stack Dial (second target)

```
m5stack:esp32:m5stack_dial:USBMode=default,CDCOnBoot=cdc,FlashSize=8M,PartitionScheme=default_8MB
```

### One-time setup

```
arduino-cli config init
arduino-cli config add board_manager.additional_urls https://static-cdn.m5stack.com/resource/arduino/package_m5stack_index.json
arduino-cli core update-index
arduino-cli core install m5stack:esp32
arduino-cli lib install M5Dial     # installs M5Unified + M5GFX as deps
```

### Download mode (human step)

Hold **G0** on the back StampS3, plug in USB-C, release.

The Dial also enumerates differently per mode: download mode shows as an Espressif "USB
JTAG/serial debug unit"; running mode shows as M5Stack "Dial" (TinyUSB CDC). Re-run `board list`
after any mode change — auto-reset via RTS after upload is not reliable, and a manual replug is
sometimes needed.

### Notes

- The default 4 MB partition scheme is **too small** for this sketch; `default_8MB` is required.
- USB HID needs `USBMode=default` (TinyUSB), not the default `hwcdc`.
- **No PSRAM** on the StampS3 — avoid large full-screen sprites; stream assets from flash.

---

## General

- Use a **data** USB-C cable, not charge-only.
- Inspect any board's options with `arduino-cli board details --fqbn <FQBN>` rather than guessing
  option names — they differ between cores.
- `arduino-cli board list` reports VID/PID under `--format json`; use it to confirm *which* device
  and which mode you have before flashing anything.
