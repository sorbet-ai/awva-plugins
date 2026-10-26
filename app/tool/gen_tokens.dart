// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The design tokens (design/tokens.json) as the app's Dart (lib/src/tokens.g.dart) and the plugins'
// editor's C++ (../include/awva_plugin/design_tokens.h): one source, so the two never drift.
//   dart run tool/gen_tokens.dart          (in app/; test/tokens_test.dart checks they are current)
import 'dart:convert';
import 'dart:io';

const dartOut = 'lib/src/tokens.g.dart';
const cppOut = '../include/awva_plugin/design_tokens.h';

Map<String, dynamic> readTokens([String path = 'design/tokens.json']) => jsonDecode(File(path).readAsStringSync()) as Map<String, dynamic>;

/// "#RRGGBB" or "#AARRGGBB" as 0xAARRGGBB
String argb(String hex) {
  final h = hex.substring(1).toUpperCase();
  return '0x${h.length == 6 ? 'FF$h' : h}';
}

String num_(num v) => v == v.roundToDouble() ? v.toStringAsFixed(0) : v.toString();

/// a C++ float literal
String cf(num v) => v == v.roundToDouble() ? '${v.toStringAsFixed(0)}.0f' : '${v}f';

String dartTokens(Map<String, dynamic> t) {
  final colors = t['colors'] as Map<String, dynamic>;
  final b = StringBuffer()
    ..writeln('// GENERATED from design/tokens.json by tool/gen_tokens.dart: do not edit.')
    ..writeln('// SPDX-FileCopyrightText: 2026 Sorbet AI')
    ..writeln('// SPDX-License-Identifier: Apache-2.0')
    ..writeln("part of 'theme.dart';")
    ..writeln();
  for (final theme in ['light', 'dark']) {
    b.writeln('const _${theme}Colors = AwvaColors(');
    for (final e in colors.entries) {
      b.writeln('  ${e.key}: Color(${argb((e.value as Map)[theme] as String)}),');
    }
    b
      ..writeln('  dark: ${theme == 'dark'},')
      ..writeln(');')
      ..writeln();
  }
  b.writeln('class _Sizes {');
  for (final e in (t['sizes'] as Map<String, dynamic>).entries) {
    b.writeln('  static const double ${e.key} = ${num_(e.value as num)};');
  }
  b.writeln('}');
  b.writeln();
  b.writeln('class _TypeToken {');
  b.writeln('  const _TypeToken(this.size, this.line, this.weight, this.spacing);');
  b.writeln('  final double size, line, spacing;');
  b.writeln('  final int weight;');
  b.writeln('}');
  b.writeln();
  final type = t['type'] as Map<String, dynamic>;
  b.writeln("const _family = '${type['family']}';");
  for (final e in type.entries.where((e) => e.value is Map)) {
    final s = e.value as Map<String, dynamic>;
    b.writeln('const _${e.key} = _TypeToken(${num_(s['size'] as num)}, ${num_(s['line'] as num)}, ${s['weight']}, ${num_(s['spacing'] as num)});');
  }
  return b.toString();
}

String cppTokens(Map<String, dynamic> t) {
  final colors = t['colors'] as Map<String, dynamic>;
  final b = StringBuffer()
    ..writeln('// GENERATED from app/design/tokens.json by app/tool/gen_tokens.dart: do not edit.')
    ..writeln('// SPDX-FileCopyrightText: 2026 Sorbet AI')
    ..writeln('// SPDX-License-Identifier: Apache-2.0')
    ..writeln('//')
    ..writeln('// The awva design tokens (app/DESIGN.md) for the plugins\' editor; the app\'s theme has the same.')
    ..writeln('#pragma once')
    ..writeln()
    ..writeln('#include <cstdint>')
    ..writeln()
    ..writeln('namespace awva_tokens {')
    ..writeln()
    ..writeln('struct Colors { // 0xAARRGGBB')
    ..writeln('\tuint32_t ${colors.keys.join(', ')};')
    ..writeln('\tbool dark;')
    ..writeln('};');
  for (final theme in ['light', 'dark']) {
    final k = theme == 'light' ? 'kLight' : 'kDark';
    b.writeln('inline constexpr Colors $k = {${colors.values.map((v) => '${argb((v as Map)[theme] as String)}u').join(', ')}, ${theme == 'dark'}};');
  }
  b.writeln();
  for (final e in (t['sizes'] as Map<String, dynamic>).entries) {
    b.writeln('inline constexpr float ${e.key} = ${cf(e.value as num)};');
  }
  b
    ..writeln()
    ..writeln('struct Type {')
    ..writeln('\tfloat size, line;')
    ..writeln('\tint weight;')
    ..writeln('\tfloat spacing;')
    ..writeln('};');
  final type = t['type'] as Map<String, dynamic>;
  b.writeln('inline constexpr const char* kFamily = "${type['family']}";');
  for (final e in type.entries.where((e) => e.value is Map)) {
    final s = e.value as Map<String, dynamic>;
    b.writeln('inline constexpr Type type${e.key[0].toUpperCase()}${e.key.substring(1)} = {${cf(s['size'] as num)}, ${cf(s['line'] as num)}, ${s['weight']}, ${cf(s['spacing'] as num)}};');
  }
  final w = (t['windows'] as Map)['editor'] as Map<String, dynamic>;
  b.writeln();
  for (final e in w.entries) {
    b.writeln('inline constexpr int kEditor${e.key[0].toUpperCase()}${e.key.substring(1)} = ${e.value};');
  }
  b
    ..writeln()
    ..writeln('} // namespace awva_tokens');
  return b.toString();
}

void main() {
  final t = readTokens();
  File(dartOut).writeAsStringSync(dartTokens(t));
  File(cppOut).writeAsStringSync(cppTokens(t));
  stdout.writeln('wrote $dartOut and $cppOut');
}
