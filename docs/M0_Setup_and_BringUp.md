# Draupnir M0 — Toolchain Setup + Dial Bring-up

Goal: install everything, flash a known-good M5 example (proves the toolchain), then flash our bring-up sketch that shows the **encoder, knob button, and touch** live on the round screen. When this works, M0 is done.

Everything here is on your computer; I can't flash for you, so we'll go step by step and you tell me what the screen / Serial Monitor shows.

---

## Part 1 — Install the Arduino IDE
1. Go to https://www.arduino.cc/en/software and download **Arduino IDE 2.x** for your OS.
2. Install and open it.

## Part 2 — Add M5Stack board support
1. In the IDE: **File → Preferences** (Windows) / **Arduino IDE → Settings** (Mac).
2. In **Additional boards manager URLs**, paste:
   ```
   https://static-cdn.m5stack.com/resource/arduino/package_m5stack_index.json
   ```
   (If there's already a URL there, click the little icon and add this on its own line.)
3. **OK**.
4. Open **Tools → Board → Boards Manager** (or the boards icon in the left rail).
5. Search **M5Stack** → install the **"M5Stack"** package by M5Stack. This takes a few minutes (it pulls the ESP32 core too).

## Part 3 — Install the M5Dial library
1. Open **Sketch → Include Library → Manage Libraries** (or the books icon).
2. Search **M5Dial** → **Install**.
3. When it asks to **install dependencies** (M5Unified, M5GFX, etc.), click **Install All**.

## Part 4 — Select the board
1. **Tools → Board → M5Stack → M5Dial.**
2. Leave the other Tools settings at their defaults for now — we do **not** change USB mode yet (that's M1). Default USB/CDC settings give the smoothest first flash.

## Part 5 — Put the Dial in download mode & pick the port
The M5Dial flashes most reliably in download mode:
1. On the **back**, find the **StampS3** module and its tiny **G0** button.
2. **Hold G0**, plug the USB-C cable into the computer, then **release G0**. It's now in download mode.
3. **Tools → Port** → select the new port that appeared (COMx on Windows, /dev/cu.* on Mac).
   - No port? See Troubleshooting at the bottom.

## Part 6 — Sanity flash (M5's own example)
Do this first so we know the toolchain works before running my code:
1. **File → Examples → M5Dial → Basic → display** (or the example simply named **display**).
2. Click **Upload** (the → arrow).
3. Success = the screen shows M5's demo graphics. 🎉

If that worked, your toolchain is 100% good. If not, jump to Troubleshooting.

## Part 7 — Flash the Draupnir bring-up sketch
1. **File → New Sketch.**
2. Delete the empty template, and paste the entire contents of **`Draupnir_M0_bringup.ino`** (the other file I gave you).
3. **File → Save As** → name it `Draupnir_M0_bringup` (Arduino will make a folder for it).
4. Put the Dial in download mode again (Part 5, steps 1–2) if it isn't already, confirm the Port, and click **Upload**.

## Part 8 — What success looks like
On the round screen you should see a little dashboard:
- **DRAUPNIR M0** title
- **Encoder:** a number that changes as you turn the knob
- **Button:** flips **up ↔ DOWN** as you press the knob (and a short beep)
- **Touch:** shows x,y coordinates when you touch the screen

Also open **Tools → Serial Monitor**, set baud to **115200** — you'll see the same events printed as you interact.

Turn the knob, press it, tap the screen. If all four react, **M0 is done** and we move to M1 (USB HID). Tell me what you see — including anything weird — and we'll go from there.

---

## Troubleshooting
- **No port appears:** re-do the G0 download-mode trick (hold G0, plug in, release). On Windows, if still nothing, install the M5 USB driver from https://docs.m5stack.com/en/download . Try a different USB-C cable — many are charge-only; you need a **data** cable.
- **Upload fails / "port busy" / timeout:** enter download mode again right before uploading; close the Serial Monitor while uploading; try a different USB port (rear-panel ports on desktops are more reliable than hubs).
- **Screen stays black after a successful upload:** the sketch sets brightness; if black, tell me — we may need to bump `setBrightness` or check the example flashed.
- **Compile error mentioning M5GFX/M5Unified:** the dependency install didn't complete — re-open Library Manager, search each of M5Dial / M5Unified / M5GFX and make sure all are installed.
- **After flashing, the Port disappears / can't re-upload:** normal for native-USB boards — just re-enter download mode (G0) before each upload for now.

Send me the exact error text or a photo of the screen and I'll debug it with you.
