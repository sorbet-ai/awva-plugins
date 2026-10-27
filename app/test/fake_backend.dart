// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// A fixed core for the widget tests and the screenshots: the catalog and sounds the real core gave
// (test/fixtures, made by tool/make_fixtures.dart), a package or two, two MIDI ports, a device.
import 'dart:convert';
import 'dart:io';

import 'package:awva/src/core.dart';

class FakeBackend implements AwvaBackend {
  FakeBackend({this.engine = 'bowed', this.packages = true}) {
    final cat = jsonDecode(File('test/fixtures/catalog.json').readAsStringSync()) as List;
    _catalog = [for (final e in cat) EngineInfo(e as Map<String, dynamic>)];
    if (packages) {
      _catalog.add(EngineInfo({
        'key': 'package:clarinet-v3-fit', 'label': 'clarinet v3 (fit)', 'source': 'clarinet-v3-fit', 'builtin': false, 'id': 'clarinet',
        'title': 'Clarinet', 'version': 'da3ad97', 'summary': 'single reed', 'colour': '', 'ok': true, 'error': '', 'refused': <String>[],
      }));
      _catalog.add(EngineInfo({
        'key': 'package:sax-old', 'label': 'sax v2', 'source': 'sax-old', 'builtin': false, 'id': '', 'title': 'sax-old', 'version': '',
        'summary': '', 'colour': '', 'ok': false, 'error': 'built for a different awva (1726a05; this app needs da3ad97)', 'refused': <String>[],
      }));
    }
    _sound = jsonDecode(File('test/fixtures/sound_$engine.json').readAsStringSync()) as Map<String, dynamic>;
  }
  final String engine;
  final bool packages;
  late List<EngineInfo> _catalog;
  late Map<String, dynamic> _sound;
  final settings = <String, String>{};
  final calls = <String>[];
  double master = -18;
  Status st = const Status(masterDb: -18, peakL: 0.31, peakR: 0.27, cpu: 0.04, voices: 3, audioState: 1, sampleRate: 48000, bufferFrames: 144);

  @override
  List<EngineInfo> catalog() => _catalog;
  @override
  String enginesDir() => r'C:\Users\you\AppData\Roaming\awva\engines';
  @override
  int rescan() {
    calls.add('rescan');
    return _catalog.length;
  }
  @override
  bool selectEngine(String key) {
    calls.add('engine $key');
    return _catalog.any((e) => e.key == key && e.ok);
  }

  @override
  Sound? sound() => Sound(_sound);
  @override
  bool selectPreset(int index) {
    calls.add('preset $index');
    _sound['preset'] = index;
    return true;
  }

  @override
  void setParam(int index, double value) {
    calls.add('param $index ${value.toStringAsFixed(3)}');
    ((_sound['params'] as List)[index] as Map)['value'] = value;
  }

  @override
  void setMono(bool on) {
    calls.add('mono $on');
    _sound['mono'] = on;
  }

  @override
  void panic() => calls.add('panic');
  @override
  void setMasterDb(double db) {
    calls.add('master ${db.toStringAsFixed(0)}');
    master = db;
    st = Status(masterDb: db, peakL: st.peakL, peakR: st.peakR, cpu: st.cpu, audioState: 1, sampleRate: 48000, bufferFrames: 144);
  }

  @override
  Status status() => st;
  @override
  List<MidiPortInfo> midiPorts() => [
        MidiPortInfo({'name': 'MPE controller', 'enabled': true, 'state': 'open', 'message': ''}),
        MidiPortInfo({'name': 'Keyboard', 'enabled': false, 'state': 'closed', 'message': ''}),
      ];
  @override
  void midiRefresh() {}
  @override
  void midiEnable(String name, bool on) => calls.add('midi $name $on');
  @override
  AudioInfo audio() => AudioInfo({
        'backend': 'WASAPI', 'device': device, 'state': 'running', 'message': audioMessage, 'rate': 48000, 'buffer': buffer, 'mode': plays ?? mode,
        'wanted': mode, 'frames': 256, 'exclusive_available': true, 'volume': (plays ?? mode) == 'exclusive' ? 0.3 : -1, 'bus': bus, 'learned': learned,
        'chosen': chosen, 'missing': notice.isNotEmpty, 'notice': notice,
      });
  // the outputs; chosen: the one asked for ('' the system's default); notice: the chosen one is missing
  String chosen = '', notice = '';
  final outputs = [
    OutputDeviceInfo({'id': 'spk', 'name': 'Speakers', 'bus': 'other', 'default': true}),
    OutputDeviceInfo({'id': 'dac', 'name': 'USB DAC', 'bus': 'usb', 'default': false}),
    OutputDeviceInfo({'id': 'buds', 'name': 'Buds', 'bus': 'bluetooth', 'default': false}),
  ];
  @override
  List<OutputDeviceInfo> audioDevices() => outputs;
  @override
  bool setAudioDevice(String id) {
    calls.add('output $id');
    chosen = id;
    device = outputs.firstWhere((d) => d.id == id, orElse: () => outputs.first).name;
    return true;
  }

  // the output: a device on a bus; plays: what plays when it isn't what was asked for
  String device = 'Speakers', bus = 'other', audioMessage = '';
  String? plays;
  int buffer = 144, learned = 0;
  @override
  bool audioRestart() => true;
  String mode = 'shared';
  @override
  bool setAudioMode(String m, int frames) {
    calls.add('audio $m $frames');
    mode = m;
    return true;
  }

  @override
  LatencyInfo latency() => LatencyInfo({
        'mode': mode, 'rate': 48000, 'period_frames': 144, 'period_ms': 3.0, 'buffer_frames': 144, 'buffer_ms': 3.0, 'stream_ms': 4.2,
        'block_frames': 32, 'block_ms': 0.67, 'step_ms': 2.0, 'midi_count': 412, 'midi_min_ms': 2.1, 'midi_mean_ms': 3.0, 'midi_max_ms': 3.9,
        'total_ms': 10.2, 'glitches': 0, 'recoveries': 0,
      });
  @override
  String setting(String key) => settings[key] ?? '';
  @override
  void setSetting(String key, String value) => settings[key] = value;
  @override
  String lastError() => '';
  @override
  void stop() {}
}
