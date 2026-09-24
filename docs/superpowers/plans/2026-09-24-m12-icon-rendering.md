# M12 — Crisp Icon Rendering Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Macro icons render crisply on the Waveshare knob, drawn from the `icon` name via a compiled-in glyph set, with an app-supplied 48×48 bitmap covering names the firmware does not know.

**Architecture:** A generator derives an LVGL font and a sorted name→codepoint table from the companion app's `macroIcons` map. The firmware looks the macro's `icon` name up in that table and draws the glyph with `lv_draw_label`; if the name is unknown it falls back to a new `icon_bmp48` field, then to today's 18×18 `icon_xbm`, then to the macro's name as text. A new `get_glyphs` command lets the app discover which names the device knows so it only pays the bitmap cost for gaps.

**Tech Stack:** Python 3.11 (generator), `lv_font_conv` (Node, build-time only), LVGL 8.4, Arduino ESP32 core, ArduinoJson 7, Flutter/Dart.

**Spec:** `docs/superpowers/specs/2026-09-23-waveshare-icon-rendering-design.md` — read it before Task 1; this plan argues from it.

## Global Constraints

Every task's requirements implicitly include these.

- **Additive only.** `icon_xbm` keeps its name, its 18×18 size, its meaning, and is still always sent. Nothing existing may be removed, renamed, or repurposed. The retired-but-in-use M5Dial reads these fields and will never be updated. (Spec §2.1)
- **No scaling of font glyphs.** The font is generated at 48 px and drawn at 48 px. Nearest-neighbour upscaling is the defect being fixed. (Spec §3.1)
- **The app's `macroIcons` map is the single source of truth for icon names.** The firmware's table is generated from it, never hand-edited. (Spec §3.2)
- **Icon dimensions:** source font 48 px, 4 bpp. `icon_bmp48` is 48×48 1 bpp = 288 bytes = **576 hex characters**. `icon_xbm` remains 18×18 = 54 bytes = **108 hex characters**.
- **`lv_font_conv` flags must match the existing convention** recorded in `orbitron_18.c`'s header: `--bpp 4 --format lvgl --no-compress --lv-include lvgl.h`. `--no-compress` is mandatory because `lv_conf.h:410` sets `LV_USE_FONT_COMPRESSED 0`.
- **Firmware has no host test framework.** Its gate is `arduino-cli compile` plus hardware. Dart logic is testable and must be tested.
- **Waveshare FQBN** (one line, exact):
  `esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc,FlashMode=qio,FlashSize=16M,PartitionScheme=default_8MB,PSRAM=disabled`

---

## File Structure

**Created:**
- `design/generate_icon_font.py` — the generator. Reads the app's icon map, emits the font and the name table.
- `tools/fonts/Lucide.ttf` — the font input, placed beside the existing `Orbitron.ttf`. **Gitignored**, matching the repo's standing policy that TTFs are build inputs and the generated `.c` table is what ships.
- `firmware/Waveshare_LVGL_Test/lucide_48.c` — generated LVGL font. Never hand-edited.
- `firmware/Waveshare_LVGL_Test/icon_names.h` — generated sorted name→codepoint table plus set version. Never hand-edited.
- `companion_app/test/icon_gap_test.dart` — tests for the gap/size logic.

**Modified:**
- `firmware/Waveshare_LVGL_Test/lv_conf.h:399` — add `LV_FONT_DECLARE(lucide_48)`.
- `firmware/Waveshare_LVGL_Test/Waveshare_LVGL_Test.ino` — icon lookup, tier-1 and tier-2 draw paths.
- `firmware/Waveshare_LVGL_Test/ble_engine.cpp` — `get_glyphs` command.
- `companion_app/lib/utils/icon_generator.dart` — size-parameterised rasteriser.
- `companion_app/lib/state/draupnir_state.dart` — glyph-list fetch/cache, gap-only `icon_bmp48`, size pre-check.
- `companion_app/lib/screens/editor_panel.dart` — pass the gap set through on save.

---

## Task 1: The generator and its artifacts

**Files:**
- Create: `design/generate_icon_font.py`
- Create: `tools/fonts/Lucide.ttf` (copied from the pub cache; **gitignored, not committed** — see Step 2)
- Create: `firmware/Waveshare_LVGL_Test/icon_names.h` (generated)
- Create: `firmware/Waveshare_LVGL_Test/lucide_48.c` (generated)

**Interfaces:**
- Consumes: `companion_app/lib/utils/macro_icons.dart` (`macroIcons` map), the `lucide_icons_flutter` package's `lib/lucide_icons.dart`.
- Produces: `ICON_NAMES[]`, `ICON_NAMES_COUNT`, `ICON_SET_VERSION`, `icon_name_entry_t` (all in `icon_names.h`), and `lv_font_t lucide_48` (in `lucide_48.c`).

- [ ] **Step 1: Install the converter**

`lv_font_conv` is not on PATH. Install it once:

```bash
npm install -g lv_font_conv
lv_font_conv --version
```

Expected: a version string. If npm is unavailable, stop and raise it — this task cannot proceed without it.

- [ ] **Step 2: Vendor the font**

```bash
cp "$HOME/AppData/Local/Pub/Cache/hosted/pub.dev/lucide_icons_flutter-3.1.15/assets/lucide.ttf" \
   tools/fonts/Lucide.ttf
ls -l tools/fonts/Lucide.ttf
```

This is the family named `Lucide` in the package's `pubspec.yaml` — the one `LucideIcons.<name>` resolves to.

**It is deliberately NOT committed.** `.gitignore:44-46` ignores `tools/fonts/` with an explicit policy: *"The generated LVGL .c font tables are what get committed; the source TTF is a build input, not source."* `Orbitron.ttf` has never been tracked either. Anyone regenerating obtains the TTF by re-running this step from their own pub cache; the generated `lucide_48.c` is what the build and the repo actually depend on.

- [ ] **Step 3: Write the generator**

Create `design/generate_icon_font.py`:

```python
#!/usr/bin/env python3
"""Generate the firmware's Lucide glyph set from the companion app's icon map.

The app's macroIcons map is the single source of truth for which icons exist (see
docs/superpowers/specs/2026-09-23-waveshare-icon-rendering-design.md 3.2). This script derives
the firmware's font and name table from it, so the two can only drift by someone failing to
re-run this -- which shows up as icons degrading to the app-supplied bitmap, never as a mismatch
that renders the wrong glyph.

Usage:  python design/generate_icon_font.py
"""
import hashlib
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
MACRO_ICONS = REPO / "companion_app/lib/utils/macro_icons.dart"
FONT_TTF = REPO / "tools/fonts/Lucide.ttf"
OUT_FONT = REPO / "firmware/Waveshare_LVGL_Test/lucide_48.c"
OUT_NAMES = REPO / "firmware/Waveshare_LVGL_Test/icon_names.h"
FONT_PX = 48

# The installed package. Pinned by companion_app/pubspec.lock; update here if that version moves.
LUCIDE_PKG = Path.home() / (
    "AppData/Local/Pub/Cache/hosted/pub.dev/lucide_icons_flutter-3.1.15/lib/lucide_icons.dart"
)

# "'play': LucideIcons.play," -- the app map's entry shape.
MAP_ENTRY = re.compile(r"^\s*'([A-Za-z0-9_]+)':\s*LucideIcons\.([A-Za-z0-9_]+)\s*,", re.M)
# "static const IconData play = const IconData(57660," -- the package's declaration shape.
DECL = re.compile(
    r"static const IconData\s+([A-Za-z0-9_]+)\s*=\s*const IconData\(\s*(\d+)\s*,", re.M
)


def die(msg):
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(1)


def main():
    if not MACRO_ICONS.exists():
        die(f"cannot find {MACRO_ICONS}")
    if not FONT_TTF.exists():
        die(f"cannot find {FONT_TTF} -- see this script's task in the M12 plan, step 2")
    if not LUCIDE_PKG.exists():
        die(f"cannot find the lucide package at {LUCIDE_PKG}; run `flutter pub get` first")

    entries = MAP_ENTRY.findall(MACRO_ICONS.read_text(encoding="utf-8"))
    if not entries:
        die("parsed zero icons out of macro_icons.dart -- has the map's format changed?")

    codepoints = {m[0]: int(m[1]) for m in DECL.findall(LUCIDE_PKG.read_text(encoding="utf-8"))}

    resolved = {}
    missing = []
    for name, ident in entries:
        cp = codepoints.get(ident)
        if cp is None:
            missing.append(f"{name} -> LucideIcons.{ident}")
        else:
            resolved[name] = cp
    if missing:
        die("these map entries have no codepoint in the installed package:\n  "
            + "\n  ".join(missing))

    # Sorted by byte order, because the firmware binary-searches with strcmp().
    ordered = sorted(resolved.items(), key=lambda kv: kv[0].encode("utf-8"))

    # Version = a hash of exactly what the device can draw, so it changes if and only if the
    # set does. The app uses it to know whether its cached glyph list is still valid.
    digest = hashlib.sha256(
        "".join(f"{n}:{c};" for n, c in ordered).encode("utf-8")
    ).hexdigest()[:12]

    write_names_header(ordered, digest)
    run_font_conv([c for _, c in ordered])

    print(f"generated {len(ordered)} glyphs, set version {digest}")
    print(f"  {OUT_NAMES}")
    print(f"  {OUT_FONT}")


def write_names_header(ordered, digest):
    lines = [
        "// GENERATED by design/generate_icon_font.py -- DO NOT EDIT.",
        "//",
        "// Derived from companion_app/lib/utils/macro_icons.dart, which is the single source of",
        "// truth for icon names. Re-run the generator after changing that map; a name present in",
        "// the app but missing here degrades to the app-supplied icon_bmp48, which is correct but",
        "// silent, so the regeneration step is easy to forget and worth checking in review.",
        "//",
        "// Sorted by strcmp order: icon_codepoint() binary-searches this table.",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        "typedef struct {",
        "  const char *name;",
        "  uint32_t    cp;",
        "} icon_name_entry_t;",
        "",
        f'#define ICON_SET_VERSION "{digest}"',
        f"#define ICON_NAMES_COUNT {len(ordered)}",
        "",
        "static const icon_name_entry_t ICON_NAMES[ICON_NAMES_COUNT] = {",
    ]
    width = max(len(n) for n, _ in ordered) + 4
    for name, cp in ordered:
        # The comma between the two fields is load-bearing and easy to lose inside the
        # padding: without it a row reads `{ "activity"  0xE038 }`, which is a C syntax error
        # rather than anything that degrades. verify_header() exists because that shipped once.
        field = f'{chr(34)}{name}{chr(34)},'
        lines.append(f'  {{ {field:<{width}} 0x{cp:04X} }},')
    lines += ["};", ""]
    text = "\n".join(lines)
    verify_header(text, len(ordered))
    OUT_NAMES.write_text(text, encoding="utf-8", newline="\n")


ROW = re.compile(r'^\s*\{\s*"[^"]+"\s*,\s*0x[0-9A-Fa-f]+\s*\},$')


def verify_header(text, expected):
    """Check the emitted table is valid C before it reaches disk.

    Nothing else in this milestone parses or compiles this file until Task 2 includes it, so a
    malformed row would otherwise surface as a firmware build failure several tasks downstream,
    with the generator -- the actual culprit -- long since reviewed and accepted.
    """
    rows = [ln for ln in text.splitlines() if ln.strip().startswith('{ "')]
    bad = [ln for ln in rows if not ROW.match(ln)]
    if bad:
        die("generated rows are not valid C initializers; first offender:\n  " + bad[0])
    if len(rows) != expected:
        die(f"emitted {len(rows)} table rows but resolved {expected} icons")


def run_font_conv(codepoints):
    # Flags match the convention recorded in orbitron_18.c's generated header. --no-compress is
    # mandatory: lv_conf.h sets LV_USE_FONT_COMPRESSED 0, and a compressed font against that
    # config renders garbage rather than failing to build.
    ranges = ",".join(f"0x{c:04X}" for c in codepoints)
    cmd = [
        "lv_font_conv",
        "--font", str(FONT_TTF),
        "--size", str(FONT_PX),
        "--bpp", "4",
        "--format", "lvgl",
        "--range", ranges,
        "--no-compress",
        "--lv-include", "lvgl.h",
        "-o", str(OUT_FONT),
    ]
    try:
        subprocess.run(cmd, check=True)
    except FileNotFoundError:
        die("lv_font_conv not found. Install it: npm install -g lv_font_conv")
    except subprocess.CalledProcessError as e:
        die(f"lv_font_conv failed with exit {e.returncode}")


if __name__ == "__main__":
    main()
```

- [ ] **Step 4: Run it**

```bash
cd "F:/Projects/Draupnir/draupnir"
python design/generate_icon_font.py
```

Expected: `generated 157 glyphs, set version <12 hex chars>` and both files written. If the count is not 157, the app map changed since this plan was written — that is fine, but note the new number, it is used again in Task 8.

- [ ] **Step 5: Verify the generation is deterministic**

```bash
python design/generate_icon_font.py
git diff --stat firmware/Waveshare_LVGL_Test/icon_names.h firmware/Waveshare_LVGL_Test/lucide_48.c
```

Expected: **no diff.** A second run must produce byte-identical output — otherwise every rebuild churns the repo and real changes get lost in the noise. If `lucide_48.c` differs between runs, check whether `lv_font_conv` is embedding a timestamp and strip it in `run_font_conv`.

- [ ] **Step 6: Sanity-check the table**

`verify_header()` now checks every row's shape inside the generator, on every run. This step
independently re-derives sort order and uniqueness — the two properties the generator cannot
check about itself — and re-checks the row shape, so a bug in `verify_header` cannot also hide
the class of defect it was added to catch.

```bash
head -30 firmware/Waveshare_LVGL_Test/icon_names.h
grep -c '{ "' firmware/Waveshare_LVGL_Test/icon_names.h
python - <<'PY'
import re, pathlib
t = pathlib.Path("firmware/Waveshare_LVGL_Test/icon_names.h").read_text()
names = re.findall(r'\{\s*"([^"]+)"', t)
assert names == sorted(names, key=lambda s: s.encode()), "table is not in strcmp order"
assert len(names) == len(set(names)), "duplicate names in table"
print(f"ok: {len(names)} names, sorted, unique")
PY
```

Expected: the count matches Step 4, and the assertions pass. Sort order is load-bearing — the binary search in Task 2 silently returns wrong answers on an unsorted table.

- [ ] **Step 7: Commit**

```bash
git add design/generate_icon_font.py \
        firmware/Waveshare_LVGL_Test/icon_names.h firmware/Waveshare_LVGL_Test/lucide_48.c
git commit -m "feat(m12): generate the firmware glyph set from the app's icon map"
```

---

## Task 2: Firmware — name lookup and the tier-1 font draw

**Files:**
- Modify: `firmware/Waveshare_LVGL_Test/lv_conf.h:399`
- Modify: `firmware/Waveshare_LVGL_Test/Waveshare_LVGL_Test.ino` (includes, new helpers, `ring_draw_event_cb`)

**Interfaces:**
- Consumes: `ICON_NAMES`, `ICON_NAMES_COUNT` from Task 1; `lucide_48` from Task 1.
- Produces: `static uint32_t icon_codepoint(const char *name)` returning 0 when unknown; `static int icon_utf8(uint32_t cp, char *out)` returning bytes written.

- [ ] **Step 1: Declare the font to LVGL**

In `lv_conf.h`, line 399 currently reads:

```c
#define LV_FONT_CUSTOM_DECLARE   LV_FONT_DECLARE(orbitron_14) LV_FONT_DECLARE(orbitron_18) LV_FONT_DECLARE(orbitron_24) LV_FONT_DECLARE(orbitron_bold_24)
```

Append the new font:

```c
#define LV_FONT_CUSTOM_DECLARE   LV_FONT_DECLARE(orbitron_14) LV_FONT_DECLARE(orbitron_18) LV_FONT_DECLARE(orbitron_24) LV_FONT_DECLARE(orbitron_bold_24) LV_FONT_DECLARE(lucide_48)
```

- [ ] **Step 2: Include the table**

In `Waveshare_LVGL_Test.ino`, beside the existing `#include "icons.h"`:

```c
#include "icon_names.h"
```

- [ ] **Step 3: Add the lookup and UTF-8 helpers**

Add these above `ring_draw_event_cb()`, next to the existing `ICON_*` macros:

```c
// Name -> glyph codepoint, or 0 when this firmware has no glyph for that name.
//
// Binary search over ICON_NAMES, which the generator emits in strcmp order. A name we do not
// know is the NORMAL case for an icon added to the app after this firmware shipped -- the caller
// falls back to the app-supplied bitmap rather than treating it as an error.
static uint32_t icon_codepoint(const char *name) {
  if (name == nullptr || *name == '\0') return 0;
  int lo = 0, hi = ICON_NAMES_COUNT - 1;
  while (lo <= hi) {
    int mid = lo + (hi - lo) / 2;
    int c = strcmp(name, ICON_NAMES[mid].name);
    if (c == 0) return ICON_NAMES[mid].cp;
    if (c < 0) hi = mid - 1;
    else       lo = mid + 1;
  }
  return 0;
}

// Encode one codepoint as UTF-8 into `out` (at least 5 bytes), NUL-terminated. Returns the byte
// count, excluding the terminator.
//
// Hand-rolled rather than using LVGL's _lv_txt_unicode_to_utf8, which is an internal symbol that
// has moved between LVGL versions. Lucide's glyphs live in the private use area (0xE000-0xF8FF),
// so the three-byte branch is the one that actually runs; the others are here so the function is
// correct rather than merely sufficient.
static int icon_utf8(uint32_t cp, char *out) {
  if (cp < 0x80) {
    out[0] = (char)cp;                                    out[1] = '\0'; return 1;
  } else if (cp < 0x800) {
    out[0] = (char)(0xC0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3F));                  out[2] = '\0'; return 2;
  } else if (cp < 0x10000) {
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));                  out[3] = '\0'; return 3;
  }
  out[0] = (char)(0xF0 | (cp >> 18));
  out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
  out[3] = (char)(0x80 | (cp & 0x3F));                    out[4] = '\0'; return 4;
}
```

- [ ] **Step 4: Restructure the wedge icon block into a four-tier chain**

This step rewrites the block rather than inserting into it, because the tiers cannot be chained
any other way: `else` must be followed by a single statement or block, and the existing code
declares variables (`uint8_t iconbits[54]`, the `const char *` lookups) between what would be the
branches. **Hoist every declaration above the chain, then chain cleanly.**

Find the block in `ring_draw_event_cb()` that currently begins:

```c
      uint8_t iconbits[54];
      const char *ixbm = macro.isNull() ? nullptr : (const char *)(macro["icon_xbm"] | (const char *)nullptr);
      if (wedge_icon_decode(ixbm, iconbits)) {
```

Replace those three lines with the declarations and the tier-1 branch below. **Leave the body of
the existing `wedge_icon_decode` branch and its trailing `else` (the text-label path) exactly as
they are** — they become tiers 3 and 4 unchanged.

```c
      // FOUR-TIER ICON CHAIN. Declarations first: the tiers are an if/else-if chain, and C++ has
      // nowhere to put a declaration between branches.
      //
      //   1  a glyph this firmware has           -> crisp, antialiased, no payload
      //   2  an app-supplied 48x48               -> a name added after this firmware shipped
      //   3  the 18x18 icon_xbm, upscaled        -> unchanged; what older profiles carry
      //   4  the macro's name as text            -> unchanged
      //
      // Order is load-bearing and invisible to the compiler: get it wrong and icons render
      // blocky, which looks exactly like the bug this milestone fixes.
      uint8_t iconbits[54];
      const char *iname = macro.isNull() ? nullptr : (const char *)(macro["icon"]       | (const char *)nullptr);
      const char *ixbm  = macro.isNull() ? nullptr : (const char *)(macro["icon_xbm"]   | (const char *)nullptr);
      uint32_t icp = icon_codepoint(iname);

      if (icp != 0) {
        // TIER 1 -- drawn at the font's native 48px, with no scaling anywhere.
        //
        // Two passes: a one-pixel offset shadow in the opposite contrast colour, then the glyph.
        // Same treatment the bitmap path uses and for the same reason -- picking black-or-white
        // by luminance alone tops out near 4.6:1 on a mid-tone wedge, so the shadow is what
        // guarantees a hard edge on ANY user-chosen colour.
        char gbuf[5];
        icon_utf8(icp, gbuf);

        lv_draw_label_dsc_t gl;
        lv_draw_label_dsc_init(&gl);
        gl.font  = &lucide_48;
        gl.opa   = LV_OPA_COVER;
        gl.align = LV_TEXT_ALIGN_CENTER;

        // lv_area_t bounds are inclusive, so a 48px span is c-24 .. c+23.
        lv_area_t ga = { (lv_coord_t)(lx - 24), (lv_coord_t)(ly - 24),
                         (lv_coord_t)(lx + 23), (lv_coord_t)(ly + 23) };
        lv_area_t gs = { (lv_coord_t)(ga.x1 + 1), (lv_coord_t)(ga.y1 + 1),
                         (lv_coord_t)(ga.x2 + 1), (lv_coord_t)(ga.y2 + 1) };

        gl.color = contrast_shadow_on(color);
        lv_draw_label(draw_ctx, &gl, &gs, gbuf, NULL);
        gl.color = contrast_on(color);
        lv_draw_label(draw_ctx, &gl, &ga, gbuf, NULL);
      } else if (wedge_icon_decode(ixbm, iconbits)) {
```

Task 3 inserts tier 2 between these two branches.

> **Verified:** both helpers exist with these exact signatures — `contrast_on(uint32_t bg)` at `Waveshare_LVGL_Test.ino:239` and `contrast_shadow_on(uint32_t bg)` at `:244`, each returning `lv_color_t`.

- [ ] **Step 5: Compile**

```bash
arduino-cli compile --fqbn "esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc,FlashMode=qio,FlashSize=16M,PartitionScheme=default_8MB,PSRAM=disabled" firmware/Waveshare_LVGL_Test
```

Expected: success. **Record the reported program-storage figure** — it was 1,037,522 bytes (31%) before M12, and the spec budgets roughly +177 KB. If it grew by much more than that, stop and investigate before flashing.

- [ ] **Step 6: Commit**

```bash
git add firmware/Waveshare_LVGL_Test/lv_conf.h firmware/Waveshare_LVGL_Test/Waveshare_LVGL_Test.ino
git commit -m "feat(m12): draw wedge icons from the compiled glyph set"
```

---

## Task 3: Firmware — the `icon_bmp48` fallback (tier 2)

**Files:**
- Modify: `firmware/Waveshare_LVGL_Test/Waveshare_LVGL_Test.ino`

**Interfaces:**
- Consumes: the tier-1 `else` chain from Task 2.
- Produces: `ICON_BMP48_PX`, `ICON_BMP48_STRIDE`, `ICON_BMP48_BYTES`, `static bool icon_bmp48_decode(const char *hex, uint8_t *out288)`.

- [ ] **Step 1: Add the constants and decoder**

Beside the existing `ICON_SRC_*` macros:

```c
// The app-supplied fallback, for names this firmware has no glyph for. Drawn 1:1 -- the whole
// point is that nothing is upscaled.
#define ICON_BMP48_PX     48
#define ICON_BMP48_STRIDE ((ICON_BMP48_PX + 7) / 8)              /* 6   */
#define ICON_BMP48_BYTES  (ICON_BMP48_STRIDE * ICON_BMP48_PX)    /* 288 */
```

And, next to `wedge_icon_decode()`:

```c
// Decode a 48x48 1bpp XBM hex string. Mirrors wedge_icon_decode(), including the per-byte bit
// reversal -- the app emits LSB-first (XBM convention) and LV_IMG_CF_ALPHA_1BIT reads MSB-first.
//
// Deliberately a separate function rather than a size parameter on wedge_icon_decode(): that one
// carries a static_assert tying it to a bare uint8_t[54] at its call site, and widening it would
// weaken the check that stops the 18x18 path from overrunning.
static bool icon_bmp48_decode(const char *hex, uint8_t *out) {
  if (hex == nullptr || strlen(hex) != (size_t)(ICON_BMP48_BYTES * 2)) return false;
  for (int b = 0; b < ICON_BMP48_BYTES; b++) {
    char pair[3] = { hex[b * 2], hex[b * 2 + 1], '\0' };
    char *end = nullptr;
    long v = strtol(pair, &end, 16);
    if (end != pair + 2) return false;      // non-hex character
    uint8_t x = (uint8_t)v;
    x = (uint8_t)(((x & 0xF0) >> 4) | ((x & 0x0F) << 4));
    x = (uint8_t)(((x & 0xCC) >> 2) | ((x & 0x33) << 2));
    x = (uint8_t)(((x & 0xAA) >> 1) | ((x & 0x55) << 1));
    out[b] = x;
  }
  return true;
}
```

- [ ] **Step 2: Add the tier-2 branch**

Task 2 left a chain of `if (icp != 0) { ... } else if (wedge_icon_decode(...)) { ... } else { ... }`.
Tier 2 goes **between** the first two branches.

First add the two declarations alongside the ones Task 2 hoisted, above the chain:

```c
      // `static` because 288 bytes is a lot for the LVGL task's 4 KB stack, and this callback
      // only ever runs on that one task (LV_EVENT_DRAW_MAIN_END is dispatched from
      // lv_timer_handler), so a single shared buffer is safe -- no reentrancy.
      static uint8_t icon48[ICON_BMP48_BYTES];
      const char *ib48  = macro.isNull() ? nullptr : (const char *)(macro["icon_bmp48"] | (const char *)nullptr);
```

Then change the line that currently reads:

```c
      } else if (wedge_icon_decode(ixbm, iconbits)) {
```

into the tier-2 branch followed by that same line:

```c
      } else if (icon_bmp48_decode(ib48, icon48)) {
        // TIER 2 -- the app supplied a 48x48 for a name this firmware has no glyph for. Drawn
        // 1:1, so it is sharper than the upscaled 18x18 even though it is still 1bpp and
        // therefore aliased.
        lv_img_dsc_t idata;
        idata.header.cf          = LV_IMG_CF_ALPHA_1BIT;
        idata.header.always_zero = 0;
        idata.header.reserved    = 0;
        idata.header.w           = ICON_BMP48_PX;
        idata.header.h           = ICON_BMP48_PX;
        idata.data_size          = ICON_BMP48_BYTES;
        idata.data               = icon48;

        lv_draw_img_dsc_t idsc;
        lv_draw_img_dsc_init(&idsc);
        idsc.recolor_opa = LV_OPA_COVER;

        lv_area_t ia = { (lv_coord_t)(lx - 24), (lv_coord_t)(ly - 24),
                         (lv_coord_t)(lx + 23), (lv_coord_t)(ly + 23) };
        lv_area_t sa = { (lv_coord_t)(ia.x1 + 1), (lv_coord_t)(ia.y1 + 1),
                         (lv_coord_t)(ia.x2 + 1), (lv_coord_t)(ia.y2 + 1) };

        idsc.recolor = contrast_shadow_on(color);
        lv_draw_img(draw_ctx, &idsc, &sa, &idata);
        idsc.recolor = contrast_on(color);
        lv_draw_img(draw_ctx, &idsc, &ia, &idata);
      } else if (wedge_icon_decode(ixbm, iconbits)) {
```

- [ ] **Step 3: Compile**

```bash
arduino-cli compile --fqbn "esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc,FlashMode=qio,FlashSize=16M,PartitionScheme=default_8MB,PSRAM=disabled" firmware/Waveshare_LVGL_Test
```

Expected: success, with program storage up only a few hundred bytes from Task 2.

- [ ] **Step 4: Verify the fallback chain reads correctly**

```bash
grep -n "TIER 1\|TIER 2\|wedge_icon_decode(ixbm\|wedge_label_fit" firmware/Waveshare_LVGL_Test/Waveshare_LVGL_Test.ino
```

Expected: the four tiers appear in order — tier 1, tier 2, the 18×18 decode, then the text label. Any other order means the `else` chain was assembled wrong, which compiles cleanly and fails only on hardware.

- [ ] **Step 5: Commit**

```bash
git add firmware/Waveshare_LVGL_Test/Waveshare_LVGL_Test.ino
git commit -m "feat(m12): fall back to an app-supplied 48x48 for unknown icon names"
```

---

## Task 4: Firmware — the `get_glyphs` command

**Files:**
- Modify: `firmware/Waveshare_LVGL_Test/ble_engine.cpp`

**Interfaces:**
- Consumes: `ICON_NAMES`, `ICON_NAMES_COUNT`, `ICON_SET_VERSION` from Task 1; `BleChunkSink`, `bleSendPreamble()`, `sendBleMessage()` already in this file.
- Produces: the wire response `{"status":"ok","set":"<12 hex>","glyphs":["..."]}`.

- [ ] **Step 1: Include the table**

At the top of `ble_engine.cpp`, with the other includes:

```c
#include "icon_names.h"
```

- [ ] **Step 2: Add the handler**

In `handleBleCommand()`, after the `trigger` branch (`ble_engine.cpp:410`), add another `else if`:

```c
  } else if (cmd == "get_glyphs") {
    // Which icon names this firmware can draw, so the app only pays the icon_bmp48 cost for the
    // ones it cannot (see the M12 design 4.1). Roughly 2 KB; streamed through the chunk sink
    // rather than built in a String, because the whole point of the sink is to avoid holding the
    // full response in heap at once.
    //
    // A device without this command answers "Unknown command" below, and the app treats ANY
    // non-success as "no glyph list" and stops sending icon_bmp48 entirely. That single rule is
    // the whole backward-compatibility story -- do not make this branch clever.
    bleSendPreamble();
    BleChunkSink sink;
    sink.print("{\"status\":\"ok\",\"set\":\"" ICON_SET_VERSION "\",\"glyphs\":[");
    for (int i = 0; i < ICON_NAMES_COUNT; i++) {
      if (i) sink.print(",");
      sink.print("\"");
      sink.print(ICON_NAMES[i].name);
      sink.print("\"");
    }
    sink.print("]}\n");
    if (sink.flushRemainder()) {
      Serial.printf("[ble] get_glyphs: streamed %u bytes (%d names)\n",
                    (unsigned)sink.totalSent, ICON_NAMES_COUNT);
    } else {
      Serial.println("[ble] get_glyphs: send aborted (ack retries exhausted)");
      sendBleMessage("{\"status\":\"error\",\"message\":\"Send failed\"}");
    }
```

> **Verified:** `BleChunkSink` is declared `class BleChunkSink : public Print` (`ble_engine.cpp:101`), so `print(const char *)` is inherited from Arduino's `Print`. `totalSent` (`:104`, `size_t`) and `flushRemainder()` (`:122`) are correct as written.

- [ ] **Step 3: Compile**

```bash
arduino-cli compile --fqbn "esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc,FlashMode=qio,FlashSize=16M,PartitionScheme=default_8MB,PSRAM=disabled" firmware/Waveshare_LVGL_Test
```

Expected: success.

- [ ] **Step 4: Commit**

```bash
git add firmware/Waveshare_LVGL_Test/ble_engine.cpp
git commit -m "feat(m12): add get_glyphs so the app can discover the device's icon set"
```

---

## Task 5: App — fetch and cache the glyph list

**Files:**
- Modify: `companion_app/lib/state/draupnir_state.dart`

**Interfaces:**
- Consumes: `_sendBleRequest`, `_log` (already in this file).
- Produces: `Set<String>? deviceGlyphs` (null = unknown/unsupported), `String? deviceGlyphSet`, `Future<void> fetchGlyphs()`.

- [ ] **Step 1: Add the fields**

Beside `needsConfigMode` and `lastSaveFailure`:

```dart
  // Which icon names the connected device can draw from its own glyph set.
  //
  // NULL means "unknown", and is the state for every device that does not answer get_glyphs --
  // the retired M5Dial, and any Waveshare on firmware older than M12. That is deliberately the
  // same state as "not asked yet", because both lead to the same behaviour: send no icon_bmp48
  // at all, and let the device fall back to the 18x18 icon_xbm it has always received.
  Set<String>? deviceGlyphs;

  // The device's glyph-set version, for logging and for a future cache. Null when unknown.
  String? deviceGlyphSet;
```

- [ ] **Step 2: Add the fetch**

Next to `fetchProfiles()`:

```dart
  /// Asks the device which icon names it can draw. Safe to call against any device.
  ///
  /// Never throws and never sets [error]: a device without the command is not a fault, it is the
  /// common case for older firmware, and treating it as an error would put a red screen in front
  /// of someone whose knob works perfectly.
  Future<void> fetchGlyphs() async {
    try {
      final response = await _sendBleRequest({'cmd': 'get_glyphs'});
      if (response['status'] == 'ok' && response['glyphs'] is List) {
        deviceGlyphs = (response['glyphs'] as List).map((e) => e.toString()).toSet();
        deviceGlyphSet = response['set']?.toString();
        _log('[glyphs] device knows ${deviceGlyphs!.length} icons (set=$deviceGlyphSet)');
      } else {
        deviceGlyphs = null;
        deviceGlyphSet = null;
        _log('[glyphs] device has no glyph list; icon_bmp48 will not be sent');
      }
    } catch (e) {
      deviceGlyphs = null;
      deviceGlyphSet = null;
      _log('[glyphs] get_glyphs failed: $e');
    }
    notifyListeners();
  }
```

- [ ] **Step 3: Call it after connecting**

In `connectBluetooth()`, find `await fetchProfiles();` and make it:

```dart
      // Before the profiles, so the first save after connecting already knows which icons need a
      // bitmap. Cheap (~2 KB, once per connection) and never fatal.
      await fetchGlyphs();
      await fetchProfiles();
```

- [ ] **Step 4: Clear it on disconnect**

Find the disconnect handler that nulls `connectedDevice`/`rxChar`/`txChar` and add:

```dart
      deviceGlyphs = null;
      deviceGlyphSet = null;
```

Stale glyph data across a reconnect to a *different* board would send bitmaps to a device that does not want them, or withhold them from one that does.

- [ ] **Step 5: Analyze and commit**

```bash
cd companion_app && flutter analyze 2>&1 | grep -E "^\s*(error|warning)"
```

Expected: only the pre-existing `dart:io` unused-import warning in `editor_panel.dart`.

```bash
git add companion_app/lib/state/draupnir_state.dart
git commit -m "feat(m12): fetch and cache the device's glyph list on connect"
```

---

## Task 6: App — size-parameterised rasteriser, and gap-only `icon_bmp48`

**Files:**
- Modify: `companion_app/lib/utils/icon_generator.dart`
- Modify: `companion_app/lib/screens/editor_panel.dart`
- Create: `companion_app/test/icon_gap_test.dart`

**Interfaces:**
- Consumes: `resolveIcon` from `macro_icons.dart`; `deviceGlyphs` from Task 5.
- Produces: `generateXbmHex(IconData, {int size})`, `bool needsBmp48(String? iconName, Set<String>? deviceGlyphs)`.

- [ ] **Step 1: Write the failing test**

Create `companion_app/test/icon_gap_test.dart`:

```dart
import 'package:flutter_test/flutter_test.dart';
import 'package:companion_app/utils/icon_generator.dart';

void main() {
  group('needsBmp48', () {
    test('false when the device has no glyph list', () {
      // Older firmware and the retired M5Dial. Sending a bitmap they cannot use wastes payload
      // against an 8 KB buffer for no benefit.
      expect(needsBmp48('play', null), isFalse);
    });

    test('false when the device already knows the name', () {
      expect(needsBmp48('play', {'play', 'pause'}), isFalse);
    });

    test('true when the device does not know the name', () {
      expect(needsBmp48('gamepad2', {'play', 'pause'}), isTrue);
    });

    test('false for a macro with no icon', () {
      // Tier 4 -- the wedge draws the macro's name. There is nothing to rasterise.
      expect(needsBmp48(null, {'play'}), isFalse);
      expect(needsBmp48('', {'play'}), isFalse);
    });
  });
}
```

- [ ] **Step 2: Run it and watch it fail**

```bash
cd companion_app && flutter test test/icon_gap_test.dart
```

Expected: FAIL — `needsBmp48` is not defined.

- [ ] **Step 3: Parameterise the rasteriser and add `needsBmp48`**

In `icon_generator.dart`, replace the fixed-18 implementation. Keep `generateXbmHexForIcon` as a thin wrapper so existing callers are untouched:

```dart
/// Generates an XBM hex string for a square 1bpp icon.
///
/// `size` is the bitmap edge in pixels. 18 is the interchange size every device understands and
/// the retired M5Dial requires; 48 is the Waveshare's tier-2 fallback, drawn 1:1 so it is sharper
/// than an upscaled 18. The glyph is laid out at roughly 78% of the bitmap edge, which is what
/// the original hand-tuned 14-in-18 worked out to.
Future<String> generateXbmHex(IconData iconData, {int size = 18}) async {
  final int width = size;
  final int height = size;
  final int stride = (width + 7) ~/ 8;

  final pictureRecorder = ui.PictureRecorder();
  final canvas = ui.Canvas(pictureRecorder);

  canvas.drawRect(
    Rect.fromLTWH(0, 0, width.toDouble(), height.toDouble()),
    Paint()..color = Colors.black,
  );

  final textPainter = TextPainter(textDirection: TextDirection.ltr);
  textPainter.text = TextSpan(
    text: String.fromCharCode(iconData.codePoint),
    style: TextStyle(
      fontSize: size * 14.0 / 18.0,
      fontFamily: iconData.fontFamily,
      package: iconData.fontPackage,
      color: Colors.white,
    ),
  );
  textPainter.layout();

  final dx = (width - textPainter.width) / 2;
  final dy = (height - textPainter.height) / 2;
  textPainter.paint(canvas, Offset(dx, dy));

  final picture = pictureRecorder.endRecording();
  final image = await picture.toImage(width, height);
  final byteData = await image.toByteData(format: ui.ImageByteFormat.rawRgba);
  if (byteData == null) return "";

  final bytes = byteData.buffer.asUint8List();
  final xbmBytes = List.filled(stride * height, 0);

  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      final idx = (y * width + x) * 4;
      if (bytes[idx] > 127) {
        xbmBytes[y * stride + (x ~/ 8)] |= (1 << (x % 8));
      }
    }
  }

  final sb = StringBuffer();
  for (final b in xbmBytes) {
    sb.write(b.toRadixString(16).padLeft(2, '0'));
  }
  return sb.toString();
}

/// Back-compatible wrapper: the 18x18 interchange bitmap every device understands.
Future<String> generateXbmHexForIcon(IconData iconData) =>
    generateXbmHex(iconData, size: 18);

/// Whether this macro needs an app-supplied 48x48 for the connected device.
///
/// True only when we KNOW the device's glyph list and the name is absent from it. A null list
/// means older firmware or the retired M5Dial, which cannot use icon_bmp48 at all -- sending it
/// would spend payload against an 8 KB receive buffer for nothing.
bool needsBmp48(String? iconName, Set<String>? deviceGlyphs) {
  if (iconName == null || iconName.isEmpty) return false;
  if (deviceGlyphs == null) return false;
  return !deviceGlyphs.contains(iconName);
}
```

- [ ] **Step 4: Run the tests**

```bash
cd companion_app && flutter test
```

Expected: PASS, 31/31 (27 existing + 4 new).

- [ ] **Step 5: Emit `icon_bmp48` on save**

In `editor_panel.dart`'s `_saveMacro()`, the icon block currently reads:

```dart
    final glyph = resolveIcon(_selectedIcon);
    if (glyph != null) {
      iconXbm = await generateXbmHexForIcon(glyph);
    }
```

Replace with:

```dart
    // icon_xbm is ALWAYS sent: it is what the retired M5Dial consumes and what makes older
    // Waveshare firmware degrade rather than break. icon_bmp48 is additional, and only when the
    // device told us it has no glyph for this name.
    String iconBmp48 = "";
    final glyph = resolveIcon(_selectedIcon);
    if (glyph != null) {
      iconXbm = await generateXbmHexForIcon(glyph);
      if (needsBmp48(_selectedIcon, state.deviceGlyphs)) {
        iconBmp48 = await generateXbmHex(glyph, size: 48);
      }
    }
```

and add the field to the macro map passed to `state.updateMacro`, after `'icon_xbm': iconXbm,`
— **only when it has content**:

```dart
      if (iconBmp48.isNotEmpty) 'icon_bmp48': iconBmp48,
```

The collection-`if` matters. `updateMacro` replaces the whole macro object, so omitting the key
is what *removes* a stale bitmap once the firmware gains that glyph. Writing `''` instead would
leave a dead key on every macro forever, spending document bytes against the 8 KB receive buffer
that Task 7 exists to protect.

- [ ] **Step 6: Analyze, test, commit**

```bash
cd companion_app && flutter analyze 2>&1 | grep -E "^\s*(error|warning)" && flutter test 2>&1 | tail -2
```

Expected: only the pre-existing warning; 31/31 pass.

```bash
git add companion_app/lib/utils/icon_generator.dart companion_app/lib/screens/editor_panel.dart companion_app/test/icon_gap_test.dart
git commit -m "feat(m12): send a 48x48 bitmap only for icons the device lacks"
```

---

## Task 7: App — oversized-document pre-check

**Files:**
- Modify: `companion_app/lib/state/draupnir_state.dart`
- Modify: `companion_app/test/icon_gap_test.dart`

**Interfaces:**
- Consumes: `profilesData`, `lastSaveFailure` from earlier tasks.
- Produces: `String? oversizeWarning(int documentBytes, int gapIconCount)`, `kMaxDocumentBytes`.

- [ ] **Step 1: Write the failing test**

Append to `companion_app/test/icon_gap_test.dart`:

```dart
  group('oversizeWarning', () {
    test('null for a document that fits', () {
      expect(oversizeWarning(2000, 0), isNull);
    });

    test('explains the cause when gap icons are what made it too big', () {
      final msg = oversizeWarning(9000, 12);
      expect(msg, isNotNull);
      expect(msg, contains('12'));
      expect(msg!.toLowerCase(), contains('firmware'));
    });

    test('still warns when the document is too big with no gap icons', () {
      // Not the expected cause, so the message must not blame icons that are not there.
      final msg = oversizeWarning(9000, 0);
      expect(msg, isNotNull);
      expect(msg!.contains('0 icon'), isFalse);
    });
  });
```

- [ ] **Step 2: Run it and watch it fail**

```bash
cd companion_app && flutter test test/icon_gap_test.dart
```

Expected: FAIL — `oversizeWarning` is not defined.

- [ ] **Step 3: Implement it**

Add to `icon_generator.dart` (it lives with the other icon-payload logic, and keeping it out of state makes it testable without a BLE stack):

```dart
/// The device refuses a message larger than its BLE receive buffer. BLE_RX_BUFFER_SIZE is 8192
/// on the Waveshare; this leaves headroom for the command wrapper and chunk framing.
const int kMaxDocumentBytes = 7500;

/// A specific, actionable message when the serialised document is too large, or null when it
/// fits.
///
/// The device already refuses oversized messages safely (M6 RX bounds) and the refusal surfaces
/// through the save-retry path, so this is not a safety net -- it is the difference between "the
/// device refused the save" and knowing WHY, which for a document full of gap icons is a thing
/// the user can actually act on by updating the firmware.
String? oversizeWarning(int documentBytes, int gapIconCount) {
  if (documentBytes <= kMaxDocumentBytes) return null;
  if (gapIconCount > 0) {
    return 'This profile is too large to send ('
        '${(documentBytes / 1024).toStringAsFixed(1)} KB). '
        '$gapIconCount of its icons are newer than the device\'s firmware, so the app has to '
        'send each one as an image. Updating the firmware, or choosing icons the device already '
        'has, will fix both the size and how sharp they look.';
  }
  return 'This profile is too large to send ('
      '${(documentBytes / 1024).toStringAsFixed(1)} KB). '
      'Try removing a few macros or shortening their text actions.';
}
```

- [ ] **Step 4: Run the tests**

```bash
cd companion_app && flutter test
```

Expected: PASS, 34/34.

- [ ] **Step 5: Wire it into the save path**

In `saveProfiles()`, immediately after the stale-state clearing block and before `_sendBleRequest`:

```dart
    // Checked before sending rather than after refusal: the device's rejection is safe but says
    // only that the message was too big, and the user cannot act on that.
    final encoded = jsonEncode(profilesData);
    final gapCount = _countGapIcons();
    final oversize = oversizeWarning(encoded.length, gapCount);
    if (oversize != null) {
      _log('[ERR] save_profiles aborted: ${encoded.length} bytes, $gapCount gap icons');
      lastSaveFailure = oversize;
      isLoading = false;
      notifyListeners();
      return false;
    }
```

and add the helper beside it:

```dart
  /// Macros carrying an icon_bmp48, i.e. icons this device's firmware has no glyph for.
  int _countGapIcons() {
    final doc = profilesData;
    if (doc == null) return 0;
    int n = 0;
    for (final p in (doc['profiles'] as List? ?? const [])) {
      for (final m in ((p as Map)['macros'] as List? ?? const [])) {
        final bmp = (m as Map)['icon_bmp48'];
        if (bmp is String && bmp.isNotEmpty) n++;
      }
    }
    return n;
  }
```

Add `import 'package:companion_app/utils/icon_generator.dart';` if `draupnir_state.dart` does not already import it, and confirm `dart:convert` is imported for `jsonEncode`.

- [ ] **Step 6: Analyze, test, commit**

```bash
cd companion_app && flutter analyze 2>&1 | grep -E "^\s*(error|warning)" && flutter test 2>&1 | tail -2
```

Expected: only the pre-existing warning; 34/34 pass.

```bash
git add companion_app/lib companion_app/test
git commit -m "feat(m12): warn before sending a document the device will refuse"
```

---

## Task 8: Hardware verification

**Files:** none — this task produces evidence, not code.

**Interfaces:** consumes everything above.

This is the real gate. The firmware has no host tests, and every tier below tier 1 is invisible to the compiler.

- [ ] **Step 1: Build and install both halves**

```bash
arduino-cli compile --fqbn "esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc,FlashMode=qio,FlashSize=16M,PartitionScheme=default_8MB,PSRAM=disabled" firmware/Waveshare_LVGL_Test
cd companion_app && flutter build apk --release
```

Ask the owner to put the Waveshare in download mode (hold BOOT, replug — auto-reset does not work on this board), then flash and install:

```bash
arduino-cli upload -p <PORT> --fqbn "<FQBN above>" firmware/Waveshare_LVGL_Test
adb -s <DEVICE> install -r companion_app/build/app/outputs/flutter-apk/app-release.apk
```

- [ ] **Step 2: Walk the spec's done-criteria**

Work through §8 of the design doc. Each needs an explicit observation, not an inference:

1. Generator is idempotent — proven in Task 1 Step 5.
2. Firmware compiles, flash within budget — record the figure.
3. **Known icons are crisp.** Specifically check `gitPullRequest`, `alignLeft`, `listOrdered`, `braces` — the glyphs M11 predicted would be unreadable. They are the reason this milestone exists.
4. **An unknown name draws the app's 48×48.** Force this: temporarily remove one name from `macroIcons`, regenerate, reflash — or bind a macro while the app is connected to firmware built before Task 1.
5. **A profile saved before M12 renders exactly as before** (tier 3). Load one that predates this work.
6. **A macro with no icon draws its name** (tier 4).
7. **The retired M5Dial, unflashed, still loads and renders the same profiles.** This is the additive-only constraint's only real test.
8. **The app sends no `icon_bmp48` to a device that does not answer `get_glyphs`** — check the debug log for `[glyphs] device has no glyph list`.
9. Oversized-document pre-check produces the specific message.
10. Round trip: save, power-cycle the knob, icons still correct.

- [ ] **Step 3: Record the results**

Add a dated section to `docs/HANDOFF.md` §4 in the style of the existing entries, stating what was observed for each criterion — including anything that failed.

- [ ] **Step 4: Commit and open the PR**

```bash
git add docs/HANDOFF.md
git commit -m "docs: M12 verified on hardware"
git push -u origin <branch>
gh pr create --base master --title "M12: crisp icon rendering on the Waveshare" --body-file <notes>
```

---

## Notes for whoever executes this

**Every symbol this plan names has been checked against the tree** — the contrast helpers, `BleChunkSink`'s interface, the `lv_font_conv` flags (taken from `orbitron_18.c`'s own generated header), `LV_USE_FONT_COMPRESSED 0`, and the Lucide package's codepoint format. If one does not match, the tree has moved since 2026-09-24; check `git log` on that file before working around it.

**The thing most likely to be silently wrong:** the tier ordering in Tasks 2 and 3. An `else` chain assembled in the wrong order compiles cleanly, and only shows up as "the icon is blocky when it should be crisp" — which looks exactly like the bug this milestone is fixing. Task 3 Step 4 exists to catch that.

**Do not hand-edit** `icon_names.h` or `lucide_48.c`. If a name is wrong, fix `macro_icons.dart` and regenerate.
