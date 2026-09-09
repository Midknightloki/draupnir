# M8b — Uncap `pos` Implementation Plan

**Goal:** Make `pos` a stable identifier and ring-ordering key rather than a physical slot, on both
firmwares and in the Companion App, and bump the schema to 3 behind a real version check.

**Architecture:** `runningMacros[]` stops being indexed by `pos` and becomes a fixed pool of 16
entries keyed by identity. Total macros (32) and concurrently-running macros (16) become separate
limits.

**Tech Stack:** Arduino ESP32 core 3.3.11, LVGL 8.4.0, ArduinoJson 7.4.3, M5Unified/M5GFX, Flutter.

**Spec:** `docs/superpowers/specs/2026-08-26-m8b-uncap-pos-design.md` — read it before starting any
task. It carries the reasoning; this plan carries the steps.

## Global Constraints

- `MAX_MACROS 32` (highest legal `pos` is 31), `RUNNING_SLOTS 16`, `SCHEMA_VERSION 3`.
- The macro engine is **`loop()`-task only**. UI callbacks use `macros_request_fire()` /
  `macros_request_stop_all()`. `macros_any_running()` stays the one read-only call safe from the
  LVGL task.
- LVGL objects only under `lvgl_lock()`; acquire the lock **before** clearing a request flag.
- Stop running macros before reloading profiles — every `ActiveMacro` holds a `JsonObject` into
  `profilesDoc`, which reload invalidates.
- **Behaviour parity between boards**, except the one divergence §4 of the spec documents
  explicitly (M5Dial ring stays 16 positions).
- No host tests, no CI. Verification = compile gate + the hardware criteria in spec §7.
- Do not touch the NeoTrellis init loop `for (int i = 0; i < 16; i++)` (~M5Dial line 1249) — it
  iterates physical hardware, not macro slots.

---

### Task 1: Waveshare macro engine — pool keyed by identity

**Files:** Modify `firmware/Waveshare_LVGL_Test/macro_engine.h`, `macro_engine.cpp`

**Interfaces produced:** the public API in `macro_engine.h` is unchanged in signature — callers
keep passing a `pos`. Only the internal representation changes. `NUM_MACRO_SLOTS` is replaced by
`MAX_MACROS` / `RUNNING_SLOTS`; anything outside the engine referencing `NUM_MACRO_SLOTS` must move
to `MAX_MACROS` (Task 2 covers the `.ino`).

- [ ] **Step 1:** In `macro_engine.h`, replace `#define NUM_MACRO_SLOTS 16` with `MAX_MACROS 32`,
      `RUNNING_SLOTS 16`, `SCHEMA_VERSION 3`. Document that the two limits are now distinct and why
      (total storable vs. simultaneously playing).
- [ ] **Step 2:** In `macro_engine.cpp`, rename `ActiveMacro.slotIdx` to `pos` and size the array
      `runningMacros[RUNNING_SLOTS]`. Add a comment that the array is a **pool**, that its index
      carries no meaning, and that `pos` is the identity.
- [ ] **Step 3:** Add two static helpers: `find_running_slot(int pos)` returning the index of the
      active entry playing `pos` or `-1`; `claim_free_slot()` returning a free index or `-1`.
- [ ] **Step 4:** Rewrite `macros_fire(pos)`: reject `pos < 0 || pos >= MAX_MACROS`; use
      `find_running_slot()` for the existing toggle-off path; otherwise `claim_free_slot()`. **If
      the pool is full, log and return without firing** — never evict a running macro.
- [ ] **Step 5:** Rewrite `macros_is_running(pos)` as `find_running_slot(pos) >= 0`. Update
      `macros_stop_all()`, `macros_update()`, `macros_any_running()` to loop `RUNNING_SLOTS` and
      match on `.pos` where they currently index by it. `macros_stop_all()` must still reset `.pos`
      to `-1` alongside clearing `.active`, and must still drop the `JsonObject`.
- [ ] **Step 6:** Compile gate.

```
./arduino-cli.exe compile --fqbn "esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc,FlashMode=qio,FlashSize=16M,PartitionScheme=default_8MB,PSRAM=disabled" firmware/Waveshare_LVGL_Test
```

- [ ] **Step 7:** Commit.

---

### Task 2: Waveshare ring — invert `scan_active_positions()`

**Files:** Modify `firmware/Waveshare_LVGL_Test/Waveshare_LVGL_Test.ino`

**Interfaces consumed:** `MAX_MACROS` from Task 1.

- [ ] **Step 1:** Resize `active_positions[]` to `MAX_MACROS`.
- [ ] **Step 2:** Rewrite `scan_active_positions()` to walk the active profile's `macros` array
      rather than counting `pos` upward. Collect each macro's `pos`; skip and log any outside
      `0..MAX_MACROS-1` or duplicated; stop at `MAX_MACROS` entries.
- [ ] **Step 3:** **Sort `active_positions[0..active_count)` ascending.** This is load-bearing: the
      old upward loop produced sorted order for free, the array walk yields document order, and
      wedge order is `active_positions[]` order — without the sort the ring silently reorders, and
      reorders differently after each save. Add a comment saying exactly that.
- [ ] **Step 4:** Add a new engine accessor rather than reaching into `profilesDoc` from the
      `.ino`: `int profiles_active_macro_count()` and `int profiles_macro_pos_at(int idx)` in
      `macro_engine.{h,cpp}`, so the `.ino` stays free of document internals — matching how it
      already goes through `profiles_find_macro()`.
- [ ] **Step 5:** Compile gate (same command as Task 1), then commit.

---

### Task 3: Waveshare — schema version check

**Files:** Modify `firmware/Waveshare_LVGL_Test/macro_engine.cpp`, `ble_engine.cpp`

- [ ] **Step 1:** Bump the default-JSON literal from `"version": 2` to `"version": 3`.
- [ ] **Step 2:** Add `static bool schema_version_ok(JsonDocument &doc)` — true when `version` is
      absent (treat as legacy, accept) or `<= SCHEMA_VERSION`. Refuse only versions **above** what
      this firmware knows; reading v2 must keep working, since the change is a strict relaxation.
- [ ] **Step 3:** In the load path, refuse a too-new document exactly as a parse failure is
      handled today — fall back to the built-in defaults and log. Booting to an empty ring because
      some future client wrote a v4 file is the worse failure.
- [ ] **Step 4:** In `ble_engine.cpp`'s `save_profiles`, validate **before the atomic rename
      commits**, so a refused document leaves the stored profiles untouched. Report the refusal
      back over BLE rather than failing silently.
- [ ] **Step 5:** Compile gate, then commit.

---

### Task 4: M5Dial — same pool change, engine only

**Files:** Modify `firmware/M5_M6_config/M5_M6_config.ino`

- [ ] **Step 1:** Rename `ActiveMacro.keyIndex` to `pos`; keep the array at 16 but make it a pool.
      Add the same "index carries no meaning" comment.
- [ ] **Step 2:** Add `findRunningSlot(int pos)` / `claimFreeSlot()`; rewrite
      `fireMacro(macro, pos)` to use them, with the same pool-full refusal.
- [ ] **Step 3:** Update `updateMacros()` and the `anyRunning` scan (~line 303) to loop the pool.
- [ ] **Step 4:** `killAllMacros()` — the entanglement. It currently does
      `trellis.pixels.setPixelColor(i, 0)` using the array index, assuming index == key index.
      Change to `if (trellisFound && slot.pos < 16) trellis.pixels.setPixelColor(slot.pos, 0)`.
      **Do not touch the trellis init loop** — it iterates 16 real keys.
- [ ] **Step 5:** Ring draw (~line 282) reads `runningMacros[i].active` while walking its 16 fixed
      ring positions. Route it through `findRunningSlot(i) >= 0`. The ring stays 16 positions —
      that divergence is deliberate (spec §4).
- [ ] **Step 6:** Bump its default `"version": 2` → `3`; add the same version check on load and on
      BLE save.
- [ ] **Step 7:** Compile gate, then commit.

```
./arduino-cli.exe compile --fqbn "m5stack:esp32:m5stack_dial:USBMode=default,CDCOnBoot=cdc,FlashSize=8M,PartitionScheme=default_8MB" firmware/M5_M6_config
```

---

### Task 5: Companion App — deck renders what exists

**Files:** Modify `companion_app/lib/screens/dashboard_screen.dart`, and the state class holding
macro mutation (`companion_app/lib/state/draupnir_state.dart` or equivalent — locate it first).

- [ ] **Step 1:** Replace `itemCount: 16` with `state.currentMacros.length + 1` (the trailing "+"),
      sorted ascending by `pos`. Hide the "+" once the profile holds `MAX_MACROS` (32).
- [ ] **Step 2:** The tile at index `n` now renders `sortedMacros[n]`, not "the macro whose `pos`
      is `n`". Remove the `firstWhere(pos == index)` lookup and the empty-cell branch.
- [ ] **Step 3:** **`_editingKeyIdx` must hold the `pos`, not the grid index.** They were the same
      number and are not any more; getting this wrong edits the wrong macro in any gapped profile.
      Audit every read of it.
- [ ] **Step 4:** "+" appends a macro at the **lowest free `pos`** so deletion gaps are reused
      before the ceiling is approached.
- [ ] **Step 5:** Long-press a tile deletes that macro, with a confirmation dialog.
- [ ] **Step 6:** `flutter build apk --debug` (or `flutter analyze` if a full build is
      unavailable), then commit.

---

### Task 6: Documentation

**Files:** Modify `docs/Draupnir_Spec.md`, `docs/HANDOFF.md`

- [ ] **Step 1:** §6 — delete the "⚠️ NOT YET IMPLEMENTED" box; state the implemented behaviour,
      the 32/16 limits, and that `version` is now checked.
- [ ] **Step 2:** §6 — correct the v3 bump rationale. It cannot make already-deployed v2 firmware
      refuse anything, because that firmware has no check. The honest claim: from v3 onward, a
      version a device does not understand is refused rather than silently mangled.
- [ ] **Step 3:** §3 and `HANDOFF.md` — document the accepted divergence: on the M5Dial a macro
      above `pos` 15 stores, loads, fires and stops but does not appear on its 16-position ring.
      Say why it was accepted and where the fix belongs.
- [ ] **Step 4:** §10 — M8b row to Done once hardware verification passes. **Not before.**
- [ ] **Step 5:** Commit.

---

## Verification

Compile gates are mine. The hardware criteria are in spec §7 and need the owner at the boards —
**criterion 1 on each board is "no regression on an existing profile"**, run before any uncapped-
`pos` testing, because the pool rewrite touches every fire, stop and query path in the product.
