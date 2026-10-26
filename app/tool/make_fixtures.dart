// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The screenshots' data, from the real core: the catalog and a sound, as the core gives them.
//   dart run tool/make_fixtures.dart CORE_LIBRARY BUILTIN_DIR
import 'dart:convert';
import 'dart:io';

import 'package:awva/src/core.dart';

void main(List<String> args) {
  final core = FfiBackend.start(path: args[0], options: {
    'no_audio': true,
    'midi': false,
    'settings': '',
    'builtin_dir': args[1],
    'engines_dir': '${Directory.systemTemp.path}/awva-fixtures-none',
  });
  final enc = const JsonEncoder.withIndent('  ');
  final catalog = jsonDecode(_raw(core, 'catalog'));
  File('test/fixtures/catalog.json').writeAsStringSync(enc.convert(catalog));
  for (final (key, preset) in [('builtin:bowed', 2), ('builtin:va', 1)]) {
    core.selectEngine(key);
    core.selectPreset(preset);
    sleep(const Duration(milliseconds: 100)); // (the audio thread applies it)
    File('test/fixtures/sound_${key.split(':')[1]}.json').writeAsStringSync(enc.convert(jsonDecode(_raw(core, 'sound'))));
  }
  core.stop();
}

String _raw(FfiBackend core, String what) => what == 'catalog'
    ? jsonEncode([
        for (final e in core.catalog())
          {
            'key': e.key, 'label': e.label, 'source': e.source, 'builtin': e.builtin, 'id': e.id, 'title': e.title,
            'version': e.version, 'summary': e.summary, 'colour': e.colour, 'ok': e.ok, 'error': e.error, 'refused': e.refused,
          }
      ])
    : core.soundJson();
