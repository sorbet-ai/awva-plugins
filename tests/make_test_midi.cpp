// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The MIDI-file proof's performance as a Standard MIDI File (the SDK's make_midi: 480 ticks per quarter at 120
// bpm): MPE on member channels 2..5 with pitch bends, channel pressure and CC74, overlapping notes,
// a master-channel note with poly pressure and the mod wheel, the sustain pedal. Some events fall
// on the 2 ms MIDI steps exactly, most between them.
// Usage: make_test_midi out.mid
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "awva_sdk/smf.h"

using awva::sdk::MidiEvent;

int main(int argc, char** argv) {
	if (argc != 2) {
		std::fprintf(stderr, "usage: make_test_midi out.mid\n");
		return 2;
	}
	std::vector<MidiEvent> ev;
	auto add = [&](double t, int a, int b, int c, int len = 3) {
		ev.push_back({t, {static_cast<uint8_t>(a), static_cast<uint8_t>(b), static_cast<uint8_t>(c)}, static_cast<uint8_t>(len)});
	};
	// four MPE notes, staggered, each on its own member channel
	const int keys[4] = {53, 57, 60, 64};
	for (int n = 0; n < 4; n++) {
		const int ch = 1 + n; // channels 2..5
		const double on = 0.1 + 0.37 * n, off = on + 1.6;
		add(on, 0xb0 | ch, 74, 64);
		add(on, 0xe0 | ch, 0, 64);
		add(on, 0x90 | ch, keys[n], 70 + 12 * n);
		for (int k = 1; k < 60; k++) {
			const double t = on + 0.025 * k;
			if (t >= off) break;
			add(t, 0xd0 | ch, static_cast<int>(40 + 60 * std::sin(0.11 * k + n)) & 0x7f, 0, 2);
			const int bend = 8192 + static_cast<int>(600 * std::sin(0.2 * k));
			add(t + 0.004, 0xe0 | ch, bend & 0x7f, (bend >> 7) & 0x7f);
			if (k % 3 == 0) add(t + 0.009, 0xb0 | ch, 74, (64 + 4 * k) & 0x7f);
		}
		add(off, 0x80 | ch, keys[n], 40);
	}
	// the master channel: a note with poly pressure and the mod wheel, under the pedal
	add(2.3, 0xb0, 64, 127);
	add(2.4, 0x90, 48, 90);
	for (int k = 0; k < 40; k++) {
		add(2.45 + 0.03 * k, 0xa0, 48, (20 + 2 * k) & 0x7f);
		add(2.46 + 0.03 * k, 0xb0, 1, (3 * k) & 0x7f);
	}
	add(3.8, 0x80, 48, 0);
	add(4.2, 0xb0, 64, 0);
	// (make_midi writes the events in the order given)
	std::stable_sort(ev.begin(), ev.end(), [](const MidiEvent& a, const MidiEvent& b) { return a.t < b.t; });
	const std::vector<uint8_t> file = awva::sdk::make_midi(ev);
	FILE* f = std::fopen(argv[1], "wb");
	if (!f || std::fwrite(file.data(), 1, file.size(), f) != file.size()) {
		std::fprintf(stderr, "make_test_midi: cannot write %s\n", argv[1]);
		return 2;
	}
	std::fclose(f);
	std::printf("make_test_midi: %zu events, %zu bytes\n", ev.size(), file.size());
	return 0;
}
