/*
 * Draupnir M0 — M5Dial bring-up
 * Confirms display + encoder + knob button + touch, and prints to Serial.
 * Board: M5Stack -> M5Dial   |   Library: M5Dial (+ M5Unified, M5GFX)
 * Flash in download mode (hold G0, plug in, release). Serial Monitor @ 115200.
 */
#include "M5Dial.h"

long lastEnc   = 0x7fffffff;
bool lastBtn   = false;
bool lastTouch = false;
int  lastTx = -1, lastTy = -1;

void drawUI(long enc, bool btn, bool touched, int tx, int ty) {
  auto& d = M5Dial.Display;
  d.fillScreen(TFT_BLACK);
  d.setTextDatum(middle_center);
  d.setFont(&fonts::FreeSansBold12pt7b);

  d.setTextColor(TFT_CYAN, TFT_BLACK);
  d.drawString("DRAUPNIR M0", 120, 45);

  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.drawString("Enc: " + String(enc), 120, 95);
  d.drawString(String("Btn: ") + (btn ? "DOWN" : "up"), 120, 130);

  if (touched) {
    d.setTextColor(TFT_GREEN, TFT_BLACK);
    d.drawString("Touch " + String(tx) + "," + String(ty), 120, 165);
  } else {
    d.setTextColor(TFT_DARKGREY, TFT_BLACK);
    d.drawString("Touch --", 120, 165);
  }
}

void setup() {
  auto cfg = M5.config();
  M5Dial.begin(cfg, /*enableEncoder=*/true, /*enableRFID=*/false);
  Serial.begin(115200);
  M5Dial.Display.setBrightness(160);
  drawUI(0, false, false, 0, 0);
  Serial.println("Draupnir M0 bring-up ready");
}

void loop() {
  M5Dial.update();

  long enc      = M5Dial.Encoder.read();
  bool btn      = M5Dial.BtnA.isPressed();
  auto t        = M5Dial.Touch.getDetail();
  bool touched  = t.isPressed();
  int  tx = t.x, ty = t.y;

  bool changed = (enc != lastEnc) || (btn != lastBtn) || (touched != lastTouch) ||
                 (touched && (tx != lastTx || ty != lastTy));

  if (changed) {
    drawUI(enc, btn, touched, tx, ty);
    if (enc != lastEnc)   Serial.printf("Encoder: %ld\n", enc);
    if (btn != lastBtn)   Serial.printf("Button : %s\n", btn ? "DOWN" : "up");
    if (touched)          Serial.printf("Touch  : %d,%d\n", tx, ty);
    lastEnc = enc; lastBtn = btn; lastTouch = touched; lastTx = tx; lastTy = ty;
  }

  // short beep on each fresh knob press
  if (M5Dial.BtnA.wasPressed()) {
    M5Dial.Speaker.tone(4000, 30);
  }

  delay(5);
}
