#pragma once
#include <Arduino.h>

// Verbose input/macro tracing, compiled out by default.
//
// Kept rather than deleted because it is what made the M6 H3 verification possible: before it
// existed, a tap that never fired a macro was indistinguishable from a tap that never arrived,
// and whether a macro was actually running when a profile save landed was unobservable. Both
// questions came up and both were answered by these lines (see docs/HANDOFF.md).
//
// It is off by default because it emits a line per touch event and several per macro fire, which
// is far too noisy for normal running and would drown the [diag]/[ble] logs that matter.
//
// Set to 1 when diagnosing input handling or macro firing, then flash. Costs nothing when 0 --
// the arguments are not evaluated.
#define DRAUPNIR_TRACE_INPUT 0

#if DRAUPNIR_TRACE_INPUT
#define TRACE(...) Serial.printf(__VA_ARGS__)
#else
#define TRACE(...) \
  do {             \
  } while (0)
#endif
