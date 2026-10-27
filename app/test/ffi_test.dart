// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The UI's side of the core, against the real core library (../build/app; AWVA_CORE to point
// elsewhere): the null sink (a fake output list) and no MIDI ports, so it opens no device.
import 'dart:io';

import 'package:awva/src/core.dart';
import 'package:flutter_test/flutter_test.dart';

String _corePath() {
  final env = Platform.environment['AWVA_CORE'];
  if (env != null && env.isNotEmpty) return env;
  final dir = '${Directory.current.path}/../build/app';
  if (Platform.isWindows) return '$dir/awva_app_core.dll';
  if (Platform.isMacOS) return '$dir/libawva_app_core.dylib';
  return '$dir/libawva_app_core.so';
}

Future<Status> _waitFor(FfiBackend core, bool Function(Status) ok) async {
  for (var i = 0; i < 100; i++) {
    final s = core.status();
    if (ok(s)) return s;
    await Future<void>.delayed(const Duration(milliseconds: 20));
  }
  return core.status();
}

void main() {
  late FfiBackend core;
  final none = Directory.systemTemp.createTempSync('awva-ffi');

  setUpAll(() {
    core = FfiBackend.start(path: _corePath(), options: {
      'no_audio': true,
      'midi': false,
      'settings': '',
      'builtin_dir': '${File(_corePath()).parent.path}/engines',
      'engines_dir': none.path,
      'simulate_devices': [
        {'id': 'spk', 'name': 'Speakers', 'bus': 'other'},
        {'id': 'buds', 'name': 'Buds', 'bus': 'bluetooth'},
      ],
    });
  });
  tearDownAll(() {
    core.stop();
    none.deleteSync(recursive: true);
  });

  test('the catalog lists the built-in engines, every one playable', () {
    final engines = core.catalog();
    expect(engines.length, greaterThanOrEqualTo(10));
    expect(engines.every((e) => e.builtin && e.ok), isTrue);
    expect(engines.map((e) => e.id), containsAll(['va', 'bowed', 'trumpet']));
    expect(core.audio().state, 'null');
  });

  test('an engine plays, with its presets and heroes', () async {
    expect(core.selectEngine('builtin:va'), isTrue);
    final s = core.sound()!;
    expect(s.engineTitle, 'Virtual Analog');
    expect(s.presets, isNotEmpty);
    expect(s.heroes, isNotEmpty);
    expect(s.params.last.name, 'output');
    expect(core.selectEngine('builtin:nosuch'), isFalse);
    expect(core.lastError(), contains('no such engine'));
  });

  test('a preset changes the values; the generation tells the UI to read them again', () async {
    core.selectEngine('builtin:va');
    final g0 = core.status().generation;
    expect(core.selectPreset(1), isTrue);
    final st = await _waitFor(core, (s) => s.generation != g0);
    expect(st.generation, isNot(g0));
    expect(core.sound()!.preset, 1);
  });

  test('a parameter set reaches the engine', () async {
    final i = core.sound()!.heroes.first;
    final p = core.sound()!.params[i];
    final v = p.min + (p.max - p.min) * 0.25;
    core.setParam(i, v);
    await Future<void>.delayed(const Duration(milliseconds: 100));
    expect(core.sound()!.params[i].value, closeTo(v, (p.max - p.min) * 1e-3 + (p.kind != 0 ? 0.5 : 0)));
  });

  test('an MPE note sounds, through the safety chain, and the master level is the core\'s', () async {
    core.setMasterDb(0);
    // an MPE note on member channel 2: bend centre, Note On, pressure, then off
    core.ump([0x20E10040, 0x20913C64, 0x20D15000]);
    final s = await _waitFor(core, (s) => s.peakL > 0.01);
    expect(s.peakL, greaterThan(0.01));
    expect(s.peakL, lessThanOrEqualTo(0.8913));
    expect(s.midiEvents, greaterThanOrEqualTo(3));
    expect(s.masterDb, 0);
    core.ump([0x20813C40]);
    core.setMasterDb(-60);
    expect(core.status().masterDb, -60);
  });

  test('the outputs listed, one chosen, an unknown one refused', () {
    final d = core.audioDevices();
    expect(d.map((x) => x.name), ['Speakers', 'Buds']);
    expect(d.first.isDefault, isTrue);
    expect(d.last.bus, 'bluetooth');
    expect(core.setAudioDevice('buds'), isTrue);
    expect(core.audio().device, 'Buds');
    expect(core.audio().chosen, 'buds');
    expect(core.setAudioDevice('nosuch'), isFalse);
    expect(core.setAudioDevice(''), isTrue);
    expect(core.audio().device, 'Speakers');
  });

  test('settings round trip', () {
    core.setSetting('theme', 'dark');
    expect(core.setting('theme'), 'dark');
  });
}
