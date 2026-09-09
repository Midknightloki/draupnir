import 'package:flutter/material.dart';

class AppTheme {
  // Deep dark backgrounds inspired by VIA
  static const Color background = Color(0xFF111111);
  static const Color surface = Color(0xFF1E1E1E);
  static const Color surfaceHighlight = Color(0xFF2A2A2A);
  
  // Razer Synapse / Gamer accents
  static const Color accent = Color(0xFF00FF00); // Bright Neon Green
  static const Color accentDim = Color(0xFF004400); 

  static const Color textPrimary = Color(0xFFEEEEEE);
  static const Color textSecondary = Color(0xFFAAAAAA);
  
  static const List<Color> cyberpunkPalette = [
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
