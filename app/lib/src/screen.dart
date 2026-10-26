// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The one screen (DESIGN.md, "Layout"): the header (the mark, the engine, the preset, MIDI, the
// level, the theme, settings), the sound (its name and its hero knobs, every parameter below on
// request), the footer (audio and MIDI in one line), and the settings panel over the right edge.
// The same screen is the plugins' editor, without the engine picker and the device settings.
import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart';

import 'controller.dart';
import 'core.dart';
import 'theme.dart';
import 'widgets.dart';

class AwvaApp extends StatelessWidget {
  const AwvaApp({super.key, required this.controller, this.brightness});
  final Controller controller;

  /// the OS's, unless given (tests)
  final Brightness? brightness;

  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: controller,
      builder: (context, _) {
        final os = brightness ?? MediaQuery.platformBrightnessOf(context);
        final dark = switch (controller.theme) {
          ThemeChoice.dark => true,
          ThemeChoice.light => false,
          ThemeChoice.system => os == Brightness.dark,
        };
        final colors = dark ? AwvaColors.darkTheme : AwvaColors.light;
        return AwvaTheme(
          colors: colors,
          child: WidgetsApp(
            title: 'awva',
            color: colors.sage,
            debugShowCheckedModeBanner: false,
            textStyle: AwvaType.body(colors.text1),
            pageRouteBuilder: <T>(RouteSettings s, WidgetBuilder b) => PageRouteBuilder<T>(settings: s, pageBuilder: (c, _, _) => b(c)),
            home: AwvaTheme(colors: colors, child: MainScreen(controller: controller)),
          ),
        );
      },
    );
  }
}

class MainScreen extends StatefulWidget {
  const MainScreen({super.key, required this.controller});
  final Controller controller;
  @override
  State<MainScreen> createState() => _MainScreenState();
}

class _MainScreenState extends State<MainScreen> {
  bool _settings = false, _all = false;

  Controller get k => widget.controller;

  @override
  Widget build(BuildContext context) {
    final c = AwvaTheme.of(context);
    // Ctrl+R (Cmd+R on macOS): scan the engines folder again
    return CallbackShortcuts(
      bindings: {
        const SingleActivator(LogicalKeyboardKey.keyR, control: true): k.rescan,
        const SingleActivator(LogicalKeyboardKey.keyR, meta: true): k.rescan,
      },
      child: Focus(autofocus: true, child: _screen(c)),
    );
  }

  Widget _screen(AwvaColors c) {
    return ColoredBox(
      color: c.bg0,
      child: DefaultTextStyle(
        style: AwvaType.body(c.text1),
        child: ListenableBuilder(
          listenable: k,
          builder: (context, _) => Stack(children: [
            Column(children: [
              _Header(k: k, onSettings: () => setState(() => _settings = !_settings)),
              Expanded(child: _Body(k: k, all: _all, onAll: () => setState(() => _all = !_all))),
              _Footer(k: k),
            ]),
            if (_settings) Positioned.fill(child: GestureDetector(onTap: () => setState(() => _settings = false), child: const ColoredBox(color: Color(0x00000000)))),
            if (_settings)
              Positioned(
                top: 0,
                bottom: 0,
                right: 0,
                width: 380,
                child: TweenAnimationBuilder<double>(
                  tween: Tween(begin: 1, end: 0),
                  duration: AwvaSize.open,
                  curve: Curves.easeOutCubic,
                  builder: (_, t, child) => Transform.translate(offset: Offset(t * 380, 0), child: child),
                  child: _Settings(k: k, onClose: () => setState(() => _settings = false)),
                ),
              ),
          ]),
        ),
      ),
    );
  }
}

// --- the header ----------------------------------------------------------------------------------

class _Header extends StatelessWidget {
  const _Header({required this.k, required this.onSettings});
  final Controller k;
  final VoidCallback onSettings;

  @override
  Widget build(BuildContext context) {
    final c = AwvaTheme.of(context);
    final s = k.sound;
    return Container(
      height: AwvaSize.header,
      padding: const EdgeInsets.symmetric(horizontal: AwvaSize.s4),
      child: Row(children: [
        Image.asset('assets/logo/awva.png', height: 22, filterQuality: FilterQuality.medium),
        const SizedBox(width: AwvaSize.s5),
        Builder(
          builder: (anchor) => _Picker(
            title: s?.engineTitle ?? 'No engine',
            detail: s == null || s.engineBuiltin ? null : s.engineLabel,
            onTap: () => _engineMenu(anchor),
          ),
        ),
        const SizedBox(width: AwvaSize.s2),
        AwvaButton(onTap: s == null || s.presets.isEmpty ? null : () => k.step(-1), icon: Icons.chevronLeft, tooltip: 'previous preset'),
        Builder(
          builder: (anchor) => _Picker(
            title: s == null ? '' : (s.presets.isEmpty ? 'Default' : s.presetName),
            onTap: s == null || s.presets.isEmpty ? null : () => _presetMenu(anchor),
          ),
        ),
        AwvaButton(onTap: s == null || s.presets.isEmpty ? null : () => k.step(1), icon: Icons.chevronRight, tooltip: 'next preset'),
        const Spacer(),
        ValueListenableBuilder<bool>(
          valueListenable: k.midiActive,
          builder: (_, on, _) => Row(children: [
            Dot(on ? c.blue : (k.ports.any((p) => p.state == 'open') ? c.text3 : c.bg3)),
            const SizedBox(width: 6),
            Text('MIDI', style: AwvaType.small(c.text2)),
          ]),
        ),
        const SizedBox(width: AwvaSize.s5),
        ValueListenableBuilder<Status>(
          valueListenable: k.status,
          builder: (_, st, _) => LevelControl(
            db: st.masterDb,
            peakL: st.peakL,
            peakR: st.peakR,
            limiting: st.reductionDb > 0.5,
            onChanged: k.setMaster,
          ),
        ),
        const SizedBox(width: AwvaSize.s3),
        AwvaButton(
          onTap: () => k.setTheme(switch (k.theme) {
            ThemeChoice.system => c.dark ? ThemeChoice.light : ThemeChoice.dark,
            ThemeChoice.light => ThemeChoice.dark,
            ThemeChoice.dark => ThemeChoice.light,
          }),
          icon: Icons.theme,
          tooltip: 'light or dark',
        ),
        AwvaButton(onTap: onSettings, icon: Icons.settings, tooltip: 'settings'),
      ]),
    );
  }

  void _engineMenu(BuildContext anchor) {
    final items = <MenuItem>[const MenuItem(title: 'built in', heading: true)];
    for (final e in k.engines.where((e) => e.builtin)) {
      items.add(MenuItem(
        title: e.title,
        detail: e.ok ? '' : e.error,
        selected: e.key == k.sound?.engineKey,
        enabled: e.ok,
        onTap: () => k.selectEngine(e.key),
      ));
    }
    final packages = k.engines.where((e) => !e.builtin).toList();
    items.add(const MenuItem(title: 'your engines', heading: true));
    if (packages.isEmpty) items.add(MenuItem(title: 'None yet', detail: 'Add packages to ${k.enginesDir}', enabled: false));
    for (final e in packages) {
      items.add(MenuItem(
        title: e.label,
        detail: e.ok ? '' : e.error,
        trailing: e.ok ? e.title : '',
        selected: e.key == k.sound?.engineKey,
        enabled: e.ok,
        onTap: () => k.selectEngine(e.key),
      ));
    }
    items.add(MenuItem(title: 'Rescan your engines', trailing: 'Ctrl+R', onTap: k.rescan));
    showMenuAt(anchor, items, width: 300);
  }

  void _presetMenu(BuildContext anchor) {
    final s = k.sound!;
    showMenuAt(anchor, [
      for (var i = 0; i < s.presets.length; i++)
        MenuItem(
          title: s.presets[i].name,
          trailing: s.presets[i].factory ? '' : 'file',
          selected: i == s.preset,
          onTap: () => k.selectPreset(i),
        ),
    ], width: 280);
  }
}

class _Picker extends StatefulWidget {
  const _Picker({required this.title, this.detail, required this.onTap});
  final String title;
  final String? detail;
  final VoidCallback? onTap;
  @override
  State<_Picker> createState() => _PickerState();
}

class _PickerState extends State<_Picker> {
  bool _hover = false;
  @override
  Widget build(BuildContext context) {
    final c = AwvaTheme.of(context);
    return MouseRegion(
      cursor: widget.onTap != null ? SystemMouseCursors.click : SystemMouseCursors.basic,
      onEnter: (_) => setState(() => _hover = true),
      onExit: (_) => setState(() => _hover = false),
      child: GestureDetector(
        onTap: widget.onTap,
        child: AnimatedContainer(
          duration: AwvaSize.quick,
          height: 34,
          padding: const EdgeInsets.symmetric(horizontal: 10),
          decoration: BoxDecoration(color: _hover && widget.onTap != null ? c.bg2 : null, borderRadius: BorderRadius.circular(AwvaSize.radiusSmall)),
          child: Row(mainAxisSize: MainAxisSize.min, children: [
            ConstrainedBox(
              constraints: const BoxConstraints(maxWidth: 220),
              child: Text(widget.title, maxLines: 1, overflow: TextOverflow.ellipsis, style: AwvaType.title(c.text1)),
            ),
            if (widget.detail != null) ...[
              const SizedBox(width: 8),
              Container(
                padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 1),
                decoration: BoxDecoration(color: c.butter.withValues(alpha: 0.18), borderRadius: BorderRadius.circular(4)),
                child: Text(widget.detail!, style: AwvaType.small(c.butter)),
              ),
            ],
            if (widget.onTap != null) ...[const SizedBox(width: 4), AwvaIcon(Icons.chevronDown, color: c.text3)],
          ]),
        ),
      ),
    );
  }
}

// --- the sound -----------------------------------------------------------------------------------

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
            Text(s.presets.isEmpty ? s.engineTitle : s.presetName, style: AwvaType.sound(c.text1)),
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
            GestureDetector(
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
    final c = AwvaTheme.of(context);
    return SizedBox(
      width: AwvaSize.knob + 24,
      height: AwvaSize.knob + 22,
      child: Column(mainAxisAlignment: MainAxisAlignment.center, children: [
        const Spacer(),
        AwvaSwitch(value: p.value >= 0.5, onChanged: onChanged),
        const Spacer(),
        Text(paramLabel(p.name), maxLines: 1, overflow: TextOverflow.ellipsis, style: AwvaType.body(c.text2)),
      ]),
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
        SizedBox(width: 180, child: Text('legato', style: AwvaType.body(c.text2))),
        AwvaSwitch(value: s.legato, onChanged: k.setLegato),
      ]),
    ));
    return Column(crossAxisAlignment: CrossAxisAlignment.start, children: rows);
  }
}

/// a parameter as a row: its name, a ribbon slider, its value
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
        SizedBox(width: 180, child: Text(paramLabel(p.name), maxLines: 1, overflow: TextOverflow.ellipsis, style: AwvaType.body(c.text2))),
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

              return MouseRegion(
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

// --- the footer ----------------------------------------------------------------------------------

class _Footer extends StatelessWidget {
  const _Footer({required this.k});
  final Controller k;
  @override
  Widget build(BuildContext context) {
    final c = AwvaTheme.of(context);
    final open = k.ports.where((p) => p.state == 'open').map((p) => p.name).toList();
    final busy = k.ports.where((p) => p.state == 'busy' || p.state == 'failed').toList();
    final a = k.audio;
    return Container(
      height: AwvaSize.footer,
      padding: const EdgeInsets.symmetric(horizontal: AwvaSize.s4),
      child: ValueListenableBuilder<Status>(
        valueListenable: k.status,
        builder: (_, st, _) {
          final audio = switch (a.state) {
            'running' =>
              '${a.device}${a.mode == 'exclusive' ? ' (exclusive)' : ''} · ${(a.rate / 1000).toStringAsFixed(1)} kHz · key to sound ${k.latency.totalMs.toStringAsFixed(1)} ms',
            'null' => 'No audio output (--no-audio)',
            'failed' => 'Audio: ${a.message.isEmpty ? 'not running' : a.message}',
            _ => 'Audio off',
          };
          return Row(children: [
            Dot(a.state == 'running' ? c.sage : (a.state == 'failed' ? c.error : c.text3), size: 6),
            const SizedBox(width: 6),
            Flexible(child: Text(audio, maxLines: 1, overflow: TextOverflow.ellipsis, style: AwvaType.small(a.state == 'failed' ? c.error : c.text3))),
            const SizedBox(width: AwvaSize.s4),
            Text('CPU ${(st.cpu * 100).round()}%', style: AwvaType.small(c.text3)),
            if (st.reductionDb > 0.5) ...[const SizedBox(width: AwvaSize.s4), Text('limiter −${st.reductionDb.toStringAsFixed(1)} dB', style: AwvaType.small(c.butter))],
            const Spacer(),
            Flexible(
              flex: 2,
              child: Text(
                busy.isNotEmpty
                    ? '${busy.first.name}: ${busy.first.message}'
                    : (open.isEmpty ? 'No MIDI input' : 'MIDI in: ${open.join(', ')}'),
                maxLines: 1,
                overflow: TextOverflow.ellipsis,
                textAlign: TextAlign.right,
                style: AwvaType.small(busy.isNotEmpty ? c.butter : c.text3),
              ),
            ),
          ]);
        },
      ),
    );
  }
}

// --- settings ------------------------------------------------------------------------------------

class _Settings extends StatelessWidget {
  const _Settings({required this.k, required this.onClose});
  final Controller k;
  final VoidCallback onClose;

  @override
  Widget build(BuildContext context) {
    final c = AwvaTheme.of(context);
    Widget head(String t) => Padding(padding: const EdgeInsets.only(top: AwvaSize.s5, bottom: AwvaSize.s2), child: Text(t, style: AwvaType.micro(c.text3)));
    final a = k.audio;
    final packages = k.engines.where((e) => !e.builtin).toList();
    return Container(
      decoration: BoxDecoration(
        color: c.bg1,
        border: Border(left: BorderSide(color: c.hairline)),
        boxShadow: [BoxShadow(color: c.shadow, blurRadius: 24)],
      ),
      child: SingleChildScrollView(
        padding: const EdgeInsets.fromLTRB(AwvaSize.s5, AwvaSize.s3, AwvaSize.s5, AwvaSize.s5),
        child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
          Row(children: [Text('Settings', style: AwvaType.title(c.text1)), const Spacer(), AwvaButton(onTap: onClose, icon: Icons.close, tooltip: 'close')]),
          head('appearance'),
          Row(children: [
            for (final t in ThemeChoice.values) ...[
              _Segment(label: t.name[0].toUpperCase() + t.name.substring(1), selected: k.theme == t, onTap: () => k.setTheme(t)),
              const SizedBox(width: 4),
            ],
          ]),
          head('audio output'),
          Text(a.device.isEmpty ? (a.state == 'null' ? 'None (--no-audio)' : 'No device') : a.device, style: AwvaType.body(c.text1)),
          Text(
            a.state == 'running'
                ? '${a.backend} ${a.mode} · ${a.rate} Hz. ${a.mode == 'exclusive' ? 'The Windows volume is applied by awva (now ${(a.volume * 100).round()}%).' : 'Follows the system volume.'}'
                : (a.message.isEmpty ? a.backend : a.message),
            style: AwvaType.small(a.state == 'failed' ? c.error : c.text3),
          ),
          if (a.state == 'running' && a.message.isNotEmpty) Text(a.message, style: AwvaType.small(c.butter)),
          if (a.exclusiveAvailable) ...[
            const SizedBox(height: AwvaSize.s2),
            Row(children: [
              _Segment(label: 'Shared', selected: a.wanted != 'exclusive', onTap: () => k.setAudioMode('shared')),
              const SizedBox(width: 4),
              _Segment(label: 'Exclusive', selected: a.wanted == 'exclusive', onTap: () => k.setAudioMode('exclusive')),
            ]),
            if (a.wanted == 'exclusive') ...[
              const SizedBox(height: AwvaSize.s2),
              Row(children: [
                Text('buffer', style: AwvaType.small(c.text3)),
                const SizedBox(width: AwvaSize.s2),
                // (0: the device's smallest buffer, the least delay; larger if it crackles)
                for (final f in const [0, 256, 512]) ...[
                  _Segment(label: f == 0 ? 'smallest' : '$f', selected: a.frames == f, onTap: () => k.setAudioMode('exclusive', f)),
                  const SizedBox(width: 4),
                ],
              ]),
            ],
            if (k.exclusiveNote)
              Container(
                margin: const EdgeInsets.only(top: AwvaSize.s2),
                padding: const EdgeInsets.all(AwvaSize.s3),
                decoration: BoxDecoration(color: c.butter.withValues(alpha: 0.14), borderRadius: BorderRadius.circular(AwvaSize.radiusSmall)),
                child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                  Text(
                    'Exclusive mode gives awva the output alone: other apps go quiet, and the Windows mixer is bypassed. '
                    'awva applies the Windows volume itself. Check the level before you play loud.',
                    style: AwvaType.small(c.text1),
                  ),
                  const SizedBox(height: AwvaSize.s2),
                  AwvaButton(onTap: k.noteExclusiveRead, label: 'Got it'),
                ]),
              ),
          ],
          const SizedBox(height: AwvaSize.s2),
          AwvaButton(onTap: k.restartAudio, icon: Icons.refresh, label: 'Restart audio'),
          head('delay, key to sound'),
          _LatencyTable(l: k.latency, mode: a.mode),
          head('midi inputs'),
          if (k.ports.isEmpty) Text('No MIDI inputs. Connect a controller; it shows here.', style: AwvaType.small(c.text3)),
          for (final p in k.ports)
            Padding(
              padding: const EdgeInsets.only(bottom: AwvaSize.s2),
              child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                Row(children: [
                  Dot(switch (p.state) { 'open' => c.sage, 'busy' => c.butter, 'failed' => c.error, _ => c.bg3 }),
                  const SizedBox(width: AwvaSize.s2),
                  Expanded(child: Text(p.name, maxLines: 1, overflow: TextOverflow.ellipsis, style: AwvaType.body(c.text1))),
                  AwvaSwitch(value: p.enabled, onChanged: (v) => k.enablePort(p.name, v)),
                ]),
                if (p.message.isNotEmpty) Padding(padding: const EdgeInsets.only(left: 16, top: 2), child: Text(p.message, style: AwvaType.small(c.butter))),
              ]),
            ),
          head('play'),
          Row(children: [
            Expanded(child: Text('Legato (one voice, slurred)', style: AwvaType.body(c.text1))),
            AwvaSwitch(value: k.sound?.legato ?? false, onChanged: k.setLegato),
          ]),
          const SizedBox(height: AwvaSize.s2),
          AwvaButton(onTap: k.panic, icon: Icons.stop, label: 'All notes off'),
          head('your engines'),
          Text(k.enginesDir, style: AwvaType.small(c.text2)),
          const SizedBox(height: AwvaSize.s1),
          Text('A folder per engine package (awva-package/1); see the README.', style: AwvaType.small(c.text3)),
          const SizedBox(height: AwvaSize.s2),
          AwvaButton(onTap: k.rescan, icon: Icons.refresh, label: 'Rescan'),
          for (final e in packages)
            Padding(
              padding: const EdgeInsets.only(top: AwvaSize.s2),
              child: Row(crossAxisAlignment: CrossAxisAlignment.start, children: [
                Padding(padding: const EdgeInsets.only(top: 5), child: Dot(e.ok ? c.sage : c.error, size: 6)),
                const SizedBox(width: AwvaSize.s2),
                Expanded(
                  child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                    Text(e.label, style: AwvaType.body(c.text1)),
                    Text(e.ok ? '${e.title}${e.version.isEmpty ? '' : ' · ${e.version}'} · ${e.source}' : e.error,
                        style: AwvaType.small(e.ok ? c.text3 : c.error)),
                    for (final r in e.refused) Text('preset refused: $r', style: AwvaType.small(c.butter)),
                  ]),
                ),
              ]),
            ),
        ]),
      ),
    );
  }
}

/// the delay chain, each figure with where it comes from
class _LatencyTable extends StatelessWidget {
  const _LatencyTable({required this.l, required this.mode});
  final LatencyInfo l;
  final String mode;
  @override
  Widget build(BuildContext context) {
    final c = AwvaTheme.of(context);
    String ms(double v) => '${v.toStringAsFixed(1)} ms';
    Widget row(String what, String value, String source, {bool strong = false}) => Padding(
          padding: const EdgeInsets.only(bottom: 2),
          child: Row(crossAxisAlignment: CrossAxisAlignment.start, children: [
            Expanded(child: Text(what, style: (strong ? AwvaType.bodyStrong : AwvaType.small)(strong ? c.text1 : c.text2))),
            SizedBox(width: 116, child: Text(value, textAlign: TextAlign.right, style: AwvaType.value(strong ? c.text1 : c.text2))),
            const SizedBox(width: AwvaSize.s3),
            SizedBox(width: 62, child: Text(source, style: AwvaType.small(c.text3))),
          ]),
        );
    return Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
      row('MIDI in to output', l.midiCount > 0 ? '${ms(l.midiMean)} (${l.midiMin.toStringAsFixed(1)}–${l.midiMax.toStringAsFixed(1)})' : 'play a note',
          l.midiCount > 0 ? 'measured' : ''),
      row('engine step', 'up to ${ms(l.stepMs)}', 'by design'),
      row('engine block', '${l.blockFrames} fr · ${ms(l.blockMs)}', 'by design'),
      row('output buffer', '${l.bufferFrames} fr · ${ms(l.bufferMs)}', 'driver'),
      row('device latency', l.streamMs >= 0 ? ms(l.streamMs) : 'not reported', 'driver'),
      row('key to sound', ms(l.totalMs), 'sum', strong: true),
    ]);
  }
}

class _Segment extends StatelessWidget {
  const _Segment({required this.label, required this.selected, required this.onTap});
  final String label;
  final bool selected;
  final VoidCallback onTap;
  @override
  Widget build(BuildContext context) {
    final c = AwvaTheme.of(context);
    return MouseRegion(
      cursor: SystemMouseCursors.click,
      child: GestureDetector(
        onTap: onTap,
        child: AnimatedContainer(
          duration: AwvaSize.quick,
          height: 30,
          padding: const EdgeInsets.symmetric(horizontal: 12),
          alignment: Alignment.center,
          decoration: BoxDecoration(color: selected ? c.bg3 : c.bg2, borderRadius: BorderRadius.circular(AwvaSize.radiusSmall)),
          child: Text(label, style: AwvaType.bodyStrong(selected ? c.text1 : c.text2)),
        ),
      ),
    );
  }
}
