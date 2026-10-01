#pragma once

// Firmware version, semantic. Bump the PATCH for fixes, MINOR for features, MAJOR for a change
// that breaks the app's expectations of the BLE protocol.
//
// The app compares this against the version in its bundled firmware manifest to decide whether
// to offer an update. It is a plain string compared by the app's parser, not by the device --
// the device never decides whether an update is warranted, it only reports what it is running.
#define DRAUPNIR_FW_VERSION "1.0.1"
