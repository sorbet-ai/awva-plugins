// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The core's C++ side (core.h), without a running core: an offline render through the app's own
// path, MIDI 1.0 bytes as UMP, and the MPE controller performance the checks play.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#include "core.h"
#include "instance.h"
#include "player.h"

namespace awva_app {

bool render_offline(const nsc_component& c, const PresetEntry* preset, bool mono, const std::vector<TimedUmp>& events, uint64_t frames,
    int rate, float master_db, std::vector<float>* l, std::vector<float>* r, bool safety) {
	auto in = std::make_unique<Instance>(c);
	if (in->init(rate) != 0) return false;
	if (preset && preset->factory >= 0) c.set_preset(in->state, preset->factory);
	if (preset && preset->factory < 0 && c.load_preset(in->state, preset->text.data(), static_cast<uint32_t>(preset->text.size()), nullptr) != 1)
		return false;
	if (mono) in->set_mono(true);
	Player p;
	p.prepare(rate);
	p.safety.set_now_db(master_db);
	p.inst = in.get();
	if (!safety) { // (the instance alone: what the Player gives the safety chain)
		l->assign(static_cast<size_t>(frames), 0.0f);
		r->assign(static_cast<size_t>(frames), 0.0f);
		size_t k = 0;
		for (uint64_t f = 0; f < frames; f += 256) {
			const uint64_t n = std::min<uint64_t>(256, frames - f);
			for (; k < events.size() && events[k].frame < f + n; k++) p.event(events[k].frame, events[k].w);
			in->render(l->data() + f, r->data() + f, static_cast<uint32_t>(n));
		}
		p.inst = nullptr;
		return true;
	}
	const uint64_t total = frames + Limiter::kDelay;
	std::vector<float> a(static_cast<size_t>(total)), b(static_cast<size_t>(total));
	size_t k = 0;
	constexpr uint64_t kChunk = 256; // (any size gives the same samples: the instance's own block grid)
	for (uint64_t f = 0; f < total; f += kChunk) {
		const uint64_t n = std::min(kChunk, total - f);
		for (; k < events.size() && events[k].frame < f + n; k++) p.event(events[k].frame, events[k].w);
		(void)p.render(a.data() + f, b.data() + f, static_cast<uint32_t>(n));
	}
	p.inst = nullptr;
	l->assign(a.begin() + Limiter::kDelay, a.end());
	r->assign(b.begin() + Limiter::kDelay, b.end());
	return true;
}

std::vector<uint32_t> midi1_to_ump(const uint8_t* bytes, size_t n) {
	std::vector<uint32_t> out;
	size_t i = 0;
	while (i < n) {
		const uint8_t s = bytes[i];
		if (s < 0x80) {
			i++;
			continue;
		}
		if (s >= 0xf0) { // (system: skipped, a SysEx to its end)
			i++;
			if (s == 0xf0)
				while (i < n && bytes[i] != 0xf7) i++;
			continue;
		}
		const uint8_t hi = s & 0xf0;
		const size_t len = hi == 0xc0 || hi == 0xd0 ? 2 : 3;
		if (i + len > n) break;
		const uint32_t d1 = bytes[i + 1] & 0x7f, d2 = len == 3 ? bytes[i + 2] & 0x7f : 0;
		out.push_back(0x20000000u | static_cast<uint32_t>(s) << 16 | d1 << 8 | d2);
		i += len;
	}
	return out;
}

std::vector<TimedUmp> mpe_controller_performance(int rate, double seconds) {
	std::vector<TimedUmp> v;
	auto cc = [&](uint64_t f, int ch, int c, int val) { v.push_back({f, {0x20b00000u | static_cast<uint32_t>(ch) << 16 | static_cast<uint32_t>(c) << 8 | static_cast<uint32_t>(val), 0, 0, 0}}); };
	auto msg = [&](uint64_t f, uint32_t status, int ch, int d1, int d2) {
		v.push_back({f, {0x20000000u | (status | static_cast<uint32_t>(ch)) << 16 | static_cast<uint32_t>(d1) << 8 | static_cast<uint32_t>(d2), 0, 0, 0}});
	};
	const int voices = 4;
	const double range = 48.0;
	// midi_config(): the MCM on channel 1 (index 0), the bend range on each member channel
	cc(0, 0, 101, 0);
	cc(0, 0, 100, 6);
	cc(0, 0, 6, voices);
	cc(0, 0, 101, 127);
	cc(0, 0, 100, 127);
	for (int n = 0; n < voices; n++) {
		cc(0, 1 + n, 101, 0);
		cc(0, 1 + n, 100, 0);
		cc(0, 1 + n, 6, static_cast<int>(range));
		cc(0, 1 + n, 101, 127);
		cc(0, 1 + n, 100, 127);
	}
	// a phrase: four notes, each on its own member channel (voice n: channel 2 + n), overlapping,
	// with a glide on the third and vibrato on the last; the controller's update rate is about 1 kHz
	const int keys[4] = {62, 66, 69, 74};
	const double on_s[4] = {0.10, 0.55, 1.00, 1.45};
	const double len_s[4] = {1.20, 1.10, 1.00, std::max(1.2, seconds - 1.75)};
	for (int n = 0; n < voices; n++) {
		const int ch = 1 + n;
		const uint64_t on = static_cast<uint64_t>(on_s[n] * rate);
		const uint64_t off = static_cast<uint64_t>((on_s[n] + len_s[n]) * rate);
		auto bend14 = [&](double semis) { return std::clamp(static_cast<int>(semis * (8192.0 / range) + 8192.0 + 0.5), 0, 16383); };
		int b = bend14(0.0);
		msg(on, 0xe0, ch, b & 0x7f, b >> 7); // (the bend before the Note On: the tuning)
		msg(on, 0x90, ch, keys[n], 70 + 10 * n);
		int last_p = -1, last_y = -1, last_b = b;
		for (uint64_t f = on + rate / 1000; f < off; f += static_cast<uint64_t>(rate / 1000)) {
			const double t = static_cast<double>(f - on) / rate, T = static_cast<double>(off - on) / rate;
			const double env = std::min(1.0, t / 0.08) * std::min(1.0, (T - t) / 0.15);
			const int p = std::clamp(static_cast<int>((0.35 + 0.45 * env + 0.1 * std::sin(6.0 * t)) * 127.0 * env + 0.5), 0, 127);
			const int y = std::clamp(static_cast<int>(64 + 64 * 0.4 * std::sin(1.3 * t + n) + 0.5), 0, 127);
			double semis = 0.0;
			if (n == 2 && t > 0.4) semis = std::min(2.0, (t - 0.4) * 4.0); // a glide up a tone
			if (n == 3 && t > 0.3) semis = 0.25 * std::sin(2.0 * 3.14159265358979 * 5.5 * (t - 0.3)); // vibrato
			b = bend14(semis);
			if (b != last_b) msg(f, 0xe0, ch, b & 0x7f, b >> 7), last_b = b;
			if (p != last_p) msg(f, 0xd0, ch, p, 0), last_p = p;
			if (y != last_y) cc(f, ch, 74, y), last_y = y;
		}
		msg(off, 0x80, ch, keys[n], 64);
	}
	std::stable_sort(v.begin(), v.end(), [](const TimedUmp& a, const TimedUmp& b) { return a.frame < b.frame; });
	return v;
}

} // namespace awva_app
