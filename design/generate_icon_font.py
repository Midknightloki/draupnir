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
import shutil
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
# The legacyIconAliases map -- "'text': 'type'," -- both sides are macroIcons-style names, not
# LucideIcons identifiers.
ALIAS_BLOCK = re.compile(r"legacyIconAliases\s*=\s*\{(.*?)\n\};", re.S)
ALIAS_ENTRY = re.compile(r"'([A-Za-z0-9_]+)':\s*'([A-Za-z0-9_]+)'")

# lv_font_conv's own cmap, parsed back out of the C it just emitted -- see verify_font_codepoints.
CMAP_BLOCK = re.compile(
    r"\{\s*\.range_start\s*=\s*(\d+),\s*\.range_length\s*=\s*(\d+),\s*\.glyph_id_start\s*=\s*\d+,"
    r"\s*\.unicode_list\s*=\s*([A-Za-z0-9_]+),\s*\.glyph_id_ofs_list\s*=\s*[A-Za-z0-9_]+,"
    r"\s*\.list_length\s*=\s*(\d+),\s*\.type\s*=\s*([A-Za-z0-9_]+)\s*\}"
)
UNICODE_LIST_ARRAY = re.compile(
    r"static const uint16_t (unicode_list_\d+)\[\]\s*=\s*\{([^}]*)\};", re.S
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

    macro_icons_text = MACRO_ICONS.read_text(encoding="utf-8")
    entries = MAP_ENTRY.findall(macro_icons_text)
    if not entries:
        die("parsed zero icons out of macro_icons.dart -- has the map's format changed?")
    aliases = parse_legacy_aliases(macro_icons_text)

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

    # legacyIconAliases entries (e.g. 'text' -> 'type') add a NAME row pointing at a codepoint
    # that already exists under its target's own name -- they need no new glyph. Without this, a
    # macro saved with an alias name never matches a row in ICON_NAMES, icon_codepoint() returns
    # 0 forever, and the app pays for a 576-byte icon_bmp48 on every save for a glyph the device
    # has always had (M12 whole-branch review, Minor finding 6).
    alias_missing = []
    for alias, target in aliases.items():
        if alias in resolved:
            die(f"legacyIconAliases entry '{alias}' collides with a macroIcons name")
        cp = resolved.get(target)
        if cp is None:
            alias_missing.append(f"{alias} -> {target}")
        else:
            resolved[alias] = cp
    if alias_missing:
        die("these legacyIconAliases entries point at a name absent from macroIcons:\n  "
            + "\n  ".join(alias_missing))

    # Sorted by byte order, because the firmware binary-searches with strcmp().
    ordered = sorted(resolved.items(), key=lambda kv: kv[0].encode("utf-8"))

    # Version = a hash of exactly what the device can draw, so it changes if and only if the
    # set does. The app uses it to know whether its cached glyph list is still valid.
    digest = hashlib.sha256(
        "".join(f"{n}:{c};" for n, c in ordered).encode("utf-8")
    ).hexdigest()[:12]

    # Generate the font FIRST and verify it actually contains every codepoint the table is about
    # to claim, and only THEN write the header. Previously this ran the other way around: if
    # lv_font_conv failed partway, die() exited leaving a committed-ready header listing names
    # whose glyphs are not in lucide_48.c. The firmware would take tier 1 for those names, LVGL
    # would draw a 1px placeholder box, and tiers 2-4 (icon_bmp48, the name label) would never run
    # -- the only path in the whole design where a wedge renders nothing usable (M12 whole-branch
    # review, Important finding 1).
    unique_codepoints = sorted({cp for _, cp in ordered})
    run_font_conv(unique_codepoints)
    verify_font_codepoints(unique_codepoints)

    write_names_header(ordered, digest)

    print(f"generated {len(ordered)} names ({len(unique_codepoints)} unique glyphs), "
          f"set version {digest}")
    print(f"  {OUT_NAMES}")
    print(f"  {OUT_FONT}")


def parse_legacy_aliases(macro_icons_text):
    m = ALIAS_BLOCK.search(macro_icons_text)
    if not m:
        die("cannot find legacyIconAliases in macro_icons.dart -- has its format changed?")
    return dict(ALIAS_ENTRY.findall(m.group(1)))


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
    # Field is the quoted name PLUS the separating comma -- icon_name_entry_t has two members,
    # so the initializer needs `{ "name", 0xNNNN },`. Padding is applied to this whole field so
    # the hex column still lines up.
    width = max(len(n) for n, _ in ordered) + 4
    for name, cp in ordered:
        field = f'{chr(34)}{name}{chr(34)},'
        lines.append(f'  {{ {field:<{width}}0x{cp:04X} }},')
    lines += ["};", ""]
    OUT_NAMES.write_text("\n".join(lines), encoding="utf-8", newline="\n")
    verify_names_header(len(ordered))


def verify_names_header(expected_count):
    # Nothing else in this generator parses or compiles its own output, which is exactly how a
    # missing comma in the struct initializer shipped undetected: it's a syntax error, but only a
    # C compiler including this header would ever notice. Enforce the row shape here instead, so
    # a malformed table fails the generator, not Task 2's build.
    text = OUT_NAMES.read_text(encoding="utf-8")
    row_re = re.compile(r'^\s*\{\s*"[^"]+"\s*,\s*0x[0-9A-Fa-f]+\s*\},$', re.M)
    rows = row_re.findall(text)
    if len(rows) != expected_count:
        die(f"{OUT_NAMES} failed verification: {len(rows)} well-formed "
            f'`{{ "name", 0xNNNN }},` rows found, expected {expected_count} -- '
            "fix write_names_header and regenerate")


def run_font_conv(codepoints):
    # Flags match the convention recorded in orbitron_18.c's generated header. --no-compress is
    # mandatory: lv_conf.h sets LV_USE_FONT_COMPRESSED 0, and a compressed font against that
    # config renders garbage rather than failing to build.
    ranges = ",".join(f"0x{c:04X}" for c in codepoints)
    # On Windows, npm installs a .cmd shim; subprocess's list form doesn't apply PATHEXT
    # resolution the way a shell does, so a bare "lv_font_conv" raises FileNotFoundError even
    # though it's on PATH. shutil.which() does the same PATHEXT search a shell would.
    exe = shutil.which("lv_font_conv") or "lv_font_conv"
    cmd = [
        exe,
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


def verify_font_codepoints(expected_codepoints):
    """Confirms lucide_48.c's own cmap actually covers every codepoint we asked for.

    lv_font_conv exiting 0 means it ran, not that every requested codepoint made it into the
    output -- a codepoint the source TTF doesn't cover is the kind of failure that could still
    exit clean. This parses the cmap back out of the generated C (rather than trusting the exit
    code alone) so that case dies here instead of shipping a header/font pair where the header
    promises a glyph the font doesn't have (M12 whole-branch review, Important finding 1).

    lv_font_conv's cmap comes in two shapes for our purposes: a contiguous range (`.unicode_list
    = NULL`, every codepoint in [range_start, range_start+range_length) is present), or a sparse
    list (`.unicode_list = unicode_list_N`, whose entries are OFFSETS from range_start -- one per
    present codepoint). Multiple cmap blocks are unioned. If lv_font_conv ever changes its output
    format, the regexes below stop matching and this dies with a clear message rather than
    silently verifying nothing.
    """
    if not OUT_FONT.exists():
        die(f"{OUT_FONT} does not exist after run_font_conv -- lv_font_conv did not write it")
    text = OUT_FONT.read_text(encoding="utf-8")
    if not text.strip():
        die(f"{OUT_FONT} is empty after run_font_conv")

    arrays = dict(UNICODE_LIST_ARRAY.findall(text))

    blocks = CMAP_BLOCK.findall(text)
    if not blocks:
        die(f"{OUT_FONT}: could not parse any cmap block out of the generated font -- "
            "lv_font_conv's output format may have changed; update CMAP_BLOCK / "
            "verify_font_codepoints() in design/generate_icon_font.py")

    font_codepoints = set()
    for range_start, range_length, list_name, list_length, _cmap_type in blocks:
        range_start = int(range_start)
        range_length = int(range_length)
        list_length = int(list_length)
        if list_name == "NULL":
            # Contiguous: every codepoint in the range has a glyph.
            font_codepoints.update(range(range_start, range_start + range_length))
            continue
        body = arrays.get(list_name)
        if body is None:
            die(f"{OUT_FONT}: cmap references {list_name}, which is not defined in the file")
        offsets = [int(tok, 0) for tok in re.findall(r"0x[0-9A-Fa-f]+|\d+", body)]
        if len(offsets) != list_length:
            die(f"{OUT_FONT}: {list_name} has {len(offsets)} entries but the cmap says "
                f"list_length={list_length}")
        font_codepoints.update(range_start + off for off in offsets)

    missing = sorted(cp for cp in expected_codepoints if cp not in font_codepoints)
    if missing:
        die(f"{OUT_FONT} is missing {len(missing)} requested codepoint(s) after conversion: "
            + ", ".join(f"0x{cp:04X}" for cp in missing)
            + " -- lv_font_conv reported success but the emitted cmap does not cover them")


if __name__ == "__main__":
    main()
