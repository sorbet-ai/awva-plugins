// GENERATED from design/tokens.json by tool/gen_tokens.dart: do not edit.
// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
part of 'theme.dart';

const _lightColors = AwvaColors(
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

const _darkColors = AwvaColors(
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

class _Sizes {
  static const double s1 = 4;
  static const double s2 = 8;
  static const double s3 = 12;
  static const double s4 = 16;
  static const double s5 = 24;
  static const double s6 = 32;
  static const double s7 = 48;
  static const double header = 56;
  static const double footer = 32;
  static const double row = 32;
  static const double radiusSmall = 6;
  static const double radius = 10;
  static const double radiusLarge = 14;
  static const double knob = 76;
  static const double knobStroke = 5;
  static const double sliderStroke = 4;
  static const double meterStroke = 2;
  static const double iconSmall = 14;
  static const double iconRow = 16;
  static const double tipWidth = 300;
}

class _TypeToken {
  const _TypeToken(this.size, this.line, this.weight, this.spacing);
  final double size, line, spacing;
  final int weight;
}

const _family = 'DM Sans';
const _micro = _TypeToken(11, 14, 600, 0.6);
const _small = _TypeToken(12, 16, 400, 0);
const _smallStrong = _TypeToken(12, 16, 600, 0);
const _body = _TypeToken(13, 18, 400, 0);
const _bodyStrong = _TypeToken(13, 18, 500, 0);
const _value = _TypeToken(12, 16, 500, 0);
const _title = _TypeToken(15, 20, 600, 0);
const _sound = _TypeToken(26, 32, 500, -0.3);
