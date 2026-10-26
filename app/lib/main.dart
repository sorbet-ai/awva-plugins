// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// awva, the standalone instrument. Options:
//   --no-audio           no audio device: the core's null sink (smoke runs, tests)
//   --hidden             no window (the runners read it too; smoke runs on someone's machine)
//   --no-midi            no MIDI ports opened
//   --engines-dir DIR    the packages folder (default: %APPDATA%\awva\engines,
//                        ~/Library/Application Support/awva/engines)
//   --probe-wav FILE     every output sample, after the safety chain, also into FILE (float WAV)
//   --core FILE          the core library (default: beside the app)
//   --exit-after S       quit after S seconds (smoke runs)
import 'dart:async';
import 'dart:io';
import 'dart:ui' show AppExitType;

import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart';

import 'src/controller.dart';
import 'src/core.dart';
import 'src/screen.dart';

String? _arg(List<String> args, String name) {
  final i = args.indexOf(name);
  return i >= 0 && i + 1 < args.length ? args[i + 1] : null;
}

void main(List<String> args) {
  WidgetsFlutterBinding.ensureInitialized();
  final options = <String, Object?>{
    if (args.contains('--no-audio')) 'no_audio': true,
    if (args.contains('--no-midi')) 'midi': false,
    if (_arg(args, '--engines-dir') != null) 'engines_dir': _arg(args, '--engines-dir'),
    if (_arg(args, '--probe-wav') != null) 'probe_wav': _arg(args, '--probe-wav'),
  };
  final core = FfiBackend.start(path: _arg(args, '--core'), options: options);
  final controller = Controller(core);
  final exitAfter = double.tryParse(_arg(args, '--exit-after') ?? '');
  if (exitAfter != null) {
    Timer(Duration(milliseconds: (exitAfter * 1000).round()), () async {
      final s = core.status();
      stdout.writeln('awva: ${core.audio().state}, ${s.midiEvents} MIDI messages, ${s.faults} faults');
      controller.dispose();
      core.stop();
      // (through the platform, as a window close: exit() under a running engine aborts on macOS)
      await ServicesBinding.instance.exitApplication(AppExitType.required);
    });
  }
  runApp(AwvaApp(controller: controller));
}
