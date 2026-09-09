#pragma once
#include <ArduinoJson.h>
#include <Print.h>

// TWO LIMITS, DELIBERATELY SEPARATE. They used to be one number only because a single array
// served both jobs -- runningMacros[] was indexed BY pos, so "how many macros can exist" and "how
// many can play at once" were forced to be equal. They are different questions:
//
//   MAX_MACROS    a data-model limit. The highest legal pos is MAX_MACROS-1. Sized well past what
//                 the ring can render legibly (at 32 macros a wedge is 11 degrees and the 45px
//                 icons stopped fitting around 16-20), so the cap is not what anyone hits first.
//   RUNNING_SLOTS a RAM limit: concurrently-playing macros. 16 is unchanged from before.
//
// pos is a stable identifier and the ring-ordering key -- NOT an index into anything. See
// docs/Draupnir_Spec.md section 6.
#define MAX_MACROS     32
#define RUNNING_SLOTS  16

// The profiles.json schema version this firmware understands. A document declaring MORE than this
// is refused rather than loaded-and-silently-mangled; see schema_version_ok() in macro_engine.cpp.
// Reading older versions stays fine -- v3 is a strict relaxation of v2, so every v2 file is a
// valid v3 file.
#define SCHEMA_VERSION 3

// Ported from firmware/M5_M6_config/M5_M6_config.ino's profile/macro engine, kept as close to
// verbatim as the display-agnostic parts allow. Reuses the exact same profiles.json schema so
// the companion app and BLE protocol (ported in a later phase) need zero changes.

// Registers the Keyboard/ConsumerControl/Mouse HID interfaces. Call before USB.begin() in
// setup() -- interfaces must be registered before the composite USB device descriptor is
// finalized (same ordering M5_M6_config.ino uses and documents).
void hid_init();

void profiles_init();

// Re-reads /profiles.json (already written by a BLE save_profiles command) into profilesDoc,
// without touching LittleFS.begin()/state_init() again -- those are one-time setup calls, done
// only by profiles_init(). Mirrors M5_M6_config.ino's loadProfiles(), which profiles_init()
// itself is built on top of.
void profiles_reload();

// Active profile's name/color, and the macro (if any) at ring position `pos` (0-15). Returns a
// null JsonObject (check with .isNull()) if there's no macro at that position.
const char *profiles_active_name();
JsonObject profiles_find_macro(int pos);

// Same lookup as profiles_find_macro(), but against an arbitrary profile index rather than only
// the active one. Used by save_profiles's icon_xbm merge (ble_engine.cpp), which must cross-
// reference every profile in the INCOMING document against what's already stored on the device,
// not just whichever one happens to be on screen right now. Returns a null JsonObject (check
// with .isNull()) if profileIdx or pos don't match anything.
JsonObject profiles_find_macro_in(int profileIdx, int pos);

// True if this firmware understands the document's declared schema version -- i.e. `version` is
// absent (legacy) or <= SCHEMA_VERSION. Refuses only versions ABOVE what we know; older is always
// fine, since v3 is a strict relaxation of v2 and every v2 file is a valid v3 file.
//
// Called on the load path with the whole document, and by ble_engine's save_profiles with the
// incoming `profiles` object BEFORE anything reaches flash.
bool profiles_schema_version_ok(JsonVariantConst doc);

// How many macros the ACTIVE profile declares, and the `pos` of the idx'th one in DOCUMENT order
// (not sorted -- the caller sorts, see scan_active_positions()). Together these let the ring
// enumerate what exists instead of probing every pos in a fixed range, which is what the old
// 16-slot scan did and what made the cap structural.
//
// These exist so the .ino never touches profilesDoc directly, matching how it already goes
// through profiles_find_macro(). Returns 0 / -1 when there is no active profile.
int profiles_active_macro_count();
int profiles_macro_pos_at(int idx);

// The active document's settings.brightness -- the SEED for NVS brightness, never a source of
// truth once NVS has been written (see device_state.h). 160 if absent or out of range.
uint8_t profiles_default_brightness();

// settings.orientation, 0..3 (0/90/180/270 degrees). 0 if absent or out of range.
// profiles.json is the SINGLE source of truth for this -- deliberately unlike brightness, which
// is NVS-authoritative. Brightness has one writer (the device); orientation will have two once
// the Settings menu gains an entry, and NVS cannot reconcile two writers without the device
// writing back to JSON anyway -- at which point NVS is a redundant second copy.
uint8_t profiles_orientation(void);

// Active profile's "color" (e.g. "#3080E0"), "#FFFFFF" if absent. Used for the ring's
// directional profile indicators.
const char *profiles_active_color();

int profiles_count();
int profiles_active_index();

// Switches the active profile. Returns false (and does nothing) if `idx` is out of range or
// already active. Stops every running macro first and persists the new index to NVS.
//
// loop()-task ONLY, same constraint as macros_fire(): it calls macros_stop_all(), which mutates
// runningMacros[] with no locking. Every ActiveMacro also holds a JsonObject into the profile
// being switched away from, so skipping the stop is the H3 use-after-free on a new trigger.
//
// The CALLER is responsible for rebuilding the ring UI afterwards, under lvgl_lock().
bool profiles_set_active(int idx);

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
//
// Also drains fireQueue (see macro_engine.cpp): a fire can be queued after this stop was decided
// but before it runs, and would otherwise survive to restart the macro this call just stopped.
// That means a fire queued behind a MACRO_CMD_STOP_ALL sentinel is discarded too, not just
// fires queued before it -- "stop all" that lets a still-queued tap start something a moment
// later is not stopping all.
void macros_stop_all();

// Cross-task-safe form of macros_stop_all(), for the swipe-down "kill all" gesture, whose event
// callback runs on the LVGL task. Enqueues a sentinel that macros_update() picks up and acts on
// from loop(), exactly as macros_request_fire() does for a normal fire.
void macros_request_stop_all();

// True if any macro is currently running -- drives the ring's "this macro is running" pulse and
// tells the UI whether a kill-all gesture would do anything.
//
// Unlike the mutating calls above, this is safe to read from the LVGL task: it only reads the
// `active` booleans, never writes, and a one-frame-stale answer costs at most one frame of pulse.
bool macros_any_running();

// Rotary macros bind the encoder instead of playing. The encoder runs on its own task and the
// macro engine is loop()-only, so a turn must be ENQUEUED, exactly as macros_request_fire() does.
// dir > 0 executes actions[0]; dir < 0 executes actions[1].
void macros_request_rotary_step(int dir);

// True while a rotary macro owns the encoder. The UI reads these to draw its screen.
bool        macros_rotary_active(void);
const char *macros_rotary_name(void);
// Returns the raw "#RRGGBB" string, not a parsed value: parse_hex_color() lives in the .ino and
// the macro engine stays free of display concerns.
const char *macros_rotary_color(void);

// Clears ONLY the rotary binding, leaving runningMacros[] alone. Exiting a rotary screen stops
// the knob driving that macro's actions; it is not a reason to kill unrelated macros, and the
// M5Dial does not kill them either -- M9 requires the two boards to be indistinguishable.
//
// macros_stop_all() still clears rotary state as well, so the profile-reload path keeps its
// use-after-free protection: rotaryMacro holds a JsonObject into profilesDoc.
//
// loop()-task only, same constraint as the rest of the engine's mutating API (macros_fire(),
// macros_stop_all()): it mutates rotaryActive/rotaryMacro directly with no locking. Callers on
// any other task (LVGL, BLE) must not call this -- there is no request-queue form because
// nothing outside loop() currently needs one; add one if that changes rather than calling this
// directly from another task.
void macros_rotary_stop(void);
