// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The core (app/core, awva_app.h) through dart:ffi. The UI holds no logic: it shows what these
// return and calls these when the user acts. AwvaBackend is the interface; FfiBackend is the real
// core; the tests also give the screen a fixed backend.
import 'dart:convert';
import 'dart:ffi';
import 'dart:io';

import 'package:ffi/ffi.dart';

class EngineInfo {
  EngineInfo(Map<String, dynamic> j)
      : key = j['key'] as String,
        label = j['label'] as String,
        source = j['source'] as String,
        builtin = j['builtin'] as bool,
        id = j['id'] as String,
        title = j['title'] as String,
        version = j['version'] as String,
        summary = j['summary'] as String,
        colour = j['colour'] as String,
        ok = j['ok'] as bool,
        error = j['error'] as String,
        refused = (j['refused'] as List).cast<String>();
  final String key, label, source, id, title, version, summary, colour, error;
  final bool builtin, ok;
  final List<String> refused;
}

class ParamInfo {
  ParamInfo(Map<String, dynamic> j)
      : name = j['name'] as String,
        group = j['group'] as String,
        min = (j['min'] as num).toDouble(),
        max = (j['max'] as num).toDouble(),
        kind = j['kind'] as int,
        def = (j['def'] as num).toDouble(),
        value = (j['value'] as num?)?.toDouble() ?? 0;
  final String name, group;
  final double min, max, def;
  final int kind; // 0 continuous, 1 steps, 2 toggle
  double value;
}

class PresetInfo {
  PresetInfo(Map<String, dynamic> j)
      : name = j['name'] as String,
        factory = j['factory'] as bool;
  final String name;
  final bool factory;
}

class Sound {
  Sound(Map<String, dynamic> j)
      : engineKey = (j['engine'] as Map)['key'] as String,
        engineTitle = (j['engine'] as Map)['title'] as String,
        engineLabel = (j['engine'] as Map)['label'] as String,
        engineSummary = (j['engine'] as Map)['summary'] as String,
        engineBuiltin = (j['engine'] as Map)['builtin'] as bool,
        presets = [for (final p in j['presets'] as List) PresetInfo(p as Map<String, dynamic>)],
        preset = j['preset'] as int,
        heroes = (j['heroes'] as List).cast<int>(),
        params = [for (final p in j['params'] as List) ParamInfo(p as Map<String, dynamic>)],
        legato = j['legato'] as bool;
  final String engineKey, engineTitle, engineLabel, engineSummary;
  final bool engineBuiltin;
  final List<PresetInfo> presets;
  final int preset;
  final List<int> heroes;
  final List<ParamInfo> params;
  final bool legato;

  String get presetName => preset >= 0 && preset < presets.length ? presets[preset].name : 'Custom';
}

class MidiPortInfo {
  MidiPortInfo(Map<String, dynamic> j)
      : name = j['name'] as String,
        enabled = j['enabled'] as bool,
        state = j['state'] as String,
        message = j['message'] as String;
  final String name, state, message;
  final bool enabled;
}

class AudioInfo {
  AudioInfo(Map<String, dynamic> j)
      : backend = j['backend'] as String? ?? '',
        device = j['device'] as String? ?? '',
        state = j['state'] as String? ?? 'off',
        message = j['message'] as String? ?? '',
        rate = j['rate'] as int? ?? 0,
        buffer = j['buffer'] as int? ?? 0;
  final String backend, device, state, message;
  final int rate, buffer;
}

class Status {
  const Status({
    this.peakL = 0,
    this.peakR = 0,
    this.reductionDb = 0,
    this.masterDb = -18,
    this.cpu = 0,
    this.voices = 0,
    this.audioState = 0,
    this.sampleRate = 0,
    this.bufferFrames = 0,
    this.midiEvents = 0,
    this.generation = 0,
    this.xruns = 0,
    this.faults = 0,
    this.engineStopped = false,
  });
  final double peakL, peakR, reductionDb, masterDb, cpu;
  final int voices, audioState, sampleRate, bufferFrames, midiEvents, generation, xruns, faults;
  final bool engineStopped;
}

abstract class AwvaBackend {
  List<EngineInfo> catalog();
  String enginesDir();
  int rescan();
  bool selectEngine(String key);
  Sound? sound();
  bool selectPreset(int index);
  void setParam(int index, double value);
  void setLegato(bool on);
  void panic();
  void setMasterDb(double db);
  Status status();
  List<MidiPortInfo> midiPorts();
  void midiRefresh();
  void midiEnable(String name, bool on);
  AudioInfo audio();
  bool audioRestart();
  String setting(String key);
  void setSetting(String key, String value);
  String lastError();
  void stop();
}

// --- the real core -------------------------------------------------------------------------------

final class _Status extends Struct {
  @Float()
  external double peakL, peakR, reductionDb, masterDb, cpu;
  @Int32()
  external int voices, audioState, sampleRate, bufferFrames;
  @Uint32()
  external int midiEvents, generation, xruns, faults;
  @Int32()
  external int engineStopped;
}

typedef _Str = Pointer<Utf8> Function();
typedef _StrN = Pointer<Utf8> Function();

class FfiBackend implements AwvaBackend {
  FfiBackend._(this._lib)
      : _catalog = _lib.lookupFunction<_StrN, _Str>('awva_catalog'),
        _enginesDir = _lib.lookupFunction<_StrN, _Str>('awva_engines_dir'),
        _sound = _lib.lookupFunction<_StrN, _Str>('awva_sound'),
        _midiPorts = _lib.lookupFunction<_StrN, _Str>('awva_midi_ports'),
        _audio = _lib.lookupFunction<_StrN, _Str>('awva_audio'),
        _lastError = _lib.lookupFunction<_StrN, _Str>('awva_last_error'),
        _rescan = _lib.lookupFunction<Int32 Function(), int Function()>('awva_rescan'),
        _selectEngine = _lib.lookupFunction<Int32 Function(Pointer<Utf8>), int Function(Pointer<Utf8>)>('awva_select_engine'),
        _selectPreset = _lib.lookupFunction<Int32 Function(Int32), int Function(int)>('awva_select_preset'),
        _setParam = _lib.lookupFunction<Int32 Function(Int32, Float), int Function(int, double)>('awva_set_param'),
        _setLegato = _lib.lookupFunction<Void Function(Int32), void Function(int)>('awva_set_legato'),
        _panic = _lib.lookupFunction<Void Function(), void Function()>('awva_panic'),
        _setMaster = _lib.lookupFunction<Void Function(Float), void Function(double)>('awva_set_master_db'),
        _status = _lib.lookupFunction<Void Function(Pointer<_Status>), void Function(Pointer<_Status>)>('awva_status_get'),
        _midiRefresh = _lib.lookupFunction<Void Function(), void Function()>('awva_midi_refresh'),
        _midiEnable = _lib.lookupFunction<Void Function(Pointer<Utf8>, Int32), void Function(Pointer<Utf8>, int)>('awva_midi_enable'),
        _audioRestart = _lib.lookupFunction<Int32 Function(), int Function()>('awva_audio_restart'),
        _setting = _lib.lookupFunction<Pointer<Utf8> Function(Pointer<Utf8>), Pointer<Utf8> Function(Pointer<Utf8>)>('awva_setting'),
        _setSetting = _lib.lookupFunction<Void Function(Pointer<Utf8>, Pointer<Utf8>), void Function(Pointer<Utf8>, Pointer<Utf8>)>(
            'awva_set_setting'),
        _ump = _lib.lookupFunction<Void Function(Pointer<Uint32>, Int32), void Function(Pointer<Uint32>, int)>('awva_ump'),
        _stop = _lib.lookupFunction<Void Function(), void Function()>('awva_stop'),
        _statusBuf = calloc<_Status>();

  /// the core library beside the app (Windows: the exe's folder; macOS: Contents/Frameworks), or [path]
  static String defaultPath() {
    final exe = File(Platform.resolvedExecutable).parent.path;
    if (Platform.isWindows) return '$exe\\awva_app_core.dll';
    if (Platform.isMacOS) return '$exe/../Frameworks/libawva_app_core.dylib';
    return '$exe/lib/libawva_app_core.so';
  }

  /// loads and starts the core; options as awva_start() takes them
  static FfiBackend start({String? path, Map<String, Object?> options = const {}}) {
    final lib = DynamicLibrary.open(path ?? defaultPath());
    final start = lib.lookupFunction<Int32 Function(Pointer<Utf8>), int Function(Pointer<Utf8>)>('awva_start');
    final o = jsonEncode(options).toNativeUtf8();
    final r = start(o);
    calloc.free(o);
    if (r != 0) throw StateError('the core did not start ($r)');
    return FfiBackend._(lib);
  }

  final DynamicLibrary _lib;
  final _Str _catalog, _enginesDir, _sound, _midiPorts, _audio, _lastError;
  final int Function() _rescan, _audioRestart;
  final int Function(Pointer<Utf8>) _selectEngine;
  final int Function(int) _selectPreset;
  final int Function(int, double) _setParam;
  final void Function(int) _setLegato;
  final void Function() _panic, _midiRefresh, _stop;
  final void Function(double) _setMaster;
  final void Function(Pointer<_Status>) _status;
  final void Function(Pointer<Utf8>, int) _midiEnable;
  final Pointer<Utf8> Function(Pointer<Utf8>) _setting;
  final void Function(Pointer<Utf8>, Pointer<Utf8>) _setSetting;
  final void Function(Pointer<Uint32>, int) _ump;
  final Pointer<_Status> _statusBuf;

  DynamicLibrary get library => _lib;

  dynamic _json(_Str f) => jsonDecode(f().toDartString());

  T _withStr<T>(String s, T Function(Pointer<Utf8>) f) {
    final p = s.toNativeUtf8();
    try {
      return f(p);
    } finally {
      calloc.free(p);
    }
  }

  @override
  List<EngineInfo> catalog() => [for (final e in _json(_catalog) as List) EngineInfo(e as Map<String, dynamic>)];
  @override
  String enginesDir() => _enginesDir().toDartString();
  @override
  int rescan() => _rescan();
  @override
  bool selectEngine(String key) => _withStr(key, _selectEngine) == 0;
  @override
  Sound? sound() {
    final j = _json(_sound) as Map<String, dynamic>;
    return j.isEmpty ? null : Sound(j);
  }

  /// the sound as the core gives it (the fixtures)
  String soundJson() => _sound().toDartString();

  @override
  bool selectPreset(int index) => _selectPreset(index) == 0;
  @override
  void setParam(int index, double value) => _setParam(index, value);
  @override
  void setLegato(bool on) => _setLegato(on ? 1 : 0);
  @override
  void panic() => _panic();
  @override
  void setMasterDb(double db) => _setMaster(db);
  @override
  Status status() {
    _status(_statusBuf);
    final s = _statusBuf.ref;
    return Status(
      peakL: s.peakL,
      peakR: s.peakR,
      reductionDb: s.reductionDb,
      masterDb: s.masterDb,
      cpu: s.cpu,
      voices: s.voices,
      audioState: s.audioState,
      sampleRate: s.sampleRate,
      bufferFrames: s.bufferFrames,
      midiEvents: s.midiEvents,
      generation: s.generation,
      xruns: s.xruns,
      faults: s.faults,
      engineStopped: s.engineStopped != 0,
    );
  }

  @override
  List<MidiPortInfo> midiPorts() => [for (final p in _json(_midiPorts) as List) MidiPortInfo(p as Map<String, dynamic>)];
  @override
  void midiRefresh() => _midiRefresh();
  @override
  void midiEnable(String name, bool on) => _withStr(name, (p) => _midiEnable(p, on ? 1 : 0));
  @override
  AudioInfo audio() => AudioInfo(_json(_audio) as Map<String, dynamic>);
  @override
  bool audioRestart() => _audioRestart() == 0;
  @override
  String setting(String key) => _withStr(key, (p) => _setting(p).toDartString());
  @override
  void setSetting(String key, String value) => _withStr(key, (k) => _withStr(value, (v) => _setSetting(k, v)));
  @override
  String lastError() => _lastError().toDartString();

  /// UMP words into the sound, as from a port (tests)
  void ump(List<int> words) {
    final p = calloc<Uint32>(words.length);
    for (var i = 0; i < words.length; i++) {
      p[i] = words[i];
    }
    _ump(p, words.length);
    calloc.free(p);
  }

  @override
  void stop() => _stop();
}
