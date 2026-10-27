// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The screen at three window sizes (the 720x440 minimum, the 1040x640 default, a large one) in
// both themes, with long names: the sound, every parameter open, the settings panel, the engine
// and preset menus, all without an overflow or an exception; the Tab order and the keys; a MIDI
// port that leaves and comes back, nothing stale in the header, the footer or the panel.
import 'package:awva/src/controller.dart';
import 'package:awva/src/core.dart';
import 'package:awva/src/screen.dart';
import 'package:awva/src/widgets.dart';
import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart';
import 'package:flutter_test/flutter_test.dart';

import 'fake_backend.dart';

const sizes = [Size(720, 440), Size(1040, 640), Size(1400, 900)];

Future<void> _fonts() async {
  final loader = FontLoader('DM Sans')..addFont(rootBundle.load('assets/fonts/DMSans.ttf'));
  await loader.load();
}

/// a long engine title with a long package label, and a long preset name
void _longNames(FakeBackend core) {
  final s = core.soundMap;
  (s['engine'] as Map)['title'] = 'Bowed Strings, the long edition of the title';
  (s['engine'] as Map)['label'] = 'strings v12 (fitted against the 2026 recordings)';
  (s['engine'] as Map)['builtin'] = false;
  ((s['presets'] as List)[s['preset'] as int] as Map)['name'] = 'a preset with a very long name indeed, longer than the column';
}

void main() {
  setUpAll(_fonts);

  for (final size in sizes) {
    for (final (theme, b) in [('light', Brightness.light), ('dark', Brightness.dark)]) {
      testWidgets('${size.width.toInt()}x${size.height.toInt()} $theme: the screen, its menus and panel, long names, no overflow', (t) async {
        String report = '';
        final prev = FlutterError.onError;
        FlutterError.onError = (d) {
          report = d.toString();
          prev?.call(d);
        };
        addTearDown(() => FlutterError.onError = prev);
        Future<void> clean(String where) async {
          await t.pumpAndSettle();
          final e = t.takeException();
          expect(e, isNull, reason: '$where: $report');
        }

        t.view.physicalSize = size;
        t.view.devicePixelRatio = 1;
        addTearDown(t.view.reset);
        final core = FakeBackend(engine: 'va');
        _longNames(core);
        final k = Controller(core, poll: false);
        await t.pumpWidget(AwvaApp(controller: k, brightness: b));
        // the logo decoded (its width counts in the header)
        await t.runAsync(() => precacheImage(const AssetImage('assets/logo/awva.png'), t.element(find.byType(MainScreen))));
        await clean('the sound');
        expect(find.byType(Knob), findsWidgets);

        await t.tap(find.text('Every parameter'));
        await clean('every parameter');
        await t.tap(find.text('Every parameter'));

        await t.tap(find.bySemanticsLabel('settings'));
        await clean('settings');
        // Esc closes the panel
        await t.sendKeyEvent(LogicalKeyboardKey.escape);
        await clean('settings closed');
        expect(find.text('Settings'), findsNothing);

        await t.tap(find.bySemanticsLabel('settings'));
        await t.pumpAndSettle();
        await t.tap(find.text('System default'));
        await clean('the output menu');
        await t.sendKeyEvent(LogicalKeyboardKey.escape);
        await t.pumpAndSettle();
        await t.sendKeyEvent(LogicalKeyboardKey.escape);
        await t.pumpAndSettle();

        await t.tap(find.textContaining('Bowed Strings, the long'));
        await clean('the engine menu');
        await t.sendKeyEvent(LogicalKeyboardKey.escape);
        await t.pumpAndSettle();
        await t.tap(find.bySemanticsLabel('next preset'));
        await clean('the next preset');
        await t.tap(find.textContaining('a preset with a very long').first);
        await clean('the preset menu');
        await t.sendKeyEvent(LogicalKeyboardKey.escape);
        await clean('menus closed');
      });
    }
  }

  testWidgets('Tab moves through the controls; Enter, Space and the arrow keys work them', (t) async {
    t.view.physicalSize = const Size(1040, 640);
    t.view.devicePixelRatio = 1;
    addTearDown(t.view.reset);
    final core = FakeBackend();
    final k = Controller(core, poll: false);
    await t.pumpWidget(AwvaApp(controller: k, brightness: Brightness.light));
    await t.pumpAndSettle();
    // Tab walks the header (the pickers, the arrows, the level, the theme, settings), then the
    // sound: every stop is one of the app's controls, named
    String? focused() => FocusManager.instance.primaryFocus?.debugLabel;
    Future<void> tabTo(String label) async {
      for (var i = 0; i < 40; i++) {
        await t.sendKeyEvent(LogicalKeyboardKey.tab);
        await t.pump();
        if (focused() == label) return;
      }
      fail('no Tab stop "$label" in 40 stops');
    }

    final stops = <String>{};
    for (var i = 0; i < 12; i++) {
      await t.sendKeyEvent(LogicalKeyboardKey.tab);
      await t.pump();
      stops.add(focused() ?? '?');
    }
    expect(stops, containsAll(['Bowed Strings', 'previous preset', 'cello', 'next preset', 'level', 'light or dark', 'settings']));
    expect(stops.contains('?'), isFalse, reason: 'every stop is a named control: $stops');
    // the level: the arrow keys move it by 1 dB, Delete puts it back to -18
    await tabTo('level');
    await t.sendKeyEvent(LogicalKeyboardKey.arrowUp);
    await t.pump();
    expect(core.calls, contains('master -17'));
    await t.sendKeyEvent(LogicalKeyboardKey.delete);
    await t.pump();
    expect(core.calls.last, 'master -18');
    // the theme button: Enter presses it
    await tabTo('light or dark');
    await t.sendKeyEvent(LogicalKeyboardKey.enter);
    await t.pumpAndSettle();
    expect(core.settings['theme'], 'dark');
    // a knob: the arrows, Home
    await tabTo('knob force_hi');
    final before = core.calls.length;
    await t.sendKeyEvent(LogicalKeyboardKey.arrowRight);
    await t.pump();
    expect(core.calls.length, before + 1);
    expect(core.calls.last, startsWith('param '));
    await t.sendKeyEvent(LogicalKeyboardKey.home);
    await t.pump();
    expect(core.calls.last, startsWith('param '));
  });

  testWidgets('a MIDI port leaves and comes back: the header dot, the footer and the panel follow', (t) async {
    t.view.physicalSize = const Size(1040, 900);
    t.view.devicePixelRatio = 1;
    addTearDown(t.view.reset);
    final core = FakeBackend();
    final k = Controller(core, poll: false);
    await t.pumpWidget(AwvaApp(controller: k, brightness: Brightness.light));
    await t.pumpAndSettle();
    expect(find.text('MIDI in: MPE controller'), findsOneWidget);
    await t.tap(find.bySemanticsLabel('settings'));
    await t.pumpAndSettle();
    expect(find.text('MPE controller'), findsOneWidget);
    // unplugged: the core lists it no more; the UI reads the ports again (its 2 s tick)
    core.ports = [core.ports.last];
    k.slowTick();
    await t.pumpAndSettle();
    expect(find.text('No MIDI input'), findsOneWidget);
    expect(find.text('MPE controller'), findsNothing, reason: 'the panel lists the ports that are there');
    expect(t.takeException(), isNull);
    // back, busy in another app first, then open
    core.ports = [
      MidiPortInfo({'name': 'MPE controller', 'enabled': true, 'state': 'busy', 'message': 'held by another app: close it, or turn the port off here'}),
      core.ports.last,
    ];
    k.slowTick();
    await t.pumpAndSettle();
    expect(find.textContaining('MPE controller: held by another app'), findsOneWidget, reason: 'the footer says what to do');
    core.ports = [
      MidiPortInfo({'name': 'MPE controller', 'enabled': true, 'state': 'open', 'message': ''}),
      core.ports.last,
    ];
    k.slowTick();
    await t.pumpAndSettle();
    expect(find.text('MIDI in: MPE controller'), findsOneWidget);
    expect(find.textContaining('held by another app'), findsNothing, reason: 'nothing stale once it is open');
    expect(t.takeException(), isNull);
  });
}
