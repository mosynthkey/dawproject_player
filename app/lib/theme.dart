import 'package:flutter/material.dart';

abstract final class DawColors {
  static const appBg = Color(0xFF14161D);
  static const base = Color(0xFF22272D);
  static const surface = Color(0xFF2A2F38);
  static const accent = Color(0xFF80CCFF);
  static const accentMuted = Color(0x3380CCFF);
  static const border = Color(0x14FFFFFF);
  static const textHigh = Color(0xFFFFFFFF);
  static const textBody = Color(0xB3FFFFFF);
  static const textLow = Color(0x66FFFFFF);
  static const warning = Color(0xFFF5A830);
}

abstract final class DawTheme {
  static ThemeData get dark {
    return ThemeData(
      brightness: Brightness.dark,
      useMaterial3: true,
      scaffoldBackgroundColor: DawColors.appBg,
      colorScheme: const ColorScheme(
        brightness: Brightness.dark,
        primary: DawColors.accent,
        onPrimary: Colors.black,
        secondary: DawColors.accent,
        onSecondary: Colors.black,
        error: Colors.redAccent,
        onError: Colors.white,
        surface: DawColors.base,
        onSurface: DawColors.textHigh,
      ),
      dividerColor: DawColors.border,
      iconTheme: const IconThemeData(color: DawColors.textHigh),
    );
  }
}
