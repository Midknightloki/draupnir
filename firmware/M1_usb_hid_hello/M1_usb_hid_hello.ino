#include "M5Dial.h"
#include "USB.h"
#include "USBHIDKeyboard.h"

USBHIDKeyboard Keyboard;

void drawUI(String statusText) {
  auto& d = M5Dial.Display;
  d.fillScreen(TFT_BLACK);
  d.setTextDatum(middle_center);
  d.setFont(&fonts::FreeSansBold12pt7b);

  d.setTextColor(TFT_CYAN, TFT_BLACK);
  d.drawString("DRAUPNIR M1", 120, 50);

  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.drawString("USB HID", 120, 90);

  d.setTextColor(TFT_GREEN, TFT_BLACK);
  d.drawString(statusText, 120, 150);
}

void setup() {
  auto cfg = M5.config();
  M5Dial.begin(cfg, true, false);
  
  // Start USB HID
  Keyboard.begin();
  USB.begin();

  Serial.begin(115200);
  M5Dial.Display.setBrightness(160);
  drawUI("Press Knob");
  Serial.println("Draupnir M1 USB HID ready");
}

void loop() {
  M5Dial.update();

  if (M5Dial.BtnA.wasPressed()) {
    Serial.println("Knob pressed. Sending keystrokes...");
    drawUI("Sending...");
    
    // Play a short tone
    M5Dial.Speaker.tone(4000, 30);
    
    // Type the fixed string
    Keyboard.println("Hello from Draupnir!");
    
    delay(500); // brief pause to show "Sending..."
    drawUI("Press Knob");
  }

  delay(10);
}
