// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The few controls the UI is made of (DESIGN.md, "Components"): the knob, the level fader, the
// meter, the switch, buttons, a popover menu, and the icons (drawn: a ribbon-weight line, round caps).
import 'dart:math' as math;

import 'package:flutter/gestures.dart';
import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart';

import 'theme.dart';

String paramLabel(String name) => name.replaceAll('_', ' ');

String formatValue(double v, double min, double max, int kind) {
  if (kind != 0) return v.round().toString();
  final span = (max - min).abs();
  if (span >= 100) return v.toStringAsFixed(0);
  if (span >= 10) return v.toStringAsFixed(1);
  return v.toStringAsFixed(2);
}

/// a wheel turn over a control: the control's alone (claimed from the pointer signal resolver, so a
/// scroll view around it, or the host around the plugin's editor, doesn't scroll too); dy > 0: down
void claimWheel(PointerSignalEvent e, void Function(double dy) onWheel) {
  if (e is! PointerScrollEvent) return;
  GestureBinding.instance.pointerSignalResolver.register(e, (s) => onWheel((s as PointerScrollEvent).scrollDelta.dy));
}

// --- the knob ------------------------------------------------------------------------------------

/// A value as a ribbon arc (270 degrees, round caps): drag up or down (shift: fine), scroll, double
/// click for the default. A bipolar range (min < 0 < max) draws from the centre.
class Knob extends StatefulWidget {
  const Knob({
    super.key,
    required this.label,
    required this.value,
    required this.min,
    required this.max,
    required this.def,
    required this.kind,
    required this.onChanged,
    this.size = AwvaSize.knob,
  });
  final String label;
  final double value, min, max, def;
  final int kind;
  final ValueChanged<double> onChanged;
  final double size;

  @override
  State<Knob> createState() => _KnobState();
}

class _KnobState extends State<Knob> {
  bool _drag = false, _hover = false;
  double _v = 0;

  double get _span => widget.max - widget.min;
  double _norm(double v) => _span == 0 ? 0 : ((v - widget.min) / _span).clamp(0.0, 1.0);

  void _set(double v) {
    v = v.clamp(widget.min, widget.max);
    if (widget.kind != 0) v = v.roundToDouble();
    if (v != widget.value) widget.onChanged(v);
  }

  @override
  Widget build(BuildContext context) {
    final c = AwvaTheme.of(context);
    final active = _drag || _hover;
    final text = formatValue(widget.value, widget.min, widget.max, widget.kind);
    return MouseRegion(
      cursor: SystemMouseCursors.resizeUpDown,
      onEnter: (_) => setState(() => _hover = true),
      onExit: (_) => setState(() => _hover = false),
      child: Listener(
        onPointerSignal: (e) => claimWheel(e, (dy) => _set(widget.value - dy.sign * (widget.kind != 0 ? 1.0 : _span / 100))),
        child: GestureDetector(
          onDoubleTap: () => _set(widget.def),
          onVerticalDragStart: (_) => setState(() {
            _drag = true;
            _v = widget.value;
          }),
          onVerticalDragUpdate: (d) {
            final fine = HardwareKeyboard.instance.isShiftPressed;
            _v = (_v - d.delta.dy / (fine ? 800 : 200) * _span).clamp(widget.min, widget.max);
            _set(_v);
          },
          onVerticalDragEnd: (_) => setState(() => _drag = false),
          child: SizedBox(
            width: widget.size + 24,
            child: Column(mainAxisSize: MainAxisSize.min, children: [
              SizedBox(
                width: widget.size,
                height: widget.size,
                child: CustomPaint(
                  painter: _KnobPainter(
                    v: _norm(widget.value),
                    from: widget.min < 0 && widget.max > 0 ? _norm(0) : 0,
                    track: c.bg3,
                    arc: c.sage,
                    dot: active ? c.text1 : c.text2,
                  ),
                  child: Center(child: Text(text, style: AwvaType.value(active ? c.text1 : c.text2))),
                ),
              ),
              const SizedBox(height: AwvaSize.s1),
              Text(paramLabel(widget.label),
                  maxLines: 1, overflow: TextOverflow.ellipsis, textAlign: TextAlign.center, style: AwvaType.body(c.text2)),
            ]),
          ),
        ),
      ),
    );
  }
}

class _KnobPainter extends CustomPainter {
  _KnobPainter({required this.v, required this.from, required this.track, required this.arc, required this.dot});
  final double v, from;
  final Color track, arc, dot;

  static const _start = math.pi * 0.75, _sweep = math.pi * 1.5;

  @override
  void paint(Canvas canvas, Size size) {
    final r = Rect.fromLTWH(0, 0, size.width, size.height).deflate(AwvaSize.knobStroke / 2 + 1);
    final p = Paint()
      ..style = PaintingStyle.stroke
      ..strokeWidth = AwvaSize.knobStroke
      ..strokeCap = StrokeCap.round;
    canvas.drawArc(r, _start, _sweep, false, p..color = track);
    final a = math.min(from, v), b = math.max(from, v);
    if (b - a > 0.001) canvas.drawArc(r, _start + _sweep * a, _sweep * (b - a), false, p..color = arc);
    // the pointer: a short ribbon end at the value
    final ang = _start + _sweep * v;
    final c = r.center, rad = r.width / 2 - AwvaSize.knobStroke - 3;
    canvas.drawLine(c + Offset(math.cos(ang), math.sin(ang)) * (rad - 7), c + Offset(math.cos(ang), math.sin(ang)) * rad,
        Paint()
          ..color = dot
          ..strokeWidth = 2.5
          ..strokeCap = StrokeCap.round);
  }

  @override
  bool shouldRepaint(_KnobPainter o) => o.v != v || o.from != from || o.track != track || o.arc != arc || o.dot != dot;
}

// --- the level: a fader with its meter ------------------------------------------------------------

/// The master level, dB: a short horizontal ribbon, drag or scroll; double click: -18 dB. The meter
/// (the output's peaks, L and R) runs under it; it turns butter while the limiter works.
class LevelControl extends StatelessWidget {
  const LevelControl({super.key, required this.db, required this.onChanged, required this.peakL, required this.peakR, required this.limiting});
  final double db, peakL, peakR;
  final bool limiting;
  final ValueChanged<double> onChanged;

  static const double minDb = -60, maxDb = 6, width = 112;
  static double _pos(double db) => ((db - minDb) / (maxDb - minDb)).clamp(0.0, 1.0);
  static double _peakPos(double peak) => peak <= 0 ? 0 : _pos(20 * math.log(peak) / math.ln10);

  @override
  Widget build(BuildContext context) {
    final c = AwvaTheme.of(context);
    void at(double x) => onChanged((minDb + (x / width).clamp(0.0, 1.0) * (maxDb - minDb)).roundToDouble());
    return Row(mainAxisSize: MainAxisSize.min, children: [
      MouseRegion(
        cursor: SystemMouseCursors.resizeLeftRight,
        child: Listener(
          onPointerSignal: (e) => claimWheel(e, (dy) => onChanged((db - dy.sign).clamp(minDb, maxDb))),
          child: GestureDetector(
            onDoubleTap: () => onChanged(-18),
            onHorizontalDragUpdate: (d) => at(d.localPosition.dx),
            onTapDown: (d) => at(d.localPosition.dx),
            child: SizedBox(
              width: width,
              height: 24,
              child: CustomPaint(
                painter: _LevelPainter(
                  level: _pos(db),
                  l: _peakPos(peakL),
                  r: _peakPos(peakR),
                  track: c.bg3,
                  fill: c.text2,
                  meter: limiting ? c.butter : c.blue,
                  knob: c.text1,
                ),
              ),
            ),
          ),
        ),
      ),
      const SizedBox(width: AwvaSize.s2),
      SizedBox(
        width: 52,
        child: Text(db <= minDb ? 'off' : '${db.toStringAsFixed(0)} dB', textAlign: TextAlign.right, style: AwvaType.value(c.text2)),
      ),
    ]);
  }
}

class _LevelPainter extends CustomPainter {
  _LevelPainter({required this.level, required this.l, required this.r, required this.track, required this.fill, required this.meter, required this.knob});
  final double level, l, r;
  final Color track, fill, meter, knob;

  @override
  void paint(Canvas canvas, Size s) {
    final w = s.width;
    final p = Paint()
      ..strokeCap = StrokeCap.round
      ..style = PaintingStyle.stroke;
    // the level: a 4 px ribbon
    const y = 8.0;
    canvas.drawLine(const Offset(2, y), Offset(w - 2, y), p..strokeWidth = 4..color = track);
    canvas.drawLine(const Offset(2, y), Offset(2 + (w - 4) * level, y), p..color = fill);
    canvas.drawCircle(Offset(2 + (w - 4) * level, y), 5, Paint()..color = knob);
    // the meter: two 2 px lines under it
    for (final (i, v) in [(0, l), (1, r)]) {
      final yy = 17.0 + i * 4;
      canvas.drawLine(Offset(2, yy), Offset(w - 2, yy), p..strokeWidth = 2..color = track);
      if (v > 0) canvas.drawLine(Offset(2, yy), Offset(2 + (w - 4) * v, yy), p..color = meter);
    }
  }

  @override
  bool shouldRepaint(_LevelPainter o) => o.level != level || o.l != l || o.r != r || o.meter != meter || o.track != track;
}

// --- buttons, switch, chips ----------------------------------------------------------------------

/// A quiet button: text (and an icon), a fill on hover; primary: sage.
class AwvaButton extends StatefulWidget {
  const AwvaButton({super.key, required this.onTap, this.label, this.icon, this.primary = false, this.tooltip});
  final VoidCallback? onTap;
  final String? label;
  final IconPainter? icon;
  final bool primary;
  final String? tooltip;

  @override
  State<AwvaButton> createState() => _AwvaButtonState();
}

class _AwvaButtonState extends State<AwvaButton> {
  bool _hover = false;

  @override
  Widget build(BuildContext context) {
    final c = AwvaTheme.of(context);
    final enabled = widget.onTap != null;
    final fg = !enabled ? c.text3 : (widget.primary ? c.onSage : (_hover ? c.text1 : c.text2));
    final bg = widget.primary ? c.sage : (widget.label != null ? (_hover && enabled ? c.bg3 : c.bg2) : (_hover && enabled ? c.bg2 : const Color(0x00000000)));
    Widget child = AnimatedContainer(
      duration: AwvaSize.quick,
      height: 32,
      padding: EdgeInsets.symmetric(horizontal: widget.label == null ? 8 : 12),
      decoration: BoxDecoration(color: bg, borderRadius: BorderRadius.circular(AwvaSize.radiusSmall)),
      child: Row(mainAxisSize: MainAxisSize.min, children: [
        if (widget.icon != null) AwvaIcon(widget.icon!, color: fg),
        if (widget.icon != null && widget.label != null) const SizedBox(width: 6),
        if (widget.label != null) Text(widget.label!, style: AwvaType.bodyStrong(fg)),
      ]),
    );
    child = MouseRegion(
      cursor: enabled ? SystemMouseCursors.click : SystemMouseCursors.basic,
      onEnter: (_) => setState(() => _hover = true),
      onExit: (_) => setState(() => _hover = false),
      child: GestureDetector(onTap: widget.onTap, child: child),
    );
    return Semantics(button: true, label: widget.tooltip ?? widget.label, child: child);
  }
}

/// on/off: a short ribbon with a dot
class AwvaSwitch extends StatelessWidget {
  const AwvaSwitch({super.key, required this.value, required this.onChanged});
  final bool value;
  final ValueChanged<bool> onChanged;

  @override
  Widget build(BuildContext context) {
    final c = AwvaTheme.of(context);
    return MouseRegion(
      cursor: SystemMouseCursors.click,
      child: GestureDetector(
        onTap: () => onChanged(!value),
        child: AnimatedContainer(
          duration: AwvaSize.quick,
          width: 32,
          height: 18,
          padding: const EdgeInsets.all(3),
          alignment: value ? Alignment.centerRight : Alignment.centerLeft,
          decoration: BoxDecoration(color: value ? c.sage : c.bg3, borderRadius: BorderRadius.circular(9)),
          child: Container(width: 12, height: 12, decoration: BoxDecoration(color: value ? c.onSage : c.text3, shape: BoxShape.circle)),
        ),
      ),
    );
  }
}

/// a small dot: open / busy / off
class Dot extends StatelessWidget {
  const Dot(this.color, {super.key, this.size = 8});
  final Color color;
  final double size;
  @override
  Widget build(BuildContext context) =>
      Container(width: size, height: size, decoration: BoxDecoration(color: color, shape: BoxShape.circle));
}

// --- the popover menu ----------------------------------------------------------------------------

class MenuItem {
  const MenuItem({required this.title, this.detail = '', this.trailing = '', this.selected = false, this.enabled = true, this.onTap, this.heading = false});
  final String title, detail, trailing;
  final bool selected, enabled, heading;
  final VoidCallback? onTap;
}

/// a list under its anchor (the anchor's context), bg1 with a soft shadow
Future<void> showMenuAt(BuildContext anchor, List<MenuItem> items, {double width = 320}) {
  final box = anchor.findRenderObject()! as RenderBox;
  final at = box.localToGlobal(Offset(0, box.size.height + 6));
  return Navigator.of(anchor).push(_MenuRoute(at: at, items: items, width: width, colors: AwvaTheme.of(anchor)));
}

class _MenuRoute extends PopupRoute<void> {
  _MenuRoute({required this.at, required this.items, required this.width, required this.colors});
  final Offset at;
  final List<MenuItem> items;
  final double width;
  final AwvaColors colors;

  @override
  Color? get barrierColor => null;
  @override
  bool get barrierDismissible => true;
  @override
  String? get barrierLabel => 'close';
  @override
  Duration get transitionDuration => AwvaSize.quick;

  @override
  Widget buildPage(BuildContext context, Animation<double> a, Animation<double> b) {
    final c = colors;
    final screen = MediaQuery.of(context).size;
    final left = at.dx.clamp(8.0, math.max(8.0, screen.width - width - 8)).toDouble();
    final maxH = math.max(120.0, screen.height - at.dy - 16);
    return AwvaTheme(
      colors: c,
      child: Stack(children: [
        Positioned(
          left: left,
          top: at.dy,
          width: width,
          child: FadeTransition(
            opacity: a,
            child: Container(
              constraints: BoxConstraints(maxHeight: maxH),
              padding: const EdgeInsets.symmetric(vertical: 6),
              decoration: BoxDecoration(
                color: c.bg1,
                borderRadius: BorderRadius.circular(AwvaSize.radius),
                border: Border.all(color: c.hairline),
                boxShadow: [BoxShadow(color: c.shadow, blurRadius: 24, offset: const Offset(0, 8))],
              ),
              child: SingleChildScrollView(
                child: Column(mainAxisSize: MainAxisSize.min, children: [
                  for (final it in items)
                    it.heading
                        ? Padding(
                            padding: const EdgeInsets.fromLTRB(14, 10, 14, 4),
                            child: Align(alignment: Alignment.centerLeft, child: Text(it.title, style: AwvaType.micro(c.text3))),
                          )
                        : _MenuRow(item: it, onTap: () {
                            Navigator.of(context).pop();
                            it.onTap?.call();
                          }),
                ]),
              ),
            ),
          ),
        ),
      ]),
    );
  }
}

class _MenuRow extends StatefulWidget {
  const _MenuRow({required this.item, required this.onTap});
  final MenuItem item;
  final VoidCallback onTap;
  @override
  State<_MenuRow> createState() => _MenuRowState();
}

class _MenuRowState extends State<_MenuRow> {
  bool _hover = false;
  @override
  Widget build(BuildContext context) {
    final c = AwvaTheme.of(context);
    final it = widget.item;
    return MouseRegion(
      cursor: it.enabled ? SystemMouseCursors.click : SystemMouseCursors.basic,
      onEnter: (_) => setState(() => _hover = true),
      onExit: (_) => setState(() => _hover = false),
      child: GestureDetector(
        onTap: it.enabled ? widget.onTap : null,
        child: Container(
          margin: const EdgeInsets.symmetric(horizontal: 6),
          padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 6),
          decoration: BoxDecoration(color: _hover && it.enabled ? c.bg2 : null, borderRadius: BorderRadius.circular(AwvaSize.radiusSmall)),
          child: Row(crossAxisAlignment: CrossAxisAlignment.start, children: [
            SizedBox(width: 14, child: it.selected ? Padding(padding: const EdgeInsets.only(top: 5), child: Dot(c.sage, size: 6)) : null),
            Expanded(
              child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                Text(it.title, style: AwvaType.body(it.enabled ? (it.selected ? c.text1 : c.text1) : c.text3)),
                if (it.detail.isNotEmpty) Text(it.detail, style: AwvaType.small(it.enabled ? c.text3 : c.error)),
              ]),
            ),
            if (it.trailing.isNotEmpty) Padding(padding: const EdgeInsets.only(left: 8), child: Text(it.trailing, style: AwvaType.small(c.text3))),
          ]),
        ),
      ),
    );
  }
}

// --- icons ---------------------------------------------------------------------------------------

typedef IconPainter = void Function(Canvas canvas, Size size, Paint paint);

class AwvaIcon extends StatelessWidget {
  const AwvaIcon(this.painter, {super.key, required this.color, this.size = 16});
  final IconPainter painter;
  final Color color;
  final double size;
  @override
  Widget build(BuildContext context) => SizedBox(width: size, height: size, child: CustomPaint(painter: _IconPaint(painter, color)));
}

class _IconPaint extends CustomPainter {
  _IconPaint(this.f, this.color);
  final IconPainter f;
  final Color color;
  @override
  void paint(Canvas canvas, Size size) => f(
      canvas,
      size,
      Paint()
        ..color = color
        ..style = PaintingStyle.stroke
        ..strokeWidth = 1.6
        ..strokeCap = StrokeCap.round
        ..strokeJoin = StrokeJoin.round);
  @override
  bool shouldRepaint(_IconPaint o) => o.color != color || o.f != f;
}

class Icons {
  static void chevronDown(Canvas c, Size s, Paint p) =>
      c.drawPath(Path()..moveTo(s.width * .28, s.height * .40)..lineTo(s.width * .5, s.height * .62)..lineTo(s.width * .72, s.height * .40), p);
  static void chevronLeft(Canvas c, Size s, Paint p) =>
      c.drawPath(Path()..moveTo(s.width * .60, s.height * .28)..lineTo(s.width * .38, s.height * .5)..lineTo(s.width * .60, s.height * .72), p);
  static void chevronRight(Canvas c, Size s, Paint p) =>
      c.drawPath(Path()..moveTo(s.width * .40, s.height * .28)..lineTo(s.width * .62, s.height * .5)..lineTo(s.width * .40, s.height * .72), p);

  /// settings: three ribbons with their stops
  static void settings(Canvas c, Size s, Paint p) {
    for (final (y, x) in [(.28, .65), (.5, .35), (.72, .58)]) {
      c.drawLine(Offset(s.width * .15, s.height * y), Offset(s.width * .85, s.height * y), p);
      c.drawCircle(Offset(s.width * x, s.height * y), 2.2, Paint()..color = p.color);
    }
  }

  /// the theme: a circle half filled
  static void theme(Canvas c, Size s, Paint p) {
    final r = Rect.fromCircle(center: s.center(Offset.zero), radius: s.width * .32);
    c.drawCircle(r.center, r.width / 2, p);
    c.drawArc(r, -math.pi / 2, math.pi, true, Paint()..color = p.color);
  }

  static void close(Canvas c, Size s, Paint p) {
    c.drawLine(Offset(s.width * .3, s.height * .3), Offset(s.width * .7, s.height * .7), p);
    c.drawLine(Offset(s.width * .7, s.height * .3), Offset(s.width * .3, s.height * .7), p);
  }

  static void refresh(Canvas c, Size s, Paint p) {
    final r = Rect.fromCircle(center: s.center(Offset.zero), radius: s.width * .3);
    c.drawArc(r, -math.pi * .35, math.pi * 1.6, false, p);
    final e = r.center + Offset(math.cos(-math.pi * .35), math.sin(-math.pi * .35)) * r.width / 2;
    c.drawPath(Path()..moveTo(e.dx - 3.5, e.dy - 2)..lineTo(e.dx, e.dy)..lineTo(e.dx - 0.5, e.dy - 4), p);
  }

  /// all notes off: a square
  static void stop(Canvas c, Size s, Paint p) =>
      c.drawRRect(RRect.fromRectAndRadius(Rect.fromCenter(center: s.center(Offset.zero), width: s.width * .46, height: s.height * .46), const Radius.circular(2)), p);
}
