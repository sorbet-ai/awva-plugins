// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The sound: its name, its hero knobs, and every parameter on request (ribbon sliders).
part of 'screen.dart';

class _Body extends StatelessWidget {
  const _Body({required this.k, required this.all, required this.onAll});
  final Controller k;
  final bool all;
  final VoidCallback onAll;

  @override
  Widget build(BuildContext context) {
    final c = AwvaTheme.of(context);
    final s = k.sound;
    if (s == null) {
      return Center(child: Text(k.message ?? 'No engine to play.', style: AwvaType.body(c.text2)));
    }
    final heroes = s.heroes.isNotEmpty ? s.heroes : [for (var i = 0; i < s.params.length && i < 8; i++) i];
    return SingleChildScrollView(
      padding: const EdgeInsets.fromLTRB(AwvaSize.s6, AwvaSize.s5, AwvaSize.s6, AwvaSize.s6),
      child: Center(
        child: ConstrainedBox(
          constraints: const BoxConstraints(maxWidth: 4 * (AwvaSize.knob + 24) + 3 * AwvaSize.s6 + 120),
          child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
            Text(s.presets.isEmpty ? s.engineTitle : s.presetName, maxLines: 1, overflow: TextOverflow.ellipsis, style: AwvaType.sound(c.text1)),
            const SizedBox(height: AwvaSize.s1),
            Text(s.engineSummary, maxLines: 2, style: AwvaType.body(c.text2)),
            if (k.message != null) ...[
              const SizedBox(height: AwvaSize.s2),
              Text(k.message!, style: AwvaType.body(c.error)),
            ],
            ValueListenableBuilder<Status>(
              valueListenable: k.status,
              builder: (_, st, _) => st.engineStopped
                  ? Padding(
                      padding: const EdgeInsets.only(top: AwvaSize.s2),
                      child: Text('This engine went past its safe level and was stopped. Choose another engine.', style: AwvaType.body(c.error)),
                    )
                  : const SizedBox.shrink(),
            ),
            const SizedBox(height: AwvaSize.s6),
            Wrap(
              spacing: AwvaSize.s6,
              runSpacing: AwvaSize.s5,
              // (four to a row: 8 heroes are two rows of four)
              children: [
                for (final i in heroes)
                  if (i < s.params.length)
                    s.params[i].kind == 2
                        ? _ToggleTile(p: s.params[i], onChanged: (v) => k.setParam(i, v ? 1 : 0))
                        : Knob(
                            label: s.params[i].name,
                            icon: s.params[i].icon,
                            title: s.params[i].label,
                            hover: s.params[i].hover,
                            value: s.params[i].value,
                            min: s.params[i].min,
                            max: s.params[i].max,
                            def: s.params[i].def,
                            kind: s.params[i].kind,
                            onChanged: (v) => k.setParam(i, v),
                          ),
              ],
            ),
            const SizedBox(height: AwvaSize.s6),
            AwvaFocus(
              label: 'every parameter',
              onPress: onAll,
              child: GestureDetector(
                onTap: onAll,
                child: MouseRegion(
                  cursor: SystemMouseCursors.click,
                  child: Row(mainAxisSize: MainAxisSize.min, children: [
                    Text('Every parameter', style: AwvaType.bodyStrong(c.text2)),
                    const SizedBox(width: 4),
                    Transform.rotate(angle: all ? 0 : -1.5708, child: AwvaIcon(Icons.chevronDown, color: c.text3)),
                  ]),
                ),
              ),
            ),
            if (all) ...[const SizedBox(height: AwvaSize.s3), _AllParams(k: k, s: s)],
          ]),
        ),
      ),
    );
  }
}

class _ToggleTile extends StatelessWidget {
  const _ToggleTile({required this.p, required this.onChanged});
  final ParamInfo p;
  final ValueChanged<bool> onChanged;
  @override
  Widget build(BuildContext context) {
    return ParamTip(
      label: p.label,
      hover: p.hover,
      child: SizedBox(
        width: AwvaSize.knob + 24,
        height: AwvaSize.knob + 22,
        child: Column(mainAxisAlignment: MainAxisAlignment.center, children: [
          const Spacer(),
          AwvaSwitch(value: p.value >= 0.5, onChanged: onChanged),
          const Spacer(),
          ParamName(name: p.name, icon: p.icon),
        ]),
      ),
    );
  }
}

class _AllParams extends StatelessWidget {
  const _AllParams({required this.k, required this.s});
  final Controller k;
  final Sound s;
  @override
  Widget build(BuildContext context) {
    final c = AwvaTheme.of(context);
    final rows = <Widget>[];
    String? group;
    for (var i = 0; i < s.params.length; i++) {
      final p = s.params[i];
      if (p.group != group) {
        group = p.group;
        rows.add(Padding(
          padding: const EdgeInsets.only(top: AwvaSize.s4, bottom: AwvaSize.s1),
          child: Text(group.isEmpty ? 'general' : group, style: AwvaType.micro(c.text3)),
        ));
      }
      rows.add(_ParamRow(p: p, onChanged: (v) => k.setParam(i, v)));
    }
    rows.add(Padding(
      padding: const EdgeInsets.only(top: AwvaSize.s4, bottom: AwvaSize.s1),
      child: Text('play', style: AwvaType.micro(c.text3)),
    ));
    rows.add(SizedBox(
      height: AwvaSize.row,
      child: Row(children: [
        SizedBox(
          width: 180,
          child: Row(children: [
            ParamIcon('mono', color: c.text2, background: c.bg0, size: AwvaSize.iconRow),
            const SizedBox(width: AwvaSize.s2),
            Text('mono', style: AwvaType.body(c.text2)),
          ]),
        ),
        AwvaSwitch(value: s.mono, onChanged: k.setMono),
      ]),
    ));
    return Column(crossAxisAlignment: CrossAxisAlignment.start, children: rows);
  }
}

/// a parameter as a row: its icon and name (its tooltip on them), a ribbon slider, its value
class _ParamRow extends StatelessWidget {
  const _ParamRow({required this.p, required this.onChanged});
  final ParamInfo p;
  final ValueChanged<double> onChanged;
  @override
  Widget build(BuildContext context) {
    final c = AwvaTheme.of(context);
    final span = p.max - p.min;
    double norm(double v) => span == 0 ? 0 : ((v - p.min) / span).clamp(0.0, 1.0);
    return SizedBox(
      height: AwvaSize.row,
      child: Row(children: [
        SizedBox(
          width: 180,
          child: Row(children: [
            ParamTip(
              label: p.label,
              hover: p.hover,
              child: Row(mainAxisSize: MainAxisSize.min, children: [
                ParamIcon(p.icon, color: c.text2, background: c.bg0, size: AwvaSize.iconRow),
                const SizedBox(width: AwvaSize.s2),
                SizedBox(
                  width: 180 - AwvaSize.iconRow - AwvaSize.s2 - 4,
                  child: Text(paramLabel(p.name), maxLines: 1, overflow: TextOverflow.ellipsis, style: AwvaType.body(c.text2)),
                ),
              ]),
            ),
          ]),
        ),
        if (p.kind == 2)
          AwvaSwitch(value: p.value >= 0.5, onChanged: (v) => onChanged(v ? 1 : 0))
        else
          Expanded(
            child: LayoutBuilder(builder: (context, box) {
              void at(double x) {
                var v = p.min + (x / box.maxWidth).clamp(0.0, 1.0) * span;
                if (p.kind != 0) v = v.roundToDouble();
                onChanged(v);
              }

              final step = p.kind != 0 ? 1.0 : span / 100;
              return AwvaFocus(
                label: 'row ${p.name}',
                radius: AwvaSize.row / 2,
                onArrow: (dir, fine) => onChanged((p.value + dir * (fine && p.kind == 0 ? step / 10 : step)).clamp(p.min, p.max)),
                onHome: () => onChanged(p.min),
                onEnd: () => onChanged(p.max),
                onReset: () => onChanged(p.def),
                child: MouseRegion(
                cursor: SystemMouseCursors.resizeLeftRight,
                child: GestureDetector(
                  onDoubleTap: () => onChanged(p.def),
                  onTapDown: (d) => at(d.localPosition.dx),
                  onHorizontalDragUpdate: (d) => at(d.localPosition.dx),
                  child: SizedBox(
                    height: AwvaSize.row,
                    child: CustomPaint(painter: _RibbonPainter(v: norm(p.value), from: p.min < 0 && p.max > 0 ? norm(0) : 0, track: c.bg3, fill: c.sage)),
                  ),
                ),
                ),
              );
            }),
          ),
        SizedBox(width: 64, child: Text(formatValue(p.value, p.min, p.max, p.kind), textAlign: TextAlign.right, style: AwvaType.value(c.text2))),
      ]),
    );
  }
}

class _RibbonPainter extends CustomPainter {
  _RibbonPainter({required this.v, required this.from, required this.track, required this.fill});
  final double v, from;
  final Color track, fill;
  @override
  void paint(Canvas canvas, Size s) {
    final y = s.height / 2;
    final p = Paint()
      ..strokeWidth = 4
      ..strokeCap = StrokeCap.round;
    canvas.drawLine(Offset(2, y), Offset(s.width - 2, y), p..color = track);
    final a = from < v ? from : v, b = from < v ? v : from;
    canvas.drawLine(Offset(2 + (s.width - 4) * a, y), Offset(2 + (s.width - 4) * b, y), p..color = fill);
  }

  @override
  bool shouldRepaint(_RibbonPainter o) => o.v != v || o.from != from || o.track != track || o.fill != fill;
}
