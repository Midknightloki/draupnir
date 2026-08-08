#include "device_state.h"
#include <Arduino.h>
#include <Preferences.h>

static Preferences prefs;

// Namespace kept as "draupnir" and the key as "activeProfile" so an already-deployed board
// keeps its stored profile across this refactor. Both keys are within NVS's 15-character limit
// ("activeProfile" is 13).
static const char *NVS_NAMESPACE  = "draupnir";
static const char *KEY_ACTIVE     = "activeProfile";
static const char *KEY_BRIGHTNESS = "brightness";

void state_init() {
  prefs.begin(NVS_NAMESPACE, false);
  Serial.println("[state] nvs opened (namespace=draupnir)");
}

int state_active_profile(int fallback) {
  return prefs.getInt(KEY_ACTIVE, fallback);
}

// Read-before-write on both setters: NVS is flash, and these are called from gesture handlers.
// Skipping an identical write costs one read and avoids a wear cycle on every no-op.
void state_set_active_profile(int idx) {
  if (prefs.getInt(KEY_ACTIVE, -1) == idx) return;
  prefs.putInt(KEY_ACTIVE, idx);
  Serial.printf("[state] activeProfile -> %d (persisted)\n", idx);
}

uint8_t state_brightness(uint8_t fallback) {
  return prefs.getUChar(KEY_BRIGHTNESS, fallback);
}

void state_set_brightness(uint8_t duty) {
  if (prefs.getUChar(KEY_BRIGHTNESS, 0) == duty) return;
  prefs.putUChar(KEY_BRIGHTNESS, duty);
  Serial.printf("[state] brightness -> %u (persisted)\n", (unsigned)duty);
}
