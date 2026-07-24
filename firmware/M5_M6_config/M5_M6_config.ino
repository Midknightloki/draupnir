#include "M5Dial.h"
#include <LittleFS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include "USB.h"
#include "USBHIDKeyboard.h"
#include "USBHIDConsumerControl.h"
#include "USBHIDMouse.h"
#include "index_html.h"
#include <Adafruit_NeoTrellis.h>
#include "icons.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include "freertos/queue.h"
#include "freertos/semphr.h"

#define SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"
// Flow-control ack the app writes to RX after processing each TX chunk: 2 bytes,
// [BLE_CHUNK_ACK_MARKER, seq]. seq echoes the chunk's sequence byte (prefixed onto every TX
// chunk) so a late ack from a chunk we already gave up retrying can't be mistaken for the
// current one. 0xFE can't collide with a real JSON command chunk (those are ASCII/UTF-8, <0x80).
#define BLE_CHUNK_ACK_MARKER 0xFE

enum AppMode { RUN_MODE, CONFIG_MODE, WIFI_SETUP_MODE };
AppMode currentMode = RUN_MODE;
String pairingToken = "";

BLEServer *pServer = nullptr;
BLECharacteristic *pTxCharacteristic = nullptr;
bool deviceConnected = false;
bool oldDeviceConnected = false;
// RX reassembly buffer for chunked BLE commands — a save_profiles command can be ~12KB+.
// Growing a String one byte at a time via += to that size repeatedly hit failed reallocations
// once the heap fragmented (largestBlock measured as low as ~9KB mid-session), silently
// truncating the reassembled command down to whatever tail happened to fit.
// Deliberately NOT a compile-time-sized static/global array: reserving ~24KB of .bss
// unconditionally at link time (before BLE has initialized anything) was confirmed by direct
// testing to starve BLE bring-up itself — advertising silently never started. Instead, malloc'd
// ONCE in setup() right after BLEDevice::init(), while the heap is still fresh/unfragmented, so
// it's a single clean allocation rather than either a permanent static reservation or thousands
// of incremental String reallocs.
#define BLE_RX_BUFFER_SIZE 16384
static char *bleRxBuf = nullptr;
static size_t bleRxLen = 0;
// Cross-core queue: BLE task (Core 0) -> main loop (Core 1)
// volatile flag is NOT sufficient on ESP32-S3 — L1 caches are not coherent across cores
// xQueueSend/xQueueReceive include the necessary memory barriers
QueueHandle_t bleRxQueue = nullptr;
// App-level flow control for chunked BLE sends: notify() has no delivery guarantee (confirmed by
// testing — the stack reports SUCCESS_NOTIFY even when the central never surfaces the packet at
// all), so instead of trusting it we wait for the app to ack each chunk before sending the next.
// (BLECharacteristic::indicate() looks like the "proper" fix but this library's indicate() has a
// real bug: if a single confirm ever times out, its internal gate semaphore is left permanently
// taken, wedging all future sends until reboot — verified by reading BLECharacteristic.cpp.)
// A single-slot queue (not a semaphore) carries the acked sequence byte itself, given from the
// BLE stack task when the ack write arrives, consumed on the main loop.
QueueHandle_t bleAckQueue = nullptr;


Adafruit_NeoTrellis trellis;
bool trellisFound = false;

USBHIDKeyboard Keyboard;
USBHIDConsumerControl ConsumerControl;
USBHIDMouse Mouse;
Preferences prefs;
WebServer server(80);

JsonDocument profilesDoc;
int activeProfileIdx = 0;
int selectedMacroIdx = 0;
long oldPosition = -999;
bool mdnsStarted = false;
bool uiNeedsRedraw = false;
unsigned long lastRedrawTime = 0;

const char* defaultProfilesJson = R"=====(
{
  "version": 2,
  "activeProfile": 0,
  "settings": { "brightness": 160, "ledBrightness": 60, "buzzer": true },
  "profiles": [
    {
      "name": "Editing",
      "macros": [
        { "pos": 0, "name": "Build", "icon": "hammer", "color": "#E0A030", "actions": [ { "type": "key", "mods": ["CTRL","SHIFT"], "key": "b" } ] },
        { "pos": 1, "name": "Sign-off", "icon": "text", "color": "#3080E0", "actions": [ { "type": "text", "value": "- Eitri, FORGE Master\n" } ] },
        { "pos": 2, "name": "Mute", "icon": "mic", "color": "#E03030", "actions": [ { "type": "consumer", "code": "MUTE" } ] }
      ]
    },
    {
      "name": "Gaming",
      "macros": [
        { "pos": 0, "name": "GG", "icon": "text", "color": "#00FF00", "actions": [ { "type": "text", "value": "gg wp\n" } ] }
      ]
    }
  ]
}
)=====";

struct ActiveMacro {
  bool active = false;
  bool isToggle = false;
  int keyIndex = -1;
  JsonObject macroDef;
  int currentActionIndex = 0;
  unsigned long nextActionTime = 0;
  uint16_t color = 0;
};
ActiveMacro runningMacros[16];

bool inRotaryMode = false;
JsonObject activeRotaryMacro;
long rotaryStartPos = 0;

uint16_t hexToRGB565(const char* hex) {
  if (hex == nullptr || strlen(hex) < 6) return 0;
  int offset = (hex[0] == '#') ? 1 : 0;
  long rgb = strtol(hex + offset, nullptr, 16);
  uint8_t r = (rgb >> 16) & 0xFF;
  uint8_t g = (rgb >> 8) & 0xFF;
  uint8_t b = rgb & 0xFF;
  return M5Dial.Display.color565(r, g, b);
}

void drawRotaryUI() {
  auto& d = M5Dial.Display;
  d.fillScreen(TFT_BLACK);
  d.setTextDatum(middle_center);
  
  const char* name = activeRotaryMacro["name"] | "Rotary";
  const char* colorHex = activeRotaryMacro["color"] | "#FFFFFF";
  uint16_t color = hexToRGB565(colorHex);
  
  d.drawCircle(120, 120, 110, color);
  d.drawCircle(120, 120, 109, color);
  
  d.setFont(&fonts::Orbitron_Light_24);
  d.setTextColor(color, TFT_BLACK);
  d.drawString(name, 120, 120);
  
  d.setFont(&fonts::Orbitron_Light_24);
  d.setTextColor(TFT_DARKGRAY, TFT_BLACK);
  d.drawString("TURN DIAL", 120, 70);
  d.drawString("TAP TO EXIT", 120, 180);
  
  d.fillTriangle(40, 120, 50, 110, 50, 130, color);
  d.fillTriangle(200, 120, 190, 110, 190, 130, color);
}

void drawRunUI() {
  if (inRotaryMode) {
    drawRotaryUI();
    return;
  }
  
  auto& d = M5Dial.Display;
  d.fillScreen(TFT_BLACK);
  d.setTextDatum(middle_center);
  
  JsonArray profiles = profilesDoc["profiles"];
  if (profiles.isNull() || profiles.size() == 0) {
    d.setTextColor(TFT_RED, TFT_BLACK);
    d.drawString("No Profiles", 120, 120);
    return;
  }
  
  JsonObject prof = profiles[activeProfileIdx];
  const char* profName = prof["name"] | "Profile";
  
  const char* profColorHex = prof["color"] | "#FF00FF";
  uint16_t profColor = hexToRGB565(profColorHex);
  if (activeProfileIdx > 0) {
    d.fillTriangle(62, 104, 76, 112, 76, 96, profColor);
  }
  if (activeProfileIdx < profiles.size() - 1) {
    d.fillTriangle(178, 104, 164, 112, 164, 96, profColor);
  }
  
  JsonArray macros = prof["macros"];
  
  int centerX = 120;
  int centerY = 120;
  int radius = 100;
  
  const char* selectedMacroName = nullptr;
  uint16_t selectedMacroColor = TFT_WHITE;
  uint16_t unselectedColor = hexToRGB565("#24242D");
  
  for (int i = 0; i < 16; i++) {
    float angle = -PI / 2 + (i * PI * 2 / 16.0);
    int cx = centerX + cos(angle) * radius;
    int cy = centerY + sin(angle) * radius;
    
    JsonObject mObj;
    bool hasMacro = false;
    for (JsonObject m : macros) {
      int pos = m["pos"] | -1;
      if (pos == i) {
        mObj = m;
        hasMacro = true;
        break;
      }
    }
    
    if (i == selectedMacroIdx) {
      uint16_t needleColor = hexToRGB565("#BB0A00");
      int nx = centerX + cos(angle) * (radius - 20);
      int ny = centerY + sin(angle) * (radius - 20);
      int bx1 = centerX + cos(angle - PI/2) * 3;
      int by1 = centerY + sin(angle - PI/2) * 3;
      int bx2 = centerX + cos(angle + PI/2) * 3;
      int by2 = centerY + sin(angle + PI/2) * 3;
      d.fillTriangle(nx, ny, bx1, by1, bx2, by2, needleColor);
      
      if (hasMacro) {
        selectedMacroName = mObj["name"] | "Macro";
        selectedMacroColor = hexToRGB565(mObj["color"] | "#FFFFFF");
      }
    }
    
    if (hasMacro) {
      uint16_t color = hexToRGB565(mObj["color"] | "#FFFFFF");
      d.fillCircle(cx, cy, 18, color);
      
      const char* iconXbmStr = mObj["icon_xbm"] | "";
      if (strlen(iconXbmStr) == 108) {
        uint8_t xbmData[54];
        for (int b = 0; b < 54; b++) {
          char hex[3] = { iconXbmStr[b*2], iconXbmStr[b*2+1], '\0' };
          xbmData[b] = (uint8_t)strtol(hex, NULL, 16);
        }
        d.drawXBitmap(cx - 9, cy - 9, xbmData, 18, 18, TFT_BLACK);
      } else {
        const char* iconStr = mObj["icon"] | "";
        if (strcmp(iconStr, "tool") == 0 || strcmp(iconStr, "hammer") == 0) {
          d.drawXBitmap(cx - icon_tool_width/2, cy - icon_tool_height/2, icon_tool_bits, icon_tool_width, icon_tool_height, TFT_BLACK);
        } else if (strcmp(iconStr, "type") == 0 || strcmp(iconStr, "text") == 0) {
          d.drawXBitmap(cx - icon_type_width/2, cy - icon_type_height/2, icon_type_bits, icon_type_width, icon_type_height, TFT_BLACK);
        } else if (strcmp(iconStr, "mic") == 0) {
          d.drawXBitmap(cx - icon_mic_width/2, cy - icon_mic_height/2, icon_mic_bits, icon_mic_width, icon_mic_height, TFT_BLACK);
        }
      }
    } else {
      d.fillCircle(cx, cy, 18, unselectedColor);
    }
    
    if (runningMacros[i].active) {
      d.drawCircle(cx, cy, 19, TFT_GREEN);
      d.drawCircle(cx, cy, 20, TFT_GREEN);
    }
  }
  
  d.setFont(&fonts::Orbitron_Light_24);
  d.setTextColor(profColor, TFT_BLACK);
  d.drawString(profName, 120, 105);

  if (selectedMacroName != nullptr) {
    d.setTextColor(selectedMacroColor, TFT_BLACK);
    d.drawString(selectedMacroName, 120, 160);
  } else {
    d.setTextColor(TFT_DARKGRAY, TFT_BLACK);
    d.drawString("Empty", 120, 160);
  }
  
  // Draw Kill All if any macro is running
  bool anyRunning = false;
  for (int i=0; i<16; i++) {
    if (runningMacros[i].active) anyRunning = true;
  }
  if (anyRunning) {
    d.setFont(&fonts::Orbitron_Light_24);
    d.setTextColor(TFT_RED, TFT_BLACK);
    d.drawString("Kill all", 120, 65);
    d.fillTriangle(120, 40, 115, 50, 125, 50, TFT_RED); // Up arrow
  }
}

void loadProfiles() {
  File file = LittleFS.open("/profiles.json", "r");
  if (!file) {
    Serial.println("Failed to open profiles.json, creating default");
    file = LittleFS.open("/profiles.json", "w");
    file.print(defaultProfilesJson);
    file.close();
    file = LittleFS.open("/profiles.json", "r");
  }
  
  DeserializationError error = deserializeJson(profilesDoc, file);
  file.close();
  
  if (error) {
    Serial.println("Failed to parse profiles.json");
  } else {
    Serial.println("Loaded profiles.json");
    activeProfileIdx = prefs.getInt("activeProfile", 0);
    JsonArray profiles = profilesDoc["profiles"];
    if (activeProfileIdx >= profiles.size()) activeProfileIdx = 0;
  }
}

uint8_t getModifierCode(const char* mod) {
  String m = String(mod);
  m.toUpperCase();
  if (m == "CTRL") return KEY_LEFT_CTRL;
  if (m == "SHIFT") return KEY_LEFT_SHIFT;
  if (m == "ALT") return KEY_LEFT_ALT;
  if (m == "WIN" || m == "GUI") return KEY_LEFT_GUI;
  return 0;
}

uint16_t getConsumerCode(const char* code) {
  String c = String(code);
  c.toUpperCase();
  if (c == "MUTE") return CONSUMER_CONTROL_MUTE;
  if (c == "VOL_UP") return CONSUMER_CONTROL_VOLUME_INCREMENT;
  if (c == "VOL_DOWN") return CONSUMER_CONTROL_VOLUME_DECREMENT;
  if (c == "PLAY_PAUSE") return CONSUMER_CONTROL_PLAY_PAUSE;
  if (c == "NEXT") return CONSUMER_CONTROL_SCAN_NEXT;
  if (c == "PREV") return CONSUMER_CONTROL_SCAN_PREVIOUS;
  return 0;
}

uint8_t getMouseButtonCode(const char* btn) {
  String b = String(btn);
  b.toUpperCase();
  if (b == "LEFT") return MOUSE_LEFT;
  if (b == "RIGHT") return MOUSE_RIGHT;
  if (b == "MIDDLE") return MOUSE_MIDDLE;
  if (b == "MB4" || b == "BACKWARD") return MOUSE_BACKWARD;
  if (b == "MB5" || b == "FORWARD") return MOUSE_FORWARD;
  return 0;
}

uint8_t getSpecialKeyCode(const char* key) {
  String k = String(key);
  k.toUpperCase();
  if (k == "PRINTSCREEN" || k == "PRTSCN") return 0xCE;
  if (k == "ESC") return 0xB1;
  if (k == "TAB") return 0xB3;
  if (k == "ENTER" || k == "RETURN") return 0xB0;
  if (k == "SPACE") return 0x20;
  if (k == "BACKSPACE") return 0xB2;
  if (k == "DELETE" || k == "DEL") return 0xD4;
  if (k == "UP") return 0xDA;
  if (k == "DOWN") return 0xD9;
  if (k == "LEFT") return 0xD8;
  if (k == "RIGHT") return 0xD7;
  if (k == "HOME") return 0xD2;
  if (k == "END") return 0xD5;
  if (k == "PAGEUP") return 0xD3;
  if (k == "PAGEDOWN") return 0xD6;
  if (k == "F1") return 0xC2;
  if (k == "F2") return 0xC3;
  if (k == "F3") return 0xC4;
  if (k == "F4") return 0xC5;
  if (k == "F5") return 0xC6;
  if (k == "F6") return 0xC7;
  if (k == "F7") return 0xC8;
  if (k == "F8") return 0xC9;
  if (k == "F9") return 0xCA;
  if (k == "F10") return 0xCB;
  if (k == "F11") return 0xCC;
  if (k == "F12") return 0xCD;
  return 0;
}

void fireMacro(JsonObject macro, int keyIdx) {
  const char* mode = macro["mode"] | "play_once";
  
  if (strcmp(mode, "rotary") == 0) {
    inRotaryMode = true;
    activeRotaryMacro = macro;
    requestRedraw();
    return;
  }
  
  bool isToggle = (strcmp(mode, "toggle") == 0);
  
  if (runningMacros[keyIdx].active && runningMacros[keyIdx].isToggle) {
    // Kill toggle macro
    runningMacros[keyIdx].active = false;
    if (trellisFound) {
      trellis.pixels.setPixelColor(keyIdx, 0);
      trellis.pixels.show();
    }
    // Redraw UI to possibly remove Kill All button
    requestRedraw();
    return;
  }
  
  runningMacros[keyIdx].active = true;
  runningMacros[keyIdx].isToggle = isToggle;
  runningMacros[keyIdx].keyIndex = keyIdx;
  runningMacros[keyIdx].macroDef = macro;
  runningMacros[keyIdx].currentActionIndex = 0;
  runningMacros[keyIdx].nextActionTime = millis();
  
  const char* colorHex = macro["color"] | "#FFFFFF";
  long rgb = strtol(colorHex + 1, nullptr, 16);
  runningMacros[keyIdx].color = rgb;
  
  requestRedraw(); // Show kill all button if needed
}

void executeAction(JsonObject action) {
  const char* type = action["type"];
  if (strcmp(type, "key") == 0) {
    JsonArray mods = action["mods"];
    for (const char* mod : mods) {
      Keyboard.press(getModifierCode(mod));
    }
    const char* keyStr = action["key"];
    if (keyStr && strlen(keyStr) > 0) {
      uint8_t code = getSpecialKeyCode(keyStr);
      if (code > 0) {
        Keyboard.press(code);
      } else {
        Keyboard.press(keyStr[0]);
      }
    }
    Keyboard.releaseAll();
  } else if (strcmp(type, "text") == 0) {
    const char* text = action["value"];
    if (text) Keyboard.print(text);
  } else if (strcmp(type, "consumer") == 0) {
    const char* code = action["code"];
    ConsumerControl.press(getConsumerCode(code));
    ConsumerControl.release();
  } else if (strcmp(type, "mouse") == 0) {
    const char* btn = action["button"];
    const char* evt = action["event"] | "click";
    String e = String(evt);
    e.toUpperCase();
    String b = String(btn);
    b.toUpperCase();
    
    if (b == "SCROLL_UP") {
      Mouse.move(0, 0, 1, 0);
    } else if (b == "SCROLL_DOWN") {
      Mouse.move(0, 0, -1, 0);
    } else if (b == "SCROLL_LEFT") {
      Mouse.move(0, 0, 0, -1);
    } else if (b == "SCROLL_RIGHT") {
      Mouse.move(0, 0, 0, 1);
    } else {
      uint8_t mcode = getMouseButtonCode(btn);
      if (e == "PRESS" || e == "HOLD") {
        Mouse.press(mcode);
      } else if (e == "RELEASE") {
        Mouse.release(mcode);
      } else if (e == "DOUBLE_CLICK") {
        Mouse.click(mcode);
        delay(50);
        Mouse.click(mcode);
      } else {
        Mouse.click(mcode);
      }
    }
  }
}

void updateMacros() {
  unsigned long now = millis();
  for (int i = 0; i < 16; i++) {
    if (runningMacros[i].active) {
      if (now >= runningMacros[i].nextActionTime) {
        JsonArray actions = runningMacros[i].macroDef["actions"];
        if (runningMacros[i].currentActionIndex >= actions.size()) {
          if (runningMacros[i].isToggle) {
            runningMacros[i].currentActionIndex = 0; 
          } else {
            runningMacros[i].active = false;
            if (trellisFound) {
              trellis.pixels.setPixelColor(i, 0);
              trellis.pixels.show();
            }
            requestRedraw(); // hide kill all if needed
            continue;
          }
        }
        
        // Execute action
        JsonObject action = actions[runningMacros[i].currentActionIndex];
        const char* type = action["type"];
        unsigned long actionDelay = 10;
        
        executeAction(action);
        
        if (strcmp(type, "delay") == 0) {
          actionDelay = action["ms"] | 100;
        }
        
        runningMacros[i].currentActionIndex++;
        runningMacros[i].nextActionTime = millis() + actionDelay;
      }
    }
  }
}

void killAllMacros() {
  bool changed = false;
  for (int i = 0; i < 16; i++) {
    if (runningMacros[i].active) {
      runningMacros[i].active = false;
      changed = true;
      if (trellisFound) trellis.pixels.setPixelColor(i, 0);
    }
  }
  if (trellisFound) trellis.pixels.show();
  Keyboard.releaseAll();
  ConsumerControl.release();
  Mouse.release(MOUSE_ALL);
  
  if (changed) {
    requestRedraw();
  }
}

void requestRedraw() {
  uiNeedsRedraw = true;
}

TrellisCallback trellisEvent(keyEvent evt) {
  int keyIdx = evt.bit.NUM;
  if (evt.bit.EDGE == SEESAW_KEYPAD_EDGE_RISING) {
    
    if (currentMode == RUN_MODE) {
      JsonArray profiles = profilesDoc["profiles"];
      JsonObject prof = profiles[activeProfileIdx];
      JsonArray macros = prof["macros"];
      
      bool handled = false;
      for (JsonObject m : macros) {
        if (m["pos"] == keyIdx) {
          fireMacro(m, keyIdx);
          handled = true;
          break;
        }
      }
      
      if (runningMacros[keyIdx].active) {
        // Blink it brightly or set to its color
        trellis.pixels.setPixelColor(keyIdx, runningMacros[keyIdx].color);
      } else {
        trellis.pixels.setPixelColor(keyIdx, 0x00FF00); // Default flash if empty
      }
    } else {
       trellis.pixels.setPixelColor(keyIdx, 0x00FF00); 
    }
    trellis.pixels.show();
    
  } else if (evt.bit.EDGE == SEESAW_KEYPAD_EDGE_FALLING) {
    if (!runningMacros[keyIdx].active) {
      trellis.pixels.setPixelColor(keyIdx, 0); 
      trellis.pixels.show();
    }
  }
  return 0;
}

// Per-chunk payload size (excluding our 1-byte sequence prefix). Verified directly by testing:
// full 500B chunks reliably got SUCCESS_NOTIFY from the stack but never reached the app (0/5
// retries acked over 4s), while a ~20B size (hit by accident via an earlier MTU-lookup bug)
// delivered 160+ consecutive chunks with zero retries. WiFi/BLE coexistence — this firmware runs
// WiFi STA + a WebServer alongside BLE — was the leading suspect, so WiFi is now paused for the
// duration of any BLE connection (see MyServerCallbacks). 100B is a middle ground between that
// proven-reliable size and full throughput now that the radio isn't shared; re-measure if drops
// come back.
const int BLE_CHUNK_PAYLOAD_SIZE = 100;

// Sends one chunk, prefixed with a sequence byte, via notify() and blocks until the app echoes
// that same seq back as an ack before returning, so the caller can safely overwrite the
// characteristic's value buffer for the next chunk. notify() has no over-the-air delivery
// guarantee — confirmed by testing: the BLE stack reports SUCCESS_NOTIFY (handed to the
// controller) even when the central's Android stack never surfaces the packet at all — so a
// missing ack is resolved by resending the same chunk, not just waiting longer. The sequence
// number lets us tell a genuinely-missing ack apart from a late ack for a chunk we already
// gave up on, so a retry can never be mistaken for confirmation of the wrong chunk — and lets
// the app-side detect a resend that arrives after the original was already processed.
// Returns false only once retries are exhausted — caller should abort the rest of the message
// rather than keep blasting chunks the app was never confirmed to have received.
bool sendNotifyAndWaitAck(const uint8_t *data, size_t len, uint8_t seq) {
  uint8_t framed[501];
  framed[0] = seq;
  memcpy(framed + 1, data, len);
  const int maxAttempts = 5;
  const uint32_t attemptTimeoutMs = 800;
  for (int attempt = 1; attempt <= maxAttempts; attempt++) {
    Serial.print("BLE TX: notify seq=");
    Serial.print(seq);
    Serial.print(" len=");
    Serial.print(len);
    Serial.print(" attempt=");
    Serial.println(attempt);
    pTxCharacteristic->setValue(framed, len + 1);
    pTxCharacteristic->notify();
    // A stale ack (a late arrival from an earlier retry of a PRIOR chunk, still in flight from
    // the app when we started this attempt) must not cost this attempt its full timeout budget —
    // keep waiting out the REMAINING time for the real ack instead of treating "got something,
    // just not a match" the same as "got nothing" and immediately resending. Resending on every
    // stale ack was amplifying exactly this duplicate-ack noise instead of letting it settle.
    uint32_t deadline = millis() + attemptTimeoutMs;
    bool ackedThisAttempt = false;
    while (true) {
      int32_t remaining = (int32_t)(deadline - millis());
      if (remaining <= 0) break;
      uint8_t ackedSeq;
      if (xQueueReceive(bleAckQueue, &ackedSeq, pdMS_TO_TICKS(remaining)) != pdTRUE) break;
      if (ackedSeq == seq) {
        ackedThisAttempt = true;
        break;
      }
      Serial.print("BLE TX: stale ack seq=");
      Serial.print(ackedSeq);
      Serial.print(" expected=");
      Serial.println(seq);
    }
    if (ackedThisAttempt) return true;
    Serial.print("BLE TX: chunk ack timed out, attempt ");
    Serial.println(attempt);
  }
  Serial.println("BLE TX: chunk ack timed out after all retries, aborting message");
  return false;
}

// Shared preamble for any chunked TX: log link state, drain any stale ack left over from a
// prior aborted message, and give the central's BLE stack a moment to settle after enabling
// notifications — sending immediately (observed ~250ms after CCCD-enable) reproducibly missed
// the very first packet.
void bleSendPreamble() {
  Serial.print("BLE TX: connectedCount=");
  Serial.print(pServer->getConnectedCount());
  Serial.print(" peerDevices=");
  Serial.println(pServer->getPeerDevices(false).size());
  uint8_t dummy;
  while (xQueueReceive(bleAckQueue, &dummy, 0) == pdTRUE) {}
  delay(300);
}

// The ,"icon_xbm":"<hex>" pairs are stripped from get_profiles responses on the fly (the app
// only needs the icon *name*; XBM bitmaps are display-side data). Streaming state machine
// below matches this marker byte-by-byte.
static const char ICON_XBM_MARKER[] = ",\"icon_xbm\":\"";

// Streams a large JSON response straight into the chunked notify+ack pipeline through a fixed
// chunk-size buffer, so the full ~12KB get_profiles payload never exists in RAM at once.
//
// WHY THIS EXISTS (heap fragmentation, no-PSRAM S3): the previous implementation built the
// payload as Strings — serializeJson into an ~11-12KB String, concatenated into a second
// ~equal-size wrapper String (≈24KB contiguous peak), then held that String alive for the whole
// multi-second chunked send while the BLE stack allocated into the freed holes. After 1-2
// fetches per boot the largest contiguous free block dropped below what serializeJson's String
// needed and it came back EMPTY (never partial — an up-front allocation failure), producing the
// observed exactly-27-byte {"status":"ok","profiles":} corruption. This sink's peak heap cost
// is ~0 (one chunk buffer + a few ints, on the stack).
//
// Usage: print()/write() the message through it (serializeJson accepts any Print), end the
// message with the '\n' delimiter, then call flushRemainder() and check failed.
class BleChunkSink : public Print {
public:
  bool failed = false;     // sticky: set when a chunk exhausts its ack retries
  size_t totalSent = 0;    // bytes emitted after icon_xbm stripping (for logging)

  size_t write(uint8_t c) override {
    if (failed) return 0;
    if (_skipping) {
      // Swallowing an icon_xbm hex value: it contains no quotes or escapes, so it ends at the
      // next '"' (also swallowed — the marker's opening quote was never emitted).
      if (c == '"') _skipping = false;
      return 1;
    }
    if (c == (uint8_t)ICON_XBM_MARKER[_matched]) {
      _matched++;
      if (ICON_XBM_MARKER[_matched] == '\0') { // full marker matched — swallow the value next
        _matched = 0;
        _skipping = true;
      }
      return 1; // matched bytes are withheld until the match fails or completes
    }
    if (_matched > 0) {
      // Partial match broken: emit the withheld marker prefix, then re-run this byte against
      // the marker start. (Safe single-step fallback: ',' only occurs at position 0 of the
      // marker, so no longer suffix of a broken match can begin a new match.)
      int had = _matched;
      _matched = 0;
      for (int i = 0; i < had; i++) {
        if (!emit((uint8_t)ICON_XBM_MARKER[i])) return 0;
      }
      if (c == (uint8_t)ICON_XBM_MARKER[0]) {
        _matched = 1;
        return 1;
      }
    }
    return emit(c) ? 1 : 0;
  }

  size_t write(const uint8_t *buffer, size_t size) override {
    size_t n = 0;
    while (n < size && write(buffer[n]) == 1) n++;
    return n;
  }

  // Send whatever partial chunk remains. Call once, after the trailing '\n' delimiter.
  bool flushRemainder() {
    if (failed) return false;
    // A withheld partial marker match at end-of-stream can't be a real icon_xbm (the message
    // ends "}\n"), but emit it anyway for correctness.
    int had = _matched;
    _matched = 0;
    for (int i = 0; i < had; i++) {
      if (!emit((uint8_t)ICON_XBM_MARKER[i])) return false;
    }
    if (_fill > 0) {
      if (!sendNotifyAndWaitAck(_buf, _fill, _seq)) { failed = true; return false; }
      _seq++;
      _fill = 0;
    }
    return true;
  }

private:
  uint8_t _buf[BLE_CHUNK_PAYLOAD_SIZE];
  int _fill = 0;
  uint8_t _seq = 0;      // same per-message sequence numbering the app already dedups on
  int _matched = 0;      // bytes of ICON_XBM_MARKER currently matched (withheld)
  bool _skipping = false; // inside an icon_xbm hex value

  bool emit(uint8_t c) {
    _buf[_fill++] = c;
    totalSent++;
    if (_fill == BLE_CHUNK_PAYLOAD_SIZE) {
      if (!sendNotifyAndWaitAck(_buf, _fill, _seq)) { failed = true; return false; }
      _seq++;
      _fill = 0;
    }
    return true;
  }
};

void sendBleMessage(const String &msg) {
  Serial.print("BLE TX: sending message of length ");
  Serial.println(msg.length());
  bleSendPreamble();
  int len = msg.length();
  int offset = 0;
  uint8_t seq = 0;
  while (offset < len) {
    int chunkSize = min(BLE_CHUNK_PAYLOAD_SIZE, len - offset);
    if (!sendNotifyAndWaitAck((const uint8_t*)(msg.c_str() + offset), chunkSize, seq)) return;
    offset += chunkSize;
    seq++;
  }
  // Send the newline delimiter at the end
  uint8_t nl = '\n';
  if (!sendNotifyAndWaitAck(&nl, 1, seq)) return;
  Serial.println("BLE TX: sent");
}

// Takes a mutable char* on purpose: deserializeJson(doc, char*) parses ZERO-COPY — req's
// strings point into cmdStr itself (unescaped in place) instead of being duplicated, which
// roughly halves peak RAM while parsing a ~12KB save_profiles command. cmdStr must therefore
// stay alive until this function returns (the caller frees it afterwards).
void handleBleCommand(char *cmdStr) {
  Serial.print("handleBleCommand: ");
  Serial.println(cmdStr);
  JsonDocument req;
  DeserializationError err = deserializeJson(req, cmdStr);
  if (err) {
    Serial.print("JSON Parse Error: ");
    Serial.println(err.c_str());
    sendBleMessage("{\"status\":\"error\",\"message\":\"Invalid JSON\"}");
    return;
  }
  
  String cmd = req["cmd"] | "";
  String token = req["token"] | "";
  
  if (cmd == "pair") {
    if (currentMode != CONFIG_MODE) {
      sendBleMessage("{\"status\":\"error\",\"message\":\"Not in Config Mode\"}");
      return;
    }
    pairingToken = String(esp_random(), HEX) + String(esp_random(), HEX) + String(esp_random(), HEX);
    prefs.putString("pairingToken", pairingToken);
    sendBleMessage("{\"status\":\"ok\",\"token\":\"" + pairingToken + "\"}");
    return;
  }
  
  // Authorization check for other commands
  if (currentMode != CONFIG_MODE && (token != pairingToken || pairingToken.length() == 0)) {
    sendBleMessage("{\"status\":\"error\",\"message\":\"Unauthorized\"}");
    return;
  }
  
  if (cmd == "get_profiles") {
    // Stream the response straight from the already-loaded global profilesDoc into the chunked
    // BLE pipeline via BleChunkSink — the payload is NEVER built as a String. Two OOM bugs have
    // lived in this branch already: (1) re-parsing profiles.json into a second JsonDocument
    // failed once the BLE stack had claimed heap, and (2) the String-based double-buffer build
    // fragmented the heap until serializeJson returned empty after 1-2 fetches per boot
    // (the {"status":"ok","profiles":} corruption). See BleChunkSink for details.
    Serial.printf("get_profiles: heap before: free=%u largestBlock=%u\n",
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    bleSendPreamble();
    BleChunkSink sink;
    sink.print("{\"status\":\"ok\",\"profiles\":");
    serializeJson(profilesDoc, sink);           // icon_xbm stripped on the fly by the sink
    sink.print("}\n");                           // '\n' = end-of-message delimiter for the app
    if (sink.flushRemainder()) {
      Serial.print("get_profiles: streamed bytes = ");
      Serial.println(sink.totalSent);
      Serial.println("BLE TX: sent");
    } else {
      Serial.println("get_profiles: send aborted (chunk ack retries exhausted)");
    }
    Serial.printf("get_profiles: heap after: free=%u largestBlock=%u\n",
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  }
  else if (cmd == "save_profiles") {
    JsonObject profilesObj = req["profiles"];
    if (profilesObj.isNull()) {
      sendBleMessage("{\"status\":\"error\",\"message\":\"No profiles object provided\"}");
      return;
    }
    
    File f = LittleFS.open("/profiles.json", "w");
    if (f) {
      serializeJson(profilesObj, f);
      f.close();
      
      killAllMacros();
      loadProfiles();
      
      int brightness = profilesDoc["settings"]["brightness"] | 160;
      M5Dial.Display.setBrightness(brightness);
      
      int orientation = profilesDoc["settings"]["orientation"] | 0;
      M5Dial.Display.setRotation(orientation);

      requestRedraw();
      sendBleMessage("{\"status\":\"ok\"}");
    } else {
      sendBleMessage("{\"status\":\"error\",\"message\":\"Failed to write file\"}");
    }
  } 
  else if (cmd == "trigger") {
    int pIdx = req["profile"] | activeProfileIdx;
    int mIdx = req["macro"] | -1;
    
    JsonArray profiles = profilesDoc["profiles"];
    if (pIdx >= 0 && pIdx < profiles.size()) {
      JsonObject prof = profiles[pIdx];
      JsonArray macros = prof["macros"];
      bool fired = false;
      for (JsonObject m : macros) {
        int pos = m["pos"] | -1;
        if (pos == mIdx) {
          fireMacro(m, mIdx);
          fired = true;
          break;
        }
      }
      if (fired) {
        sendBleMessage("{\"status\":\"ok\"}");
      } else {
        sendBleMessage("{\"status\":\"error\",\"message\":\"Macro not found\"}");
      }
    } else {
      sendBleMessage("{\"status\":\"error\",\"message\":\"Profile not found\"}");
    }
  } 
  else {
    sendBleMessage("{\"status\":\"error\",\"message\":\"Unknown command\"}");
  }
}

class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
      deviceConnected = true;
      // The actual WiFi pause happens on the main loop (see the deviceConnected/oldDeviceConnected
      // transition handling below), not here — this callback runs on the BLE stack's own task, and
      // driving a WiFi driver reconfiguration from that foreign task while the main loop is
      // concurrently building the get_profiles response corrupted it (verified: reverting this to
      // a loop-driven toggle fixed a reproducible empty-JSON response that appeared as soon as this
      // callback called WiFi.mode() directly).
      Serial.print("BLE Client Connected, connectedCount=");
      Serial.print(pServer->getConnectedCount());
      Serial.print(" peerDevices=");
      Serial.println(pServer->getPeerDevices(false).size());
    };

    void onDisconnect(BLEServer* pServer) {
      deviceConnected = false;
      // A half-received command from a dropped connection must not poison the next one.
      bleRxLen = 0;
      Serial.print("BLE Client Disconnected, connectedCount=");
      Serial.print(pServer->getConnectedCount());
      Serial.print(" peerDevices=");
      Serial.println(pServer->getPeerDevices(false).size());
    }
};

class MyCallbacks: public BLECharacteristicCallbacks {
    String lastRxValue;
    void onWrite(BLECharacteristic *pCharacteristic) {
      String rxValue = pCharacteristic->getValue();
      if (rxValue.length() == 2 && (uint8_t)rxValue[0] == BLE_CHUNK_ACK_MARKER) {
        uint8_t seq = (uint8_t)rxValue[1];
        xQueueOverwrite(bleAckQueue, &seq);
        return;
      }
      // The BLE stack has been observed to invoke onWrite() twice for a single write from the
      // app — the same underlying quirk documented for the doubled TX notify-status callback.
      // Harmless there (just a log line); here it used to double-append into the RX buffer and,
      // combined with heap fragmentation, corrupt the reassembled save_profiles command down to
      // a garbage tail. Skip an exact repeat of the immediately-previous write.
      if (rxValue == lastRxValue) {
        Serial.println("BLE RX: duplicate write ignored");
        return;
      }
      lastRxValue = rxValue;

      Serial.print("BLE RX bytes: ");
      Serial.println(rxValue.length());
      if (bleRxBuf == nullptr) return; // malloc failed at boot — already logged as fatal
      if (rxValue.length() > 0) {
        for (int i = 0; i < rxValue.length(); i++) {
          char c = rxValue[i];
          if (c == '\n') {
            bleRxBuf[bleRxLen] = '\0';
            // Pass to main loop via queue — FreeRTOS queue ops include memory barriers
            // that guarantee cross-core visibility (volatile alone is insufficient on ESP32-S3)
            char *cmdCopy = strdup(bleRxBuf);
            bleRxLen = 0;
            if (cmdCopy != nullptr && bleRxQueue != nullptr) {
              if (xQueueSend(bleRxQueue, &cmdCopy, 0) != pdTRUE) {
                Serial.println("BLE RX queue full, dropping command");
                free(cmdCopy);
              } else {
                Serial.println("BLE RX queued OK");
              }
            }
          } else if (bleRxLen < BLE_RX_BUFFER_SIZE - 1) {
            bleRxBuf[bleRxLen++] = c;
          } else {
            Serial.println("BLE RX: command exceeds buffer, dropping");
            bleRxLen = 0;
          }
        }
      }
    }
};

// Pure diagnostics — does not participate in the ack flow control. Logs what notify()
// actually did, since a silent no-op (e.g. central hasn't re-enabled the CCCD yet) looks
// identical to a dropped-over-the-air packet from the ack-timeout's point of view.
class TxLogCallbacks: public BLECharacteristicCallbacks {
    void onStatus(BLECharacteristic *pCharacteristic, Status s, uint32_t code) {
      Serial.print("BLE TX notify status [core=");
      Serial.print(xPortGetCoreID());
      Serial.print(" task=");
      Serial.print(pcTaskGetName(NULL));
      Serial.print(" us=");
      Serial.print((unsigned long)esp_timer_get_time());
      Serial.print("]: ");
      switch (s) {
        case SUCCESS_NOTIFY: Serial.println("SUCCESS_NOTIFY"); break;
        case SUCCESS_INDICATE: Serial.println("SUCCESS_INDICATE"); break;
        case ERROR_INDICATE_DISABLED: Serial.println("ERROR_INDICATE_DISABLED"); break;
        case ERROR_NOTIFY_DISABLED: Serial.println("ERROR_NOTIFY_DISABLED"); break;
        case ERROR_GATT: Serial.print("ERROR_GATT code="); Serial.println(code); break;
        case ERROR_NO_CLIENT: Serial.println("ERROR_NO_CLIENT"); break;
        case ERROR_NO_SUBSCRIBER: Serial.println("ERROR_NO_SUBSCRIBER"); break;
        case ERROR_INDICATE_TIMEOUT: Serial.println("ERROR_INDICATE_TIMEOUT"); break;
        case ERROR_INDICATE_FAILURE: Serial.println("ERROR_INDICATE_FAILURE"); break;
        default: Serial.println((int)s); break;
      }
    }
};

bool isAuthorized() {
  if (currentMode == CONFIG_MODE) return true;
  if (server.hasHeader("Authorization")) {
    String auth = server.header("Authorization");
    if (auth == "Bearer " + pairingToken && pairingToken.length() > 0) return true;
  }
  return false;
}

void setupWebServer() {
  server.enableCORS(true);
  
  const char * headerkeys[] = {"Authorization"};
  size_t headerkeyssize = sizeof(headerkeys)/sizeof(char*);
  server.collectHeaders(headerkeys, headerkeyssize);
  
  server.on("/", HTTP_GET, [](){
    if (currentMode != CONFIG_MODE) {
      server.send(403, "text/plain", "Forbidden. Swipe down on Draupnir to enter Config Mode.");
      return;
    }
    server.send(200, "text/html", INDEX_HTML);
  });
  
  server.on("/api/pair", HTTP_POST, []() {
    if (currentMode != CONFIG_MODE) {
      server.send(403, "application/json", "{\"status\":\"error\",\"message\":\"Not in Config Mode\"}");
      return;
    }
    pairingToken = String(esp_random(), HEX) + String(esp_random(), HEX) + String(esp_random(), HEX);
    prefs.putString("pairingToken", pairingToken);
    server.send(200, "application/json", "{\"token\":\"" + pairingToken + "\"}");
  });

  server.on("/api/profiles", HTTP_GET, [](){
    if (!isAuthorized()) {
      server.send(401, "application/json", "{\"status\":\"error\",\"message\":\"Unauthorized\"}");
      return;
    }
    File file = LittleFS.open("/profiles.json", "r");
    if(!file){
      server.send(500, "text/plain", "Failed to open file");
      return;
    }
    server.streamFile(file, "application/json");
    file.close();
  });
  
  server.on("/api/profiles", HTTP_POST, [](){
    if (!isAuthorized()) {
      server.send(401, "application/json", "{\"status\":\"error\",\"message\":\"Unauthorized\"}");
      return;
    }
    if(server.hasArg("plain")) {
      String body = server.arg("plain");
      File f = LittleFS.open("/profiles.json", "w");
      if(f) {
        f.print(body);
        f.close();
        server.send(200, "text/plain", "OK");
        killAllMacros();
        loadProfiles();
        
        int brightness = profilesDoc["settings"]["brightness"] | 160;
        M5Dial.Display.setBrightness(brightness);
        
        int orientation = profilesDoc["settings"]["orientation"] | 0;
        M5Dial.Display.setRotation(orientation);

        requestRedraw();
      } else {
        server.send(500, "text/plain", "Failed to write");
      }
    } else {
      server.send(400, "text/plain", "No body");
    }
  });
  
  server.on("/api/trigger", HTTP_POST, [](){
    if (!isAuthorized()) {
      server.send(401, "application/json", "{\"status\":\"error\",\"message\":\"Unauthorized\"}");
      return;
    }
    if(server.hasArg("plain")) {
      String body = server.arg("plain");
      JsonDocument req;
      DeserializationError err = deserializeJson(req, body);
      if(!err) {
        int pIdx = req["profile"] | activeProfileIdx;
        int mIdx = req["macro"] | -1;
        
        JsonArray profiles = profilesDoc["profiles"];
        if (pIdx >= 0 && pIdx < profiles.size()) {
          JsonObject prof = profiles[pIdx];
          JsonArray macros = prof["macros"];
          bool fired = false;
          for (JsonObject m : macros) {
            int pos = m["pos"] | -1;
            if (pos == mIdx) {
              fireMacro(m, mIdx);
              fired = true;
              break;
            }
          }
          if (fired) {
            server.send(200, "application/json", "{\"status\":\"ok\"}");
          } else {
            server.send(404, "application/json", "{\"status\":\"error\",\"message\":\"Macro not found\"}");
          }
        } else {
          server.send(404, "application/json", "{\"status\":\"error\",\"message\":\"Profile not found\"}");
        }
      } else {
        server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Invalid JSON\"}");
      }
    } else {
      server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"No body\"}");
    }
  });

  server.begin();
}

void enterConfigMode() {
  currentMode = CONFIG_MODE;
  killAllMacros();
  
  auto& d = M5Dial.Display;
  d.fillScreen(TFT_BLACK);
  d.setTextDatum(middle_center);
  d.setFont(&fonts::Orbitron_Light_24);
  d.setTextColor(TFT_GREEN, TFT_BLACK);
  d.drawString("CONFIG MODE", 120, 60);
  
  d.setFont(&fonts::Orbitron_Light_24);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.drawString("IP Address:", 120, 110);
  d.drawString(WiFi.localIP().toString(), 120, 140);
  
  d.setTextColor(TFT_DARKGRAY, TFT_BLACK);
  d.drawString("TAP TO EXIT", 120, 190);
}

void enterWiFiSetupMode() {
  currentMode = WIFI_SETUP_MODE;
  
  killAllMacros();
  
  auto& d = M5Dial.Display;
  d.fillScreen(TFT_BLACK);
  d.setTextDatum(middle_center);
  d.setFont(&fonts::Orbitron_Light_24);
  d.setTextColor(TFT_ORANGE, TFT_BLACK);
  d.drawString("Wi-Fi SETUP", 120, 60);
  
  d.setFont(&fonts::Orbitron_Light_24);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.drawString("Connect to hotspot:", 120, 120);
  d.drawString("Draupnir-Setup", 120, 150);
  
  server.stop(); 
  
  WiFiManager wm;
  bool res = wm.startConfigPortal("Draupnir-Setup");
  
  if (!res) {
    Serial.println("Failed to connect or hit timeout");
  } else {
    Serial.println("Connected to Wi-Fi!");
  }
  
  ESP.restart();
}

void setup() {
  auto cfg = M5.config();
  M5Dial.begin(cfg, true, false);
  
  Serial.begin(115200);
  
  Wire.begin(13, 15);
  if (!trellis.begin(0x2E)) {
    Serial.println("NeoTrellis not found!");
    trellisFound = false;
  } else {
    Serial.println("NeoTrellis started");
    trellisFound = true;
    for (int i = 0; i < 16; i++) {
      trellis.activateKey(i, SEESAW_KEYPAD_EDGE_RISING);
      trellis.activateKey(i, SEESAW_KEYPAD_EDGE_FALLING);
      trellis.registerCallback(i, trellisEvent);
    }
  }
  
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS Mount Failed");
  }
  
  prefs.begin("draupnir", false);
  pairingToken = prefs.getString("pairingToken", "");
  
  loadProfiles();
  
  int brightness = profilesDoc["settings"]["brightness"] | 160;
  M5Dial.Display.setBrightness(brightness);
  
  int orientation = profilesDoc["settings"]["orientation"] | 0;
  M5Dial.Display.setRotation(orientation);
  
  WiFi.mode(WIFI_STA);
  WiFi.begin();

  setupWebServer();

  // USB must start before BLE on ESP32-S3 — USB.begin() disrupts BLE if it runs after
  Keyboard.begin();
  ConsumerControl.begin();
  Mouse.begin();
  USB.begin();
  delay(200); // let USB settle before BLE init

  // Initialize BLE
  bleRxQueue = xQueueCreate(4, sizeof(char*));
  bleAckQueue = xQueueCreate(1, sizeof(uint8_t));
  BLEDevice::init("Draupnir");
  bleRxBuf = (char*)malloc(BLE_RX_BUFFER_SIZE);
  if (bleRxBuf == nullptr) {
    Serial.println("FATAL: failed to allocate BLE RX buffer");
  }
  BLEDevice::setMTU(512);
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService *pService = pServer->createService(SERVICE_UUID);
  pTxCharacteristic = pService->createCharacteristic(
                        CHARACTERISTIC_UUID_TX,
                        BLECharacteristic::PROPERTY_NOTIFY
                      );
  pTxCharacteristic->addDescriptor(new BLE2902());
  pTxCharacteristic->setCallbacks(new TxLogCallbacks());

  BLECharacteristic *pRxCharacteristic = pService->createCharacteristic(
                                           CHARACTERISTIC_UUID_RX,
                                           BLECharacteristic::PROPERTY_WRITE |
                                           BLECharacteristic::PROPERTY_WRITE_NR
                                         );
  pRxCharacteristic->setCallbacks(new MyCallbacks());

  pService->start();

  pServer->getAdvertising()->addServiceUUID(SERVICE_UUID);
  pServer->getAdvertising()->setScanResponse(true);
  pServer->getAdvertising()->setMinPreferred(0x06);
  pServer->getAdvertising()->setMaxPreferred(0x12);
  BLEDevice::startAdvertising();
  Serial.println("BLE Started Advertising");

  drawRunUI();
  Serial.println("Draupnir M5/M6 Ready");
}

bool waitRelease = false;

void loop() {
  M5Dial.update();
  
  if (trellisFound) {
    trellis.read();
  }
  
  if (currentMode == RUN_MODE) {
    server.handleClient();
    updateMacros();
    
    if (WiFi.status() == WL_CONNECTED && !mdnsStarted) {
      if (MDNS.begin("draupnir")) {
        Serial.println("MDNS started: draupnir.local");
        MDNS.addService("http", "tcp", 80);
      }
      Serial.print("Wi-Fi connected! IP: ");
      Serial.println(WiFi.localIP());
      mdnsStarted = true;
    }
    
    long newPosRaw = M5Dial.Encoder.read();
    long newPos = newPosRaw / 4;
    if (newPos != oldPosition) {
      if (inRotaryMode) {
        JsonArray actions = activeRotaryMacro["actions"];
        if (newPos > oldPosition) {
          if (actions.size() > 0) executeAction(actions[0]);
          oldPosition++;
        } else {
          if (actions.size() > 1) executeAction(actions[1]);
          oldPosition--;
        }
      } else {
        M5Dial.Speaker.tone(1000, 10);
        selectedMacroIdx = (newPos % 16);
        if (selectedMacroIdx < 0) selectedMacroIdx += 16;
        oldPosition = newPos;
        requestRedraw();
      }
    }
    
    if (uiNeedsRedraw && millis() - lastRedrawTime > 50) {
      drawRunUI();
      uiNeedsRedraw = false;
      lastRedrawTime = millis();
    }
    
    if (M5Dial.BtnA.wasReleased()) {
      if (inRotaryMode) {
        inRotaryMode = false;
        M5Dial.Speaker.tone(2000, 30);
        requestRedraw();
      } else {
        Serial.println("Knob pressed. Firing macro...");
        M5Dial.Speaker.tone(4000, 30);
        
        JsonArray profiles = profilesDoc["profiles"];
        JsonObject prof = profiles[activeProfileIdx];
        JsonArray macros = prof["macros"];
        
        for (JsonObject m : macros) {
          if (m["pos"] == selectedMacroIdx) {
            fireMacro(m, selectedMacroIdx);
            break;
          }
        }
      }
    }
    
    if (M5Dial.BtnA.wasHold()) {
      M5Dial.Speaker.tone(2000, 100);
      waitRelease = true;
      enterWiFiSetupMode();
    }
    
    auto touch = M5Dial.Touch.getDetail();
    if (touch.wasReleased()) {
      bool changed = false;
      JsonArray profiles = profilesDoc["profiles"];
      int numProfiles = profiles.size();
      
      if (touch.distanceY() < -40 && abs(touch.distanceX()) < 30) {
        // Swipe Up -> Kill All
        M5Dial.Speaker.tone(1000, 50);
        delay(50);
        M5Dial.Speaker.tone(800, 50);
        killAllMacros();
      } else if (touch.distanceY() > 40 && abs(touch.distanceX()) < 30) {
        // Swipe Down -> Enter Config Mode
        M5Dial.Speaker.tone(1500, 50);
        enterConfigMode();
      } else if (abs(touch.distanceX()) < 10 && touch.y > 60 && touch.y < 180) { 
        if (inRotaryMode && touch.x >= 80 && touch.x <= 160 && touch.y >= 80 && touch.y <= 160) {
          inRotaryMode = false;
          M5Dial.Speaker.tone(2000, 30);
          requestRedraw();
        } else if (!inRotaryMode) {
          if (touch.x < 80 && activeProfileIdx > 0) {
            activeProfileIdx--;
            changed = true;
          } else if (touch.x > 160 && activeProfileIdx < numProfiles - 1) {
            activeProfileIdx++;
            changed = true;
          } else if (touch.x >= 80 && touch.x <= 160 && touch.y >= 80 && touch.y <= 160) {
            Serial.println("Screen tapped. Firing macro...");
            M5Dial.Speaker.tone(4000, 30);
            
            JsonObject prof = profiles[activeProfileIdx];
            JsonArray macros = prof["macros"];
            
            for (JsonObject m : macros) {
              if (m["pos"] == selectedMacroIdx) {
                fireMacro(m, selectedMacroIdx);
                break;
              }
            }
          }
        }
      } else if (touch.distanceX() > 40 && activeProfileIdx > 0 && !inRotaryMode) { 
        activeProfileIdx--;
        changed = true;
      } else if (touch.distanceX() < -40 && activeProfileIdx < numProfiles - 1 && !inRotaryMode) { 
        activeProfileIdx++;
        changed = true;
      }
      
      if (changed) {
        prefs.putInt("activeProfile", activeProfileIdx);
        M5Dial.Speaker.tone(3000, 30);
        killAllMacros(); // Kill macros on profile change just to be safe
        requestRedraw();
      }
    }
  } else if (currentMode == CONFIG_MODE) {
    server.handleClient();
    auto touch = M5Dial.Touch.getDetail();
    if (touch.wasReleased() || M5Dial.BtnA.wasReleased()) {
      currentMode = RUN_MODE;
      M5Dial.Speaker.tone(2000, 30);
      requestRedraw();
    }
  } else if (currentMode == WIFI_SETUP_MODE) {
    server.handleClient();
    
    if (waitRelease && M5Dial.BtnA.isReleased()) {
      waitRelease = false;
    }
    
    if (!waitRelease && M5Dial.BtnA.wasPressed()) {
      ESP.restart();
    }
  }
  
  // Dispatch queued BLE command (safe to call notify from main loop)
  char *rxPtr = nullptr;
  if (bleRxQueue != nullptr && xQueueReceive(bleRxQueue, &rxPtr, 0) == pdTRUE) {
    Serial.println("Main loop: dispatching BLE command");
    if (rxPtr != nullptr) {
      handleBleCommand(rxPtr); // zero-copy parse; rxPtr must outlive the call
      free(rxPtr);
    }
  }

  // BLE reconnection handling
  if (!deviceConnected && oldDeviceConnected) {
    // Restore WiFi/web access for Config Mode now that BLE no longer needs exclusive radio time.
    // Routes are already registered from setup()'s setupWebServer() call — just restart the
    // listener, don't re-register them.
    WiFi.mode(WIFI_STA);
    WiFi.begin();
    server.begin();
    delay(500); // give the bluetooth stack the chance to get ready
    BLEDevice::startAdvertising(); // restart advertising (pServer->startAdvertising silently fails on ESP32)
    Serial.println("Restart BLE advertising");
    oldDeviceConnected = deviceConnected;
  }
  if (deviceConnected && !oldDeviceConnected) {
    // WiFi STA + the web server aren't needed at the same time as a BLE session in practice — and
    // WiFi/BLE radio coexistence was the leading suspect for large BLE notify chunks being
    // silently dropped (verified: tiny ~20B chunks were reliable, 500B chunks weren't). Freeing
    // the radio for BLE's exclusive use while a client is connected removes that contention.
    server.stop();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    oldDeviceConnected = deviceConnected;
  }
  
  delay(5);
}
