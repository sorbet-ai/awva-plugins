// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The footer: audio and MIDI in one line, the CPU, the limiter while it works, a notice.
part of 'screen.dart';

class _Footer extends StatelessWidget {
  const _Footer({required this.k});
  final Controller k;
  @override
  Widget build(BuildContext context) {
    final c = AwvaTheme.of(context);
    final open = k.ports.where((p) => p.state == 'open').map((p) => p.name).toList();
    final busy = k.ports.where((p) => p.state == 'busy' || p.state == 'failed').toList();
    final a = k.audio;
    // exclusive asked for, shared playing (a Bluetooth output that glitched at every buffer, a volume
    // that can't be read, a device that refused): the reason, in one line
    final fallback = a.state == 'running' && a.wanted == 'exclusive' && a.mode != 'exclusive' && a.message.isNotEmpty;
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
            if (k.note != null) ...[const SizedBox(width: AwvaSize.s4), Text(k.note!, style: AwvaType.small(c.butter))],
            if (fallback || a.notice.isNotEmpty) ...[
              const SizedBox(width: AwvaSize.s4),
              Flexible(child: Text(a.notice.isNotEmpty ? a.notice : a.message, maxLines: 1, overflow: TextOverflow.ellipsis, style: AwvaType.small(c.butter))),
            ],
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
