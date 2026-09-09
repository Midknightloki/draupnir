#!/usr/bin/env python3
"""Regenerate the Draupnir Forge app assets from the master logo.

Run from the repo root:  python design/generate_app_icons.py

Deliberately does NOT use flutter_launcher_icons. Five PNGs plus one XML are cheaper than a
build-time dependency, and doing it here keeps the exact scaling decisions visible and reviewable
instead of buried in a package's defaults.

Requires Pillow.
"""
import io
import os

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = os.path.join(HERE, "Logo_1@0.5x.png")
APP = os.path.join(ROOT, "companion_app")
RES = os.path.join(APP, "android", "app", "src", "main", "res")

# Matches AppTheme.background, so the launcher icon and the app agree.
BG = (17, 17, 17, 255)

# The in-app logo is quantised to a 128-colour palette: the mark uses few flat colours, so this
# costs nothing visible and takes the asset from ~265 KB to ~39 KB. Verified that Pillow keeps the
# alpha channel through quantize() -- losing it would put a solid box behind the logo on the dark
# theme, which is exactly the kind of silent degradation that is easy to ship unnoticed.
IN_APP_WIDTH = 512
IN_APP_COLORS = 128

# The AppBar mark. The full logo is unreadable at ~40px -- the raven's linework and the rune rim
# turn to mush -- so small placements get the centre bind-rune alone, which is bold, high-contrast
# and legible at that size. Bounding box measured from the master by isolating gold pixels
# (high R, mid G, low B), not eyeballed: x 40-67%, y 31-73%, plus a small margin.
GLYPH_BOX = (0.395, 0.305, 0.675, 0.735)
GLYPH_PX = 192

# Legacy launcher icons get an opaque dark ground; a transparent legacy icon renders
# inconsistently across launchers, and the mark is drawn for a dark backdrop.
LEGACY_DENSITIES = [("mdpi", 48), ("hdpi", 72), ("xhdpi", 96), ("xxhdpi", 144), ("xxxhdpi", 192)]
LEGACY_CONTENT = 0.86

# Adaptive icons (Android 8+) are masked by the launcher; the outer ~22% of the foreground can be
# cropped, so the mark occupies only the inner ~62% and survives a circle, squircle or
# rounded-square mask intact.
ADAPTIVE_DENSITIES = [("mdpi", 108), ("hdpi", 162), ("xhdpi", 216), ("xxhdpi", 324), ("xxxhdpi", 432)]
ADAPTIVE_CONTENT = 0.62


def fit_square(img, canvas_px, content_frac, background=None):
    """Scale img to occupy content_frac of a square canvas, centred, preserving aspect ratio."""
    from PIL import Image

    target = int(canvas_px * content_frac)
    w, h = img.size
    scale = target / max(w, h)
    new = img.resize((max(1, int(w * scale)), max(1, int(h * scale))), Image.LANCZOS)
    canvas = Image.new("RGBA", (canvas_px, canvas_px), background or (0, 0, 0, 0))
    canvas.alpha_composite(new, ((canvas_px - new.width) // 2, (canvas_px - new.height) // 2))
    return canvas


def main():
    from PIL import Image

    src = Image.open(SRC).convert("RGBA")
    print("source:", SRC, src.size)

    assets_dir = os.path.join(APP, "assets")
    os.makedirs(assets_dir, exist_ok=True)
    w, h = src.size
    logo = src.resize((IN_APP_WIDTH, int(h * IN_APP_WIDTH / w)), Image.LANCZOS)
    logo = logo.quantize(colors=IN_APP_COLORS, method=Image.FASTOCTREE)
    out = os.path.join(assets_dir, "logo.png")
    logo.save(out, optimize=True)
    print("  assets/logo.png", logo.size, os.path.getsize(out), "bytes")

    gx0, gy0, gx1, gy1 = GLYPH_BOX
    glyph = src.crop((int(w * gx0), int(h * gy0), int(w * gx1), int(h * gy1)))
    glyph = fit_square(glyph, GLYPH_PX, 1.0)
    # The rune is one colour plus a gradient, so a small palette costs nothing visible.
    glyph = glyph.quantize(colors=64, method=Image.FASTOCTREE)
    out = os.path.join(assets_dir, "glyph.png")
    glyph.save(out, optimize=True)
    print("  assets/glyph.png", glyph.size, os.path.getsize(out), "bytes")

    for name, px in LEGACY_DENSITIES:
        out = os.path.join(RES, "mipmap-" + name, "ic_launcher.png")
        os.makedirs(os.path.dirname(out), exist_ok=True)
        fit_square(src, px, LEGACY_CONTENT, BG).convert("RGB").save(out, optimize=True)
        print("  ic_launcher", name, px)

    for name, px in ADAPTIVE_DENSITIES:
        out = os.path.join(RES, "mipmap-" + name, "ic_launcher_foreground.png")
        os.makedirs(os.path.dirname(out), exist_ok=True)
        fit_square(src, px, ADAPTIVE_CONTENT).save(out, optimize=True)
        print("  ic_launcher_foreground", name, px)

    anydpi = os.path.join(RES, "mipmap-anydpi-v26")
    os.makedirs(anydpi, exist_ok=True)
    io.open(os.path.join(anydpi, "ic_launcher.xml"), "w", encoding="utf-8", newline="\n").write(
        '<?xml version="1.0" encoding="utf-8"?>\n'
        '<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">\n'
        '    <background android:drawable="@color/ic_launcher_background" />\n'
        '    <foreground android:drawable="@mipmap/ic_launcher_foreground" />\n'
        '</adaptive-icon>\n'
    )
    values = os.path.join(RES, "values")
    os.makedirs(values, exist_ok=True)
    io.open(os.path.join(values, "colors.xml"), "w", encoding="utf-8", newline="\n").write(
        '<?xml version="1.0" encoding="utf-8"?>\n'
        '<resources>\n'
        '    <!-- Matches AppTheme.background so the launcher icon and the app agree. -->\n'
        '    <color name="ic_launcher_background">#111111</color>\n'
        '</resources>\n'
    )
    print("  adaptive icon descriptor + background colour written")


if __name__ == "__main__":
    main()
