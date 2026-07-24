#include "macro_engine.h"
#include <LittleFS.h>
#include <Preferences.h>
#include "USBHIDKeyboard.h"
#include "USBHIDConsumerControl.h"
#include "USBHIDMouse.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

static USBHIDKeyboard Keyboard;
static USBHIDConsumerControl ConsumerControl;
static USBHIDMouse Mouse;

static Preferences prefs;
static JsonDocument profilesDoc;
static int activeProfileIdx = 0;

// Same schema/content as M5_M6_config.ino's default, trimmed to what's useful for testing all
// four action types (key combo, text, consumer control, and a delay-containing sequence) on
// this board specifically.
static const char *defaultProfilesJson = R"=====(
{
  "version": 2,
  "activeProfile": 0,
  "settings": { "brightness": 160 },
  "profiles": [
    {
      "name": "Editing",
      "color": "#3080E0",
      "macros": [
        { "pos": 0, "name": "Build", "color": "#E0A030", "mode": "play_once", "actions": [ { "type": "key", "mods": ["CTRL","SHIFT"], "key": "b" } ] },
        { "pos": 1, "name": "Sign-off", "color": "#3080E0", "mode": "play_once", "actions": [ { "type": "text", "value": "- Eitri, FORGE Master\n" } ] },
        { "pos": 2, "name": "Mute", "color": "#E03030", "mode": "play_once", "actions": [ { "type": "consumer", "code": "MUTE" } ] },
        { "pos": 3, "name": "Slow Type", "color": "#30E080", "mode": "play_once", "actions": [
          { "type": "text", "value": "one" },
          { "type": "delay", "ms": 500 },
          { "type": "text", "value": "two" },
          { "type": "delay", "ms": 500 },
          { "type": "text", "value": "three\n" }
        ] },
        { "pos": 4, "name": "Caps Lock", "color": "#A030E0", "mode": "toggle", "actions": [ { "type": "key", "key": "CAPSLOCK" } ] }
      ]
    }
  ]
}
)=====";

struct ActiveMacro {
  bool active = false;
  bool isToggle = false;
  int slotIdx = -1;
  JsonObject macroDef;
  int currentActionIndex = 0;
  unsigned long nextActionTime = 0;
};
static ActiveMacro runningMacros[NUM_MACRO_SLOTS];
static QueueHandle_t fireQueue = nullptr;

void hid_init() {
  Keyboard.begin();
  ConsumerControl.begin();
  Mouse.begin();
  fireQueue = xQueueCreate(8, sizeof(int));
}

void macros_request_fire(int pos) {
  if (!fireQueue) return;
  xQueueSend(fireQueue, &pos, 0);
}

// Every ActiveMacro holds a JsonObject that points INTO profilesDoc's memory pool.
// deserializeJson() clears and reallocates that pool, so anything still running across a reload
// would dereference freed memory on the next macros_update() tick. This is deterministic, not a
// rare race: a "toggle" macro never terminates on its own (macros_update() just rewinds
// currentActionIndex forever), and the shipped default profile contains one -- fire it, then
// save profiles from the app, and the use-after-free is guaranteed.
void macros_stop_all() {
  // A macro interrupted mid-sequence can have modifiers or mouse buttons held down. Release them
  // before clearing state, or the host is left with e.g. a stuck Ctrl and no way to clear it.
  Keyboard.releaseAll();
  Mouse.release(MOUSE_ALL);
  ConsumerControl.release();

  for (int i = 0; i < NUM_MACRO_SLOTS; i++) {
    runningMacros[i].active = false;
    runningMacros[i].isToggle = false;
    runningMacros[i].slotIdx = -1;
    runningMacros[i].macroDef = JsonObject(); // drop the reference into the old document pool
    runningMacros[i].currentActionIndex = 0;
    runningMacros[i].nextActionTime = 0;
  }
}

void profiles_reload() {
  // Deliberately here rather than at the call sites, so no future caller can forget it.
  macros_stop_all();

  File file = LittleFS.open("/profiles.json", "r");
  if (!file) {
    Serial.println("Failed to open profiles.json, creating default");
    file = LittleFS.open("/profiles.json", "w");
    file.print(defaultProfilesJson);
    file.close();
    file = LittleFS.open("/profiles.json", "r");
  }
  Serial.printf("[diag] profiles_reload: opened profiles.json, size=%u\n", (unsigned)file.size());

  DeserializationError error = deserializeJson(profilesDoc, file);
  file.close();
  Serial.printf("[diag] profiles_reload: deserializeJson done, err=%s heap=%u\n", error.c_str(), ESP.getFreeHeap());

  if (error) {
    Serial.println("Failed to parse profiles.json");
    return;
  }

  Serial.println("Loaded profiles.json");
  activeProfileIdx = prefs.getInt("activeProfile", 0);
  JsonArray profiles = profilesDoc["profiles"];
  Serial.printf("[diag] profiles_reload: activeProfileIdx=%d numProfiles=%u\n", activeProfileIdx, profiles.isNull() ? 0 : profiles.size());
  if (profiles.isNull() || activeProfileIdx >= (int)profiles.size()) activeProfileIdx = 0;
}

void profiles_init() {
  Serial.println("[diag] profiles_init: mounting LittleFS");
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
    return;
  }
  Serial.println("[diag] profiles_init: LittleFS mounted");
  prefs.begin("draupnir", false);
  Serial.println("[diag] profiles_init: prefs.begin done");
  profiles_reload();
}

const char *profiles_active_name() {
  JsonArray profiles = profilesDoc["profiles"];
  if (profiles.isNull() || activeProfileIdx >= (int)profiles.size()) return "No Profiles";
  JsonObject prof = profiles[activeProfileIdx];
  return prof["name"] | "Profile";
}

JsonObject profiles_find_macro(int pos) {
  JsonArray profiles = profilesDoc["profiles"];
  if (profiles.isNull() || activeProfileIdx >= (int)profiles.size()) return JsonObject();
  JsonObject prof = profiles[activeProfileIdx];
  JsonArray macros = prof["macros"];
  for (JsonObject m : macros) {
    int p = m["pos"] | -1;
    if (p == pos) return m;
  }
  return JsonObject();
}

void profiles_serialize(Print &out) {
  serializeJson(profilesDoc, out);
}

static uint8_t getModifierCode(const char *mod) {
  String m = String(mod);
  m.toUpperCase();
  if (m == "CTRL") return KEY_LEFT_CTRL;
  if (m == "SHIFT") return KEY_LEFT_SHIFT;
  if (m == "ALT") return KEY_LEFT_ALT;
  if (m == "WIN" || m == "GUI") return KEY_LEFT_GUI;
  return 0;
}

static uint16_t getConsumerCode(const char *code) {
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

static uint8_t getMouseButtonCode(const char *btn) {
  String b = String(btn);
  b.toUpperCase();
  if (b == "LEFT") return MOUSE_LEFT;
  if (b == "RIGHT") return MOUSE_RIGHT;
  if (b == "MIDDLE") return MOUSE_MIDDLE;
  if (b == "MB4" || b == "BACKWARD") return MOUSE_BACKWARD;
  if (b == "MB5" || b == "FORWARD") return MOUSE_FORWARD;
  return 0;
}

static uint8_t getSpecialKeyCode(const char *key) {
  String k = String(key);
  k.toUpperCase();
  if (k == "PRINTSCREEN" || k == "PRTSCN") return 0xCE;
  if (k == "ESC") return 0xB1;
  if (k == "TAB") return 0xB3;
  if (k == "ENTER" || k == "RETURN") return 0xB0;
  if (k == "SPACE") return 0x20;
  if (k == "BACKSPACE") return 0xB2;
  if (k == "DELETE" || k == "DEL") return 0xD4;
  if (k == "CAPSLOCK") return 0xC1;
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

static void executeAction(JsonObject action) {
  const char *type = action["type"];
  if (!type) return;
  if (strcmp(type, "key") == 0) {
    JsonArray mods = action["mods"];
    for (const char *mod : mods) {
      Keyboard.press(getModifierCode(mod));
    }
    const char *keyStr = action["key"];
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
    const char *text = action["value"];
    // Keyboard.write(c) calls press(c) then release(c) back to back with zero delay between
    // them, and Keyboard.print()/write(buffer) chains characters with zero delay too.
    // USBHIDKeyboard::sendReport() has no queue -- a new report sent before the host has polled
    // the previous one just overwrites and loses it. Pacing only between characters (previous
    // attempt) still left the press/release pair for the SAME character back to back; losing a
    // release there reads to the host as a stuck key, which then auto-repeats and interleaves
    // with whatever types next -- matches the scrambled (not just dropped) characters seen
    // on-device. Pacing both the press->release and release->next-press gaps explicitly fixes
    // both transitions.
    if (text) {
      for (const char *p = text; *p; p++) {
        Keyboard.press((uint8_t)*p);
        delay(8);
        Keyboard.release((uint8_t)*p);
        delay(8);
      }
    }
  } else if (strcmp(type, "consumer") == 0) {
    const char *code = action["code"];
    ConsumerControl.press(getConsumerCode(code));
    ConsumerControl.release();
  } else if (strcmp(type, "mouse") == 0) {
    const char *btn = action["button"];
    const char *evt = action["event"] | "click";
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
  // "delay" and "rotary" modes are handled by the caller (updateMacros / fireMacro), not here.
}

void macros_fire(int pos) {
  if (pos < 0 || pos >= NUM_MACRO_SLOTS) return;
  JsonObject macro = profiles_find_macro(pos);
  if (macro.isNull()) return;

  const char *mode = macro["mode"] | "play_once";
  bool isToggle = (strcmp(mode, "toggle") == 0);

  if (runningMacros[pos].active && runningMacros[pos].isToggle) {
    runningMacros[pos].active = false;
    return;
  }

  runningMacros[pos].active = true;
  runningMacros[pos].isToggle = isToggle;
  runningMacros[pos].slotIdx = pos;
  runningMacros[pos].macroDef = macro;
  runningMacros[pos].currentActionIndex = 0;
  runningMacros[pos].nextActionTime = millis();
}

bool macros_is_running(int pos) {
  if (pos < 0 || pos >= NUM_MACRO_SLOTS) return false;
  return runningMacros[pos].active;
}

void macros_update() {
  int pendingPos;
  while (fireQueue && xQueueReceive(fireQueue, &pendingPos, 0) == pdTRUE) {
    macros_fire(pendingPos);
  }

  unsigned long now = millis();
  for (int i = 0; i < NUM_MACRO_SLOTS; i++) {
    if (!runningMacros[i].active) continue;
    if (now < runningMacros[i].nextActionTime) continue;

    JsonArray actions = runningMacros[i].macroDef["actions"];
    if (runningMacros[i].currentActionIndex >= (int)actions.size()) {
      if (runningMacros[i].isToggle) {
        runningMacros[i].currentActionIndex = 0;
      } else {
        runningMacros[i].active = false;
      }
      continue;
    }

    JsonObject action = actions[runningMacros[i].currentActionIndex];
    const char *type = action["type"];
    unsigned long actionDelay = 10;

    executeAction(action);

    if (type && strcmp(type, "delay") == 0) {
      actionDelay = action["ms"] | 100;
    }

    runningMacros[i].currentActionIndex++;
    runningMacros[i].nextActionTime = millis() + actionDelay;
  }
}
