# Toolchain — arduino-cli (Antigravity / headless)

In Antigravity the agent can build and flash directly. This is the fast path; the
Arduino IDE GUI (see `M0_Setup_and_BringUp.md`) is a fallback.

## One-time setup
```
arduino-cli config init
arduino-cli config add board_manager.additional_urls https://static-cdn.m5stack.com/resource/arduino/package_m5stack_index.json
arduino-cli core update-index
arduino-cli core install m5stack:esp32
arduino-cli lib install M5Dial     # installs M5Unified + M5GFX as deps
```

## Find the board FQBN and port (don't hardcode-guess)
```
arduino-cli board listall | grep -i dial     # -> the M5Dial FQBN
arduino-cli board list                        # -> the serial PORT (after download mode)
```

## Download mode (human step)
Hold **G0** on the back StampS3, plug in USB-C, release. The port then appears.

## Compile / upload / monitor
```
arduino-cli compile --fqbn <FQBN> firmware/M0_bringup
arduino-cli upload  -p <PORT> --fqbn <FQBN> firmware/M0_bringup
arduino-cli monitor -p <PORT> -c baudrate=115200
```

## M1 (USB HID) board options
USB mode is a menu option baked into the FQBN. Inspect available options:
```
arduino-cli board details --fqbn <FQBN>
```
Then append the USB-OTG / TinyUSB option (name shown in `board details`) to the FQBN when
compiling M1, e.g. `--fqbn <FQBN>:USBMode=<tinyusb-option>`, and enable CDC-on-boot so the
serial monitor keeps working alongside HID.

## Notes
- Native-USB board: after flashing, re-enter download mode (G0) before each re-upload.
- Use a **data** USB-C cable, not charge-only.
- No PSRAM on the StampS3 — avoid large full-screen sprites; stream assets from flash.
