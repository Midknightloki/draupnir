#pragma once
#include <ArduinoJson.h>
#include <Print.h>

#define NUM_MACRO_SLOTS 16

// Ported from firmware/M5_M6_config/M5_M6_config.ino's profile/macro engine, kept as close to
// verbatim as the display-agnostic parts allow. Reuses the exact same profiles.json schema so
// the companion app and BLE protocol (ported in a later phase) need zero changes.

// Registers the Keyboard/ConsumerControl/Mouse HID interfaces. Call before USB.begin() in
// setup() -- interfaces must be registered before the composite USB device descriptor is
// finalized (same ordering M5_M6_config.ino uses and documents).
void hid_init();

void profiles_init();

// Re-reads /profiles.json (already written by a BLE save_profiles command) into profilesDoc,
// without touching LittleFS.begin()/prefs.begin() again -- those are one-time setup calls, done
// only by profiles_init(). Mirrors M5_M6_config.ino's loadProfiles(), which profiles_init()
// itself is built on top of.
void profiles_reload();

// Active profile's name/color, and the macro (if any) at ring position `pos` (0-15). Returns a
// null JsonObject (check with .isNull()) if there's no macro at that position.
const char *profiles_active_name();
JsonObject profiles_find_macro(int pos);

// Serializes the whole loaded profiles.json document straight to `out` (e.g. a BLE chunk sink),
// so callers never need direct access to the underlying JsonDocument.
void profiles_serialize(Print &out);

// Starts (or, for "toggle" mode macros, stops) the macro at ring position `pos`. No-ops if
// there's no macro at that position. Playback is asynchronous -- actions with delays don't
// block; call macros_update() every loop() tick to advance them.
//
// macros_fire() mutates runningMacros[] state directly with no locking -- only call it from the
// loop() task. Any other context (LVGL click/encoder callbacks, which run on the display
// driver's own task) MUST use macros_request_fire() instead, which just enqueues the request;
// macros_update() drains the queue and calls macros_fire() itself from loop(). Calling
// macros_fire() cross-task was the cause of the "Sign-off" macro's HID text output getting
// spliced/truncated: a tap could reset a macro's action index/text pointer on one core while
// executeAction() was still mid-read of the old state on the other.
void macros_fire(int pos);
void macros_request_fire(int pos);
void macros_update();
bool macros_is_running(int pos);

// Stops every running macro and releases any HID state (modifiers, mouse buttons, consumer
// codes) a macro interrupted mid-sequence may still be holding down.
//
// loop()-task only, same constraint as macros_fire(): it mutates runningMacros[] directly with
// no locking. profiles_reload() calls it itself as its first action -- callers do NOT need to,
// and must not call it from an LVGL or BLE callback.
//
// This exists because every ActiveMacro holds a JsonObject referencing profilesDoc's memory
// pool, which deserializeJson() frees and reallocates on reload; a macro still running across
// that boundary reads freed memory on its next macros_update() tick.
void macros_stop_all();
