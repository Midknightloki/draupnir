#pragma once
#include <stdint.h>

// Haptic feedback via the board's DRV2605 vibration-motor driver.
//
// This replaces the M5Dial build's buzzer blip: the Waveshare knob has no buzzer, but it does
// carry a DRV2605 haptic driver on the I2C bus, so confirmation feedback is a short buzz instead
// of a beep. The point is the same -- eyes-free confirmation that a gesture registered.
//
// Every entry point is a safe no-op when the chip is not found, so a wrong address or a board
// revision without the driver degrades to "no haptics" rather than blocking boot.

// Probes for the DRV2605 and configures it for waveform playback. Must be called AFTER
// Touch_Init(), which is what installs the shared I2C driver this reuses -- calling it earlier
// fails on an uninstalled bus.
//
// The bus scan this used to log is DELETED -- it was probing 0x15, the touch controller, and
// breaking it. See the note at the top of haptics.cpp. It also already answered its question:
// the bus carries 0x15 (CST816) and 0x5A (DRV2605).
void haptics_init();

// NOTE ON THIS UNIT: the firmware side is complete and verified by register readback, but the
// motor cannot be made to respond -- the DRV2605 reports an actuator fault with over-current on
// its own diagnostic. See the measurement recorded in haptics.cpp. Every entry point below still
// behaves correctly; they simply drive an output nothing is listening to on this particular
// board. Treat haptics as implemented-but-unconfirmed rather than working or broken.

// Short confirmation buzz. Safe to call from any task -- a single I2C transaction, no blocking
// waits beyond the bus timeout, and a no-op if the chip is absent.
void haptics_pulse();

bool haptics_available();
