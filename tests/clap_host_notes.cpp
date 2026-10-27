// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// clap_host_test's notes and many modes (clap_host.h): the CLAP dialect, overlapping notes.
#include "clap_host.h"

namespace host_test {

// --- notes: the CLAP dialect ---------------------------------------------------------------------------
std::vector<Timed> clap_performance(bool midi2, bool expressions) {
	std::vector<Timed> v;
	const int keys[3] = {55, 59, 62};
	for (int n = 0; n < 3; n++) {
		const uint64_t on = 4800 + 21000 * n + 37 * n, off = on + 60000;
		const uint8_t ch = 0; // (the CLAP notes' channel: a CLAP note plays on its own MIDI channel)
		const double vel = 0.5 + 0.2 * n;
		if (midi2) {
			const uint32_t v16 = static_cast<uint32_t>(vel * 65535.0 + 0.5);
			clap_event_midi2 x = {{sizeof x, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI2, 0}, 0,
			    {0x40000000u | (0x90u | ch) << 16 | static_cast<uint32_t>(keys[n]) << 8, v16 << 16, 0, 0}};
			push(v, on, x);
		} else {
			clap_event_note x = {{sizeof x, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_ON, 0}, 100 + n, 0, 0, static_cast<int16_t>(keys[n]), vel};
			push(v, on, x);
		}
		if (expressions) {
			for (int k = 1; k < 120; k++) {
				const uint64_t t = on + 480 * k + 7 * n;
				const double pres = 0.5 + 0.45 * std::sin(0.07 * k + n), tune = 0.6 * std::sin(0.05 * k), bright = 0.5 + 0.5 * std::sin(0.03 * k + 1.0);
				const double values[3] = {pres, tune, bright};
				const clap_note_expression ids[3] = {CLAP_NOTE_EXPRESSION_PRESSURE, CLAP_NOTE_EXPRESSION_TUNING, CLAP_NOTE_EXPRESSION_BRIGHTNESS};
				for (int x = 0; x < 3; x++) {
					if (midi2) {
						uint32_t w0 = 0, w1 = 0;
						const double c = std::fmin(std::fmax(values[x], 0.0), 1.0) * 4294967295.0 + 0.5;
						if (x == 0) w0 = 0x40000000u | (0xa0u | ch) << 16 | static_cast<uint32_t>(keys[n]) << 8, w1 = static_cast<uint32_t>(c);
						if (x == 1) w0 = 0x40000000u | (0x00u | ch) << 16 | static_cast<uint32_t>(keys[n]) << 8 | 3,
						            w1 = static_cast<uint32_t>((keys[n] + values[1]) * 33554432.0);
						if (x == 2) w0 = 0x40000000u | (0x00u | ch) << 16 | static_cast<uint32_t>(keys[n]) << 8 | 74, w1 = static_cast<uint32_t>(c);
						clap_event_midi2 e = {{sizeof e, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI2, 0}, 0, {w0, w1, 0, 0}};
						push(v, t, e);
					} else {
						clap_event_note_expression e = {{sizeof e, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_EXPRESSION, 0}, ids[x], 100 + n, 0, 0,
						    static_cast<int16_t>(keys[n]), values[x]};
						push(v, t, e);
					}
				}
			}
		}
		if (midi2) {
			clap_event_midi2 x = {{sizeof x, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI2, 0}, 0,
			    {0x40000000u | (0x80u | ch) << 16 | static_cast<uint32_t>(keys[n]) << 8, 0, 0, 0}};
			push(v, off, x);
		} else {
			clap_event_note x = {{sizeof x, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_OFF, 0}, 100 + n, 0, 0, static_cast<int16_t>(keys[n]), 0.0};
			push(v, off, x);
		}
	}
	std::stable_sort(v.begin(), v.end(), [](const Timed& a, const Timed& b) { return a.frame < b.frame; });
	return v;
}

int notes_test() {
	uint32_t tail = 0;
	{
		Instance in;
		in.start(48000);
		const auto* t = in.ext<clap_plugin_tail>(CLAP_EXT_TAIL);
		tail = t ? t->get(in.p) : 0;
	}
	const uint64_t frames = 4800 + 42074 + 60000 + std::min<uint64_t>(tail, 48000 * 10) + 48000;
	const auto clap = clap_performance(false, true), midi2 = clap_performance(true, true), plain = clap_performance(false, false);
	note_ends = 0;
	out_sorted = true;
	const std::vector<float> a = play(clap, frames, 256);
	check(note_ends == 3 && out_sorted, "a NOTE_END per note, in time order (" + std::to_string(note_ends) + "; tail " + std::to_string(tail) + " frames)");
	size_t first = 0;
	const bool same = same_bits(a, play(midi2, frames, 256), &first);
	check(same, "CLAP notes, pressure, tuning, brightness = their MIDI 2.0 translation (MIDI2 dialect), bit for bit");
	if (!same) std::printf("     first difference at sample %zu\n", first);
	bool all_sizes = true;
	for (int bs : {1, 7, 32, 33, 500, 4096}) all_sizes = all_sizes && same_bits(a, play(clap, frames, bs));
	all_sizes = all_sizes && same_bits(a, play(clap, frames, 0, 48000, false, nullptr, 7));
	check(all_sizes, "the same output for host blocks of 1, 7, 32, 33, 500, 4096 and random sizes");
	double peak = 0;
	bool finite = true;
	for (float x : a) {
		peak = std::fmax(peak, std::fabs(x));
		finite = finite && std::isfinite(x);
	}
	check(finite && peak > 0.001, "it sounds (peak " + std::to_string(peak) + "), every sample finite");
	check(!same_bits(a, play(plain, frames, 256)), "the expressions change the sound");
	return 0;
}

// 20 overlapping notes on 20 keys (more than the engine's voices: the oldest are taken over), each
// with its own brightness; without (skip) one of them, or with (other) its brightness changed
std::vector<Timed> many_notes(int skip, int other) {
	std::vector<Timed> v;
	for (int n = 0; n < 20; n++) {
		if (n == skip) continue;
		const uint64_t on = 2400 + 1200 * static_cast<uint64_t>(n), off = on + 24000;
		const int16_t key = static_cast<int16_t>(48 + n);
		clap_event_note x = {{sizeof x, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_ON, 0}, 500 + n, 0, 0, key, 0.6};
		push(v, on, x);
		const double bright = n == other ? 0.95 : 0.05 + 0.04 * n;
		clap_event_note_expression b = {{sizeof b, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_EXPRESSION, 0}, CLAP_NOTE_EXPRESSION_BRIGHTNESS,
		    500 + n, 0, 0, key, bright};
		push(v, on, b); // (with the note on: the same 2 ms step, so an engine that reads y at the strike sees it)
		clap_event_note_expression pr = {{sizeof pr, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_EXPRESSION, 0}, CLAP_NOTE_EXPRESSION_PRESSURE,
		    500 + n, 0, 0, key, 0.7};
		push(v, on, pr);
		clap_event_note y = {{sizeof y, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_OFF, 0}, 500 + n, 0, 0, key, 0.0};
		push(v, off, y);
	}
	std::stable_sort(v.begin(), v.end(), [](const Timed& a, const Timed& b) { return a.frame < b.frame; });
	return v;
}
int many_test(const char* lib_path) {
	nsc_library lib;
	if (nsc_load(lib_path, &lib) != 0) {
		std::fprintf(stderr, "clap_host_test: cannot load the component %s\n", lib_path);
		return 2;
	}
	const bool reads_y = (lib.c->desc->inputs & (1u << 2)) != 0; // (engine.h in_timbre)
	nsc_unload(&lib);
	uint32_t tail = 0;
	{
		Instance in;
		in.start(48000);
		const auto* t = in.ext<clap_plugin_tail>(CLAP_EXT_TAIL);
		tail = t ? t->get(in.p) : 0;
	}
	const uint64_t frames = 2400 + 1200 * 19 + 24000 + std::min<uint64_t>(tail, 48000 * 10) + 48000;
	ended_ids.clear();
	out_sorted = true;
	const std::vector<float> all = play(many_notes(-1, -1), frames, 256);
	std::vector<int32_t> ids = ended_ids;
	std::sort(ids.begin(), ids.end());
	bool once = ids.size() == 20;
	for (size_t k = 0; once && k < ids.size(); k++) once = ids[k] == static_cast<int32_t>(500 + k);
	check(once && out_sorted, "20 overlapping CLAP notes: exactly one NOTE_END each, with its id (" + std::to_string(ended_ids.size()) + " NOTE_ENDs)");
	int sound = 0, own = 0;
	for (int n = 0; n < 20; n++) {
		sound += !same_bits(all, play(many_notes(n, -1), frames, 256));
		own += !same_bits(all, play(many_notes(-1, n), frames, 256));
	}
	check(sound == 20, "every one of the 20 sounds: without it the output differs (" + std::to_string(sound) + " of 20)");
	if (reads_y) check(own == 20, "every one's brightness is its own (per-note y, one channel): changing it changes the output (" + std::to_string(own) + " of 20)");
	else std::printf("info the engine doesn't read y: brightness changes the output for %d of 20 notes\n", own);
	return 0;
}

} // namespace host_test
