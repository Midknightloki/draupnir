# Draupnir 

**A two-puck, self-contained HID macro controller: a rotary-knob brain + a 16-key RGB pad.**

Draupnir is a compact radial macro controller featuring 16 physical keys with per-key NeoPixels, arranged alongside a central rotary encoder with a round touch screen. The dial detent matches the key position, mapping macros logically. The entire system functions as a driverless USB HID device.

## Hardware Setup

Draupnir consists of two primary components cabled together:

1. **M5Stack Dial**: The brain. Features an ESP32-S3, a 1.28" round touch screen, and a rotary encoder with a built-in button.
2. **Adafruit NeoTrellis 4x4 (PCB 3954)**: 16 elastomer buttons, each with its own RGB NeoPixel. *Note: You must purchase the 4x4 silicone elastomer button pad separately.*

**Connections:**
- Connect the NeoTrellis to the M5Dial's **PORT.A** (red Grove connector) using a **Grove-to-STEMMA (JST-PH)** adapter cable.
- The Grove port supplies the required 5V for the NeoPixels while keeping the I2C logic at a safe 3.3V.

## Usage

1. **Plug in:** Connect the M5Dial to your computer using a data-capable USB-C cable. It will instantly enumerate as a standard USB Keyboard/Mouse/Consumer device.
2. **Interact:** 
   - Press any of the 16 lit keys on the NeoTrellis to instantly fire its macro.
   - Turn the dial to select a macro, and push the dial to fire it.
   - The screen will display the current profile and active macro's assigned color.
3. **Change Profiles:** Swipe left or right on the M5Dial touchscreen to switch between your configured profiles.

## Configuration (Companion App)

Draupnir doesn't require any host software to run, but it hosts its own configuration web app over Wi-Fi.

1. **Enter Config Mode:** Long-press the M5Dial touchscreen until it shows the "Wi-Fi SETUP" screen. 
2. **Connect to Wi-Fi:** The screen will display instructions to connect to the "Draupnir-Setup" hotspot (or it will display its IP address if it connects to a known network).
3. **Open the Web App:** Open a browser and navigate to the IP address shown on the screen (or `http://192.168.4.1/` if using the hotspot).
4. **Edit Macros:** In the Companion App, you can:
   - Add/Rename/Delete profiles and customize their accent colors.
   - Click a key on the virtual deck to assign actions (Keystrokes, Text strings, Media controls, Mouse movements, Delays, or special Rotary behavior).
   - Set the physical mounting orientation of your dial (0°, 90°, 180°, 270°).
5. **Save & Exit:** Once you save your profiles, Draupnir will automatically restart into run mode with your new configurations.

## Development & Flashing Firmware

To compile and flash the firmware yourself, see the documentation in the `/docs` folder:
- [M0 Setup and BringUp](docs/M0_Setup_and_BringUp.md): Step-by-step for the Arduino IDE.
- [Toolchain (arduino-cli)](docs/Toolchain_arduino-cli.md): Fast, headless deployment.

*Note: Draupnir requires the M5Unified, M5Dial, Adafruit NeoTrellis, Adafruit seesaw, and ArduinoJson libraries.*
