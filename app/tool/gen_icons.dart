// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The parameters' icons (awva's assets/icons/*.svg, at the pinned AWVA_COMMIT: deps/awva-<commit>)
// as the app's Dart (lib/src/param_icons.g.dart) and the plugins' editor's C++
// (../include/awva_plugin/param_icons.h): one drawing, so the two never differ.
//   dart run tool/gen_icons.dart          (in app/, after ../fetch_deps.sh; test/icons_test.dart
//                                          checks they are current)
//
// Each icon becomes a list of shapes on its 24 x 24 grid, drawn with round caps and joins at a
// line of 1.5 grid units (the SVGs' own): a polyline (a path, its curves and arcs flattened; a
// dashed one as its dashes), a stroked circle or a filled disc. Encoded as one list of numbers:
//   0 n x0 y0 .. x(n-1) y(n-1)   a polyline of n points
//   1 cx cy r                    a circle
//   2 cx cy r                    a disc
// A parameter's icon "concept+modifier" is the concept, a disc of the background at (19, 19) r 5.5,
// then "mod-<modifier>" (awva docs/icons.md).
import 'dart:io';
import 'dart:math' as math;

const dartOut = 'lib/src/param_icons.g.dart';
const cppOut = '../include/awva_plugin/param_icons.h';

/// the pinned awva's icons folder
String iconsDir() {
  final commit = File('../AWVA_COMMIT').readAsLinesSync().first.trim().substring(0, 7);
  return '../deps/awva-$commit/assets/icons';
}

/// name -> shapes, every icon of the folder, by name
Map<String, List<double>> readIcons([String? dir]) {
  final d = Directory(dir ?? iconsDir());
  if (!d.existsSync()) throw StateError('${d.path}: no awva icons (../fetch_deps.sh)');
  final files = d.listSync().whereType<File>().where((f) => f.path.endsWith('.svg')).toList()..sort((a, b) => a.path.compareTo(b.path));
  final out = <String, List<double>>{};
  for (final f in files) {
    final name = f.uri.pathSegments.last.replaceAll('.svg', '');
    out[name] = shapesOf(f.readAsStringSync().replaceAll('\r\n', '\n'), name);
  }
  return out;
}

Map<String, String> _attrs(String tag) => {for (final m in RegExp(r'([a-z-]+)="([^"]*)"').allMatches(tag)) m[1]!: m[2]!};

double _n(Map<String, String> a, String k) => double.parse(a[k] ?? '0');

/// one SVG's shapes
List<double> shapesOf(String svg, String name) {
  final out = <double>[];
  void poly(List<List<double>> pts) {
    if (pts.length < 2) return;
    out
      ..add(0)
      ..add(pts.length.toDouble());
    for (final p in pts) out.addAll([_round(p[0]), _round(p[1])]);
  }

  for (final m in RegExp(r'<(path|circle|rect)\b([^>]*)/?>').allMatches(svg)) {
    final a = _attrs(m[2]!);
    final filled = a['fill'] != null && a['fill'] != 'none';
    final stroked = a['stroke'] != 'none';
    List<List<List<double>>> lines;
    if (m[1] == 'circle') {
      final cx = _n(a, 'cx'), cy = _n(a, 'cy'), r = _n(a, 'r');
      if (filled) out.addAll([2, cx, cy, r]);
      if (stroked) out.addAll([1, cx, cy, r]);
      continue;
    } else if (m[1] == 'rect') {
      final x = _n(a, 'x'), y = _n(a, 'y'), w = _n(a, 'width'), h = _n(a, 'height');
      final rx = math.min(_n(a, 'rx'), math.min(w, h) / 2);
      lines = _path(
        'M${x + rx} ${y}H${x + w - rx}A$rx $rx 0 0 1 ${x + w} ${y + rx}V${y + h - rx}A$rx $rx 0 0 1 ${x + w - rx} ${y + h}'
        'H${x + rx}A$rx $rx 0 0 1 $x ${y + h - rx}V${y + rx}A$rx $rx 0 0 1 ${x + rx} ${y}Z',
        name,
      );
    } else {
      lines = _path(a['d'] ?? '', name);
    }
    if (filled) throw StateError('$name: a filled ${m[1]} (only discs are filled)');
    final dash = a['stroke-dasharray'];
    for (final l in lines) {
      if (dash == null) {
        poly(l);
      } else {
        for (final d in _dashed(l, [for (final s in dash.split(RegExp(r'[ ,]+'))) double.parse(s)])) poly(d);
      }
    }
  }
  if (out.isEmpty) throw StateError('$name: nothing drawn');
  return out;
}

double _round(double v) => (v * 1000).roundToDouble() / 1000;

/// the subpaths of a path's d, flattened to points
List<List<List<double>>> _path(String d, String name) {
  final tokens = RegExp(r'[A-Za-z]|-?(?:\d+\.?\d*|\.\d+)(?:e-?\d+)?').allMatches(d).map((m) => m[0]!).toList();
  final subs = <List<List<double>>>[];
  var cur = <List<double>>[];
  double x = 0, y = 0, sx = 0, sy = 0, cx2 = 0, cy2 = 0; // the point, the subpath's start, the last cubic's second control
  var i = 0;
  String cmd = '';
  String last = '';
  bool isNum(int k) => k < tokens.length && !RegExp(r'^[A-Za-z]$').hasMatch(tokens[k]);
  double num_() => double.parse(tokens[i++]);
  void cubic(double x1, double y1, double x2, double y2, double x3, double y3) {
    const n = 16;
    for (var k = 1; k <= n; k++) {
      final t = k / n, u = 1 - t;
      cur.add([
        u * u * u * x + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x3,
        u * u * u * y + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y3,
      ]);
    }
    cx2 = x2;
    cy2 = y2;
    x = x3;
    y = y3;
  }

  void arc(double rx, double ry, double rot, bool large, bool sweep, double x2, double y2) {
    // the endpoint parameterization to the centre one (SVG 1.1 F.6.5)
    if (rx == 0 || ry == 0) {
      cur.add([x2, y2]);
      x = x2;
      y = y2;
      return;
    }
    rx = rx.abs();
    ry = ry.abs();
    final phi = rot * math.pi / 180, cp = math.cos(phi), sp = math.sin(phi);
    final dx = (x - x2) / 2, dy = (y - y2) / 2;
    final x1p = cp * dx + sp * dy, y1p = -sp * dx + cp * dy;
    final lam = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
    if (lam > 1) {
      rx *= math.sqrt(lam);
      ry *= math.sqrt(lam);
    }
    final num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
    final den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
    var co = math.sqrt(math.max(0, num / den));
    if (large == sweep) co = -co;
    final cxp = co * rx * y1p / ry, cyp = -co * ry * x1p / rx;
    final ccx = cp * cxp - sp * cyp + (x + x2) / 2, ccy = sp * cxp + cp * cyp + (y + y2) / 2;
    double ang(double ux, double uy, double vx, double vy) {
      final a = math.atan2(ux * vy - uy * vx, ux * vx + uy * vy);
      return a;
    }

    final t1 = ang(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry);
    var dt = ang((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry);
    if (!sweep && dt > 0) dt -= 2 * math.pi;
    if (sweep && dt < 0) dt += 2 * math.pi;
    final n = math.max(4, (dt.abs() / (math.pi / 16)).ceil());
    for (var k = 1; k <= n; k++) {
      final t = t1 + dt * k / n;
      if (k == n) {
        cur.add([x2, y2]);
      } else {
        cur.add([ccx + rx * math.cos(t) * cp - ry * math.sin(t) * sp, ccy + rx * math.cos(t) * sp + ry * math.sin(t) * cp]);
      }
    }
    x = x2;
    y = y2;
  }

  while (i < tokens.length) {
    if (!isNum(i)) {
      cmd = tokens[i++];
    } else if (cmd == 'M') {
      cmd = 'L';
    } else if (cmd == 'm') {
      cmd = 'l';
    }
    final rel = cmd == cmd.toLowerCase();
    final ox = rel ? x : 0.0, oy = rel ? y : 0.0;
    switch (cmd.toUpperCase()) {
      case 'M':
        if (cur.length > 1) subs.add(cur);
        x = ox + num_();
        y = oy + num_();
        sx = x;
        sy = y;
        cur = [
          [x, y],
        ];
      case 'L':
        x = ox + num_();
        y = oy + num_();
        cur.add([x, y]);
      case 'H':
        x = ox + num_();
        cur.add([x, y]);
      case 'V':
        y = oy + num_();
        cur.add([x, y]);
      case 'C':
        final x1 = ox + num_(), y1 = oy + num_(), x2 = ox + num_(), y2 = oy + num_(), x3 = ox + num_(), y3 = oy + num_();
        cubic(x1, y1, x2, y2, x3, y3);
      case 'S':
        final reflect = last.toUpperCase() == 'C' || last.toUpperCase() == 'S';
        final x1 = reflect ? 2 * x - cx2 : x, y1 = reflect ? 2 * y - cy2 : y;
        final x2 = ox + num_(), y2 = oy + num_(), x3 = ox + num_(), y3 = oy + num_();
        cubic(x1, y1, x2, y2, x3, y3);
      case 'A':
        final rx = num_(), ry = num_(), rot = num_(), large = num_() != 0, sweep = num_() != 0;
        final x2 = ox + num_(), y2 = oy + num_();
        arc(rx, ry, rot, large, sweep, x2, y2);
      case 'Z':
        cur.add([sx, sy]);
        x = sx;
        y = sy;
        if (cur.length > 1) subs.add(cur);
        cur = [
          [x, y],
        ];
      default:
        throw StateError('$name: path command $cmd');
    }
    last = cmd;
  }
  if (cur.length > 1) subs.add(cur);
  return subs;
}

/// a polyline as its dashes (pattern: on, off, on, off, ...)
List<List<List<double>>> _dashed(List<List<double>> l, List<double> pattern) {
  final out = <List<List<double>>>[];
  var k = 0;
  var left = pattern[0];
  var on = true;
  var dash = <List<double>>[l.first];
  for (var i = 1; i < l.length; i++) {
    var ax = l[i - 1][0], ay = l[i - 1][1];
    final bx = l[i][0], by = l[i][1];
    var seg = math.sqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay));
    while (seg > 1e-9) {
      final step = math.min(seg, left);
      final t = step / seg;
      ax += (bx - ax) * t;
      ay += (by - ay) * t;
      seg -= step;
      left -= step;
      if (on) dash.add([ax, ay]);
      if (left <= 1e-9) {
        if (on && dash.length > 1) out.add(dash);
        on = !on;
        k = (k + 1) % pattern.length;
        left = pattern[k];
        dash = [
          [ax, ay],
        ];
      }
    }
  }
  if (on && dash.length > 1) out.add(dash);
  return out;
}

String _num(double v) => v == v.roundToDouble() ? v.toStringAsFixed(0) : v.toString();

String dartIcons(Map<String, List<double>> icons) {
  final b = StringBuffer()
    ..writeln('// GENERATED from awva\'s assets/icons/*.svg by tool/gen_icons.dart: do not edit.')
    ..writeln('// SPDX-FileCopyrightText: 2026 Sorbet AI')
    ..writeln('// SPDX-License-Identifier: Apache-2.0')
    ..writeln("part of 'param_icon.dart';")
    ..writeln()
    ..writeln('// name -> its shapes on the 24 grid (tool/gen_icons.dart: 0 n points.. a polyline, 1 cx cy r a circle, 2 cx cy r a disc)')
    ..writeln('const _shapes = <String, List<double>>{');
  for (final e in icons.entries) {
    b.writeln("  '${e.key}': [${e.value.map(_num).join(', ')}],");
  }
  b.writeln('};');
  return b.toString();
}

String _cf(double v) => v == v.roundToDouble() ? '${v.toStringAsFixed(0)}.0f' : '${v}f';

String cppIcons(Map<String, List<double>> icons) {
  final b = StringBuffer()
    ..writeln('// GENERATED from awva\'s assets/icons/*.svg by app/tool/gen_icons.dart: do not edit.')
    ..writeln('// SPDX-FileCopyrightText: 2026 Sorbet AI')
    ..writeln('// SPDX-License-Identifier: Apache-2.0')
    ..writeln('//')
    ..writeln('// The parameters\' icons as shapes on their 24 grid: 0 n points.. a polyline, 1 cx cy r a circle,')
    ..writeln('// 2 cx cy r a disc (round caps and joins, lines 1.5 grid units wide).')
    ..writeln('#pragma once')
    ..writeln()
    ..writeln('#include <cstring>')
    ..writeln()
    ..writeln('namespace awva_icons {')
    ..writeln()
    ..writeln('struct Icon {')
    ..writeln('\tconst char* name;')
    ..writeln('\tconst float* shapes;')
    ..writeln('\tint count;')
    ..writeln('};')
    ..writeln();
  var k = 0;
  for (final e in icons.values) {
    b.writeln('inline constexpr float kShapes$k[] = {${e.map(_cf).join(', ')}};');
    k++;
  }
  b
    ..writeln()
    ..writeln('inline constexpr Icon kIcons[] = {');
  k = 0;
  for (final e in icons.entries) {
    b.writeln('\t{"${e.key}", kShapes$k, ${e.value.length}},');
    k++;
  }
  b
    ..writeln('};')
    ..writeln()
    ..writeln('// an icon by its name ("level", "mod-pedal"); nullptr if there is none')
    ..writeln('inline const Icon* find(const char* name, size_t len) {')
    ..writeln('\tfor (const Icon& i : kIcons)')
    ..writeln('\t\tif (std::strlen(i.name) == len && std::strncmp(i.name, name, len) == 0) return &i;')
    ..writeln('\treturn nullptr;')
    ..writeln('}')
    ..writeln()
    ..writeln('} // namespace awva_icons');
  return b.toString();
}

void main(List<String> args) {
  final icons = readIcons(args.isNotEmpty ? args.first : null);
  File(dartOut).writeAsStringSync(dartIcons(icons));
  File(cppOut).writeAsStringSync(cppIcons(icons));
  stdout.writeln('wrote $dartOut and $cppOut (${icons.length} icons)');
}
