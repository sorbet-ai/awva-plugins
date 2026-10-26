// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The generated tokens (the app's and the plugins' editor's) are design/tokens.json's.
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

import '../tool/gen_tokens.dart' as gen;

void main() {
  test('lib/src/tokens.g.dart is current (dart run tool/gen_tokens.dart)', () {
    expect(File(gen.dartOut).readAsStringSync(), gen.dartTokens(gen.readTokens()));
  });
  test('include/awva_plugin/design_tokens.h is current', () {
    expect(File(gen.cppOut).readAsStringSync(), gen.cppTokens(gen.readTokens()));
  });
}
