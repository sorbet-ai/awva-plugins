// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The generated tokens (the app's and the plugins' editor's) are design/tokens.json's.
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

import '../tool/gen_tokens.dart' as gen;

// (a checkout may have CRLF line ends)
String _read(String path) => File(path).readAsStringSync().replaceAll('\r\n', '\n');

void main() {
  test('lib/src/tokens.g.dart is current (dart run tool/gen_tokens.dart)', () {
    expect(_read(gen.dartOut), gen.dartTokens(gen.readTokens()));
  });
  test('include/awva_plugin/design_tokens.h is current', () {
    expect(_read(gen.cppOut), gen.cppTokens(gen.readTokens()));
  });
}
