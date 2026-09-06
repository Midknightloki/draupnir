#include "M5Dial.h"
#include <LittleFS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include "USB.h"
#include "USBHIDKeyboard.h"
#include "USBHIDConsumerControl.h"
#include "USBHIDMouse.h"
#include <Adafruit_NeoTrellis.h>
#include "icons.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include "freertos/queue.h"
#include "freertos/semphr.h"

// These guards exist because this API's signature failure mode is compiling cleanly and
// enforcing nothing. BLECharacteristic.h:162-194 defines PROPERTY_WRITE_ENC / _AUTHEN as
// literally 0 under #if defined(CONFIG_BLUEDROID_ENABLED), and as the real BLE_GATT_CHR_F_*
// bits only under #if defined(CONFIG_NIMBLE_ENABLED). Build against the wrong one and the
// permission flags in setup() become a no-op that no test short of a hostile central would
// catch. Fail the build instead.
#if !defined(CONFIG_NIMBLE_ENABLED) && !defined(CONFIG_BT_NIMBLE_ENABLED)
#error "Expected a NimBLE-backed core. Under Bluedroid the GATT permission flags in setup() are 0 and enforce nothing."
#endif
static_assert(BLECharacteristic::PROPERTY_WRITE_ENC != 0,
              "PROPERTY_WRITE_ENC is 0 -- the RX permission flags would be a silent no-op.");
static_assert(BLECharacteristic::PROPERTY_WRITE_AUTHEN != 0,
              "PROPERTY_WRITE_AUTHEN is 0 -- the RX permission flags would be a silent no-op.");

#define SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"
// Flow-control ack the app writes to RX after processing each TX chunk: 2 bytes,
// [BLE_CHUNK_ACK_MARKER, seq]. seq echoes the chunk's sequence byte (prefixed onto every TX
// chunk) so a late ack from a chunk we already gave up retrying can't be mistaken for the
// current one. 0xFE can't collide with a real JSON command chunk (those are ASCII/UTF-8, <0x80).
#define BLE_CHUNK_ACK_MARKER 0xFE

// WIFI_SETUP_MODE is gone with the rest of the Wi-Fi surface (spec v3 §1 cuts the web config
// path permanently). CONFIG_MODE survives as an informational screen, not a gate -- see the
// note above handleBleCommand.
enum AppMode { RUN_MODE, CONFIG_MODE };
AppMode currentMode = RUN_MODE;
// The bespoke `pairingToken` / `pair` scheme is removed (spec v3 §7, M6/H1). It reimplemented —
// badly — what BLE bonding already does correctly, and the companion app no longer sends a
// token on any request. Access to the config surfaces is gated on CONFIG_MODE (a physical
// gesture on the device) only.
//
// NOTE FOR WHOEVER PICKS THIS UP: this board's BLE stack, unlike the Waveshare firmware's, does
// NOT yet set up BLE pairing/bonding or GATT permission flags, so the M5Dial BLE config channel
// has no cryptographic access control at all. Close it by porting the BLESecurity block and the
// RX/TX characteristic permission flags from firmware/Waveshare_LVGL_Test/ble_engine.cpp. It is
// the top item of the M5Dial catch-up (docs/HANDOFF.md §7); deferred only because the board
// targets are worked in sequence -- Waveshare through M10 first (spec §3, CLAUDE.md). The
// hardware is on hand; this is a scheduling decision, not a tooling limitation.
//
// DO NOT "FIX" THIS BY RESTORING THE TOKEN. An external audit (2026-08) read the removal as the
// cause of the gap and called it a blocking regression. It is not -- the token made things
// strictly worse. Compare the gate before and after (baseline is origin/main:584):
//
//   Before:  if (currentMode != CONFIG_MODE && (token != pairingToken || !pairingToken.length()))
//   After:   if (currentMode != CONFIG_MODE)
//
// The token was only ever checked OUTSIDE CONFIG_MODE, and CONFIG_MODE is the only mode that
// serves config commands -- so in the mode that mattered there was never a check. Accepted
// requests went from {CONFIG_MODE: anything} U {RUN_MODE: with token} to {CONFIG_MODE: anything},
// a strict subset. Worse, any central could send {"cmd":"pair"} while in CONFIG_MODE and be
// handed the token, which persisted to NVS and reloaded at boot -- turning one moment of physical
// access into permanent remote config access in RUN_MODE. That is why setup() now actively calls
// prefs.remove("pairingToken"). Restoring it would reopen a hole, not close one.

BLEServer *pServer = nullptr;
BLECharacteristic *pTxCharacteristic = nullptr;
bool deviceConnected = false;
bool oldDeviceConnected = false;
// Set on the BLE host task, read from loop(). volatile, and never touched by anything that
// draws -- the display belongs to the loop() task (see the pairing screen dispatch in loop()).
static volatile bool pairingActive = false;
static volatile uint32_t currentPasskey = 0;
// BLE_HS_CONN_HANDLE_NONE (0xffff, host/ble_hs.h) when nothing is connected.
static volatile uint16_t bleConnHandle = BLE_HS_CONN_HANDLE_NONE;

void drawPairingScreen(); // defined further down, next to enterConfigMode()
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

JsonDocument profilesDoc;
int activeProfileIdx = 0;
int selectedMacroIdx = 0;
long oldPosition = -999;
bool uiNeedsRedraw = false;
unsigned long lastRedrawTime = 0;

const char* defaultProfilesJson = R"=====(
{
  "version": 3,
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

// M8b: pos is a stable identifier and ring-ordering key, NOT an index. See
// docs/Draupnir_Spec.md section 6 and the Waveshare's macro_engine.cpp, which took the same
// change -- the macro engine is shared-layer, only the display differs.
//
// MAX_MACROS is the data-model ceiling (highest legal pos is 31). RUNNING_SLOTS is a RAM limit:
// how many macros can play at once. They were the same number only because one array served both
// jobs.
#define MAX_MACROS     32
#define RUNNING_SLOTS  16
#define SCHEMA_VERSION 3

struct ActiveMacro {
  bool active = false;
  bool isToggle = false;
  int pos = -1;                 // WHICH macro this slot plays -- the identity, not an index
  JsonObject macroDef;
  int currentActionIndex = 0;
  unsigned long nextActionTime = 0;
  uint16_t color = 0;
};

// A POOL. The array index means nothing -- slot 3 is not pos 3. `.pos` is the identity and every
// query goes through findRunningSlot(). Before M8b this was indexed BY pos, which is what
// actually capped macros at 16.
ActiveMacro runningMacros[RUNNING_SLOTS];

// Index of the slot playing `pos`, or -1.
int findRunningSlot(int pos) {
  for (int i = 0; i < RUNNING_SLOTS; i++) {
    if (runningMacros[i].active && runningMacros[i].pos == pos) return i;
  }
  return -1;
}

// Index of a free slot, or -1 when all RUNNING_SLOTS are busy.
int claimFreeSlot() {
  for (int i = 0; i < RUNNING_SLOTS; i++) {
    if (!runningMacros[i].active) return i;
  }
  return -1;
}

// True if this firmware understands the document's declared schema version. Refuses only versions
// ABOVE SCHEMA_VERSION -- v3 is a strict relaxation of v2, so every v2 file is a valid v3 file,
// and a missing `version` is legacy rather than an error.
bool schemaVersionOk(JsonVariantConst doc) {
  JsonVariantConst v = doc["version"];
  if (v.isNull()) return true;
  int ver = v | 0;
  return ver <= SCHEMA_VERSION;
}

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
  
  // THIS 16 IS DELIBERATE AND STAYS. This ring is 16 FIXED dots -- one per position around the
  // circle -- not the Waveshare's N-wedges-sized-to-fill. M8b uncapped the ENGINE on this board,
  // not the ring: a macro above pos 15 stores, loads, fires and stops correctly here, is
  // triggerable from the app and over BLE, but has no dot to be drawn in and cannot be selected
  // on this screen.
  //
  // That is an accepted, documented divergence between the two supported boards (see
  // docs/Draupnir_Spec.md section 4 of the M8b design). Closing it means porting the dynamic
  // wedge ring onto M5GFX at 240x240 with no LVGL, which is most of the deferred M5Dial catch-up
  // and belongs with the security gate -- not here. Do not "fix" this loop in isolation.
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
    
    // `i` here is a RING POSITION (this ring is 16 fixed dots), so ask by pos rather than
    // indexing the pool -- those coincided only while runningMacros[] was indexed by pos.
    if (findRunningSlot(i) >= 0) {
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
  
  // Draw Kill All if any macro is running. Scans the POOL, so it still sees a macro above pos 15
  // even though this ring cannot draw one.
  bool anyRunning = false;
  for (int i = 0; i < RUNNING_SLOTS; i++) {
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
  } else if (!schemaVersionOk(profilesDoc)) {
    // A file from a NEWER firmware or client. Refuse rather than load a document whose meaning we
    // do not know -- silently dropping whatever we fail to understand is the exact failure the
    // version field exists to prevent.
    Serial.printf("profiles.json declares version %d, this firmware understands %d -- refusing\n",
                  (int)(profilesDoc["version"] | 0), SCHEMA_VERSION);
    profilesDoc.clear();
    deserializeJson(profilesDoc, defaultProfilesJson);
    activeProfileIdx = 0;
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

void fireMacro(JsonObject macro, int pos) {
  const char* mode = macro["mode"] | "play_once";

  if (strcmp(mode, "rotary") == 0) {
    inRotaryMode = true;
    activeRotaryMacro = macro;
    requestRedraw();
    return;
  }

  if (pos < 0 || pos >= MAX_MACROS) {
    Serial.printf("fireMacro: pos=%d out of range\n", pos);
    return;
  }

  bool isToggle = (strcmp(mode, "toggle") == 0);

  int slot = findRunningSlot(pos);
  if (slot >= 0 && runningMacros[slot].isToggle) {
    // Kill toggle macro
    runningMacros[slot].active = false;
    runningMacros[slot].pos = -1;
    runningMacros[slot].macroDef = JsonObject();
    // The NeoTrellis LED is addressed by KEY index, which is only the same as pos for the 16
    // physical keys -- the pad genuinely has 16. A macro above that has no LED to clear.
    if (trellisFound && pos < 16) {
      trellis.pixels.setPixelColor(pos, 0);
      trellis.pixels.show();
    }
    // Redraw UI to possibly remove Kill All button
    requestRedraw();
    return;
  }

  // Re-firing a non-toggle macro already playing reuses its slot rather than claiming a second;
  // two slots on one pos would make findRunningSlot() ambiguous.
  if (slot < 0) slot = claimFreeSlot();
  if (slot < 0) {
    // Refuse rather than evict -- an eviction can strand held modifiers, and killAllMacros() is
    // the only path that releases HID state.
    Serial.printf("fireMacro: pos=%d refused, all %d running slots busy\n", pos, RUNNING_SLOTS);
    return;
  }

  runningMacros[slot].active = true;
  runningMacros[slot].isToggle = isToggle;
  runningMacros[slot].pos = pos;
  runningMacros[slot].macroDef = macro;
  runningMacros[slot].currentActionIndex = 0;
  runningMacros[slot].nextActionTime = millis();

  const char* colorHex = macro["color"] | "#FFFFFF";
  long rgb = strtol(colorHex + 1, nullptr, 16);
  runningMacros[slot].color = rgb;

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
        // Lowercase a single alphabetic key so that case can never inject a modifier.
        // Arduino's Keyboard maps ASCII through _asciimap, where 'L' means Shift+KEY_L --
        // so mods:["WIN"] + key:"L" silently became Win+Shift+L and Windows ignored it.
        // Shift must come from an explicit mods entry and nowhere else.
        //
        // ONLY alphabetic characters are folded. Punctuation like "!" or "?" legitimately
        // needs the shifted asciimap entry, since there is no unshifted keycode for them.
        char c = keyStr[0];
        if (strlen(keyStr) == 1 && c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        Keyboard.press(c);
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
  for (int i = 0; i < RUNNING_SLOTS; i++) {
    if (runningMacros[i].active) {
      if (now >= runningMacros[i].nextActionTime) {
        JsonArray actions = runningMacros[i].macroDef["actions"];
        if (runningMacros[i].currentActionIndex >= actions.size()) {
          if (runningMacros[i].isToggle) {
            runningMacros[i].currentActionIndex = 0;
          } else {
            int donePos = runningMacros[i].pos;
            // Release the slot back to the pool: identity and JsonObject too, not just the flag.
            runningMacros[i].active = false;
            runningMacros[i].pos = -1;
            runningMacros[i].macroDef = JsonObject();
            // LED is addressed by KEY index; the pad has 16 physical keys.
            if (trellisFound && donePos >= 0 && donePos < 16) {
              trellis.pixels.setPixelColor(donePos, 0);
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
  for (int i = 0; i < RUNNING_SLOTS; i++) {
    if (runningMacros[i].active) {
      int pos = runningMacros[i].pos;
      runningMacros[i].active = false;
      runningMacros[i].pos = -1;
      runningMacros[i].macroDef = JsonObject();  // drop the reference into the document pool
      changed = true;
      // The LED index is the KEY index, not the slot index -- those coincided only while this
      // array was indexed by pos. The pad has 16 physical keys; a macro above that has no LED.
      if (trellisFound && pos >= 0 && pos < 16) trellis.pixels.setPixelColor(pos, 0);
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
// delivered 160+ consecutive chunks with zero retries. WiFi/BLE coexistence was the leading
// suspect: this firmware used to run WiFi STA + a WebServer alongside BLE, and WiFi was paused
// for the duration of every BLE connection to work around it. WiFi is now deleted outright, so
// the radio is never shared and the contention cannot recur. 100B is a middle ground between
// that proven-reliable size and full throughput; re-measure if drops come back.
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

// Looks up a macro by (profileIdx, pos) in the currently-loaded profilesDoc -- i.e. the stored
// document still in memory, not yet overwritten by an incoming save. Used only by the
// save_profiles icon_xbm merge below, which must cross-reference every profile in the INCOMING
// document against what's already on the device, not just the active/on-screen profile. pos is
// only unique within a profile, hence the separate profileIdx. Returns a null JsonObject (check
// with .isNull()) if profileIdx or pos don't match anything.
static JsonObject findStoredMacro(int profileIdx, int pos) {
  JsonArray profiles = profilesDoc["profiles"];
  if (profiles.isNull() || profileIdx < 0 || profileIdx >= (int)profiles.size()) return JsonObject();
  JsonObject prof = profiles[profileIdx];
  JsonArray macros = prof["macros"];
  for (JsonObject m : macros) {
    int p = m["pos"] | -1;
    if (p == pos) return m;
  }
  return JsonObject();
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
  // The `pair` command and the per-request `token` check are removed (spec v3 §7, M6/H1) -- see
  // the note at the top of this file. The CONFIG_MODE check that stood here is removed too: it
  // was a physical-presence gate, not authentication, and it stopped nothing once the user
  // swiped down. An encrypted, MITM-authenticated link is the authorization now. CONFIG_MODE
  // survives as an informational screen.
  //
  // Belt-and-braces behind the GATT permission flags on RX (see setup()). If those ever fail to
  // enforce -- the documented failure mode of this API -- this refuses the command anyway and
  // says so on Serial, rather than executing it quietly. Runs on the loop task, reading a handle
  // the BLE task published: the hand-off-via-flag pattern the threading rules require.
  ble_gap_conn_desc desc;
  if (bleConnHandle == BLE_HS_CONN_HANDLE_NONE ||
      ble_gap_conn_find(bleConnHandle, &desc) != 0 ||
      !desc.sec_state.encrypted || !desc.sec_state.authenticated) {
    Serial.println("[ble] cmd REFUSED: link not encrypted+authenticated");
    sendBleMessage("{\"status\":\"error\",\"message\":\"Not paired\"}");
    return;
  }
  Serial.printf("[ble] cmd '%s' accepted (enc=%d auth=%d bond=%d)\n", cmd.c_str(),
                desc.sec_state.encrypted, desc.sec_state.authenticated, desc.sec_state.bonded);

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

    // Refuse a newer-schema document before anything touches flash, and TELL the app -- a save
    // that silently does nothing is the failure this check exists to remove. Note this board's
    // write is still a direct truncate-and-write rather than the Waveshare's temp/verify/rename,
    // so returning early here matters more: there is no atomic commit to abort further down.
    if (!schemaVersionOk(profilesObj)) {
      int ver = profilesObj["version"] | 0;
      Serial.printf("save_profiles: REFUSED, document declares version %d, firmware understands %d\n",
                    ver, SCHEMA_VERSION);
      char msg[128];
      snprintf(msg, sizeof(msg),
               "{\"status\":\"error\",\"message\":\"Schema version %d not supported (max %d) -- update the firmware\"}",
               ver, SCHEMA_VERSION);
      sendBleMessage(msg);
      return;
    }

    // get_profiles strips icon_xbm on the way out (see ICON_XBM_MARKER above), so the app's
    // in-memory document never holds bitmaps at all -- and saveProfiles() in the app sends its
    // WHOLE in-memory document back, only ever writing icon_xbm for the macro actually being
    // edited (companion_app/lib/screens/editor_panel.dart:106). Stripping without this merge
    // means saving after editing one macro silently wipes every other macro's icon -- a live
    // data-loss bug on this board today. Mirrors the Waveshare's fix (f53206f,
    // firmware/Waveshare_LVGL_Test/ble_engine.cpp / profiles_find_macro_in()); the JSON plumbing
    // differs -- this board keeps the still-stored profilesDoc around as a plain global to check
    // against, rather than a dedicated lookup exported from a separate macro-engine module -- but
    // the observable behaviour must match.
    //
    // Runs on the in-memory profilesObj document BEFORE serialization, ahead of the write below,
    // so it composes with the existing write path instead of restructuring it.
    {
      JsonArray incomingProfiles = profilesObj["profiles"];
      int profileIdx = 0;
      for (JsonObject incomingProfile : incomingProfiles) {
        JsonArray incomingMacros = incomingProfile["macros"];
        for (JsonObject incomingMacro : incomingMacros) {
          // An incoming icon_xbm always wins -- never overwrite a bitmap the client actually
          // sent. isNull() (absent) is checked, not emptiness (present-but-empty is left alone).
          if (!incomingMacro["icon_xbm"].isNull()) continue;
          int pos = incomingMacro["pos"] | -1;
          if (pos < 0) continue;
          // Match by pos within the profile, NOT array index -- the app may reorder macros, and
          // index matching would transplant one macro's bitmap onto another. Profiles themselves
          // are matched by index (profileIdx), since pos is only unique within a profile. No
          // stored counterpart at this pos in this profile means a genuinely new macro -- that
          // has no bitmap to inherit, which is correct, not an error to log or abort on.
          JsonObject storedMacro = findStoredMacro(profileIdx, pos);
          const char *storedIcon = storedMacro["icon_xbm"] | (const char *)nullptr;
          if (storedIcon != nullptr) {
            // Copy via String rather than aliasing the const char* -- profilesObj was
            // deserialized zero-copy from cmdStr (see handleBleCommand's parse above), and
            // profilesDoc (where storedIcon points) is about to be replaced by loadProfiles()
            // below, so an unowned pointer into it would be a lifetime trap for whoever touches
            // this next even though it happens to outlive the serialize a few lines down.
            incomingMacro["icon_xbm"] = String(storedIcon);
          }
        }
        profileIdx++;
      }
    }

    // Write-temp-then-rename. Opening /profiles.json with "w" directly truncated the only good
    // copy before a single byte of the new one was written, and the serializeJson() byte count
    // was never checked -- a power loss, a full filesystem, or a short write left a truncated,
    // unparseable config with no way back short of a reflash. Nothing here touches
    // /profiles.json until the temp file has been written, closed, and verified on disk.
    static const char *PROFILES_PATH = "/profiles.json";
    static const char *PROFILES_TMP_PATH = "/profiles.json.tmp";

    LittleFS.remove(PROFILES_TMP_PATH); // clear any leftover from a previous failed save
    File f = LittleFS.open(PROFILES_TMP_PATH, "w");
    if (!f) {
      sendBleMessage("{\"status\":\"error\",\"message\":\"Failed to open temp file\"}");
      return;
    }
    size_t expected = measureJson(profilesObj);
    size_t written = serializeJson(profilesObj, f);
    f.close();

    // Re-open to confirm close() actually flushed the full document to flash, rather than
    // trusting the writer's own byte count.
    size_t onDisk = 0;
    File verify = LittleFS.open(PROFILES_TMP_PATH, "r");
    if (verify) {
      onDisk = verify.size();
      verify.close();
    }

    if (expected == 0 || written != expected || onDisk != expected) {
      Serial.printf("[ble] save_profiles: write failed (expected=%u written=%u onDisk=%u)\n",
                    (unsigned)expected, (unsigned)written, (unsigned)onDisk);
      LittleFS.remove(PROFILES_TMP_PATH);
      // The original /profiles.json is untouched, so do NOT reload -- the in-memory document
      // still matches what is on flash.
      sendBleMessage("{\"status\":\"error\",\"message\":\"Failed to write file\"}");
      return;
    }

    if (!LittleFS.rename(PROFILES_TMP_PATH, PROFILES_PATH)) {
      // Some LittleFS/VFS builds refuse to rename onto an existing file. Only now, with a
      // verified-good temp file in hand, is it safe to drop the original.
      LittleFS.remove(PROFILES_PATH);
      if (!LittleFS.rename(PROFILES_TMP_PATH, PROFILES_PATH)) {
        Serial.println("[ble] save_profiles: rename into place failed");
        LittleFS.remove(PROFILES_TMP_PATH);
        // The device now has no config file. loadProfiles() finds none and regenerates defaults
        // -- the correct recovery, though still a failure from the caller's point of view.
        killAllMacros();
        loadProfiles();
        sendBleMessage("{\"status\":\"error\",\"message\":\"Failed to commit file\"}");
        return;
      }
    }

    Serial.printf("[ble] save_profiles: committed %u bytes\n", (unsigned)onDisk);

    // killAllMacros() BEFORE loadProfiles() is not optional. The macro engine holds JsonObject
    // references into profilesDoc, and deserializeJson() clears and reallocates that document's
    // pool -- reloading with a macro mid-sequence is a use-after-free.
    //
    // Unlike the Waveshare (which defers this via a profilesDirty flag), the M5Dial reloads
    // immediately and correctly: nothing but loop() ever touches profilesDoc on this board,
    // because it draws from loop() rather than from a separate LVGL task.
    killAllMacros();
    loadProfiles();

    int brightness = profilesDoc["settings"]["brightness"] | 160;
    M5Dial.Display.setBrightness(brightness);

    int orientation = profilesDoc["settings"]["orientation"] | 0;
    M5Dial.Display.setRotation(orientation);

    requestRedraw();
    sendBleMessage("{\"status\":\"ok\"}");
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

// IO_CAP_OUT: this device can only DISPLAY a passkey, not accept input -- the phone/app side
// enters what we show here. Regenerated per-connection (regenPassKeyOnConnect) so it's a fresh
// random code each pairing, not a fixed shared secret.
class SecurityCallbacks : public BLESecurityCallbacks {
  uint32_t onPassKeyRequest() override { return 0; } // we never have input capability
  void onPassKeyNotify(uint32_t pass_key) override {
    // Runs on the BLE host task. Sets flags ONLY -- drawing to M5Dial.Display from here would
    // race the loop() task that owns the panel. loop() watches pairingActive and paints.
    currentPasskey = pass_key;
    pairingActive = true;
    Serial.printf("[ble] show passkey: %06u\n", (unsigned)pass_key);
  }
  bool onSecurityRequest() override { return true; }
  bool onConfirmPIN(uint32_t pin) override { return true; }
  // This core (m5stack:esp32 3.3.8) is NimBLE-backed despite the Bluedroid-styled class names,
  // confirmed in its sdkconfig.h. BLESecurity.h declares BOTH overloads -- esp_ble_auth_cmpl_t
  // (line 228) and ble_gap_conn_desc* (line 238). Override the latter; the former is never
  // called here, so overriding it would look correct and never fire.
  void onAuthenticationComplete(ble_gap_conn_desc *desc) override {
    pairingActive = false;
    Serial.printf("[ble] authentication complete, encrypted=%d authenticated=%d bonded=%d\n",
                  desc->sec_state.encrypted, desc->sec_state.authenticated, desc->sec_state.bonded);
  }
};

class MyServerCallbacks: public BLEServerCallbacks {
    // NimBLE overload (BLEServer.h:306) -- gives us the connection handle, so we can request
    // security immediately and so handleBleCommand() can look the link's security state up later.
    void onConnect(BLEServer* pServer, ble_gap_conn_desc* desc) override {
      deviceConnected = true;
      bleConnHandle = desc->conn_handle;
      int rc = 0;
      bool started = BLESecurity::startSecurity(desc->conn_handle, &rc);
      // startSecurity is only a REQUEST, which a hostile central is free to ignore. The GATT
      // permission flags on RX/TX are what actually enforce; this just gets a well-behaved
      // client prompted promptly instead of on its first rejected write.
      //
      // Keep this callback cheap. It runs on the BLE stack's own task, and the last time real
      // work was done here -- reconfiguring the WiFi driver, back when WiFi existed -- it
      // corrupted the get_profiles response the main loop was concurrently building (verified:
      // a reproducible empty-JSON response appeared the moment this callback called WiFi.mode()
      // directly, and moving the work to a loop-driven toggle fixed it). WiFi is gone, but the
      // rule it taught is not: hand work off to loop(), don't do it here.
      Serial.printf("BLE Client Connected, conn_handle=%d startSecurity ok=%d rc=%d connectedCount=%d\n",
                    desc->conn_handle, started, rc, pServer->getConnectedCount());
    };

    void onDisconnect(BLEServer* pServer, ble_gap_conn_desc* desc) override {
      deviceConnected = false;
      pairingActive = false;
      bleConnHandle = BLE_HS_CONN_HANDLE_NONE;
      // A half-received command from a dropped connection must not poison the next one.
      bleRxLen = 0;
      Serial.print("BLE Client Disconnected, connectedCount=");
      Serial.print(pServer->getConnectedCount());
      Serial.print(" peerDevices=");
      Serial.println(pServer->getPeerDevices(false).size());
    }
};

// Window for the duplicate-write guard below. The app paces its chunks with an explicit 20ms
// delay AND waits for each write's ATT response, so two genuinely distinct chunks cannot arrive
// closer together than tens of milliseconds. That pacing is app-side, so it bounds the gap here
// regardless of this board's connection interval (unlike the Waveshare, this firmware does not
// call updateConnParams). A stack-level duplicate of a single write arrives within a millisecond
// or two, so 10ms separates the two cases with a wide margin.
static const uint32_t BLE_RX_DUP_WINDOW_MS = 10;

class MyCallbacks: public BLECharacteristicCallbacks {
    String lastRxValue;
    uint32_t lastRxValueMs = 0;
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
      //
      // THE WINDOW IS LOAD-BEARING, and its absence was a real, shipped bug. Without it this
      // guard never expires, so it compares against the previous write forever -- and the app's
      // first command after reconnecting is byte-identical to its first command last session
      // ({"cmd":"get_profiles"}). The result was that the device bonded and reconnected perfectly
      // and then silently swallowed every command, which presented as "cannot reconnect, and
      // restarting the app does not help" (the stale state is here, on the device, so only a
      // reboot cleared it). Captured on hardware 2026-09-06: connect, encrypted=1 authenticated=1
      // bonded=1, then "duplicate write ignored" and handleBleCommand never ran.
      //
      // Note the trap for anyone porting between the boards: ble_engine.cpp's onDisconnect says
      // lastRxValue "needs no reset ... so it self-expires". That is true THERE because the
      // Waveshare has this window. It was not true here, because this board did not.
      //
      // With the window, no reset on disconnect is needed: a reconnect takes seconds, thousands
      // of times longer than 10ms, so the guard has long since expired.
      uint32_t nowMs = millis();
      if (rxValue == lastRxValue && (nowMs - lastRxValueMs) < BLE_RX_DUP_WINDOW_MS) {
        Serial.printf("BLE RX: duplicate write ignored (%u bytes, %ums apart)\n",
                      (unsigned)rxValue.length(), (unsigned)(nowMs - lastRxValueMs));
        return;
      }
      lastRxValue = rxValue;
      lastRxValueMs = nowMs;

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

// Painted from loop() on the pairingActive false->true edge. Never called from a BLE callback:
// SecurityCallbacks::onPassKeyNotify runs on the BLE host task and only sets flags, because the
// display belongs to the loop() task.
void drawPairingScreen() {
  auto& d = M5Dial.Display;
  d.fillScreen(TFT_BLACK);
  d.setTextDatum(middle_center);
  d.setFont(&fonts::Orbitron_Light_24);
  d.setTextColor(TFT_CYAN, TFT_BLACK);
  d.drawString("PAIRING", 120, 60);

  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.drawString("Enter PIN:", 120, 110);
  char buf[8];
  snprintf(buf, sizeof(buf), "%06u", (unsigned)currentPasskey);
  d.drawString(buf, 120, 150);
}

// Split out from enterConfigMode() so the pairing screen can repaint over itself when pairing
// ends while the dial is sitting in Config Mode. requestRedraw() is only honoured by the
// RUN_MODE branch of loop(), so without this the PAIRING screen stayed up until the next tap.
// Drawing only -- no mode change, and crucially no killAllMacros().
void drawConfigModeScreen() {
  auto& d = M5Dial.Display;
  d.fillScreen(TFT_BLACK);
  d.setTextDatum(middle_center);
  d.setFont(&fonts::Orbitron_Light_24);
  d.setTextColor(TFT_GREEN, TFT_BLACK);
  d.drawString("CONFIG MODE", 120, 60);
  
  d.setFont(&fonts::Orbitron_Light_24);
  // Was the Wi-Fi IP address, back when this screen told you where to point a browser. BLE is
  // the only transport now, so the useful facts are which board this is and whether the app is
  // actually attached.
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.drawString("BLE", 120, 100);
  d.drawString("Draupnir_Mini", 120, 130);
  d.setTextColor(deviceConnected ? TFT_GREEN : TFT_DARKGRAY, TFT_BLACK);
  d.drawString(deviceConnected ? "app connected" : "waiting for app", 120, 160);

  d.setTextColor(TFT_DARKGRAY, TFT_BLACK);
  d.drawString("TAP TO EXIT", 120, 190);
}

void enterConfigMode() {
  currentMode = CONFIG_MODE;
  killAllMacros();
  drawConfigModeScreen();
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
  // Actively delete any token left in NVS by a pre-M6 build, so the key does not linger on
  // devices that have already been paired the old way.
  prefs.remove("pairingToken");

  loadProfiles();
  
  int brightness = profilesDoc["settings"]["brightness"] | 160;
  M5Dial.Display.setBrightness(brightness);
  
  int orientation = profilesDoc["settings"]["orientation"] | 0;
  M5Dial.Display.setRotation(orientation);
  
  // USB must start before BLE on ESP32-S3 — USB.begin() disrupts BLE if it runs after
  Keyboard.begin();
  ConsumerControl.begin();
  Mouse.begin();
  USB.begin();
  delay(200); // let USB settle before BLE init

  // Initialize BLE
  bleRxQueue = xQueueCreate(4, sizeof(char*));
  bleAckQueue = xQueueCreate(1, sizeof(uint8_t));
  // Advertised name. Deliberately NOT "Draupnir" — that is the Waveshare knob's name, and when
  // both boards were on the air the companion app connected to whichever answered the scan
  // first, with no way to tell them apart or to pick. The app matches any name containing
  // "draupnir" (case-insensitive) so this still discovers normally; it just gives the picker
  // something to distinguish. Changing this string breaks nothing else — the app never matches
  // on the exact name, and the service UUID is unchanged.
  BLEDevice::init("Draupnir_Mini");
  bleRxBuf = (char*)malloc(BLE_RX_BUFFER_SIZE);
  if (bleRxBuf == nullptr) {
    Serial.println("FATAL: failed to allocate BLE RX buffer");
  }
  BLEDevice::setMTU(512);

  BLESecurity::setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);
  BLESecurity::setCapability(ESP_IO_CAP_OUT);
  BLESecurity::setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  BLESecurity::setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  BLESecurity::setPassKey(false, 0); // false = randomly generated, not this fixed value
  // A passkey fixed for the board's uptime is a weaker secret than a per-pairing one, and with
  // bonding working the user only ever types it once anyway.
  //
  // ROLLBACK LADDER, in order, if unexplained resets into the ROM bootloader appear (this
  // happened on the Waveshare during M6/H1 -- see docs/M6_Hardening_WorkOrder.md):
  //   1. flip this to false,
  //   2. drop PROPERTY_WRITE_AUTHEN from the RX characteristic below,
  //   3. drop the ENC flags entirely -- which is this board's pre-M6 behaviour and is INSECURE.
  //      Do NOT stop at step 3 and call it done; report the boot reason instead.
  BLESecurity::regenPassKeyOnConnect(true);
  BLEDevice::setSecurityCallbacks(new SecurityCallbacks());

  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService *pService = pServer->createService(SERVICE_UUID);
  // ---------------------------------------------------------------------------------------
  // GATT permission enforcement. This is what the stack actually enforces; the
  // BLESecurity::startSecurity() call in MyServerCallbacks::onConnect() is only a request a
  // hostile central can ignore. Without these bits any central in radio range could write
  // save_profiles or trigger -- i.e. inject keystrokes into the attached host.
  //
  // READ THIS BEFORE CHANGING IT. The permission API here differs from every Bluedroid example
  // online. BLECharacteristic::setAccessPermissions() is a NO-OP on this core: its body is
  // wrapped in #ifdef CONFIG_BLUEDROID_ENABLED, and BLEService::start() builds
  // ble_gatt_chr_def.flags from m_properties, never from m_permissions. The enforcement bits
  // live in the PROPERTIES bitmask instead. ENC = "encrypted link"; AUTHEN additionally means
  // the key came from an MITM-protected pairing (our passkey display).
  //
  // CCCD: NimBLE creates the 0x2902 descriptor itself for any characteristic with NOTIFY, and
  // BLECharacteristic::addDescriptor() explicitly discards a manually-added BLE2902 on this core
  // (see its #ifdef CONFIG_NIMBLE_ENABLED early-return) -- so the old addDescriptor(new
  // BLE2902()) was dead code and is removed. The auto-created CCCD is protected via
  // BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC instead, which is what makes "subscribe to TX" require
  // an encrypted link.
  //
  // KNOWN GAP: BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHEN (0x10000) cannot be applied through this
  // wrapper -- BLECharacteristic.h:245 stores properties in esp_gatt_char_prop_t, a uint16_t,
  // so the bit is silently truncated. The CCCD is therefore gated on encryption but not
  // explicitly on authentication. That should not be exploitable here, because
  // setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND) means this device will not complete a
  // Just Works pairing at all, so any encrypted link is necessarily an authenticated one.
  //
  // On the Waveshare that last sentence was upgraded from inference to observation by a
  // hostile-central hardware test (nRF Connect, 2026-08-07). On THIS board it remains an
  // inference -- the negative test has NOT been run here. Do not copy the Waveshare's
  // "VERIFIED" wording across until someone actually runs it.
  //
  // It reopens the moment setAuthenticationMode() is relaxed away from *_MITM_*.
  pTxCharacteristic = pService->createCharacteristic(
                        CHARACTERISTIC_UUID_TX,
                        BLECharacteristic::PROPERTY_NOTIFY |
                        BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC
                      );
  pTxCharacteristic->setCallbacks(new TxLogCallbacks());

  BLECharacteristic *pRxCharacteristic = pService->createCharacteristic(
                                           CHARACTERISTIC_UUID_RX,
                                           BLECharacteristic::PROPERTY_WRITE |
                                           BLECharacteristic::PROPERTY_WRITE_NR |
                                           BLECharacteristic::PROPERTY_WRITE_ENC |
                                           BLECharacteristic::PROPERTY_WRITE_AUTHEN
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

void loop() {
  M5Dial.update();

  // Edge-triggered, not level-triggered: this display is painted imperatively, so repainting
  // every tick would flicker and never repainting would leave the passkey up forever.
  static bool wasPairing = false;
  bool isPairing = pairingActive;
  if (isPairing != wasPairing) {
    if (isPairing) {
      drawPairingScreen();
    } else if (currentMode == CONFIG_MODE) {
      // requestRedraw() would be ignored -- only the RUN_MODE branch below dispatches it.
      drawConfigModeScreen();
    } else {
      requestRedraw();
    }
    wasPairing = isPairing;
  }

  // Not while the passkey is up: trellis.read() dispatches trellisEvent(), which fires macros.
  if (trellisFound && !isPairing) {
    trellis.read();
  }

  if (currentMode == RUN_MODE) {
    updateMacros();

    // The pairing screen owns the display and the inputs while it is up. Someone reading a PIN
    // off the glass must not fire a macro into their host by brushing it. Macros already running
    // are still serviced above; the queued redraw is deliberately NOT dispatched in here either,
    // because drawRunUI() would paint straight over the passkey.
    if (!isPairing) {
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
    } // end !isPairing
  } else if (currentMode == CONFIG_MODE) {
    auto touch = M5Dial.Touch.getDetail();
    if (!isPairing && (touch.wasReleased() || M5Dial.BtnA.wasReleased())) {
      currentMode = RUN_MODE;
      M5Dial.Speaker.tone(2000, 30);
      requestRedraw();
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
    delay(500); // give the bluetooth stack the chance to get ready
    BLEDevice::startAdvertising(); // restart advertising (pServer->startAdvertising silently fails on ESP32)
    Serial.println("Restart BLE advertising");
    oldDeviceConnected = deviceConnected;
  }
  if (deviceConnected && !oldDeviceConnected) {
    // Was: server.stop(); WiFi.disconnect(true); WiFi.mode(WIFI_OFF). WiFi/BLE radio coexistence
    // was the leading suspect for large BLE notify chunks being silently dropped (verified: ~20B
    // chunks were reliable, 500B chunks weren't), so WiFi was paused for the duration of every
    // BLE session. Deleting WiFi outright subsumes that fix -- contention cannot recur on a radio
    // that is never brought up. Kept as an explicit branch because the transition pair is a
    // recognisable idiom and the disconnect side above still does real work.
    oldDeviceConnected = deviceConnected;
  }
  
  delay(5);
}
