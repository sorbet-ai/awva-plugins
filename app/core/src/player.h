// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The audio thread's side of the sound: the engine instance, the MIDI setup it was given (so a new
// engine hears the controller's MPE Configuration Message and bend range again), and the safety
// chain. The live audio callback and the offline renderer both play through this one class.
#pragma once

#include <cstdint>
#include <cmath>
#include <cstring>

#include "instance.h"
#include "safety.h"

namespace awva_app {

// the controller's MIDI setup, as it came: per channel the MPE Configuration Message and the
// pitch bend range (MIDI 1.0 RPN 6 and 0 in UMP type 2, or MIDI 2.0 Registered Controllers)
class MidiSetup {
public:
	void see(const uint32_t* w) {
		const uint32_t mt = w[0] >> 28;
		const int ch = static_cast<int>((w[0] >> 16) & 0x0f);
		const uint32_t status = (w[0] >> 20) & 0x0f;
		if (mt == 2 && status == 0xb) {
			const uint32_t cc = (w[0] >> 8) & 0x7f, v = w[0] & 0x7f;
			Chan& c = ch_[ch];
			if (cc == 101) c.msb = static_cast<uint8_t>(v);
			else if (cc == 100) c.lsb = static_cast<uint8_t>(v);
			else if ((cc == 6 || cc == 38) && c.msb == 0 && (c.lsb == 0 || c.lsb == 6)) {
				uint8_t* d = c.lsb == 0 ? c.range : c.mcm;
				d[cc == 6 ? 0 : 1] = static_cast<uint8_t>(v);
				d[2] = 1;
			}
		} else if (mt == 4 && status == 0x2) { // Registered Controller
			const uint32_t bank = (w[0] >> 8) & 0x7f, index = w[0] & 0x7f;
			if (bank == 0 && (index == 0 || index == 6)) {
				Chan& c = ch_[ch];
				(index == 0 ? c.range2 : c.mcm2)[0] = w[0];
				(index == 0 ? c.range2 : c.mcm2)[1] = w[1];
			}
		}
	}
	// the setup again, into a new instance at its frame f
	void replay(Instance& in, uint64_t f) const {
		for (int ch = 0; ch < 16; ch++) {
			const Chan& c = ch_[ch];
			for (const uint8_t* d : {c.mcm, c.range}) {
				if (!d[2]) continue;
				const uint32_t lsb = d == c.mcm ? 6u : 0u;
				const uint32_t base = 0x20b00000u | static_cast<uint32_t>(ch) << 16;
				const uint32_t seq[6] = {base | 101u << 8, base | 100u << 8 | lsb, base | 6u << 8 | d[0], base | 38u << 8 | d[1],
				    base | 101u << 8 | 127u, base | 100u << 8 | 127u};
				for (const uint32_t s : seq) {
					const uint32_t w[4] = {s, 0, 0, 0};
					in.push(f, w);
				}
			}
			for (const uint32_t* p : {c.mcm2, c.range2})
				if (p[0]) {
					const uint32_t w[4] = {p[0], p[1], 0, 0};
					in.push(f, w);
				}
		}
	}

private:
	struct Chan {
		uint8_t msb = 127, lsb = 127;
		uint8_t range[3] = {}, mcm[3] = {}; // data entry MSB, LSB, set
		uint32_t range2[2] = {}, mcm2[2] = {};
	};
	Chan ch_[16];
};

class Player {
public:
	Instance* inst = nullptr; // the audio thread's
	Safety safety;
	MidiSetup setup;
	uint32_t faults = 0, overloads = 0;
	bool strict = false; // a package's engine: stopped for good at its first overload

	void prepare(int rate) { safety.prepare(rate); }
	// a new instance: the controller's setup replayed into it, faded in
	Instance* swap(Instance* next) {
		Instance* old = inst;
		inst = next;
		stopped = false;
		if (inst) setup.replay(*inst, inst->frame());
		safety.fade_in(0.030f);
		return old;
	}
	// an event at an absolute frame of the instance (in time order)
	void event(uint64_t f, const uint32_t* w) {
		setup.see(w);
		if (inst) inst->push(f, w);
	}
	// the awva contract's bound on an engine's output (8), after the x0.25: beyond it the engine is broken
	static constexpr float kEngineBound = 8.0f * kGain;
	bool stopped = false; // a package engine that broke the bound: silent until the next swap

	Safety::Result render(float* l, float* r, uint32_t n) {
		if (inst && !stopped) {
			inst->render(l, r, n);
			for (uint32_t i = 0; i < n; i++)
				if (!(std::fabs(l[i]) <= kEngineBound) || !(std::fabs(r[i]) <= kEngineBound)) {
					l[i] = NAN; // (a fault below: the block muted, the engine reset)
					overloads++;
					if (strict) stopped = true;
					break;
				}
		} else {
			std::memset(l, 0, n * sizeof(float));
			std::memset(r, 0, n * sizeof(float));
		}
		Safety::Result res = safety.process(l, r, n);
		if (res.fault) {
			faults++;
			if (inst) inst->c.reset(inst->state);
		}
		return res;
	}
};

} // namespace awva_app
