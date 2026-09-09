#pragma once
#include <stdint.h>

// BLE GATT server for profile config, reusing the exact wire protocol already proven on
// firmware/M5_M6_config/M5_M6_config.ino (same service/characteristic UUIDs, same chunked
// notify+ack framing, same RX reassembly scheme) so the existing companion app needs zero
// changes to talk to this board. Real BLE Security Manager pairing (random on-screen passkey,
// authenticated + encrypted link) replaces M5Dial's software pairingToken/CONFIG_MODE scheme
// entirely -- the link itself is now the auth, so there's no separate "pair"/token command or
// per-request Authorization check.

// Call once from setup(), AFTER hid_init()/USB.begin() -- BLE must come up after USB on this
// chip (M5Dial's firmware documents USB.begin() disrupting BLE if the order is reversed).
void ble_init();

// Call every loop() tick. Drains the BLE RX command queue and dispatches commands
// (get_profiles, ...) from here -- BLE GAP/GATT callbacks run on the Bluedroid host task, not
// loop(), so touching profilesDoc or firing chunked TX from those callbacks directly would race
// the same way the LVGL click callback raced macros_update() (see macro_engine.h's fire-queue
// comment). Funneling everything through here keeps all of it single-threaded.
void ble_update();

// Pairing/connection state for the UI to poll each loop() tick and react to (show/hide a pairing
// screen, etc.) -- kept UI-agnostic here the same way macro_engine.h doesn't know about LVGL.
// ble_passkey() is only meaningful while ble_pairing_active() is true.
bool ble_pairing_active();
uint32_t ble_passkey();
bool ble_is_connected();

// True exactly once after a save_profiles command has written a new profiles.json and reloaded
// it into macro_engine's profilesDoc. The .ino should poll this each loop() tick and, if true,
// rebuild the ring UI under lvgl_lock() (ble_engine doesn't know about LVGL, same as
// macro_engine), then call ble_clear_profiles_dirty().
bool ble_profiles_dirty();
void ble_clear_profiles_dirty();
