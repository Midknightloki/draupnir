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

#define SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

enum AppMode { RUN_MODE, CONFIG_MODE, WIFI_SETUP_MODE };
AppMode currentMode = RUN_MODE;
String pairingToken = "";

BLEServer *pServer = nullptr;
BLECharacteristic *pTxCharacteristic = nullptr;
bool deviceConnected = false;
bool oldDeviceConnected = false;
String bleBuffer = "";


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

void sendBleMessage(const String &msg) {
  Serial.print("BLE TX: sending message of length ");
  Serial.println(msg.length());
  int len = msg.length();
  int offset = 0;
  while (offset < len) {
    int chunkSize = min(180, len - offset);
    pTxCharacteristic->setValue((uint8_t*)(msg.c_str() + offset), chunkSize);
    pTxCharacteristic->notify();
    offset += chunkSize;
    delay(20);
  }
  // Send the newline delimiter at the end
  uint8_t nl = '\n';
  pTxCharacteristic->setValue(&nl, 1);
  pTxCharacteristic->notify();
  Serial.println("BLE TX: sent");
}

void handleBleCommand(String cmdStr) {
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
    File file = LittleFS.open("/profiles.json", "r");
    if (!file) {
      Serial.println("get_profiles: Failed to open profiles.json");
      sendBleMessage("{\"status\":\"error\",\"message\":\"Failed to open profiles\"}");
      return;
    }
    size_t fileSize = file.size();
    Serial.print("get_profiles: profiles.json file size = ");
    Serial.println(fileSize);
    
    String profilesJson = "";
    profilesJson.reserve(fileSize);
    while (file.available()) {
      char c = (char)file.read();
      if (c != '\n' && c != '\r') {
        profilesJson += c;
      }
    }
    file.close();
    Serial.print("get_profiles: Read profilesJson length = ");
    Serial.println(profilesJson.length());
    
    String response = "";
    response.reserve(profilesJson.length() + 50);
    response += "{\"status\":\"ok\",\"profiles\":";
    response += profilesJson;
    response += "}";
    Serial.print("get_profiles: Response length = ");
    Serial.println(response.length());
    sendBleMessage(response);
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
      Serial.println("BLE Client Connected");
    };

    void onDisconnect(BLEServer* pServer) {
      deviceConnected = false;
      Serial.println("BLE Client Disconnected");
    }
};

class MyCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
      String rxValue = pCharacteristic->getValue();
      Serial.print("BLE RX bytes: ");
      Serial.println(rxValue.length());
      if (rxValue.length() > 0) {
        for (int i = 0; i < rxValue.length(); i++) {
          char c = rxValue[i];
          if (c == '\n') {
            handleBleCommand(bleBuffer);
            bleBuffer = "";
          } else {
            bleBuffer += c;
          }
        }
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
  
  // Initialize BLE
  BLEDevice::init("Draupnir");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());
  
  BLEService *pService = pServer->createService(SERVICE_UUID);
  pTxCharacteristic = pService->createCharacteristic(
                        CHARACTERISTIC_UUID_TX,
                        BLECharacteristic::PROPERTY_NOTIFY
                      );
  pTxCharacteristic->addDescriptor(new BLE2902());
  
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
  pServer->getAdvertising()->setMinPreferred(0x12);
  BLEDevice::startAdvertising();
  Serial.println("BLE Started Advertising");
  
  Keyboard.begin();
  ConsumerControl.begin();
  Mouse.begin();
  USB.begin();

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
  
  // BLE reconnection handling
  if (!deviceConnected && oldDeviceConnected) {
    delay(500); // give the bluetooth stack the chance to get ready
    pServer->startAdvertising(); // restart advertising
    Serial.println("Restart BLE advertising");
    oldDeviceConnected = deviceConnected;
  }
  if (deviceConnected && !oldDeviceConnected) {
    // do stuff on connection
    oldDeviceConnected = deviceConnected;
  }
  
  delay(5);
}
