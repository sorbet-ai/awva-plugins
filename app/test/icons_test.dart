// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The generated icon tables (the app's and the plugins' editor's) are the pinned awva's
// assets/icons; a parameter's tooltip shows its label and hover text.
import 'dart:io';

import 'package:awva/src/param_icon.dart';
import 'package:awva/src/theme.dart';
import 'package:flutter/gestures.dart';
import 'package:flutter/widgets.dart';
import 'package:flutter_test/flutter_test.dart';

import '../tool/gen_icons.dart' as gen;

// (a checkout may have CRLF line ends)
String _read(String path) => File(path).readAsStringSync().replaceAll('\r\n', '\n');

void main() {
  test('lib/src/param_icons.g.dart is current (dart run tool/gen_icons.dart)', () {
    expect(_read(gen.dartOut), gen.dartIcons(gen.readIcons()));
  });
  test('include/awva_plugin/param_icons.h is current', () {
    expect(_read(gen.cppOut), gen.cppIcons(gen.readIcons()));
  });
  test('an icon is its concept, with or without a modifier; none for an unknown or empty one', () {
    expect(hasParamIcon('release'), isTrue);
    expect(hasParamIcon('release+pedal'), isTrue);
    expect(hasParamIcon(''), isFalse);
    expect(hasParamIcon('nosuchconcept'), isFalse);
  });
  testWidgets('a parameter\'s tooltip: its label and hover text after the pointer rests on it', (t) async {
    await t.pumpWidget(WidgetsApp(
      color: const Color(0xFF000000),
      pageRouteBuilder: <T>(RouteSettings s, WidgetBuilder b) => PageRouteBuilder<T>(settings: s, pageBuilder: (c, _, _) => b(c)),
      home: AwvaTheme(
        colors: AwvaColors.light,
        child: Center(
          child: ParamTip(
            label: 'Release',
            hover: 'How long the sound takes to die away.',
            child: ParamIcon('release+pedal', color: AwvaColors.light.text2, background: AwvaColors.light.bg0),
          ),
        ),
      ),
    ));
    expect(find.text('Release', findRichText: true), findsNothing);
    final mouse = await t.createGesture(kind: PointerDeviceKind.mouse);
    await mouse.addPointer(location: Offset.zero);
    await mouse.moveTo(t.getCenter(find.byType(ParamIcon)));
    await t.pump(const Duration(milliseconds: 100));
    expect(find.textContaining('die away', findRichText: true), findsNothing);
    await t.pump(AwvaSize.tipDelay);
    expect(find.textContaining('die away', findRichText: true), findsOneWidget);
    await mouse.moveTo(Offset.zero);
    await t.pump();
    expect(find.textContaining('die away', findRichText: true), findsNothing);
    await mouse.removePointer();
  });
}
