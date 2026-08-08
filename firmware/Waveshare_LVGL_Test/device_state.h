#pragma once
#include <stdint.h>

// Small, device-owned state that survives a power cycle, in NVS (Preferences, namespace
// "draupnir"). Spec section 8 calls this "State: last profile + brightness in NVS".
//
// Deliberately free of LVGL, ledc and JSON. Callers apply the values; this file only stores
// them. That is what lets the M5Dial adopt this file unchanged and swap in
// M5Dial.Display.setBrightness() (spec section 3).
//
// profiles.json is SEED ONLY. Callers pass the JSON value in as `fallback`; once a value has
// been written here it wins forever. NEVER copy a JSON value back over a written one: the
// companion app has no brightness control, so it round-trips a stale settings.brightness on
// every macro edit and would stomp whatever was set on the knob. The same argument applies to
// activeProfile once profiles_set_active() exists -- an app save carrying a stale index would
// silently revert an on-device profile switch.
void state_init();

int  state_active_profile(int fallback);
void state_set_active_profile(int idx);

uint8_t state_brightness(uint8_t fallback);
void    state_set_brightness(uint8_t duty);
