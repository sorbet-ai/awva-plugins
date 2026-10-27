// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The settings panel: appearance, the audio output, the delay figures, MIDI inputs, play, your
// engines.
part of 'screen.dart';

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
          if (k.devices.isNotEmpty) ...[
            Align(
              alignment: Alignment.centerLeft,
              child: Builder(
                builder: (anchor) => _Segment(
                  label: k.devices.where((d) => d.id == a.chosen).map((d) => d.name).firstOrNull ?? 'System default',
                  selected: false,
                  trailing: Icons.chevronDown,
                  onTap: () => _outputMenu(anchor),
                ),
              ),
            ),
            const SizedBox(height: AwvaSize.s2),
          ],
          Text(a.device.isEmpty ? (a.state == 'null' ? 'None (--no-audio)' : 'No device') : a.device, style: AwvaType.body(c.text1)),
          Text(
            a.state == 'running'
                ? '${_bus(a.bus)} · ${a.backend} ${a.mode} · ${a.rate} Hz${a.mode == 'exclusive' && a.volume >= 0 ? ' · volume ${(a.volume * 100).round()}%' : ''}'
                : (a.message.isEmpty ? a.backend : a.message),
            style: AwvaType.small(a.state == 'failed' ? c.error : c.text3),
          ),
          if (a.notice.isNotEmpty) Text(a.notice, style: AwvaType.small(c.butter)),
          if (a.state == 'running' && a.message.isNotEmpty) Text(a.message, style: AwvaType.small(c.butter)),
          if (a.exclusiveAvailable) ...[
            const SizedBox(height: AwvaSize.s2),
            Row(children: [
              _Segment(label: 'Shared', selected: a.wanted != 'exclusive', onTap: () => k.setAudioMode('shared')),
              const SizedBox(width: 4),
              _Segment(label: 'Exclusive', selected: a.wanted == 'exclusive', onTap: () => k.setAudioMode('exclusive')),
            ]),
            if (a.wanted == 'exclusive' && a.bus == 'bluetooth') ...[
              // (a Bluetooth output: no buffer offered it can't sustain; the app raises it on glitches)
              const SizedBox(height: AwvaSize.s2),
              if (a.mode == 'exclusive')
                Text('buffer ${a.buffer} frames', style: AwvaType.small(c.text3))
              else
                AwvaButton(onTap: () => k.setAudioMode('exclusive'), icon: Icons.refresh, label: 'Try exclusive again'),
            ] else if (a.wanted == 'exclusive') ...[
              const SizedBox(height: AwvaSize.s2),
              Row(children: [
                Text('buffer', style: AwvaType.small(c.text3)),
                const SizedBox(width: AwvaSize.s2),
                // (256, about 5 ms, the default; 0 the device's smallest: less delay, more risk of glitches)
                for (final f in const [0, 128, 256, 512]) ...[
                  _Segment(label: f == 0 ? 'smallest' : '$f', selected: a.frames == f, onTap: () => k.setAudioMode('exclusive', f)),
                  const SizedBox(width: 4),
                ],
              ]),
            ],
          ],
          const SizedBox(height: AwvaSize.s2),
          AwvaButton(onTap: k.restartAudio, icon: Icons.refresh, label: 'Restart audio'),
          head('delay, key to sound'),
          _LatencyTable(l: k.latency),
          head('midi inputs'),
          if (k.ports.isEmpty) Text('No MIDI inputs', style: AwvaType.small(c.text3)),
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
            Expanded(child: Text('mono', style: AwvaType.body(c.text1))),
            AwvaSwitch(value: k.sound?.mono ?? false, onChanged: k.setMono),
          ]),
          const SizedBox(height: AwvaSize.s2),
          AwvaButton(onTap: k.panic, icon: Icons.stop, label: 'All notes off'),
          head('your engines'),
          Text(k.enginesDir, style: AwvaType.small(c.text2)),
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

  /// the outputs, read again: the system's default, then every endpoint with its bus
  void _outputMenu(BuildContext anchor) {
    k.refreshDevices();
    final chosen = k.audio.chosen;
    showMenuAt(anchor, [
      MenuItem(title: 'System default', selected: chosen.isEmpty, onTap: () => k.setAudioDevice('')),
      for (final d in k.devices) MenuItem(title: d.name, trailing: _bus(d.bus), selected: d.id == chosen, onTap: () => k.setAudioDevice(d.id)),
    ], width: 340);
  }
}

/// the delay chain, figure by figure
class _LatencyTable extends StatelessWidget {
  const _LatencyTable({required this.l});
  final LatencyInfo l;
  @override
  Widget build(BuildContext context) {
    final c = AwvaTheme.of(context);
    String ms(double v) => '${v.toStringAsFixed(1)} ms';
    Widget row(String what, String value, {bool strong = false}) => Padding(
          padding: const EdgeInsets.only(bottom: 2),
          child: Row(crossAxisAlignment: CrossAxisAlignment.start, children: [
            Expanded(child: Text(what, style: (strong ? AwvaType.bodyStrong : AwvaType.small)(strong ? c.text1 : c.text2))),
            Text(value, textAlign: TextAlign.right, style: AwvaType.value(strong ? c.text1 : c.text2)),
          ]),
        );
    return Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
      row('MIDI in to output', l.midiCount > 0 ? '${ms(l.midiMean)} (${l.midiMin.toStringAsFixed(1)}–${l.midiMax.toStringAsFixed(1)})' : '–'),
      row('engine step', 'up to ${ms(l.stepMs)}'),
      row('engine block', '${l.blockFrames} fr · ${ms(l.blockMs)}'),
      row('output buffer', '${l.bufferFrames} fr · ${ms(l.bufferMs)}'),
      row('device latency', l.streamMs >= 0 ? ms(l.streamMs) : '–'),
      row('key to sound', ms(l.totalMs), strong: true),
      const SizedBox(height: AwvaSize.s2),
      row('glitches', l.recoveries > 0 ? '${l.glitches} · re-opened ${l.recoveries}×' : '${l.glitches}'),
    ]);
  }
}

/// a bus as the output list names it
String _bus(String bus) => switch (bus) { 'bluetooth' => 'Bluetooth', 'usb' => 'USB', _ => 'wired' };

class _Segment extends StatefulWidget {
  const _Segment({required this.label, required this.selected, required this.onTap, this.trailing});
  final String label;
  final bool selected;
  final VoidCallback onTap;
  final IconPainter? trailing;
  @override
  State<_Segment> createState() => _SegmentState();
}

class _SegmentState extends State<_Segment> {
  bool _hover = false;
  @override
  Widget build(BuildContext context) {
    final c = AwvaTheme.of(context);
    final label = widget.label, selected = widget.selected, onTap = widget.onTap, trailing = widget.trailing;
    return AwvaFocus(
      label: label,
      onPress: onTap,
      child: MouseRegion(
      cursor: SystemMouseCursors.click,
      onEnter: (_) => setState(() => _hover = true),
      onExit: (_) => setState(() => _hover = false),
      child: GestureDetector(
        onTap: onTap,
        child: AnimatedContainer(
          duration: AwvaSize.quick,
          height: 30,
          padding: const EdgeInsets.symmetric(horizontal: 12),
          alignment: trailing == null ? Alignment.center : null,
          decoration: BoxDecoration(color: selected || _hover ? c.bg3 : c.bg2, borderRadius: BorderRadius.circular(AwvaSize.radiusSmall)),
          child: trailing == null
              ? Text(label, style: AwvaType.bodyStrong(selected ? c.text1 : c.text2))
              : Row(mainAxisSize: MainAxisSize.min, children: [
                  Flexible(child: Text(label, maxLines: 1, overflow: TextOverflow.ellipsis, style: AwvaType.bodyStrong(c.text1))),
                  const SizedBox(width: 4),
                  AwvaIcon(trailing, color: c.text3),
                ]),
        ),
      ),
      ),
    );
  }
}
