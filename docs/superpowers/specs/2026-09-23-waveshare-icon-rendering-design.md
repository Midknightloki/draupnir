# M12 — Crisp icon rendering on the Waveshare

**Status:** design approved 2026-09-23. Implementation not started.
**Supersedes:** the "icon rendering" item parked in `CLAUDE.md` and `docs/HANDOFF.md`.

---

## 1. The problem

Macro icons are unintelligible on the Waveshare knob. The same icons look fine on the M5Dial.

The cause is not the glyphs and not the panel. The companion app rasterises each Lucide glyph to
an **18×18 1 bpp** bitmap and stores it as `icon_xbm`. That size was chosen as an *interchange*
size back when the M5Dial's small ring dots were the target, and the M5Dial still draws it near
1:1, which is why it looks correct there.

The Waveshare does not. `Waveshare_LVGL_Test.ino` defines `ICON_SRC_PX 18` and `ICON_DST_PX 45`
and runs `icon_scale()`, a **nearest-neighbour 2.5× upscale**, before drawing. Every source pixel
becomes a 2–3 pixel block. This is not blur that a better filter would fix: a 1 px stroke inside
an 18 px glyph is already gone before scaling begins, and no resampler recovers detail that was
never sampled. Detailed glyphs — `gitPullRequest`, `alignLeft`, `listOrdered` — cannot survive the
round trip.

**18×18 was never a storage constraint.** The board has 16 MB flash (app partition 31 % used),
8 MB of PSRAM, and a 500 MB microSD. The constraint was an interchange format inherited from the
weaker board.

## 2. Decisions taken before designing

### 2.1 The M5Dial is frozen

It keeps working exactly as it does today, on the current schema, rendering `icon_xbm` at 18×18.
It receives no part of this milestone and no port is owed later.

**Consequence, and the single hardest constraint in this document: every schema and protocol
change here is additive.** `icon_xbm` stays on the wire, keeps its meaning, and is still always
sent. Nothing may be removed or repurposed.

> **Resolved 2026-09-23.** The M5Dial is now **officially retired** — not sold, not supported,
> shipping to nobody — while staying frozen and working for the owner personally. `CLAUDE.md`,
> `AGENTS.md`, `README.md`, `KICKOFF_PROMPT.md`, `Draupnir_Spec.md` §3 and the toolchain doc were
> updated together in this change.
>
> Retirement makes the additive-only rule *more* binding, not less. A retired-but-in-use board
> receives no firmware updates ever, so it can never be brought forward to meet a breaking
> change. "Frozen" is what makes `icon_xbm` permanent.

### 2.2 Built-in library only

Users choose from a curated set. Supplying their own artwork — from the phone or via the SD card —
is **out of scope**. The reported defect is fidelity, and fidelity is fixable without custom art.
This design does not foreclose custom icons later; the `icon_bmp48` path below is exactly the hook
they would use.

### 2.3 The glyph set is compiled in, not pushed over BLE

A loadable font pack was considered and rejected for this milestone. It would require, all at once:

- an `lv_fs` driver bridging LVGL to LittleFS (`LV_USE_FS_STDIO` and `LV_USE_FS_POSIX` are both
  `0` today, and no driver is registered);
- `lv_font_load()` reading the whole font into LVGL's heap, which `lv_conf.h:52` sets to
  **48 KB** — a ~177 KB set needs well over three times that, which realistically forces `PSRAM=opi`, and
  `Draupnir_Spec.md` §3 records PSRAM as deliberately disabled to keep memory configuration out
  of the M6 stability diagnosis;
- a resumable bulk transfer, since ~177 KB over the existing 100-byte acked chunks is ~1,800
  round trips against a command channel with a 30 s timeout;
- integrity verification and a boot-time crash guard, because a malformed font corrupts memory
  *inside the draw path* — it faults on the LVGL task on every frame, including the frame that
  would render an error;
- and an attacker-supplied binary parsed in the render loop, weeks after closing a security gate
  proven by refusal on both boards and immediately before a public release.

It also would not remove the compiled-in set — one is still needed at first boot and as the
known-good fallback. It is an increment on this design, not an alternative to it.

The agility a loader would have bought is instead bought by §5 (the app fills gaps) and by
**M13, OTA firmware update from the app**, which makes promoting an icon from "sent as a bitmap"
to "free and crisp" a routine release rather than an event.

## 3. Architecture

### 3.1 On-device glyph set

A Lucide subset compiled to an **LVGL font, 48 px, 4 bpp** and drawn with `lv_draw_label`.

This is the mechanism the hub chevrons already use — `ring_draw_event_cb()` draws
`LV_SYMBOL_LEFT`/`RIGHT`/`UP` from `lv_font_montserrat_28`, above a comment recording that three
iterations of hand-drawn `lv_draw_line` strokes failed where a typeface glyph worked. Same trick,
larger set.

- **4 bpp**, so glyphs are antialiased. 1 bpp would be crisper than today but still hard-edged.
- **48 px, drawn 1:1.** No scaling anywhere in the path. LVGL cannot scale a bitmap font, and
  scaling is the defect being fixed.
- **Size:** 157 glyphs × (48×48×4/8) ≈ 177 KB plus table overhead. The app partition is 3.34 MB
  with ~1.04 MB used; this takes it to roughly 37 %.
- Recolouring is just the label colour, so the existing contrast treatment — glyph in the
  contrast colour over a one-pixel drop shadow in the opposite colour — carries over unchanged.

### 3.2 The generator, and why it exists

The firmware's glyph set and the app's icon map must agree. Keeping two hand-maintained lists in
step is the failure this design is most likely to suffer six months from now, so neither list is
hand-maintained:

A script (`design/generate_icon_font.py`, to sit beside the existing
`design/generate_app_icons.py`) reads:

- **names** from `companion_app/lib/utils/macro_icons.dart` — the app's `macroIcons` map, already
  the single source of truth and already pinned by `test/macro_icons_test.dart`;
- **codepoints** from the installed `lucide_icons_flutter` package, which defines each icon as an
  `IconData` at a private-use codepoint;

and emits, into `firmware/Waveshare_LVGL_Test/`:

- `lucide_48.c` — the LVGL font, via `lv_font_conv`, containing only those codepoints;
- `icon_names.h` — a **sorted** `{const char *name; uint32_t codepoint;}` table for binary search
  at draw time, plus its length.

The app's map is the source; the firmware's set is derived. Drift becomes a regenerate-and-rebuild
step, not a visual bug someone notices later.

### 3.3 Draw path — four tiers

In `ring_draw_event_cb()`, per wedge, in order:

1. **`macro["icon"]` found in `icon_names.h`** → `lv_draw_label` with the Lucide font at 48 px.
   Crisp, antialiased, zero payload.
2. **Else `macro["icon_bmp48"]` present** → decode 48×48 1 bpp and draw 1:1 through the existing
   `LV_IMG_CF_ALPHA_1BIT` path, with `icon_scale()` bypassed.
3. **Else `macro["icon_xbm"]` present** → today's path exactly: 18×18 decoded, `icon_scale()`d
   2.5× to 45 px, drawn. Preserved so existing profiles and pre-M12 behaviour are unchanged.
4. **Else** → the macro's name as text, the existing `wedge_label_fit()` path.

Tier 3 is why nothing regresses: a profile saved before this milestone renders exactly as it does
now. Tier 4 is the text-label fallback made reachable in M11.

## 4. Protocol changes — all additive

### 4.1 New command: `get_glyphs`

Request: `{"cmd": "get_glyphs"}`

Response: `{"status": "ok", "glyphs": ["play", "pause", ...], "set": "<version>"}`

157 names, roughly 2 KB, chunked by the existing transport. Fetched **once per connection** and
cached in `DraupnirState`.

A device that does not implement it — the frozen M5Dial, or any pre-M12 Waveshare build — returns
an error or nothing. The app treats *any* non-success as "this device has no glyph list", and
consequently never sends `icon_bmp48` to it. That single rule is the whole backward-compatibility
story on the app side.

### 4.2 New macro field: `icon_bmp48`

48×48 1 bpp XBM as hex — 288 bytes, 576 hex characters. Optional.

The app includes it **only** for macros whose `icon` name is absent from the device's glyph list.
When the list is unknown (§4.1) it is never sent at all.

### 4.3 `icon_xbm` — unchanged

Still 18×18, still always sent, still the same meaning. It is what the frozen M5Dial consumes and
what makes an older Waveshare degrade rather than break.

## 5. What this buys

| Situation | Result |
|---|---|
| Known icon, M12 firmware | Font glyph. Crisp, antialiased, **no bitmap on the wire at all** |
| Icon added to the app after that knob shipped | 48×48 from the app — good, not perfect, immediately, with no firmware update |
| Pre-M12 firmware, any icon | 18×18 upscaled, exactly as today. Degraded, never broken |
| M5Dial | Untouched |
| Later firmware release | Promotes icons from tier 2 to tier 1. An optimisation, never a requirement |

Net payload for a typical profile **falls**, because known icons stop shipping a bitmap entirely.

## 6. Risks and edges

**Document size.** `icon_bmp48` is 576 hex chars. A profile where many macros use names the
firmware lacks grows fast — 24 such macros is ~13.8 KB against a `BLE_RX_BUFFER_SIZE` of 8192.
The device already refuses oversized documents (M6 RX bounds) and that refusal now surfaces
through M11's retry UI, so the failure is safe. It should not be left to be *discovered*: the app
must pre-check the serialised document size before sending and say something specific — naming
that the profile uses icons this device's firmware does not have, and that updating it will fix
both the size and the fidelity.

**Flash growth.** ~177 KB is comfortable now. It scales linearly with the icon set, so a future
doubling of the set is a real budget question, not a free one.

**Generator drift.** If `generate_icon_font.py` is not re-run after the app's map changes, the
firmware silently falls to tier 2 for the new names — degraded, not broken, which is the right
failure but also a quiet one. The generator should be runnable from a documented one-liner, and
the `set` version in `get_glyphs` should change whenever the table does.

**`lv_font_conv` is a build-time dependency** (Node). It runs on the developer's machine, not in
CI, and its output is checked in — consistent with how `orbitron_*.c` already works in this tree.

## 7. Out of scope

- User-supplied icon artwork, from the phone or the SD card.
- Any M5Dial change.
- A loadable/pushable font pack (§2.3).
- OTA firmware update — that is **M13**, and this design assumes it is coming.
- The secondary ESP32-U4WDH. **Corrected 2026-09-23** after reading the manufacturer's schematic:
  an earlier draft of this section claimed it had no link to the S3 and was reachable only
  *instead of* it. That is wrong — there is a dedicated UART (S3 GPIO48/38 to U4WDH IO23/IO18),
  and it owns the board's second encoder and a second I2S path. It is a genuinely usable
  co-processor. It stays out of scope here because M12 does not need it, not because it is
  unavailable. See `docs/Waveshare_Hardware_Reference.md` §4.
- Enabling PSRAM. Not needed by this design; still the obvious relief valve if §6's document-size
  edge ever becomes routine.

## 8. Done criteria

1. `design/generate_icon_font.py` regenerates `lucide_48.c` and `icon_names.h` from
   `macro_icons.dart`, and a clean regeneration produces no diff.
2. Firmware compiles; flash use is reported and within budget.
3. On hardware: a macro bound to a known icon draws a crisp antialiased glyph. Specifically the
   glyphs M11 predicted would fail — `gitPullRequest`, `alignLeft`, `listOrdered`, `braces` — are
   legible at ring size.
4. A macro whose name is not in the firmware's table draws the app-supplied 48×48.
5. A profile saved before M12 still renders, and needs no re-save. It renders at **tier 1**, not
   tier 3: the app has always written `icon`, so those names resolve and the font path wins —
   which is the desirable outcome (existing profiles get crisp icons for free). Tier 3 is
   reachable only for an absent, unknown, or legacy-alias `icon` name and must be forced with
   one of those to be tested at all.
6. A macro with no icon still draws its name (tier 4).
7. The M5Dial, unflashed, continues to load and render the same profiles.
8. The app never sends `icon_bmp48` to a device that does not answer `get_glyphs`.
9. Oversized-document pre-check produces a specific, actionable message rather than a refusal.
10. Round trip: save from the app, power-cycle the knob, icons still correct.
