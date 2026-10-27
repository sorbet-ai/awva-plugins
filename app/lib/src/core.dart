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
        value = (j['value'] as num?)?.toDouble() ?? 0,
        label = j['label'] as String? ?? '',
        hover = j['hover'] as String? ?? '',
        icon = j['icon'] as String? ?? '';
  final String name, group;

  /// for people (the engine's: awva ParamDesc): a label, what it does, its icon ("concept+modifier");
  /// empty from an engine built without its text
  final String label, hover, icon;
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
        mono = j['mono'] as bool;
  final String engineKey, engineTitle, engineLabel, engineSummary;
  final bool engineBuiltin;
  final List<PresetInfo> presets;
  final int preset;
  final List<int> heroes;
  final List<ParamInfo> params;
  final bool mono;

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
        buffer = j['buffer'] as int? ?? 0,
        mode = j['mode'] as String? ?? 'shared',
        wanted = j['wanted'] as String? ?? 'shared',
        frames = j['frames'] as int? ?? 128,
        exclusiveAvailable = j['exclusive_available'] as bool? ?? false,
        volume = (j['volume'] as num?)?.toDouble() ?? -1,
        bus = j['bus'] as String? ?? 'other',
        learned = j['learned'] as int? ?? 0,
        chosen = j['chosen'] as String? ?? '',
        missing = j['missing'] as bool? ?? false,
        notice = j['notice'] as String? ?? '';
  final String backend, device, state, message;

  /// the output asked for ('' the system's default); missing: it isn't there, the default plays, and
  /// notice says so
  final String chosen, notice;
  final bool missing;

  /// the endpoint's bus ("bluetooth", "usb", "other"), and the exclusive buffer the app learned it
  /// sustains (frames; -1 shared only; 0 none): a Bluetooth endpoint's buffer is the app's choice
  final String bus;
  final int learned;

  /// what plays ("shared", "exclusive", "null") and what was asked for
  final String mode, wanted;
  final int rate, buffer, frames;
  final bool exclusiveAvailable;

  /// exclusive: the Windows volume the app applies (linear); -1 none
  final double volume;
}

/// an output endpoint (awva_audio_devices): its id, name, bus ("bluetooth", "usb", "other")
class OutputDeviceInfo {
  OutputDeviceInfo(Map<String, dynamic> j)
      : id = j['id'] as String,
        name = j['name'] as String,
        bus = j['bus'] as String,
        isDefault = j['default'] as bool;
  final String id, name, bus;
  final bool isDefault;
}

/// the delay from a key to the output, part by part (awva_latency)
class LatencyInfo {
  LatencyInfo(Map<String, dynamic> j)
      : mode = j['mode'] as String? ?? '',
        rate = j['rate'] as int? ?? 0,
        periodFrames = j['period_frames'] as int? ?? 0,
        periodMs = _d(j['period_ms']),
        bufferFrames = j['buffer_frames'] as int? ?? 0,
        bufferMs = _d(j['buffer_ms']),
        streamMs = _d(j['stream_ms'], -1),
        blockFrames = j['block_frames'] as int? ?? 32,
        blockMs = _d(j['block_ms']),
        stepMs = _d(j['step_ms']),
        midiCount = j['midi_count'] as int? ?? 0,
        midiMin = _d(j['midi_min_ms']),
        midiMean = _d(j['midi_mean_ms']),
        midiMax = _d(j['midi_max_ms']),
        totalMs = _d(j['total_ms']),
        glitches = j['glitches'] as int? ?? 0,
        recoveries = j['recoveries'] as int? ?? 0;
  static double _d(Object? v, [double def = 0]) => (v as num?)?.toDouble() ?? def;
  final String mode;
  final int rate, periodFrames, bufferFrames, blockFrames, midiCount;

  /// glitches the audio thread saw (late wake-ups, slow renders, stalls) and the streams it opened
  /// again by itself
  final int glitches, recoveries;
  final double periodMs, bufferMs, streamMs, blockMs, stepMs, midiMin, midiMean, midiMax, totalMs;
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
    this.catalog = 0,
  });
  final double peakL, peakR, reductionDb, masterDb, cpu;
  final int voices, audioState, sampleRate, bufferFrames, midiEvents, generation, xruns, faults;
  final bool engineStopped;

  /// changes when the engines were scanned again (a package came or went)
  final int catalog;
}

abstract class AwvaBackend {
  List<EngineInfo> catalog();
  String enginesDir();
  int rescan();
  bool selectEngine(String key);
  Sound? sound();
  bool selectPreset(int index);
  void setParam(int index, double value);
  void setMono(bool on);
  void panic();
  void setMasterDb(double db);
  Status status();
  List<MidiPortInfo> midiPorts();
  void midiRefresh();
  void midiEnable(String name, bool on);
  AudioInfo audio();
  bool audioRestart();
  bool setAudioMode(String mode, int frames);
  List<OutputDeviceInfo> audioDevices();
  bool setAudioDevice(String id);
  LatencyInfo latency();
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
  @Uint32()
  external int catalog;
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
        _setMono = _lib.lookupFunction<Void Function(Int32), void Function(int)>('awva_set_mono'),
        _panic = _lib.lookupFunction<Void Function(), void Function()>('awva_panic'),
        _setMaster = _lib.lookupFunction<Void Function(Float), void Function(double)>('awva_set_master_db'),
        _status = _lib.lookupFunction<Void Function(Pointer<_Status>), void Function(Pointer<_Status>)>('awva_status_get'),
        _midiRefresh = _lib.lookupFunction<Void Function(), void Function()>('awva_midi_refresh'),
        _midiEnable = _lib.lookupFunction<Void Function(Pointer<Utf8>, Int32), void Function(Pointer<Utf8>, int)>('awva_midi_enable'),
        _audioRestart = _lib.lookupFunction<Int32 Function(), int Function()>('awva_audio_restart'),
        _setAudioMode = _lib.lookupFunction<Int32 Function(Pointer<Utf8>, Int32), int Function(Pointer<Utf8>, int)>('awva_set_audio_mode'),
        _latency = _lib.lookupFunction<_StrN, _Str>('awva_latency'),
        _audioDevices = _lib.lookupFunction<_StrN, _Str>('awva_audio_devices'),
        _setAudioDevice = _lib.lookupFunction<Int32 Function(Pointer<Utf8>), int Function(Pointer<Utf8>)>('awva_set_audio_device'),
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
  final _Str _catalog, _enginesDir, _sound, _midiPorts, _audio, _lastError, _latency, _audioDevices;
  final int Function(Pointer<Utf8>, int) _setAudioMode;
  final int Function() _rescan, _audioRestart;
  final int Function(Pointer<Utf8>) _selectEngine, _setAudioDevice;
  final int Function(int) _selectPreset;
  final int Function(int, double) _setParam;
  final void Function(int) _setMono;
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
  void setMono(bool on) => _setMono(on ? 1 : 0);
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
      catalog: s.catalog,
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
  bool setAudioMode(String mode, int frames) => _withStr(mode, (p) => _setAudioMode(p, frames)) == 0;
  @override
  List<OutputDeviceInfo> audioDevices() => [for (final d in _json(_audioDevices) as List) OutputDeviceInfo(d as Map<String, dynamic>)];
  @override
  bool setAudioDevice(String id) => _withStr(id, _setAudioDevice) == 0;
  @override
  LatencyInfo latency() => LatencyInfo(_json(_latency) as Map<String, dynamic>);
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
