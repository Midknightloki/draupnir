# Waveshare ESP32-S3-Knob-Touch-LCD-1.8 — hardware reference

**Transcribed from the manufacturer's schematic**, archived in
`docs/hardware/waveshare-schematic/` (five sheets, PNG). Source:
<https://www.waveshare.com/wiki/ESP32-S3-Knob-Touch-LCD-1.8> →
`ESP32-S3-Knob-Touch-LCD-1.8-schematic.zip`, dated 2025-05-27. Copyright Waveshare; redistributed
here unmodified so contributors do not have to re-download it to answer a pinout question.

**The schematic is the authority for wiring.** For anything the firmware actually *does* with a
pin, the firmware is the authority — see `firmware/Waveshare_LVGL_Test/lcd_config.h` and
`lcd_bsp.c`.

This document exists because several things previously recorded in this repo as unknown or
assumed turned out to be answered on these sheets, and two were recorded **wrong**. Those are
called out in §7.

---

## 1. ESP32-S3 (U2, ESP32-S3R8) — GPIO map

Sheet `2_ESP32S3-R8.png`. This is the MCU Draupnir runs on.

| GPIO | Net | Function |
|---|---|---|
| 0 | `I2S_SWITCH_IN` | Selects which MCU drives the audio DAC (also strapping / BOOT) |
| 1 | `BATT_ADC` | Battery sense, 10K/10K divider (sheet 4) |
| 2 | `SDMMC_D3` | microSD data 3 |
| 3 | `SDMMC_CMD` | microSD command |
| 4 | `SDMMC_SCK` | microSD clock |
| 5 | `SDMMC_D0` | microSD data 0 |
| 6 | `SDMMC_D1` | microSD data 1 |
| **7** | **`EC1_B`** | **Rotary encoder B** |
| **8** | **`EC1_A`** | **Rotary encoder A** |
| 9 | `TP_INT` | CST816 touch interrupt |
| 10 | `TP_RST` | CST816 reset |
| 11 | `TP_SDA` | I2C data — **touch *and* haptics share this bus** |
| 12 | `TP_SCL` | I2C clock — shared, as above |
| 13 | `LCD_QSPI_SCL` | Display QSPI clock |
| 14 | `LCD_QSPI_CS` | Display chip select |
| 15 | `LCD_QSPI_D0` | Display data 0 |
| 16 | `LCD_QSPI_D1` | Display data 1 |
| 17 | `LCD_QSPI_D2` | Display data 2 |
| 18 | `LCD_QSPI_D3` | Display data 3 |
| 19 | `USB_DN` | Native USB D− |
| 20 | `USB_DP` | Native USB D+ |
| 21 | `LCD_RST` | Display reset |
| **38** | **`ESP32S3_TX`** | **UART to the second MCU** — see §4 |
| 39 | `S3_I2S_DAC_BCK` | Audio bit clock |
| 40 | `S3_I2S_DAC_LRCK/WS` | Audio word select |
| 41 | `S3_I2S_DAC_DIN` | Audio data |
| 42 | `SDMMC_D2` | microSD data 2 |
| 45 | `PDM_MIC_SCK` | PDM microphone clock |
| 46 | `PDM_MIC_DATA` | PDM microphone data |
| 47 | `LCD_BLK` | Backlight, via Q1 (AO3400A MOSFET), LEDC PWM |
| **48** | **`ESP32S3_RX`** | **UART to the second MCU** — see §4 |

Flash is **U3, W25Q128JVPIQ — 16 MB**, on the dedicated SPI pins.

## 2. Rotary encoder — there is no push switch

Sheet `1_LCD&POWER.png`. **SW2 = SSCM110100**, a four-pin incremental encoder: two pins to
ground, two signal pins carrying `EC1_A` (GPIO 8) and `EC1_B` (GPIO 7), with 10K pull-ups
(R59/R60).

**No shaft push switch is wired, because the part does not have one.** `Draupnir_Spec.md` §3
previously flagged this as "confirm whether the hardware has a push action before relying on it".
It is now confirmed: **there is none.** Any design wanting a "press the knob" gesture must use
the touchscreen.

The board carries a *second* encoder, **SW1**, wired to the other MCU as `EC2_A`/`EC2_B`.
Draupnir does not use it.

## 3. microSD — 4-bit SDMMC, not SPI

Sheet `4_OTHER.png`. Connector **CARD1, TF-018**, wired for **4-bit SDMMC** (GPIO 2/3/4/5/6/42
per §1) with 10K pull-ups on D0–D3 and CMD (R10, R46–R49).

This matters: the obvious `SD.begin(cs)` SPI approach is the wrong driver for this wiring. Use
`SD_MMC` with the pins above. The firmware does not currently touch the card at all.

## 4. The second MCU — there IS a UART between the two

Sheet `3_ESP32-CHIP.png`. **U14 = ESP32-U4WDH**, 4 MB flash, with its own antenna, crystal, and
its own USB-UART bridge (**U10**, on `USB_ESP32_DP`/`DN`, with EN/IO0 auto-reset).

**The two MCUs are directly connected by a dedicated UART:**

| Net | ESP32-S3 | ESP32-U4WDH |
|---|---|---|
| `ESP32S3_RX` | GPIO 48 | IO23 |
| `ESP32S3_TX` | GPIO 38 | IO18 |

Both sheets agree, from both ends.

This corrects a conclusion recorded earlier in this project — that the second chip was reachable
"only *instead of* the S3, never alongside it" and was therefore useless as an offload target
without someone tracing the board. **That is wrong.** The chips can talk to each other at any
time, independently of USB. The U4WDH also owns the second encoder (`EC2_A`/`EC2_B` on IO19/IO22)
and a second I2S path to the audio DAC (IO25/26/27), so it is a genuinely usable co-processor
with its own I/O.

Nothing in Draupnir uses it, and nothing needs to. But it is a real resource, not a dead end, and
that should be recorded accurately.

## 5. Haptics — DRV2605L driving an **LRA**, on the touch I2C bus

Sheet `5_DAC.png`. **U13 = DRV2605LDGSR.**

| DRV2605L pin | Net |
|---|---|
| SCL | `HAPTIC_SCL` → `TP_SCL` (GPIO 12) |
| SDA | `HAPTIC_SDA` → `TP_SDA` (GPIO 11) |
| IN/TRIG | `HAPTIC_TRIG` → tied to **GND** |
| EN | `HAPTIC_EN` → tied to **3V3** (always enabled) |
| OUT+ | `LRA_P` → pad **PP2** |
| OUT− | `LRA_N` → pad **PP1** |

Three things follow, each of which closes a question this project had open:

1. **The DRV2605 is real.** `haptics.cpp` recorded its I2C address as "an assumption from a
   datasheet rather than a verified schematic". It is now verified.
2. **The motor is an LRA, not an ERM.** The nets are literally named `LRA_P`/`LRA_N`. Any driver
   configuration must select LRA mode.
3. **It shares the I2C bus with the CST816 touch controller.** This is the documented cause of
   the touch/haptics conflict, and why the blind `i2c_scan()` once sitting in `haptics_init()`
   broke touch and was deleted.

**On the open-circuit fault.** The motor connects through pads **PP1/PP2** rather than a
connector or a populated part. On the owner's unit the DRV2605 reported `DIAG_RESULT=1` and
`OC_DETECT=1` and never buzzed across three attempts, with configuration proven correct by
register readback. An open circuit at a pair of pads is at least as consistent with **no motor
fitted to this board revision** as with a failed one. Anyone reopening haptics should look at the
board before debugging software.

## 6. Power, display, audio

- **3V3 rail:** U19, TLV62569DBVT buck, 5 V → 3.3 V (`Vout = 0.6 × (1 + Rh/Rl)`).
- **Battery:** MX1.25 socket, sensed on GPIO 1 through a 10K/10K divider — so the ADC reads half
  the rail. Wiki states 3.7–4.2 V.
- **Display:** FPC connector U5; QSPI data/clock/CS per §1; backlight switched by Q1 (AO3400A)
  from `LCD_BLK` (GPIO 47). `LCD_TE` is present on the connector.
- **Audio out:** U12, PCM5100APWR I2S DAC → `OUTL`/`OUTR`, which leave on the **USB-C connector**
  (CN1), not a separate jack footprint.
- **Microphone:** MIC1, MSM261D4030H1CPM, PDM on GPIO 45/46.
- **Audio source switch:** U18, **CH445P**, selects whether the S3 or the U4WDH drives the DAC,
  controlled by `I2S_SWITCH_IN` (GPIO 0).

## 7. Corrections to earlier documentation

Three claims recorded elsewhere in this repo are wrong, and the schematic is why.

**1. The CH445P does not switch USB.** `CLAUDE.md`, `docs/Toolchain_arduino-cli.md` and
`Draupnir_Spec.md` §3 all say the USB-C plug orientation selects which MCU you reach "via a CH445P
analog switch". The CH445P (U18) is an **I2S audio** switch, on sheet 5.

The real mechanism is simpler and needs no switch at all. USB-C carries **two** D+/D− pairs
(A6/A7 and B6/B7), and connector CN1 wires one pair to the S3 (`USB_DP`/`USB_DN`) and the other
to the U4WDH (`USB_ESP32_DP`/`USB_ESP32_DN`). Flipping the plug physically changes which pair the
cable contacts. The *observable behaviour* the docs describe is exactly right — flip the plug,
reach the other chip — only the stated cause was wrong.

**2. The second MCU is not isolated.** See §4. There is a dedicated UART.

**3. The encoder has no push switch.** See §2. Previously recorded as unknown.

Two further details worth having, previously unknown rather than wrong: the microSD is **4-bit
SDMMC** (§3), and the haptic motor is an **LRA** on **pads** (§5).
