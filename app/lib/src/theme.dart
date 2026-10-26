// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The awva design tokens (DESIGN.md): the logo's three ribbons (sage, dusty blue, butter) on its
// cream, and a warm ink for the dark theme. Every colour, size and duration the UI uses is here.
import 'package:flutter/widgets.dart';

class AwvaColors {
  const AwvaColors({
    required this.bg0,
    required this.bg1,
    required this.bg2,
    required this.bg3,
    required this.hairline,
    required this.text1,
    required this.text2,
    required this.text3,
    required this.sage,
    required this.blue,
    required this.butter,
    required this.onSage,
    required this.error,
    required this.shadow,
    required this.dark,
  });

  final Color bg0, bg1, bg2, bg3, hairline;
  final Color text1, text2, text3;

  /// the primary accent: values (knob arcs), the selection, focus
  final Color sage;

  /// signal in: MIDI activity, the level meter
  final Color blue;

  /// attention: the limiter working, a warning
  final Color butter;
  final Color onSage, error, shadow;
  final bool dark;

  static const light = AwvaColors(
    bg0: Color(0xFFF6F1E7),
    bg1: Color(0xFFFBF8F2),
    bg2: Color(0xFFEEE7DA),
    bg3: Color(0xFFE4DCCB),
    hairline: Color(0xFFDDD4C2),
    text1: Color(0xFF2A2925),
    text2: Color(0xFF66625A),
    text3: Color(0xFF8E897E),
    sage: Color(0xFF5F805B),
    blue: Color(0xFF5E7D95),
    butter: Color(0xFF9A7B28),
    onSage: Color(0xFFFBF8F2),
    error: Color(0xFFB4483C),
    shadow: Color(0x2E3A3020),
    dark: false,
  );

  static const darkTheme = AwvaColors(
    bg0: Color(0xFF141613),
    bg1: Color(0xFF1B1D1A),
    bg2: Color(0xFF232622),
    bg3: Color(0xFF2D312C),
    hairline: Color(0xFF343832),
    text1: Color(0xFFF3EEE3),
    text2: Color(0xFFA7A396),
    text3: Color(0xFF6D6A61),
    sage: Color(0xFF9EB69B),
    blue: Color(0xFF7896AD),
    butter: Color(0xFFDCC584),
    onSage: Color(0xFF141613),
    error: Color(0xFFE5866F),
    shadow: Color(0x8C000000),
    dark: true,
  );
}

/// sizes, radii, durations
class AwvaSize {
  static const double s1 = 4, s2 = 8, s3 = 12, s4 = 16, s5 = 24, s6 = 32, s7 = 48;
  static const double header = 56, footer = 32, row = 32;
  static const double radiusSmall = 6, radius = 10, radiusLarge = 14;
  static const double knob = 76, knobStroke = 5;
  static const Duration quick = Duration(milliseconds: 120);
  static const Duration open = Duration(milliseconds: 200);
}

/// type: DM Sans (bundled, OFL), tabular figures everywhere
class AwvaType {
  static const _family = 'DM Sans';
  static const _tnum = [FontFeature.tabularFigures()];

  static TextStyle _s(double size, double height, int weight, Color c, {double spacing = 0}) => TextStyle(
        fontFamily: _family,
        fontSize: size,
        height: height / size,
        fontWeight: FontWeight.values[(weight ~/ 100) - 1],
        fontVariations: [FontVariation('wght', weight.toDouble()), FontVariation('opsz', size.clamp(9, 40).toDouble())],
        fontFeatures: _tnum,
        letterSpacing: spacing,
        color: c,
      );

  /// group names, column labels: 11/14, 600, lower case, spaced
  static TextStyle micro(Color c) => _s(11, 14, 600, c, spacing: 0.6);
  static TextStyle small(Color c) => _s(12, 16, 400, c);
  static TextStyle body(Color c) => _s(13, 18, 400, c);
  static TextStyle bodyStrong(Color c) => _s(13, 18, 500, c);
  static TextStyle title(Color c) => _s(15, 20, 600, c);

  /// the sound's name
  static TextStyle sound(Color c) => _s(26, 32, 500, c, spacing: -0.3);
  static TextStyle value(Color c) => _s(12, 16, 500, c);
}

/// the colours down the tree
class AwvaTheme extends InheritedWidget {
  const AwvaTheme({super.key, required this.colors, required super.child});
  final AwvaColors colors;

  static AwvaColors of(BuildContext context) => context.dependOnInheritedWidgetOfExactType<AwvaTheme>()!.colors;

  @override
  bool updateShouldNotify(AwvaTheme old) => old.colors != colors;
}
