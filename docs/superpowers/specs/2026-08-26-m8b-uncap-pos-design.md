# M8b — Uncap `pos`, schema v3

*Design spec · 2026-08-26 · supersedes the "NOT YET IMPLEMENTED" box in `docs/Draupnir_Spec.md` §6*

## 1. What this closes

`docs/Draupnir_Spec.md` §6 has declared since v3 that `pos` is **a stable identifier plus the ring
ordering key**, no longer capped at 15, no longer a physical grid slot. The firmware never
implemented that. The spec carries a warning box admitting it.

The concrete defect: a macro with `pos > 15` written by any client other than the Companion App —
nRF Connect, a hand-crafted `save_profiles`, a future desktop client — is **silently invisible and
unfireable**. It sits in flash and in `profilesDoc`, is never added to `active_positions[]`, never
drawn, never selectable, and is rejected outright by `macros_fire()`. Nothing reports this.

M8b makes the firmware match the spec, and extends the Companion App so a normal user flow can
actually produce such a macro.

## 2. Decisions

| Decision | Value | Why |
|---|---|---|
| Storage / ring ceiling | **32** macros per profile | Past the point the ring stays legible, so the cap stops being the binding constraint. ~600 bytes over today. |
| Concurrent running macros | **16** | Unchanged in count, but now a *separate* limit. Total-macros and simultaneously-running were the same number only because one array served both jobs. |
| Scope | Both firmwares **and** the Companion App deck | Firmware-only leaves the feature latent: the app's fixed 16-cell grid means no normal flow creates `pos > 15`. |
| M5Dial ring | **Unchanged**, stays 16 fixed positions | Its ring is `angle = -PI/2 + (i * PI*2/16.0)` — structurally different from the Waveshare's dynamic wedges. Rewriting it is the deferred M5Dial UI catch-up (`CLAUDE.md` work order), not M8b. |
| Schema `version` | **2 → 3**, both defaults, **plus a real check** | See §5. The bump is decorative without it. |

### The ceiling is headroom, not a target

At 32 macros a wedge spans 11°, and the 45 px icons stop fitting somewhere around 16–20 macros.
Legibility binds long before the cap does. 32 exists so that non-ring clients (a desktop config
tool, scripted provisioning) have room, and so the number stops being 16 — not because a 32-macro
ring is usable.

## 3. The structural change

**The cap is not `NUM_MACRO_SLOTS`. The cap is that `runningMacros[]` is indexed *by* `pos`.**

```c
runningMacros[pos].active = true;     // <-- this is the whole problem
```

`NUM_MACRO_SLOTS` is merely that array's size. Raising it to 32 would "work" and would be the wrong
fix: it permanently ties bytes of running-macro state to the highest addressable `pos`, so the next
uncapping pays again.

`runningMacros[]` becomes a **pool of `RUNNING_SLOTS` (16) entries, keyed by identity**. Each entry
records the `pos` it is playing. Lookups become a linear search over 16 entries — trivial next to
the JSON walk `profiles_find_macro()` already does on every fire.

```c
#define MAX_MACROS     32   /* storage + ring ceiling: highest legal pos is 31 */
#define RUNNING_SLOTS  16   /* concurrently-playing macros */
```

Both firmwares take the same change. The macro engine is shared-layer per `CLAUDE.md`; only the
display differs.

### Waveshare — `macro_engine.cpp`

- `ActiveMacro.slotIdx` already stores the `pos` and is currently written but never read as
  identity. It becomes the search key; rename to `pos` so its role is unambiguous.
- `macros_fire(pos)` — validate `0 <= pos < MAX_MACROS`; search the pool for an active entry with
  that `pos` (toggle-off path); otherwise claim a free entry. **If the pool is full, drop the fire
  and log it** — do not silently overwrite a running macro.
- `macros_is_running(pos)`, `macros_stop_all()`, `macros_update()`, `macros_any_running()` — loop
  `RUNNING_SLOTS`, matching on `.pos` where they currently index by it.

### `scan_active_positions()` inverts

Today it asks, for each `pos` in 0–15, "is there a macro here?":

```c
for (int i = 0; i < NUM_MACRO_SLOTS; i++)
  if (!profiles_find_macro(i).isNull()) active_positions[active_count++] = i;
```

That cannot extend to an uncapped `pos` — the loop bound *is* the cap. It inverts to walking the
profile's macro array and collecting the positions that exist.

**The sort is load-bearing and easy to lose.** The old loop produced `active_positions[]` in
ascending `pos` order as a side effect of counting upward. The array walk yields *document* order,
which is whatever the app happened to serialize. Ring wedge order is `active_positions[]` order, so
without an explicit ascending sort the wedges silently reorder themselves — and worse, reorder
differently after each save. **Sort ascending explicitly.**

Positions outside `0..MAX_MACROS-1`, and duplicates, are dropped with a log line rather than
accepted; and no more than `MAX_MACROS` entries are collected.

### M5Dial — engine only

Same pool change to `runningMacros[16]`, `fireMacro(macro, pos)`, `updateMacros()`,
`killAllMacros()`. Its struct field is `keyIndex`; it becomes `pos` for the same reason.

**One entanglement to handle carefully.** `killAllMacros()` contains:

```c
if (trellisFound) trellis.pixels.setPixelColor(i, 0);
```

where `i` is the running-macro array index — i.e. it assumes index == NeoTrellis key index. Under a
pool that assumption breaks. It becomes a guarded lookup on the slot's `pos`:

```c
if (trellisFound && slot.pos < 16) trellis.pixels.setPixelColor(slot.pos, 0);
```

The NeoTrellis genuinely has 16 physical keys. **The `for (int i = 0; i < 16; i++)` at the trellis
init site (~line 1249) is correct and must not be touched** — it is iterating hardware, not macro
slots. Only the loops over `runningMacros[]` change.

The ring draw at ~line 282 reads `runningMacros[i].active` while walking its 16 fixed ring
positions, to draw the green running indicator. That becomes a call to the by-`pos` lookup helper.

## 4. What M8b deliberately does not do

**On the M5Dial, a macro above `pos` 15 stores, loads, fires and stops correctly — but does not
appear on its ring.** Its ring draws 16 fixed dots and looks up which macro sits at each. Such a
macro is reachable from the Companion App and over BLE; it is not selectable on that board's
screen.

This is a real, user-visible divergence between the two supported boards, accepted deliberately
because closing it means porting the dynamic-wedge ring to a 240×240 M5GFX stack with no LVGL —
most of the deferred M5Dial catch-up, and its own hardware rounds. It belongs with the M5Dial
security gate and the non-atomic write, not here.

**It must be documented in `docs/Draupnir_Spec.md` §3 and `docs/HANDOFF.md`, not left to be
rediscovered.**

## 5. `version` — the bump needs a check to mean anything

`docs/Draupnir_Spec.md` §6 justifies the v3 bump as existing "so *v2 firmware refuses a v3 file*
rather than silently dropping every macro with `pos > 15`."

**Neither firmware reads `version` at all.** There is no check in either sketch — the only
occurrences are the two default-JSON literals. v2 firmware handed a v3 file loads it and drops the
macros, which is precisely the outcome the bump was supposed to prevent. Bumping the number alone
is decorative.

So M8b adds the check:

```c
#define SCHEMA_VERSION 3
```

On load, a document declaring `version > SCHEMA_VERSION` is **refused**. Reading v2 stays fine —
the change is a strict relaxation, and a v2 file is a valid v3 file.

**Refusal must not brick the ring.** A device that rejects its config needs somewhere to stand:

- On **BLE save**: reject before the atomic write commits, leave the stored profiles untouched, and
  report the refusal to the app. The existing atomic write path (temp → verify → rename) already
  gives this for free on the Waveshare — the refusal is one more validation ahead of the rename.
- On **boot**: if the stored file is refused, fall back to the built-in defaults exactly as a parse
  failure does today, and log it. Booting to an empty ring because a future client wrote a v4 file
  is a worse failure than running the defaults.

Correct the §6 rationale while we are there: the bump protects *future* clients and firmware. It
cannot retroactively make already-deployed v2 firmware refuse anything, because that firmware has
no check. The honest claim is that from v3 onward, a version a device does not understand is
refused rather than silently mangled.

## 6. Companion App — the deck

`dashboard_screen.dart:576` renders `itemCount: 16` where the grid index *is* the `pos`, and empty
cells double as the creation affordance ("editor mode → tap empty cell 7 → edit").

That mapping cannot survive an uncapped `pos`, and it already misrepresents the device: the app
draws 16 cells while the ring draws one wedge per *existing* macro and skips gaps entirely. A
profile with macros at `pos` 0 and 9 is two adjacent wedges on the knob and two distant cells in
the app.

**New model — the deck renders what exists:**

- Macros in ascending `pos` order, followed by a single trailing **"+"** tile.
- **"+"** appends a new macro at the **lowest free `pos`**, so gaps left by deletion are reused
  before the ceiling is approached. Hidden once the profile holds `MAX_MACROS`.
- **Long-press a tile deletes** that macro, with confirmation.
- Tapping a tile keeps both current behaviours: fire in normal mode, select for editing in editor
  mode.

`_editingKeyIdx` currently holds a grid index that doubles as `pos`. Under the new model those are
different numbers; it must hold the **`pos`**, not the grid position, or editing the fifth tile in
a gapped profile edits the wrong macro.

The app gains no `version` handling in M8b — it does not read the field today, and teaching it to
negotiate schema versions is its own piece of work.

## 7. Verification

No host test framework and no CI; this project verifies by stated criterion, compile gate, and
hardware observation. Each item below is a criterion someone can run.

**Compile gates (mine):** Waveshare FQBN, M5Dial FQBN, `flutter build` for the app.

**Hardware, Waveshare:**

1. Existing 4-macro profile still loads, renders 4 wedges, fires and stops — **no regression is the
   first criterion**, since the pool rewrite touches every fire path.
2. A profile with a macro at `pos` 20 (written from the app via "+", or by hand) renders it on the
   ring, selects it, and fires it.
3. Delete a macro from the middle; the ring re-lays out with no gap, and the wedges keep ascending
   `pos` order rather than reordering.
4. Fire 16 toggle macros, then a 17th: the 17th is refused and logged, and the 16 running ones are
   unharmed.
5. Swipe-down kill-all still stops everything, including a macro above `pos` 15.
6. A file declaring `"version": 4` is refused; the device keeps its previous profiles rather than
   booting empty.

**Hardware, M5Dial:**

7. Existing profile still loads, fires, and kills-all — including the NeoTrellis LED clear if a
   pad is attached.
8. A macro at `pos` 20 fires when triggered from the app, and does **not** appear on the ring
   (confirming the documented gap, not a bug).

**App:** create a 17th macro with "+", confirm it lands at the lowest free `pos`; long-press
delete; confirm editing a tile in a gapped profile edits the right macro.

## 8. Risk

The pool rewrite touches **every** macro fire, stop, and query path on both boards. That is the
core runtime of the product, and its failure mode — a macro that fires the wrong thing, or HID
state left held down — is exactly the class this project has shipped before (the "Sign-off"
splice, the H3 use-after-free).

Mitigation is ordering: criterion 1 on each board is *no regression on an existing profile*, run
before any uncapped-`pos` testing. The threading rules in `CLAUDE.md` §"Threading rules" bind
unchanged — the pool is still `loop()`-task only, and `macros_any_running()` remains the one
read-only call safe from the LVGL task.
