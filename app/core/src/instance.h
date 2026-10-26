// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// One engine instance, driven exactly as the plugins drive theirs (src/plugin.cpp): UMP events
// queued at absolute frames, a 32-frame block rendered when its first frame is due with every event
// of its frames (an event whose block was already rendered goes to the next block at its negative
// frame: a late event, nsc.h), x0.25 (awva_render's WAV scaling), subnormals flushed. So the app
// plays what the plugins play, sample for sample (app/core/tests/render_equal.cpp checks it).
//
// Threads: create, init and the scratch reads on the UI thread; push(), render() and the setters
// on the audio thread only (they are the component's audio-thread calls); param() on any thread.
#pragma once

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "nsc/nsc.h"

namespace awva_app {

constexpr float kGain = 0.25f; // the plugins' unity (awva_render's WAV scaling: the engines run about +-4)
constexpr int kMaxVoices = 10; // the plugins' voices

inline float flush(float x) { return x != 0.0f && std::fabs(x) < FLT_MIN ? std::copysign(0.0f, x) : x; }

inline void* region(size_t n) {
	n = std::max<size_t>((n + 63) & ~static_cast<size_t>(63), 64);
	void* p = ::operator new(n, std::align_val_t(64));
	std::memset(p, 0, n);
	return p;
}
inline void region_free(void* p) {
	if (p) ::operator delete(p, std::align_val_t(64));
}

struct Instance {
	const nsc_component& c;
	const nsc_desc& d;
	void *state = nullptr, *stream = nullptr, *bulk = nullptr; // (the regions: hot, stream, bulk)
	int sr = 48000;
	int nv;
	bool mono = false;

	uint64_t blocks = 0;
	int pos = NSC_BLOCK;
	float buf[2][NSC_BLOCK] = {};

	struct Ev {
		uint64_t frame;
		uint32_t w[4];
	};
	static constexpr size_t kQueue = 4096;
	std::vector<Ev> queue = std::vector<Ev>(kQueue);
	size_t head = 0, count = 0;
	nsc_event block_ev[1024];

	explicit Instance(const nsc_component& comp) : c(comp), d(*comp.desc), nv(std::min(kMaxVoices, comp.desc->max_voices)) {
		state = region(d.hot_bytes);
		stream = region(d.stream_bytes);
		bulk = region(d.bulk_bytes);
	}
	~Instance() {
		region_free(state);
		region_free(stream);
		region_free(bulk);
	}
	Instance(const Instance&) = delete;
	Instance& operator=(const Instance&) = delete;

	// the configuration the plugins run at: MIDI taken every block, nv voices
	int32_t init_at(int rate) {
		const nsc_config cfg = {rate, 0, nv};
		return c.init(state, stream, bulk, &cfg);
	}
	// (UI thread, before any audio-thread call) 0, or -1 for a rate the engine doesn't take
	int init(int rate) {
		if (init_at(rate) != 0) return -1;
		sr = rate;
		clear();
		return 0;
	}
	// init at another rate, the preset, values and mono kept (as the plugin's activate())
	int reinit(int rate) {
		const int preset = c.preset(state);
		std::vector<float> values(static_cast<size_t>(d.num_params));
		for (int i = 0; i < d.num_params; i++) values[static_cast<size_t>(i)] = c.param(state, i);
		if (init_at(rate) != 0) return -1;
		sr = rate;
		if (preset >= 0 && preset < d.num_presets) c.set_preset(state, preset);
		if (mono) (void)c.mode(state, NSC_MODE_MONO, 1);
		for (int i = 0; i < d.num_params; i++) (void)c.set_param(state, i, values[static_cast<size_t>(i)]);
		clear();
		return 0;
	}
	void clear() {
		blocks = 0;
		pos = NSC_BLOCK;
		head = count = 0;
	}
	// the absolute frame the next sample out has
	uint64_t frame() const { return blocks * NSC_BLOCK - static_cast<uint64_t>(NSC_BLOCK - pos); }

	// an event at an absolute frame (events in time order)
	void push(uint64_t f, const uint32_t* w) {
		if (count == kQueue) return; // (4096 events ahead of the audio: dropped)
		Ev& e = queue[(head + count) % kQueue];
		e.frame = f;
		std::memcpy(e.w, w, sizeof e.w);
		count++;
	}
	void render_block() {
		const uint64_t b0 = blocks * NSC_BLOCK;
		int n = 0;
		while (count > 0 && n < static_cast<int>(std::size(block_ev))) {
			const Ev& e = queue[head];
			if (e.frame >= b0 + NSC_BLOCK) break;
			block_ev[n].frame = static_cast<int32_t>(static_cast<int64_t>(e.frame) - static_cast<int64_t>(b0));
			block_ev[n].note_id = -1;
			std::memcpy(block_ev[n].words, e.w, sizeof e.w);
			n++;
			head = (head + 1) % kQueue;
			count--;
		}
		float* out[2] = {buf[0], buf[1]};
		c.process(state, NSC_BLOCK, block_ev, n, out); // (a block a call, as the plugins do)
		int32_t ids[32];
		while (c.ended(state, ids, 32) == 32) {
		}
		if (d.channels < 2) std::memcpy(buf[1], buf[0], sizeof buf[0]);
		blocks++;
		pos = 0;
	}
	void render(float* l, float* r, uint32_t n) {
		for (uint32_t i = 0; i < n; i++) {
			if (pos == NSC_BLOCK) render_block();
			l[i] = flush(buf[0][pos] * kGain);
			r[i] = flush(buf[1][pos] * kGain);
			pos++;
		}
	}
	void set_mono(bool on) {
		mono = on;
		(void)c.mode(state, NSC_MODE_MONO, on ? 1 : 0);
	}
	int param_index(const char* name) const {
		for (int i = 0; i < d.num_params; i++)
			if (!std::strcmp(d.params[i].name, name)) return i;
		return -1;
	}
};

} // namespace awva_app
