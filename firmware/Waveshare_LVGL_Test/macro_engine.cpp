#include "macro_engine.h"
#include "trace.h"
#include <LittleFS.h>
#include "device_state.h"
#include "USBHIDKeyboard.h"
#include "USBHIDConsumerControl.h"
#include "USBHIDMouse.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

static USBHIDKeyboard Keyboard;
static USBHIDConsumerControl ConsumerControl;
static USBHIDMouse Mouse;

static JsonDocument profilesDoc;
static int activeProfileIdx = 0;

// Same schema/content as M5_M6_config.ino's default, trimmed to what's useful for testing all
// four action types (key combo, text, consumer control, and a delay-containing sequence) on
// this board specifically.
static const char *defaultProfilesJson = R"=====(
{
  "version": 3,
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
    },
    {
      "name": "Media",
      "color": "#30C060",
      "macros": [
        { "pos": 0, "name": "Play/Pause", "color": "#30C060", "mode": "play_once", "actions": [ { "type": "consumer", "code": "PLAY_PAUSE" } ] },
        { "pos": 1, "name": "Next", "color": "#3080E0", "mode": "play_once", "actions": [ { "type": "consumer", "code": "NEXT" } ] },
        { "pos": 2, "name": "Prev", "color": "#E0A030", "mode": "play_once", "actions": [ { "type": "consumer", "code": "PREV" } ] },
        { "pos": 3, "name": "Vol Up", "color": "#A030E0", "mode": "play_once", "actions": [ { "type": "consumer", "code": "VOL_UP" } ] }
      ]
    }
  ]
}
)=====";

struct ActiveMacro {
  bool active = false;
  bool isToggle = false;
  int pos = -1;                 // WHICH macro this slot is playing -- the identity, not an index
  JsonObject macroDef;
  int currentActionIndex = 0;
  unsigned long nextActionTime = 0;
};

// A POOL, not a lookup table. The array index carries NO meaning: slot 3 is not pos 3, it is
// simply the fourth free slot something happened to claim. `.pos` is the identity, and every
// query goes through find_running_slot().
//
// This is the whole of M8b. Before it, this array was indexed BY pos:
//
//     runningMacros[pos].active = true;
//
// which is what actually capped macros at 16 -- NUM_MACRO_SLOTS was just this array's size, so
// raising the constant would have "worked" while permanently tying bytes of running-macro state
// to the highest addressable pos, and the next uncapping would pay again.
static ActiveMacro runningMacros[RUNNING_SLOTS];

// Index of the slot currently playing `pos`, or -1. Linear over 16 entries -- trivial next to the
// JSON walk profiles_find_macro() already does on every fire.
static int find_running_slot(int pos) {
  for (int i = 0; i < RUNNING_SLOTS; i++) {
    if (runningMacros[i].active && runningMacros[i].pos == pos) return i;
  }
  return -1;
}

// Index of a free slot, or -1 when all RUNNING_SLOTS are playing.
static int claim_free_slot(void) {
  for (int i = 0; i < RUNNING_SLOTS; i++) {
    if (!runningMacros[i].active) return i;
  }
  return -1;
}
static QueueHandle_t fireQueue = nullptr;

void hid_init() {
  Keyboard.begin();
  ConsumerControl.begin();
  Mouse.begin();
  fireQueue = xQueueCreate(8, sizeof(int));
}

// Sentinel pushed through the same fireQueue as a normal position, rather than adding a second
// queue: it keeps kill-all strictly ordered against any fires already queued ahead of it, so a
// swipe can never be overtaken by a tap that was requested first.
#define MACRO_CMD_STOP_ALL (-99)

#define MACRO_CMD_ROTARY_CW  (-97)
#define MACRO_CMD_ROTARY_CCW (-98)

// Holds a JsonObject into profilesDoc, exactly like ActiveMacro -- so it MUST be dropped whenever
// the document is reloaded, or a save arriving while the rotary screen is up dereferences a freed
// pool. Same defect class H3 fixed, on a new trigger. macros_stop_all() clears it.
static JsonObject rotaryMacro;
static bool       rotaryActive = false;

void macros_request_fire(int pos) {
  if (!fireQueue) {
    TRACE("[fire] request DROPPED: no queue\n");
    return;
  }
  // xQueueSend with a 0 tick timeout silently drops when the queue is full (depth 8), which
  // looks exactly like "the tap did nothing". Gated by DRAUPNIR_TRACE_INPUT.
  BaseType_t ok = xQueueSend(fireQueue, &pos, 0);
  TRACE("[fire] request pos=%d queued=%d waiting=%u\n",
                pos, (int)(ok == pdTRUE), (unsigned)uxQueueMessagesWaiting(fireQueue));
}

void macros_request_stop_all() {
  if (!fireQueue) return;
  int cmd = MACRO_CMD_STOP_ALL;
  xQueueSend(fireQueue, &cmd, 0);
}

// One sentinel per detent, not per wake. encoder_task coalesces every tick queued since it last
// woke into a single delta, which is right for the ring and the brightness gauge (both treat
// delta as a magnitude) but would silently drop detents here -- a three-detent flick would fire
// the bound action once. The M5Dial catches up one step per loop pass, so dropping them would be
// a parity break.
//
// xQueueSend with a 0 timeout drops silently when the queue is full (depth 8, drained every
// loop tick), which bounds a pathological spin rather than blocking the encoder task.
void macros_request_rotary_step(int dir) {
  if (!fireQueue || dir == 0) return;
  int cmd = (dir > 0) ? MACRO_CMD_ROTARY_CW : MACRO_CMD_ROTARY_CCW;
  int steps = (dir > 0) ? dir : -dir;
  for (int i = 0; i < steps; i++) xQueueSend(fireQueue, &cmd, 0);
}
bool        macros_rotary_active(void) { return rotaryActive; }
const char *macros_rotary_name(void)   { return rotaryMacro.isNull() ? "Rotary"  : (const char *)(rotaryMacro["name"]  | "Rotary"); }
const char *macros_rotary_color(void)  { return rotaryMacro.isNull() ? "#FFFFFF" : (const char *)(rotaryMacro["color"] | "#FFFFFF"); }

// See macro_engine.h: clears ONLY the rotary binding, deliberately leaving runningMacros[]
// untouched -- exiting a rotary screen is not a kill-all, on the M5Dial or here.
void macros_rotary_stop(void) {
  rotaryActive = false;
  rotaryMacro  = JsonObject();   // drop the reference into the document pool
}

bool macros_any_running() {
  for (int i = 0; i < RUNNING_SLOTS; i++) {
    if (runningMacros[i].active) return true;
  }
  return false;
}

// Every ActiveMacro holds a JsonObject that points INTO profilesDoc's memory pool.
// deserializeJson() clears and reallocates that pool, so anything still running across a reload
// would dereference freed memory on the next macros_update() tick. This is deterministic, not a
// rare race: a "toggle" macro never terminates on its own (macros_update() just rewinds
// currentActionIndex forever), and the shipped default profile contains one -- fire it, then
// save profiles from the app, and the use-after-free is guaranteed.
void macros_stop_all() {
  // Drain fireQueue FIRST, before touching HID state or runningMacros[]. A fire can be queued
  // (macros_request_fire()) after a stop was already decided but before this function runs --
  // e.g. Settings opening: the swipe-up sets settings_open_requested, but the release from that
  // same swipe can still land as a CLICKED while ui_mode is still UI_RING (loop() hasn't drained
  // the request flag yet), queueing a fire. Left in the queue, that fire would survive this stop
  // and be drained by the very next macros_update() tick, restarting the macro this call was
  // supposed to have killed -- looks stopped for one tick, then isn't.
  //
  // Consequence, deliberate and worth flagging: a fire queued BEHIND a MACRO_CMD_STOP_ALL
  // sentinel is now discarded too, not just fires queued ahead of it. Previously the swipe-down
  // kill-all path let anything queued after the sentinel go on to fire normally once
  // macros_update() reached it. "Stop all" that lets a still-queued tap start something a moment
  // later is not stopping all, so this is the correct semantics -- but it IS a behaviour change
  // to that existing path, hence written down here rather than left for someone to discover.
  //
  // Safe to call from inside macros_update()'s own drain loop, which is where the
  // MACRO_CMD_STOP_ALL branch calls this: xQueueReceive with a 0 tick timeout returns false the
  // instant the queue is empty, so this always terminates and never recurses back into
  // macros_stop_all(). It just means this inner drain absorbs whatever the outer loop would have
  // processed next, which is exactly the discard behaviour above.
  int discarded = 0;
  int pending;
  while (fireQueue && xQueueReceive(fireQueue, &pending, 0) == pdTRUE) {
    discarded++;
  }
  if (discarded > 0) {
    Serial.printf("[diag] macros_stop_all: discarded %d pending fire(s) from the queue\n", discarded);
  }

  // A macro interrupted mid-sequence can have modifiers or mouse buttons held down. Release them
  // before clearing state, or the host is left with e.g. a stuck Ctrl and no way to clear it.
  Keyboard.releaseAll();
  Mouse.release(MOUSE_ALL);
  ConsumerControl.release();

  for (int i = 0; i < RUNNING_SLOTS; i++) {
    runningMacros[i].active = false;
    runningMacros[i].isToggle = false;
    runningMacros[i].pos = -1;                // release the identity, not just the active flag:
                                              // a stale .pos on an inactive slot would make
                                              // find_running_slot() match a slot that is not
                                              // playing anything if `active` were ever missed.
    runningMacros[i].macroDef = JsonObject(); // drop the reference into the old document pool
    runningMacros[i].currentActionIndex = 0;
    runningMacros[i].nextActionTime = 0;
  }

  rotaryActive = false;
  rotaryMacro  = JsonObject();   // drop the reference into the old document pool
}

// Rewrites /profiles.json with the built-in default and loads it into profilesDoc. Shared by
// both recovery paths -- file missing, and file present but unparseable. Previously only the
// missing-file case regenerated defaults; a parse error merely logged and returned, leaving
// profilesDoc in whatever state a failed deserialize left it and no way back to a working config
// short of a reflash. The unguarded "w" open in that old path was also a real bug: it called
// file.print() on an invalid File and carried on regardless.
// True if this firmware understands the document's declared schema version.
//
// Refuses only versions ABOVE SCHEMA_VERSION. Older is always fine -- v3 is a strict relaxation
// of v2 (pos uncapped from 16 to MAX_MACROS), so every v2 file is a valid v3 file, and a missing
// `version` is treated as legacy rather than as an error.
//
// Without this the version number is decorative. docs/Draupnir_Spec.md section 6 justified the
// v3 bump as making v2 firmware REFUSE a v3 file rather than silently dropping every macro above
// pos 15 -- but no firmware ever read the field, so that refusal never happened. It cannot be
// made to happen retroactively in already-deployed v2 builds; what this buys is that from v3
// onward, a version a device does not understand is refused instead of silently mangled.
// Takes JsonVariantConst rather than JsonDocument& so the same check serves both callers: the
// load path passes the whole document, and ble_engine's save_profiles passes the incoming
// `profiles` OBJECT out of a different document, before anything is written to flash.
bool profiles_schema_version_ok(JsonVariantConst doc) {
  JsonVariantConst v = doc["version"];
  if (v.isNull()) return true;                 // legacy file, predates the field
  int ver = v | 0;
  return ver <= SCHEMA_VERSION;
}

static bool write_and_load_defaults(const char *reason) {
  Serial.printf("[diag] profiles: falling back to built-in defaults (%s)\n", reason);

  File out = LittleFS.open("/profiles.json", "w");
  if (!out) {
    Serial.println("[diag] profiles: FATAL failed to open profiles.json for writing defaults");
    return false;
  }
  size_t expected = strlen(defaultProfilesJson);
  size_t written = out.print(defaultProfilesJson);
  out.close();
  if (written != expected) {
    Serial.printf("[diag] profiles: FATAL short write of defaults (%u of %u bytes)\n",
                  (unsigned)written, (unsigned)expected);
    return false;
  }

  DeserializationError err = deserializeJson(profilesDoc, defaultProfilesJson);
  if (err) {
    // Only reachable if defaultProfilesJson itself is malformed -- a build-time bug.
    Serial.printf("[diag] profiles: FATAL built-in defaults failed to parse: %s\n", err.c_str());
    return false;
  }
  return true;
}

void profiles_reload() {
  // Deliberately here rather than at the call sites, so no future caller can forget it.
  macros_stop_all();

  bool loaded = false;
  File file = LittleFS.open("/profiles.json", "r");
  if (!file) {
    loaded = write_and_load_defaults("profiles.json could not be opened");
  } else {
    Serial.printf("[diag] profiles_reload: opened profiles.json, size=%u\n", (unsigned)file.size());
    DeserializationError error = deserializeJson(profilesDoc, file);
    file.close();
    Serial.printf("[diag] profiles_reload: deserializeJson done, err=%s heap=%u\n", error.c_str(), ESP.getFreeHeap());
    if (error) {
      Serial.printf("Failed to parse profiles.json (%s) -- config is corrupt, restoring defaults\n", error.c_str());
      loaded = write_and_load_defaults("profiles.json failed to parse");
    } else if (!profiles_schema_version_ok(profilesDoc)) {
      // A file from a NEWER firmware or client. Treated exactly like a parse failure: fall back
      // to the built-in defaults rather than loading a document whose meaning we do not know.
      // Booting to an empty ring because some future client wrote a v4 file is the worse failure
      // -- the device is a keyboard, and a keyboard that does nothing is useless.
      Serial.printf("profiles.json declares version %d, this firmware understands %d -- refusing\n",
                    (int)(profilesDoc["version"] | 0), SCHEMA_VERSION);
      loaded = write_and_load_defaults("profiles.json schema version too new");
    } else {
      loaded = true;
      Serial.println("Loaded profiles.json");
    }
  }

  if (!loaded) {
    // Nothing usable in flash and defaults could not be written/parsed either. Leave profilesDoc
    // empty rather than pretending: the ring renders "(no macros)" instead of crashing.
    profilesDoc.clear();
    activeProfileIdx = 0;
    Serial.println("[diag] profiles_reload: no usable profiles loaded");
    return;
  }

  activeProfileIdx = state_active_profile(0);
  JsonArray profiles = profilesDoc["profiles"];
  Serial.printf("[diag] profiles_reload: activeProfileIdx=%d numProfiles=%u\n", activeProfileIdx, profiles.isNull() ? 0 : profiles.size());
  if (profiles.isNull() || activeProfileIdx < 0 || activeProfileIdx >= (int)profiles.size()) {
    activeProfileIdx = 0;
    // Write the correction back. Clamping in RAM only left a stale out-of-range index in NVS
    // forever, re-clamped silently on every boot -- "reading it without ever writing it is the
    // same as not having it" (spec section 8).
    state_set_active_profile(activeProfileIdx);
  }
}

void profiles_init() {
  Serial.println("[diag] profiles_init: mounting LittleFS");
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
    return;
  }
  Serial.println("[diag] profiles_init: LittleFS mounted");
  state_init();
  profiles_reload();
}

const char *profiles_active_name() {
  JsonArray profiles = profilesDoc["profiles"];
  if (profiles.isNull() || activeProfileIdx >= (int)profiles.size()) return "No Profiles";
  JsonObject prof = profiles[activeProfileIdx];
  return prof["name"] | "Profile";
}

const char *profiles_active_color() {
  JsonArray profiles = profilesDoc["profiles"];
  if (profiles.isNull() || activeProfileIdx >= (int)profiles.size()) return "#FFFFFF";
  JsonObject prof = profiles[activeProfileIdx];
  return prof["color"] | "#FFFFFF";
}

int profiles_count() {
  JsonArray profiles = profilesDoc["profiles"];
  return profiles.isNull() ? 0 : (int)profiles.size();
}

int profiles_active_index() {
  return activeProfileIdx;
}

bool profiles_set_active(int idx) {
  if (idx < 0 || idx >= profiles_count()) return false;
  if (idx == activeProfileIdx) return false;
  macros_stop_all();
  activeProfileIdx = idx;
  state_set_active_profile(activeProfileIdx);
  Serial.printf("[diag] profile -> %d (%s)\n", activeProfileIdx, profiles_active_name());
  return true;
}

uint8_t profiles_default_brightness() {
  int b = profilesDoc["settings"]["brightness"] | 160;
  if (b < 0)   b = 0;
  if (b > 255) b = 255;
  return (uint8_t)b;
}

uint8_t profiles_orientation(void) {
  int o = profilesDoc["settings"]["orientation"] | 0;
  if (o < 0 || o > 3) o = 0;
  return (uint8_t)o;
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

int profiles_active_macro_count() {
  JsonArray profiles = profilesDoc["profiles"];
  if (profiles.isNull() || activeProfileIdx >= (int)profiles.size()) return 0;
  JsonObject prof = profiles[activeProfileIdx];
  JsonArray macros = prof["macros"];
  if (macros.isNull()) return 0;
  return (int)macros.size();
}

int profiles_macro_pos_at(int idx) {
  JsonArray profiles = profilesDoc["profiles"];
  if (profiles.isNull() || activeProfileIdx >= (int)profiles.size()) return -1;
  JsonObject prof = profiles[activeProfileIdx];
  JsonArray macros = prof["macros"];
  if (macros.isNull() || idx < 0 || idx >= (int)macros.size()) return -1;
  JsonObject m = macros[idx];
  return m["pos"] | -1;
}

JsonObject profiles_find_macro_in(int profileIdx, int pos) {
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
}

void macros_fire(int pos) {
  // Distinguishes "never dequeued" from "dequeued but no macro at that pos" from "dequeued and
  // started". Gated by DRAUPNIR_TRACE_INPUT.
  if (pos < 0 || pos >= MAX_MACROS) {
    TRACE("[fire] fire pos=%d REJECTED (out of range)\n", pos);
    return;
  }
  JsonObject macro = profiles_find_macro(pos);
  if (macro.isNull()) {
    TRACE("[fire] fire pos=%d REJECTED (no macro at this pos)\n", pos);
    return;
  }

  const char *mode = macro["mode"] | "play_once";
  if (strcmp(mode, "rotary") == 0) {
    rotaryMacro  = macro;
    rotaryActive = true;
    return;               // a rotary macro binds the encoder; it does not play
  }
  bool isToggle = (strcmp(mode, "toggle") == 0);

  int slot = find_running_slot(pos);
  if (slot >= 0 && runningMacros[slot].isToggle) {
    TRACE("[fire] fire pos=%d -> stopping running toggle (slot %d)\n", pos, slot);
    runningMacros[slot].active = false;
    runningMacros[slot].pos    = -1;
    runningMacros[slot].macroDef = JsonObject();
    return;
  }

  // Re-firing a non-toggle macro that is already playing REUSES its slot (restarting it from
  // action 0), rather than claiming a second one. Two slots playing the same pos would make
  // find_running_slot() ambiguous and let one macro exhaust the pool by itself.
  if (slot < 0) slot = claim_free_slot();
  if (slot < 0) {
    // Refuse rather than evict. Stealing a slot from a running macro can leave its modifiers or
    // mouse buttons held down -- macros_stop_all() is the only path that releases HID state, and
    // an eviction does not go through it.
    Serial.printf("[diag] macros_fire: pos=%d REFUSED, all %d running slots busy\n",
                  pos, RUNNING_SLOTS);
    return;
  }

  TRACE("[fire] fire pos=%d START slot=%d mode=%s actions=%u\n", pos, slot, mode,
                (unsigned)(macro["actions"].isNull() ? 0 : macro["actions"].size()));

  runningMacros[slot].active = true;
  runningMacros[slot].isToggle = isToggle;
  runningMacros[slot].pos = pos;
  runningMacros[slot].macroDef = macro;
  runningMacros[slot].currentActionIndex = 0;
  runningMacros[slot].nextActionTime = millis();
}

bool macros_is_running(int pos) {
  if (pos < 0 || pos >= MAX_MACROS) return false;
  return find_running_slot(pos) >= 0;
}

void macros_update() {
  int pendingPos;
  while (fireQueue && xQueueReceive(fireQueue, &pendingPos, 0) == pdTRUE) {
    if (pendingPos == MACRO_CMD_STOP_ALL) {
      Serial.println("[diag] kill-all requested (swipe down)");
      macros_stop_all();
    } else if (pendingPos == MACRO_CMD_ROTARY_CW || pendingPos == MACRO_CMD_ROTARY_CCW) {
      if (rotaryActive && !rotaryMacro.isNull()) {
        JsonArray acts = rotaryMacro["actions"];
        int idx = (pendingPos == MACRO_CMD_ROTARY_CW) ? 0 : 1;
        if ((int)acts.size() > idx) executeAction(acts[idx]);
      }
    } else {
      macros_fire(pendingPos);
    }
  }

  unsigned long now = millis();
  for (int i = 0; i < RUNNING_SLOTS; i++) {
    if (!runningMacros[i].active) continue;
    if (now < runningMacros[i].nextActionTime) continue;

    JsonArray actions = runningMacros[i].macroDef["actions"];
    if (runningMacros[i].currentActionIndex >= (int)actions.size()) {
      if (runningMacros[i].isToggle) {
        runningMacros[i].currentActionIndex = 0;
      } else {
        // Release the slot back to the pool: clear the identity and drop the JsonObject, not just
        // the active flag. Leaving them set would pin a reference into profilesDoc for a macro
        // that finished, and hold the slot's identity against a later find_running_slot().
        runningMacros[i].active = false;
        runningMacros[i].pos    = -1;
        runningMacros[i].macroDef = JsonObject();
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
