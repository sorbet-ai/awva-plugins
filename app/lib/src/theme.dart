// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The awva design tokens (DESIGN.md): the logo's three ribbons (sage, dusty blue, butter) on its
// cream, and a warm ink for the dark theme. Every colour, size and duration the UI uses is here.
import 'package:flutter/widgets.dart';

part 'tokens.g.dart';

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

  static const light = _lightColors;
  static const darkTheme = _darkColors;
}

/// sizes, radii, durations
class AwvaSize {
  static const double s1 = _Sizes.s1, s2 = _Sizes.s2, s3 = _Sizes.s3, s4 = _Sizes.s4, s5 = _Sizes.s5, s6 = _Sizes.s6, s7 = _Sizes.s7;
  static const double header = _Sizes.header, footer = _Sizes.footer, row = _Sizes.row;
  static const double radiusSmall = _Sizes.radiusSmall, radius = _Sizes.radius, radiusLarge = _Sizes.radiusLarge;
  static const double knob = _Sizes.knob, knobStroke = _Sizes.knobStroke;
  static const double sliderStroke = _Sizes.sliderStroke, meterStroke = _Sizes.meterStroke;

  /// a parameter's icon: by a knob's name, in a parameter's row; its tooltip's width
  static const double iconSmall = _Sizes.iconSmall, iconRow = _Sizes.iconRow, tipWidth = _Sizes.tipWidth;
  static const Duration quick = Duration(milliseconds: 120);
  static const Duration open = Duration(milliseconds: 200);

  /// how long the pointer rests on a parameter before its tooltip shows
  static const Duration tipDelay = Duration(milliseconds: 350);
}

/// type: DM Sans (bundled, OFL), tabular figures everywhere
class AwvaType {
  static const _tnum = [FontFeature.tabularFigures()];

  static TextStyle _s(_TypeToken t, Color c) => TextStyle(
        fontFamily: _family,
        fontSize: t.size,
        height: t.line / t.size,
        fontWeight: FontWeight.values[(t.weight ~/ 100) - 1],
        fontVariations: [FontVariation('wght', t.weight.toDouble()), FontVariation('opsz', t.size.clamp(9, 40).toDouble())],
        fontFeatures: _tnum,
        letterSpacing: t.spacing,
        color: c,
      );

  /// group names, column labels: lower case, spaced
  static TextStyle micro(Color c) => _s(_micro, c);
  static TextStyle small(Color c) => _s(_small, c);

  /// a tooltip's title
  static TextStyle smallStrong(Color c) => _s(_smallStrong, c);
  static TextStyle body(Color c) => _s(_body, c);
  static TextStyle bodyStrong(Color c) => _s(_bodyStrong, c);
  static TextStyle title(Color c) => _s(_title, c);

  /// the sound's name
  static TextStyle sound(Color c) => _s(_sound, c);
  static TextStyle value(Color c) => _s(_value, c);
}

/// the colours down the tree
class AwvaTheme extends InheritedWidget {
  const AwvaTheme({super.key, required this.colors, required super.child});
  final AwvaColors colors;

  static AwvaColors of(BuildContext context) => context.dependOnInheritedWidgetOfExactType<AwvaTheme>()!.colors;

  @override
  bool updateShouldNotify(AwvaTheme old) => old.colors != colors;
}
