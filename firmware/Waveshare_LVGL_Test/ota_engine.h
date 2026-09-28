#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

// OTA state machine. Owns the inactive partition, the streaming hash, and the binary-mode flag.
//
// THREADING: every function here is loop()-task ONLY except ota_active(), which the BLE host task
// reads to decide whether an inbound write is a command or image bytes. That one flag is the
// entire cross-task surface, and it is volatile for exactly that reason.

// True while binary mode is in effect. Read from the BLE host task.
bool ota_active();

// Validate and start. Returns false and fills `err` on refusal; no partition write has happened.
bool ota_begin_request(uint32_t size, const char *sha256hex, const char *sighex,
                       char *err, size_t errlen);

// Feed image bytes. Returns false on a write error, which aborts the session.
bool ota_feed(const uint8_t *data, size_t len);

// Bytes accepted so far -- the resume anchor.
uint32_t ota_offset();

// Total image size for the session in progress, as given to ota_begin_request(). 0 when no
// session is active. Exists so a UI can render a percentage without reaching into the engine's
// statics -- see the note on ota_active() above about what's allowed to cross that boundary.
uint32_t ota_expected();

// Finish: verify hash, verify signature, commit, set boot partition. Returns false and fills
// `err` on any failure, leaving the device on its existing firmware.
bool ota_finish(char *err, size_t errlen);

// Release the handle and leave binary mode.
void ota_abort();

// Call from loop(). Aborts a stalled session so a dropped transfer cannot strand the device in
// binary mode, where it would no longer understand commands.
void ota_tick();

// Clears the rollback pending state. ONLY call this in response to ota_confirm from the app --
// see the spec's "what counts as confirmation".
bool ota_confirm();
