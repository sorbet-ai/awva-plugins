// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// What the screen shows, read from the core: the catalog, the sound, the status (30 times a
// second), the MIDI ports and the audio output (every two seconds). It decides nothing; every
// action goes straight to the core.
import 'dart:async';

import 'package:flutter/foundation.dart';

import 'core.dart';

enum ThemeChoice { system, light, dark }

class Controller extends ChangeNotifier {
  Controller(this.core, {this.poll = true}) {
    theme = ThemeChoice.values.firstWhere((t) => t.name == core.setting('theme'), orElse: () => ThemeChoice.system);
    _reload();
    if (poll) {
      _fast = Timer.periodic(const Duration(milliseconds: 33), (_) => _tick());
      _slow = Timer.periodic(const Duration(seconds: 2), (_) => _slowTick());
    }
  }

  final AwvaBackend core;
  final bool poll;
  Timer? _fast, _slow;

  List<EngineInfo> engines = [];
  Sound? sound;
  List<MidiPortInfo> ports = [];
  AudioInfo audio = AudioInfo(const {});
  String enginesDir = '';
  ThemeChoice theme = ThemeChoice.system;
  String? message; // the last refusal, shown until the next action

  /// the status: its own notifier, so the meters repaint without the whole screen
  final status = ValueNotifier<Status>(const Status());
  int _generation = -1, _midiSeen = 0;

  /// MIDI activity: the time of the last message seen
  final midiActive = ValueNotifier<bool>(false);
  DateTime _midiAt = DateTime(0);

  void _reload() {
    engines = core.catalog();
    sound = core.sound();
    ports = core.midiPorts();
    audio = core.audio();
    enginesDir = core.enginesDir();
    status.value = core.status();
    notifyListeners();
  }

  void _tick() {
    final s = core.status();
    status.value = s;
    if (s.generation != _generation) {
      _generation = s.generation;
      sound = core.sound();
      notifyListeners();
    }
    if (s.midiEvents != _midiSeen) {
      _midiSeen = s.midiEvents;
      _midiAt = DateTime.now();
    }
    final on = DateTime.now().difference(_midiAt).inMilliseconds < 150;
    if (on != midiActive.value) midiActive.value = on;
  }

  void _slowTick() {
    core.midiRefresh();
    ports = core.midiPorts();
    audio = core.audio();
    notifyListeners();
  }

  void selectEngine(String key) {
    message = core.selectEngine(key) ? null : core.lastError();
    sound = core.sound();
    notifyListeners();
  }

  void selectPreset(int i) {
    message = core.selectPreset(i) ? null : core.lastError();
    notifyListeners();
  }

  void step(int d) {
    final s = sound;
    if (s == null || s.presets.isEmpty) return;
    selectPreset(((s.preset < 0 ? 0 : s.preset + d) % s.presets.length + s.presets.length) % s.presets.length);
  }

  void setParam(int i, double v) {
    core.setParam(i, v);
    sound?.params[i].value = v;
    notifyListeners();
  }

  void setLegato(bool on) {
    core.setLegato(on);
    sound = core.sound();
    notifyListeners();
  }

  void setMaster(double db) {
    core.setMasterDb(db);
    status.value = core.status();
  }

  void panic() => core.panic();

  void rescan() {
    core.rescan();
    _reload();
  }

  void setTheme(ThemeChoice t) {
    theme = t;
    core.setSetting('theme', t.name);
    notifyListeners();
  }

  void enablePort(String name, bool on) {
    core.midiEnable(name, on);
    ports = core.midiPorts();
    notifyListeners();
  }

  void restartAudio() {
    message = core.audioRestart() ? null : core.lastError();
    audio = core.audio();
    notifyListeners();
  }

  @override
  void dispose() {
    _fast?.cancel();
    _slow?.cancel();
    super.dispose();
  }
}
