# M9 — Orientation, icons, rotary mode, and M5Dial port parity

*Design, 2026-08-20. Waveshare ESP32-S3 knob (`firmware/Waveshare_LVGL_Test/`).*

Covers the M9 row in `docs/Draupnir_Spec.md` §10 plus three port gaps found by auditing the
M5Dial firmware against the Waveshare one and the companion app.

---

## 1. Why this is mostly a port, not new work

The audit changed the shape of M9 substantially. Every feature here already exists somewhere —
in the app, on the M5Dial, or both — and the Waveshare simply never received it.

| Capability | Companion app | M5Dial | Waveshare |
|---|---|---|---|
| `settings.orientation` | writes it (0–3 dropdown) | consumes it via `setRotation()` | **ignored** |
| `mode: "rotary"` | offers it, validates ≥2 actions | implements it | **silently runs as `play_once`** |
| `icon` / `icon_xbm` | renders Feather icons to 18×18 XBM | draws it via `drawXBitmap()` | **ignored** |
| `icon_xbm` stripped from `get_profiles` | needs only the icon *name* | strips it, streaming | **sends everything** |

Two consequences worth stating plainly:

**Icons are not a greenfield asset pipeline.** An earlier estimate called for choosing an icon
set, building an SVG→LVGL converter, plumbing the schema and adding app UI. All of that exists.
The app already generates the bitmap and stores it in the macro; only rendering is missing.

**`mode: "rotary"` is the most dangerous gap**, because it fails silently in the wrong direction.
A user creates a Rotary Dial macro in the app, and the Waveshare fires both actions in sequence
instead of binding them to the knob. `macro_engine.cpp:448` even carries a comment inherited from
the M5Dial claiming *"delay and rotary modes are handled by the caller"* — describing behaviour
that was never ported. That comment is worse than no comment and must go.

**Dropped from the original M9 row:** *encoder detent alignment*. It was written when selection
was a highlight travelling around a fixed ring. Since the ring rework the selection always lands
centred at 12 o'clock, so there is nothing left to align. What remains is the ~15% mechanical
double-step, which was measured, traced to two genuine contact closures, and deliberately accepted.

Also noted and **not** in scope: `settings.buzzer` and `settings.ledBrightness` appear in the
M5Dial's default JSON but are consumed by neither board. Schema debris, not a port gap.

---

## 2. Acceptance criteria

1. Setting Dial Orientation in the app rotates the display **and** touch, live on save, for all
   four values.
2. A macro with an icon shows that icon on its wedge; a macro without one still shows its name.
3. Firing a `mode: "rotary"` macro enters a rotary screen where the encoder drives `actions[0]`
   clockwise and `actions[1]` counter-clockwise; tapping exits.
4. Swipe-down still kills all running macros, including from the rotary screen.
5. Editing one macro in the app does **not** destroy any other macro's icon — on **either**
   board.
6. Adding a future UI mode requires one table entry, not edits at three separate dispatch sites.

Criteria 1–5 are behavioural or visual and need a human at the board. Criterion 6 is structural and
is judged by reading the result.

---

## 3. Mode dispatch — the refactor everything else lands on

The UI mode is currently a bare enum, and **three separate call sites each decide independently
what a mode means**: `screen_click_cb`, `screen_gesture_cb`, and `encoder_task`. That pattern has
already produced two bugs of identical shape on this branch:

- `screen_click_cb` treats *any* non-ring mode as Settings — `if (ui_mode != UI_RING) {
  settings_enter_requested = true; return; }`. Adding a fourth mode makes tap-to-exit silently open
  the brightness editor.
- Three gesture sites each independently remembered to call `lv_indev_wait_release()`, and the one
  that forgot let the panic gesture fire a macro into the attached host.

Adding rotary as a fourth mode without fixing this all but guarantees a third instance. So the
dispatch table comes first and rotary becomes one entry in it.

### The table

```c
typedef struct {
  const char *name;                             // diagnostics only
  void (*enter)(void);                          // loop() task, lvgl_lock HELD by caller
  void (*exit)(void);                           // loop() task, lvgl_lock HELD by caller
  void (*on_encoder)(int delta);                // encoder_task, lvgl_lock HELD by caller
  bool (*on_tap)(lv_coord_t x, lv_coord_t y);   // LVGL task -- flags/LVGL only
  bool (*on_gesture)(lv_dir_t dir);             // LVGL task -- flags/LVGL only
} ui_mode_def_t;

static const ui_mode_def_t UI_MODES[] = {
  [UI_RING]          = { "ring",       ... },
  [UI_SETTINGS_LIST] = { "settings",   ... },
  [UI_SETTINGS_EDIT] = { "brightness", ... },
  [UI_ROTARY]        = { "rotary",     ... },
};
```

A `NULL` handler means "this mode does not care", which the dispatcher treats exactly as returning
`false`.

### Threading contract — per handler

Each slot runs on a specific task. Writing it into the table's declaration is what turns these from
rules people remember into rules the structure states:

| Handler | Runs on | `lvgl_lock` | May do |
|---|---|---|---|
| `enter` / `exit` | `loop()` | held by caller | anything the loop task may, including `macros_stop_all()` |
| `on_encoder` | `encoder_task` | held by caller | touch LVGL; **never** call the macro engine directly |
| `on_tap` / `on_gesture` | LVGL task | already held by `lv_timer_handler` | set flags, touch LVGL; **never** the macro engine |

`on_tap` and `on_gesture` must use the request forms — `macros_request_fire()`,
`macros_request_stop_all()`, or the new rotary-step request — exactly as today's callbacks do.

### Default-safe fall-through

Both input handlers return `true` when they consume the event. When they return `false` the
dispatcher applies a default, and the defaults are chosen so that a mode which *forgets* something
gets the safe behaviour rather than no behaviour:

- **`on_gesture` returning false for `LV_DIR_BOTTOM` → kill all macros.** This is how the panic
  gesture stays universal without every mode having to remember it. A future mode that ignores
  gestures entirely still cannot trap a running macro behind itself.
- **`on_tap` returning false → do nothing.** Firing a macro is ring-specific and the ring's own
  handler does it. This makes *"no non-ring mode may fire a macro"* structural rather than a rule
  each site must independently observe.

The unconditional `lv_indev_wait_release(indev)` stays at the top of `screen_gesture_cb`, before
dispatch. A gesture is never also a tap, whatever the mode goes on to decide.

---

## 4. Orientation

### Mechanism

Rotation happens **in the panel hardware** via MADCTL (register `0x36`). The vendor already ships
a compile-time 90° path doing exactly two things — set MADCTL in the init sequence, and transform
touch coordinates. This makes both runtime-selectable from `settings.orientation`.

```c
static const uint8_t MADCTL_FOR_ORIENTATION[4] = { 0x00, 0x60, 0xC0, 0xA0 };
```

**Do not use `esp_lcd`'s rotation API or LVGL's software rotation.** Both are dead ends here:
`panel_sh8601_swap_xy()` returns `ESP_ERR_NOT_SUPPORTED` (`esp_lcd_sh8601.c:319`) and `mirror_y`
is unsupported too (`:310`). LVGL's `sw_rotate` would re-rotate every flush in software on a
device already rendering ten stripes per frame (`EXAMPLE_LVGL_BUF_HEIGHT = V_RES / 10`). MADCTL
costs nothing by comparison.

The panel is square (360×360), which is why no dimension swapping is needed anywhere.

### What is proven and what is not

Only `0x00` and `0x60` are proven on this hardware. `0xC0` and `0xA0` are the conventional values
for 180° and 270° and **must be confirmed by eye**. Likewise only the 90° touch transform is
known-good:

| Orientation | MADCTL | Touch transform | Status |
|---|---|---|---|
| 0 (0°) | `0x00` | `x = tx, y = ty` | proven |
| 1 (90°) | `0x60` | `x = ty, y = H − tx` | proven (vendor path) |
| 2 (180°) | `0xC0` | `x = H − tx, y = V − ty` | **derived, unverified** |
| 3 (270°) | `0xA0` | `x = V − ty, y = tx` | **derived, unverified** |

If a rotation renders or touches wrong, the transform is the first suspect, not the MADCTL value.

**180° and 270° are the primary use case, not edge cases.** In active use the cable wants to exit
the top or the right of the dial; any other orientation fouls placement and puts strain on the
port. So the two rotations that are *unproven* are precisely the two that must work. Expect to
iterate on them with the board in hand rather than treating a first pass as done.

### Applying it

Read at display init, and re-applied on the existing profile-reload path under `lvgl_lock()`, so
the screen rotates the moment the app saves. Writing MADCTL at runtime is
`esp_lcd_panel_io_tx_param(io, 0x36, &val, 1)`.

**Open risk:** whether a runtime MADCTL change keeps partial-refresh `esp_lcd_panel_draw_bitmap`
regions correct is unverified. If it misbehaves, the fallback is to apply orientation at boot only
and have the app say a replug is needed. Do not assume it works; check it.

### Storage — and why it differs from brightness

`profiles.json` is the single source of truth. **No NVS.**

Brightness has **one** writer, the device, and the app round-trips a stale value on every macro
edit — so adopting from JSON on save would stomp the knob's value. That is why brightness is
NVS-authoritative with JSON as seed-only.

Orientation will have **two** writers once the Settings menu gains an entry for it. Under the
brightness pattern that breaks either way: if a save does not adopt from JSON the app's dropdown
does nothing; if it does adopt, the next app save stomps the on-device change. The only escape is
the device writing back to JSON, at which point NVS is a redundant second copy. It would also
leave the app's dropdown showing a stale value after any on-device change, since the app renders
from `profiles.json`.

Orientation changes are rare and deliberate, so a ~1–2 KB atomic JSON write per change is fine —
unlike brightness, which moves per encoder detent.

### Not affected

**The encoder direction does not change with orientation.** The user turns the knob from the same
physical position however the puck is mounted, so clockwise stays clockwise. Only the display and
the touch transform rotate.

---

## 5. Icons

### Data

The app renders a Feather icon to an 18×18 monochrome bitmap and stores it as `icon_xbm` — 108
hex characters, 54 bytes — alongside `icon` (the name). Nothing new is needed on the app side.

### Rendering

Parse the hex to 54 bytes, **bit-reverse each byte**, and draw through LVGL's 1-bit alpha path:

```c
lv_img_dsc_t dsc = {
  .header = { .cf = LV_IMG_CF_ALPHA_1BIT, .w = 18, .h = 18 },
  .data_size = 54,
  .data = bits,
};
lv_draw_img_dsc_t idsc;
lv_draw_img_dsc_init(&idsc);
idsc.recolor     = contrast_on(color);   // ALPHA_1BIT carries no colour of its own
idsc.recolor_opa = LV_OPA_COVER;
lv_draw_img(draw_ctx, &idsc, &coords, &dsc);
```

**The bit reversal is mandatory.** XBM is LSB-first; LVGL reads `pos = 7 - (x & 0x7)`
(`lv_img_decoder.c:599`), which is MSB-first. Without reversing, every glyph renders mirrored
within each byte — it will compile and draw something, which is exactly how this class of bug
survives review.

`LV_IMG_CF_ALPHA_1BIT` supplies alpha only; the colour comes from `recolor`/`recolor_opa`
(`lv_draw_img.h:38`). That lets the icon take the same per-wedge black/white contrast choice the
labels already use.

**18×18 is load-bearing, and it is also the shared contract.** The app generates at 18×18 and the
M5Dial draws at 18×18, so the size is fixed by parity, not just by convenience — the hex payload
is the interchange format between all three components.

It is additionally load-bearing inside LVGL: `LV_IMG_BUF_SIZE_ALPHA_1BIT(w,h)` is `((w/8)+1)*h`
while the decoder computes its stride as `(w+7)>>3`. At w=18 both give 3 bytes per row and 54 bytes
total. At any width that is an exact multiple of 8 they **disagree**. Changing the icon size would
therefore break parity *and* silently corrupt rendering — do not.

The hex→bytes parse and the bit reversal are board-independent and could be shared; only the draw
call differs (`lv_draw_img` versus `drawXBitmap`). Sharing them is not required for M9, since the
M5Dial's parser already works, but the split is worth respecting if this is ever factored out.

### Placement

Drawn in `ring_draw_event_cb` where the wedge name label currently goes. A wedge shows its icon if
the macro has one, and falls back to the truncated name if not. The centre stack already carries
the authoritative macro name at 21:1 on black, so the wedge is free to be a glyph — and an iconned
macro sidesteps label truncation entirely.

A profile part-way through being iconned will show a mix of icons and names. That is accepted: the
alternative — icons only — reproduces the "unmarked dial" complaint that sent the ring rework back
to the drawing board.

---

## 6. Rotary macro mode

### Behaviour

Firing a macro whose `mode` is `"rotary"` does not play its actions. It enters a rotary screen
where the encoder drives them:

- clockwise → `actions[0]`
- counter-clockwise → `actions[1]`
- tap → exit back to the ring
- swipe-down → still kills all running macros

The screen is ported from the M5Dial's `drawRotaryUI()`: the macro's name centred, `TURN DIAL`
above, `TAP TO EXIT` below, and left/right indicators, all in the macro's own colour. That already
matches the Waveshare's visual vocabulary — a ring, centre text, chevron indicators — so it should
be built from the existing pieces rather than as a second style.

Tap-to-exit ports directly: the M5Dial's own screen says `TAP TO EXIT`, and the Waveshare has
touch and no knob button, so there is no other candidate gesture.

Swipe-down stays kill-all here, unlike in Settings. Settings can safely reuse it as "close"
**because opening Settings stops every running macro**. Rotary mode does not, so a looping toggle
can still be live while the rotary screen is up — which is exactly when the panic gesture is
wanted.

### The constraint the M5Dial does not have

The M5Dial calls `executeAction()` straight from its loop. The Waveshare cannot: the macro engine
is `loop()`-task only, and `encoder_task` is a separate FreeRTOS task. An encoder turn in rotary
mode must therefore **enqueue** a step rather than execute it.

Add two sentinels alongside the existing `MACRO_CMD_STOP_ALL`, pushed through the same fire queue
so ordering against pending fires is preserved:

```c
#define MACRO_CMD_ROTARY_CW  (-97)
#define MACRO_CMD_ROTARY_CCW (-98)
```

`macros_update()` drains them and executes the single corresponding action of the active rotary
macro from the loop task. `macros_request_rotary_step(int dir)` is the cross-task entry point,
exactly mirroring `macros_request_fire()`.

The active rotary macro is held as a `JsonObject` into `profilesDoc`, so — like every other
`ActiveMacro` — it must be dropped when profiles reload. `macros_stop_all()` already runs on that
path and must clear it too, or a reload while the rotary screen is up is a use-after-free of the
same class H3 fixed.

### Input routing

Rotary is a mode like any other: it is one entry in the dispatch table described in §3, declaring
its own encoder, tap and gesture behaviour. It needs no special-casing anywhere else, which is the
point of doing that refactor first.

### Removing the lying comment

`macro_engine.cpp:448` claims rotary is handled by the caller. It never was. Delete it as part of
implementing the real thing.

---

## 7. `icon_xbm` strip and merge

### Strip on read

`get_profiles` strips `,"icon_xbm":"<hex>"` pairs from the response with a streaming marker match,
as the M5Dial does. The app needs only the icon *name*; the bitmap is display-side data. At ~122
bytes per macro this is ~2 KB per fetch at 16 macros.

The Waveshare's `BleChunkSink` already streams with near-zero peak heap, so unlike the M5Dial —
where an earlier String-based implementation fragmented the heap badly enough to return an empty
payload — this is a bandwidth and latency saving, not a stability fix.

### Merge on save — and why stripping is unsafe without it

`saveProfiles()` in the app sends the **entire** document it holds in memory. `updateMacro()`
replaces one macro wholesale with a record carrying a freshly generated `icon_xbm`, and
`editor_panel.dart:106` is the only place `icon_xbm` is ever written.

So with stripping and no merge: the app's copy has no bitmaps, the user edits one macro, and the
save wipes `icon_xbm` for **every other macro**. This is a live bug on the M5Dial today.

Therefore, in `save_profiles`, before the atomic write: for each macro in the incoming document,
if it has no `icon_xbm` and the currently stored macro at the same `pos` in the same profile does,
carry the stored value across.

This makes stripping safe against **any** client — the app, a hand-edited `profiles.json`, nRF
Connect, or a future desktop client — rather than trusting one client to behave.

### The M5Dial gets the same fix — it is in scope

The M5Dial already strips and does **not** merge, so the icon-wipe is live on it today: edit one
macro in the app and every other macro on that device loses its bitmap. It is a silent data-loss
bug on a supported board.

The requirement for M9 is that a user cannot tell the two devices apart behaviourally, so the merge
is ported to `firmware/M5_M6_config/M5_M6_config.ino`'s `save_profiles` handler as part of this
milestone. The implementations differ — different JSON plumbing, different display stack — but the
observable behaviour must not.

**This is the only M5Dial change in M9.** Its other gaps — no Settings menu, no rotating ring, no
brightness gauge — stay for the board-sequencing pass after the Waveshare reaches M10, per
`CLAUDE.md`. Those are absences of Waveshare-only features rather than divergent behaviour in a
shared one.

The merge happens on the in-memory document before serialization, so it composes with the existing
H4 atomic write (temp file → verify → rename) rather than replacing it.

---

## 8. Keep intact

- `wedge_center_angle()` / `wedge_index_from_point()` — exact inverses over the animated rotation,
  hand-verified. Nothing here needs them changed.
- `lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE)` in `build_ring_ui()`.
- The single unconditional `lv_indev_wait_release(indev)` at the top of `screen_gesture_cb`. A
  gesture is never also a tap; the rotary screen's tap-to-exit does not change that.
- `HOTZONE_MIN_DX` / `HOTZONE_MAX_DY` and the chevron hot zones.
- The centre stack, the tinted bloom, the running-macro pulse, and the draw order that keeps the
  pulse painting last.
- The H4 atomic write path, which §6's merge builds on.
- Threading: LVGL objects only under `lvgl_lock()`; the macro engine is `loop()`-task only;
  callbacks hand off via flags or the fire queue; acquire the lock **before** clearing a request
  flag.

---

## 9. Verification

The compile gate is mandatory. Everything in §2 needs a human at the board. Specific things to
watch, because each is a known trap rather than a general check:

- All four orientations, display **and** touch. 180° and 270° are unverified in both respects.
- Whether a runtime orientation change leaves partial-refresh regions correct (§3's open risk).
- Icons rendering upright and unmirrored — a failed bit reversal still draws *something*.
- A rotary macro binding the knob rather than firing both actions in sequence.
- Editing one macro in the app leaving every other macro's icon intact.
- Swipe-down still killing macros from the rotary screen.
