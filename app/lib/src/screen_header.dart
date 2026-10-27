// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The screen's header (DESIGN.md, "Layout"): the mark, the engine and preset pickers, MIDI, the
// level, the theme, settings.
part of 'screen.dart';

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
      child: LayoutBuilder(builder: (context, box) {
        // a narrow window (the 720 minimum): the pickers give way first, never the level and the
        // buttons; the MIDI word goes, its dot stays
        final narrow = box.maxWidth < 900;
        // what the rest of the row takes (the logo, 22 px high and 98 wide; the arrows, MIDI, the
        // level, two buttons and the gaps); the pickers share what is left, the engine's (a title
        // and maybe a chip) three fifths of it, each 240 px at most
        final rest = 98 + 2 * (narrow ? AwvaSize.s3 : AwvaSize.s5) + AwvaSize.s2 + 64 + (narrow ? 8 : 50) + 172 + AwvaSize.s3 + 64 + 16;
        final left = (box.maxWidth - rest).clamp(144.0, double.infinity);
        final pickEngine = (left * 0.6).clamp(72.0, 240.0), pickPreset = (left * 0.4).clamp(72.0, 240.0);
        return Row(children: [
        Image.asset('assets/logo/awva.png', height: 22, filterQuality: FilterQuality.medium),
        SizedBox(width: narrow ? AwvaSize.s3 : AwvaSize.s5),
        Builder(
          builder: (anchor) => _Picker(
            title: s?.engineTitle ?? 'No engine',
            detail: s == null || s.engineBuiltin ? null : s.engineLabel,
            maxWidth: pickEngine,
            onTap: () => _engineMenu(anchor),
          ),
        ),
        const SizedBox(width: AwvaSize.s2),
        AwvaButton(onTap: s == null || s.presets.isEmpty ? null : () => k.step(-1), icon: Icons.chevronLeft, tooltip: 'previous preset'),
        Builder(
          builder: (anchor) => _Picker(
            title: s == null ? '' : (s.presets.isEmpty ? 'Default' : s.presetName),
            maxWidth: pickPreset,
            onTap: s == null || s.presets.isEmpty ? null : () => _presetMenu(anchor),
          ),
        ),
        AwvaButton(onTap: s == null || s.presets.isEmpty ? null : () => k.step(1), icon: Icons.chevronRight, tooltip: 'next preset'),
        const Spacer(),
        ValueListenableBuilder<bool>(
          valueListenable: k.midiActive,
          builder: (_, on, _) => Row(children: [
            Dot(on ? c.blue : (k.ports.any((p) => p.state == 'open') ? c.text3 : c.bg3)),
            if (!narrow) ...[const SizedBox(width: 6), Text('MIDI', style: AwvaType.small(c.text2))],
          ]),
        ),
        SizedBox(width: narrow ? AwvaSize.s3 : AwvaSize.s5),
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
      ]);
      }),
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
    if (packages.isEmpty) items.add(const MenuItem(title: 'None yet', enabled: false));
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

double _textWidth(String s, TextStyle style) =>
    (TextPainter(text: TextSpan(text: s, style: style), maxLines: 1, textDirection: TextDirection.ltr)..layout()).width;

class _Picker extends StatefulWidget {
  const _Picker({required this.title, this.detail, required this.onTap, this.maxWidth = 260});
  final String title;
  final String? detail;
  final VoidCallback? onTap;

  /// the whole picker's width at most: its title ellipsizes (and then its chip)
  final double maxWidth;
  @override
  State<_Picker> createState() => _PickerState();
}

class _PickerState extends State<_Picker> {
  bool _hover = false;
  @override
  Widget build(BuildContext context) {
    final c = AwvaTheme.of(context);
    // the room inside (the padding, the chevron): the chip its text's width up to 45 % of it, the
    // title the rest, 220 at most
    final inner = widget.maxWidth - 20 - (widget.onTap != null ? 20 : 0);
    final chipMax = widget.detail == null ? 0.0 : math.min(_textWidth(widget.detail!, AwvaType.small(c.butter)) + 12, inner * 0.45);
    final titleMax = math.min(220.0, inner - (widget.detail == null ? 0 : chipMax + 8)).clamp(24.0, 220.0);
    return AwvaFocus(
      label: widget.title,
      enabled: widget.onTap != null,
      onPress: widget.onTap,
      child: MouseRegion(
      cursor: widget.onTap != null ? SystemMouseCursors.click : SystemMouseCursors.basic,
      onEnter: (_) => setState(() => _hover = true),
      onExit: (_) => setState(() => _hover = false),
      child: GestureDetector(
        onTap: widget.onTap,
        child: AnimatedContainer(
          duration: AwvaSize.quick,
          height: 34,
          constraints: BoxConstraints(maxWidth: widget.maxWidth),
          padding: const EdgeInsets.symmetric(horizontal: 10),
          decoration: BoxDecoration(color: _hover && widget.onTap != null ? c.bg2 : null, borderRadius: BorderRadius.circular(AwvaSize.radiusSmall)),
          child: Row(mainAxisSize: MainAxisSize.min, children: [
            ConstrainedBox(
              constraints: BoxConstraints(maxWidth: titleMax),
              child: Text(widget.title, maxLines: 1, overflow: TextOverflow.ellipsis, style: AwvaType.title(c.text1)),
            ),
            if (widget.detail != null) ...[
              const SizedBox(width: 8),
              Container(
                constraints: BoxConstraints(maxWidth: chipMax),
                padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 1),
                decoration: BoxDecoration(color: c.butter.withValues(alpha: 0.18), borderRadius: BorderRadius.circular(4)),
                child: Text(widget.detail!, maxLines: 1, overflow: TextOverflow.ellipsis, style: AwvaType.small(c.butter)),
              ),
            ],
            if (widget.onTap != null) ...[const SizedBox(width: 4), AwvaIcon(Icons.chevronDown, color: c.text3)],
          ]),
        ),
      ),
      ),
    );
  }
}
