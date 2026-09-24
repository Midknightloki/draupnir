import 'package:flutter/material.dart';

class AppTheme {
  // Deep dark backgrounds inspired by VIA
  static const Color background = Color(0xFF111111);
  static const Color surface = Color(0xFF1E1E1E);
  static const Color surfaceHighlight = Color(0xFF2A2A2A);
  
  // Brand palette, sampled from the Draupnir Forge logo (assets/logo.png) rather than picked by
  // eye: cyan is the raven, magenta the rune rim, gold the centre bind-rune. Replaces the earlier
  // neon green, which was a Razer-Synapse-inspired placeholder and clashed with the mark.
  static const Color accent = Color(0xFF48F0D8);     // raven cyan
  static const Color accentDim = Color(0xFF0E3A35);  // same hue, dark enough for fills behind text
  static const Color brandMagenta = Color(0xFFC048C0); // rune rim
  static const Color brandGold = Color(0xFFF0C040);    // centre bind-rune

  static const Color textPrimary = Color(0xFFEEEEEE);
  static const Color textSecondary = Color(0xFFAAAAAA);
  
  // ===========================================================================================
  // MACRO / PROFILE COLOUR PALETTES
  //
  // Replaces a flat list of 33 X11 named colours ("Medium Violet Red", "Chartreuse") that was
  // the web-safe palette with the dull entries filtered out. It had no internal logic, so any
  // two colours picked from it were as likely to clash as to agree.
  //
  // Each palette is 4 hue families x 4 tones, laid out as a 4x4 grid so a row reads as one hue
  // deepening and a column as one tone across hues. Sixteen is not arbitrary: it fills the grid
  // exactly, and any two picks from the same palette share a colour story.
  //
  // THE DARKEST TONE IS STILL VISIBLE, deliberately. These become wedges on a black ring, so a
  // near-black swatch is an invisible macro. Every fourth step is a deep, saturated version of
  // its hue rather than a march toward #000 -- which is why even Black Trenchcoat, the palette
  // most entitled to go black, bottoms out at a readable grey.
  //
  // Names are the Cyberpunk 2020 style dichotomy and its neighbours -- pink mohawk versus black
  // trenchcoat is the genre's own argument about whether it is punk or noir.
  // ===========================================================================================

  /// Retro hot pink against teal. The 2077 arcade sign, the synthwave record sleeve.
  ///
  /// The one palette that is allowed to be obnoxious, and tuned differently from the rest
  /// because of it. The others run tint -> neon -> mid -> deep, which spends two of every four
  /// steps moving AWAY from neon: a pastel at the top, a near-dark at the bottom. That reads as
  /// muted on the knob no matter how bright the middle is. Here every step stays high-chroma
  /// and only the lightness moves, so the family ramps light-neon -> neon -> peak -> deep-neon
  /// and never washes out or goes muddy.
  static const List<Color> palettePinkMohawk = [
    Color(0xFFFF5FB5), Color(0xFFFF2D95), Color(0xFFFF0080), Color(0xFFD40069), // hot pink
    Color(0xFFF55FFF), Color(0xFFE92DFF), Color(0xFFD400FF), Color(0xFFA400D4), // magenta
    Color(0xFF5FFFD4), Color(0xFF2DFFC4), Color(0xFF00FFB3), Color(0xFF00D494), // mint/teal
    Color(0xFF5FF5FF), Color(0xFF2DEBFF), Color(0xFF00E0FF), Color(0xFF00B0CC), // cyan
  ];

  /// Phosphor green on grey. Terminal glass, falling glyphs, a long coat.
  static const List<Color> paletteBlackTrenchcoat = [
    Color(0xFFCFFFCF), Color(0xFF6BFF6B), Color(0xFF00FF41), Color(0xFF008F11), // phosphor
    Color(0xFFA3D9A3), Color(0xFF4FA84F), Color(0xFF2E7D32), Color(0xFF1B5E20), // moss
    Color(0xFFD8E8D8), Color(0xFF9AAE9A), Color(0xFF6E7F6E), Color(0xFF44513F), // grey-green
    Color(0xFFFFFFFF), Color(0xFFBFBFBF), Color(0xFF8A8A8A), Color(0xFF4A4A4A), // mono
  ];

  /// Cold blues into ultraviolet. ICE, daemons, the inside of the net.
  ///
  /// Tuned like Pink Mohawk rather than like the muted palettes: every step stays high-chroma
  /// and only the lightness moves. Ripperdoc and Nomad keep the tint/deep bookends on purpose
  /// -- bone is meant to be pale and sand is meant to be dusty -- but nothing about the inside
  /// of the net is supposed to look washed out.
  static const List<Color> paletteNetrunner = [
    Color(0xFF5FC4FF), Color(0xFF2DA8FF), Color(0xFF0084FF), Color(0xFF0066CC), // azure
    Color(0xFF8A7FFF), Color(0xFF6B5CFF), Color(0xFF4433FF), Color(0xFF3322CC), // indigo
    Color(0xFFC45FFF), Color(0xFFB02DFF), Color(0xFF9D00FF), Color(0xFF7A00CC), // violet
    Color(0xFF5FF0FF), Color(0xFF2DE8FF), Color(0xFF00DDFF), Color(0xFF00AECC), // cyan
  ];

  /// Surgical. Blood and bone against clinical teal, under a too-bright lamp.
  static const List<Color> paletteRipperdoc = [
    Color(0xFFFFB3B3), Color(0xFFFF5C5C), Color(0xFFFF0033), Color(0xFF99001F), // blood
    Color(0xFFFFD9A8), Color(0xFFFFB05C), Color(0xFFFF7A00), Color(0xFF9E4D00), // warning amber
    Color(0xFFFFFFFF), Color(0xFFF0E6D2), Color(0xFFC4B89A), Color(0xFF7A7264), // bone
    Color(0xFFCCF5F0), Color(0xFF66E0D6), Color(0xFF00C2B2), Color(0xFF00736A), // clinical teal
  ];

  /// Sodium lamps and rust. The road out of the city, seen at night.
  static const List<Color> paletteNomad = [
    Color(0xFFFFE8A8), Color(0xFFFFD24D), Color(0xFFFFB300), Color(0xFF9E6E00), // sodium
    Color(0xFFFFC2A8), Color(0xFFFF8A5C), Color(0xFFFF5500), Color(0xFF993300), // rust
    Color(0xFFF5E6C8), Color(0xFFD9C08F), Color(0xFFA68B4F), Color(0xFF6B5630), // sand
    Color(0xFFC2E8E0), Color(0xFF6BC4B5), Color(0xFF1F9E8C), Color(0xFF0F5C52), // dust teal
  ];

  /// The original flat list, kept as the DIY option: no colour story, just every bright hue
  /// on the wheel for when a macro needs to be findable rather than tasteful.
  ///
  /// Also the compatibility path. Every macro saved before the palettes existed carries one of
  /// these exact values, so keeping the list is what lets the picker still show those macros'
  /// colours as selected rather than matching nothing anywhere.
  static const List<Color> paletteDIY = [
    Color(0xFF00FF00), // Neon Green
    Color(0xFF00FF7F), // Spring Green
    Color(0xFF00FA9A), // Medium Spring Green
    Color(0xFF00FFFF), // Cyan
    Color(0xFF00CED1), // Dark Turquoise
    Color(0xFF00BFFF), // Deep Sky Blue
    Color(0xFF1E90FF), // Dodger Blue
    Color(0xFF4169E1), // Royal Blue
    Color(0xFF8A2BE2), // Blue Violet
    Color(0xFF9400D3), // Dark Violet
    Color(0xFF9932CC), // Dark Orchid
    Color(0xFFBA55D3), // Medium Orchid
    Color(0xFFFF00FF), // Magenta
    Color(0xFFFF1493), // Deep Pink
    Color(0xFFFF69B4), // Hot Pink
    Color(0xFFFFB6C1), // Light Pink
    Color(0xFFFF0000), // Red
    Color(0xFFDC143C), // Crimson
    Color(0xFFFF4500), // Orange Red
    Color(0xFFFF6347), // Tomato
    Color(0xFFFF7F50), // Coral
    Color(0xFFFFA500), // Orange
    Color(0xFFFFD700), // Gold
    Color(0xFFFFFF00), // Yellow
    Color(0xFFADFF2F), // Green Yellow
    Color(0xFF7FFF00), // Chartreuse
    Color(0xFF32CD32), // Lime Green
    Color(0xFF008000), // Green
    Color(0xFF008B8B), // Dark Cyan
    Color(0xFF4B0082), // Indigo
    Color(0xFF800080), // Purple
    Color(0xFFC71585), // Medium Violet Red
  ];

  /// The palettes in picker order. Pink Mohawk leads because it is the loudest, and this is a
  /// device whose whole job is to be looked at. DIY sits last: it is the escape hatch, not a
  /// starting point.
  static const Map<String, List<Color>> colorPalettes = {
    'Pink Mohawk': palettePinkMohawk,
    'Black Trenchcoat': paletteBlackTrenchcoat,
    'Netrunner': paletteNetrunner,
    'Ripperdoc': paletteRipperdoc,
    'Nomad': paletteNomad,
    'DIY': paletteDIY,
  };

  /// Every palette colour, flattened. Used to decide which palette tab to open on: a macro whose
  /// colour predates the palettes matches nothing here, and the picker opens on the first tab
  /// while still showing that colour as the current one.
  static List<Color> get allPaletteColors =>
      colorPalettes.values.expand((p) => p).toList();

  static ThemeData get darkTheme {
    return ThemeData(
      brightness: Brightness.dark,
      scaffoldBackgroundColor: background,
      primaryColor: accent,
      colorScheme: const ColorScheme.dark(
        primary: accent,
        secondary: accent,
        surface: surface,
        background: background,
      ),
      appBarTheme: const AppBarTheme(
        backgroundColor: background,
        elevation: 0,
        centerTitle: true,
        titleTextStyle: TextStyle(
          color: textPrimary,
          fontSize: 20,
          fontWeight: FontWeight.bold,
          letterSpacing: 1.2,
        ),
      ),
      cardTheme: CardThemeData(
        color: surface,
        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(8)),
        elevation: 4,
      ),
      elevatedButtonTheme: ElevatedButtonThemeData(
        style: ElevatedButton.styleFrom(
          backgroundColor: accent,
          foregroundColor: Colors.black,
          shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(4)),
          padding: const EdgeInsets.symmetric(horizontal: 24, vertical: 12),
          textStyle: const TextStyle(fontWeight: FontWeight.bold),
        ),
      ),
      outlinedButtonTheme: OutlinedButtonThemeData(
        style: OutlinedButton.styleFrom(
          foregroundColor: accent,
          side: const BorderSide(color: accent),
          shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(4)),
        ),
      ),
      textTheme: const TextTheme(
        bodyLarge: TextStyle(color: textPrimary),
        bodyMedium: TextStyle(color: textSecondary),
      ),
    );
  }
}
