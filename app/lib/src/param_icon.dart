// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// A parameter's icon and its tooltip (DESIGN.md, "Parameters"): the engines name an icon per
// parameter ("concept" or "concept+modifier", awva docs/icons.md) and give a label and a hover text.
// The drawings are awva's assets/icons, as shapes (param_icons.g.dart, tool/gen_icons.dart), the same
// ones the plugins' editor draws.
import 'dart:async';

import 'package:flutter/widgets.dart';

import 'theme.dart';

part 'param_icons.g.dart';

/// whether there is a drawing for this icon (a stripped engine has none: no icon is shown)
bool hasParamIcon(String icon) => icon.isNotEmpty && _shapes.containsKey(icon.split('+').first);

/// The icon of a parameter at size points: its concept, then (with a modifier) a knockout disc of
/// the background at (19, 19) r 5.5 on the 24 grid and the modifier's mark. Nothing for an unknown one.
class ParamIcon extends StatelessWidget {
  const ParamIcon(this.icon, {super.key, required this.color, required this.background, this.size = AwvaSize.iconSmall});
  final String icon;
  final Color color, background;
  final double size;

  @override
  Widget build(BuildContext context) {
    if (!hasParamIcon(icon)) return SizedBox(width: size, height: size);
    return SizedBox(
      width: size,
      height: size,
      child: CustomPaint(painter: _IconPainter(icon, color, background)),
    );
  }
}

class _IconPainter extends CustomPainter {
  _IconPainter(this.icon, this.color, this.background);
  final String icon;
  final Color color, background;

  static void _draw(Canvas canvas, List<double> s, double k, Paint line, Paint fill) {
    var i = 0;
    while (i < s.length) {
      final kind = s[i].toInt();
      if (kind == 0) {
        final n = s[i + 1].toInt();
        final p = Path()..moveTo(s[i + 2] * k, s[i + 3] * k);
        for (var j = 1; j < n; j++) p.lineTo(s[i + 2 + 2 * j] * k, s[i + 3 + 2 * j] * k);
        canvas.drawPath(p, line);
        i += 2 + 2 * n;
      } else {
        final c = Offset(s[i + 1] * k, s[i + 2] * k);
        canvas.drawCircle(c, s[i + 3] * k, kind == 1 ? line : fill);
        i += 4;
      }
    }
  }

  @override
  void paint(Canvas canvas, Size size) {
    final k = size.width / 24;
    final line = Paint()
      ..color = color
      ..style = PaintingStyle.stroke
      ..strokeWidth = 1.5 * k
      ..strokeCap = StrokeCap.round
      ..strokeJoin = StrokeJoin.round;
    final fill = Paint()..color = color;
    final parts = icon.split('+');
    _draw(canvas, _shapes[parts.first]!, k, line, fill);
    if (parts.length > 1) {
      final mod = _shapes['mod-${parts[1]}'];
      if (mod != null) {
        canvas.drawCircle(Offset(19 * k, 19 * k), 5.5 * k, Paint()..color = background);
        _draw(canvas, mod, k, line, fill);
      }
    }
  }

  @override
  bool shouldRepaint(_IconPainter o) => o.icon != icon || o.color != color || o.background != background;
}

/// A parameter's tooltip: its label (strong) and its hover text, under the child after the pointer
/// rests on it (AwvaSize.tipDelay); gone when it leaves or presses. Nothing without a text.
class ParamTip extends StatefulWidget {
  const ParamTip({super.key, required this.label, required this.hover, required this.child});
  final String label, hover;
  final Widget child;

  @override
  State<ParamTip> createState() => _ParamTipState();
}

class _ParamTipState extends State<ParamTip> {
  final _portal = OverlayPortalController();
  final _link = LayerLink();
  Timer? _timer;

  void _enter() {
    _timer?.cancel();
    _timer = Timer(AwvaSize.tipDelay, () {
      if (mounted) _portal.show();
    });
  }

  void _leave() {
    _timer?.cancel();
    if (_portal.isShowing) _portal.hide();
  }

  @override
  void dispose() {
    _timer?.cancel();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    if (widget.hover.isEmpty && widget.label.isEmpty) return widget.child;
    final c = AwvaTheme.of(context);
    return CompositedTransformTarget(
      link: _link,
      child: OverlayPortal(
        controller: _portal,
        overlayChildBuilder: (_) => Positioned(
          left: 0,
          top: 0,
          child: CompositedTransformFollower(
            link: _link,
            targetAnchor: Alignment.bottomLeft,
            offset: const Offset(0, AwvaSize.s2),
            child: IgnorePointer(
              child: Container(
                constraints: const BoxConstraints(maxWidth: AwvaSize.tipWidth),
                padding: const EdgeInsets.symmetric(horizontal: 10, vertical: AwvaSize.s2),
                decoration: BoxDecoration(
                  color: c.bg1,
                  borderRadius: BorderRadius.circular(AwvaSize.radiusSmall),
                  border: Border.all(color: c.hairline),
                  boxShadow: [BoxShadow(color: c.shadow, blurRadius: 24, offset: const Offset(0, 8))],
                ),
                child: Text.rich(
                  TextSpan(
                    children: [
                      if (widget.label.isNotEmpty) TextSpan(text: widget.label, style: AwvaType.smallStrong(c.text1)),
                      if (widget.label.isNotEmpty && widget.hover.isNotEmpty) const TextSpan(text: '\n'),
                      if (widget.hover.isNotEmpty) TextSpan(text: widget.hover, style: AwvaType.small(c.text1)),
                    ],
                  ),
                ),
              ),
            ),
          ),
        ),
        child: MouseRegion(
          onEnter: (_) => _enter(),
          onExit: (_) => _leave(),
          child: Listener(onPointerDown: (_) => _leave(), child: widget.child),
        ),
      ),
    );
  }
}
