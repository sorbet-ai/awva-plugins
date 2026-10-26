// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The screen: what it shows and what it sends the core, and its screenshots in both themes
// (goldens: `flutter test --update-goldens` writes them; test/goldens/*.png).
import 'dart:io';

import 'package:awva/src/controller.dart';
import 'package:awva/src/screen.dart';
import 'package:awva/src/widgets.dart';
import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart';
import 'package:flutter_test/flutter_test.dart';

import 'fake_backend.dart';

Future<void> _fonts() async {
  final loader = FontLoader('DM Sans')..addFont(rootBundle.load('assets/fonts/DMSans.ttf'));
  await loader.load();
}

Future<FakeBackend> _pump(WidgetTester t, {Brightness b = Brightness.light, String engine = 'bowed', Size size = const Size(1040, 640)}) async {
  t.view.physicalSize = size;
  t.view.devicePixelRatio = 1;
  final core = FakeBackend(engine: engine);
  final k = Controller(core, poll: false);
  await t.pumpWidget(AwvaApp(controller: k, brightness: b));
  await t.runAsync(() async {
    final ctx = t.element(find.byType(MainScreen));
    await precacheImage(const AssetImage('assets/logo/awva.png'), ctx);
  });
  await t.pumpAndSettle();
  return core;
}

void main() {
  setUpAll(_fonts);

  testWidgets('the header shows the engine and preset; the heroes are knobs', (t) async {
    await _pump(t);
    expect(find.text('Bowed Strings'), findsOneWidget);
    expect(find.text('cello'), findsNWidgets(2)); // (the picker and the sound's name)
    expect(find.byType(Knob), findsNWidgets(8));
    expect(find.text('force hi'), findsOneWidget);
  });

  testWidgets('a knob drag sets the parameter in the core', (t) async {
    final core = await _pump(t);
    await t.drag(find.byType(Knob).first, const Offset(0, -60));
    await t.pumpAndSettle();
    expect(core.calls.where((c) => c.startsWith('param 2 ')), isNotEmpty);
  });

  testWidgets('the next preset button selects the next preset', (t) async {
    final core = await _pump(t);
    await t.tap(find.bySemanticsLabel('next preset'));
    expect(core.calls, contains('preset 3'));
  });

  testWidgets('the engine menu lists built-ins and packages, a refused one with its reason', (t) async {
    final core = await _pump(t);
    await t.tap(find.text('Bowed Strings'));
    await t.pumpAndSettle();
    expect(find.text('clarinet v3 (fit)'), findsOneWidget);
    expect(find.textContaining('built for a different awva'), findsOneWidget);
    await t.tap(find.text('Virtual Analog'));
    await t.pumpAndSettle();
    expect(core.calls, contains('engine builtin:va'));
  });

  testWidgets('the theme follows the OS, and the choice is kept by the core', (t) async {
    final core = await _pump(t, b: Brightness.dark);
    await t.tap(find.bySemanticsLabel('light or dark'));
    await t.pumpAndSettle();
    expect(core.settings['theme'], 'light');
  });

  // (the goldens are Windows renders: another OS rasterises the type differently)
  group('screenshots', skip: !Platform.isWindows, () {
    for (final (name, b) in [('light', Brightness.light), ('dark', Brightness.dark)]) {
      testWidgets('main, $name', (t) async {
        await _pump(t, b: b);
        await expectLater(find.byType(AwvaApp), matchesGoldenFile('goldens/main_$name.png'));
      });
      testWidgets('every parameter, $name', (t) async {
        await _pump(t, b: b, engine: 'va', size: const Size(1040, 900));
        await t.tap(find.text('Every parameter'));
        await t.pumpAndSettle();
        await expectLater(find.byType(AwvaApp), matchesGoldenFile('goldens/params_$name.png'));
      });
      testWidgets('settings, $name', (t) async {
        await _pump(t, b: b);
        await t.tap(find.bySemanticsLabel('settings'));
        await t.pumpAndSettle();
        await expectLater(find.byType(AwvaApp), matchesGoldenFile('goldens/settings_$name.png'));
      });
      testWidgets('engines, $name', (t) async {
        await _pump(t, b: b);
        await t.tap(find.text('Bowed Strings'));
        await t.pumpAndSettle();
        await expectLater(find.byType(AwvaApp), matchesGoldenFile('goldens/engines_$name.png'));
      });
      testWidgets('plugin size, $name', (t) async {
        await _pump(t, b: b, size: const Size(760, 480));
        await expectLater(find.byType(AwvaApp), matchesGoldenFile('goldens/compact_$name.png'));
      });
    }
  });
}
