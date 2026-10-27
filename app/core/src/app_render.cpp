// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The core's audio thread (app_core.h): the commands applied, every MIDI ring's messages at their
// frames (a burst spread over the engine's steps), the render through the Player and its figures.
#include "app_core.h"

namespace awva_app {

// the samples as they go to the device: the probe tap
void Core::tap(const float* l, const float* r, uint32_t n) {
	if (probe_run_)
		for (uint32_t i = 0; i < n; i++) (void)probe_.push({l[i], r[i]});
}
void Core::render(float* l, float* r, uint32_t n, uint64_t host_ns) {
	const uint64_t t0 = now_ns();
	Cmd c;
	while (cmds_.pop(&c)) apply(c);
	// MIDI: every ring's messages at their frames, in time order
	const uint64_t base = player_.inst ? player_.inst->frame() : 0;
	int k = 0;
	for (auto& ring : rings_) {
		MidiEv e;
		while (k < kBatch && ring.pop(&e)) {
			batch_[k].f = base;
			batch_[k].ns = e.ns;
			std::memcpy(batch_[k].w, e.w, sizeof e.w);
			k++;
		}
	}
	// (in arrival order, across the ports: an insertion sort, nothing allocated)
	for (int i = 1; i < k; i++)
		for (int j = i; j > 0 && batch_[j].ns < batch_[j - 1].ns; j--) std::swap(batch_[j], batch_[j - 1]);
	const uint64_t step = static_cast<uint64_t>((rate_ + 499) / 500); // (the engine's 2 ms step, frames)
	for (int i = 0; i < k; i++) {
		const uint64_t f = spread_.place(batch_[i].f, step); // (step_spread.h: at most kPerStep a step)
		player_.event(f, batch_[i].w);
		// measured: from the message's time to its frame's time in this output stream
		const double at = static_cast<double>(host_ns) + (static_cast<double>(f) - static_cast<double>(base)) * 1e9 / rate_;
		const int64_t us = static_cast<int64_t>((at - static_cast<double>(batch_[i].ns)) / 1e3);
		lat_n_.fetch_add(1, std::memory_order_relaxed);
		lat_sum_.fetch_add(us, std::memory_order_relaxed);
		int64_t m = lat_min_.load(std::memory_order_relaxed);
		while (us < m && !lat_min_.compare_exchange_weak(m, us)) {
		}
		m = lat_max_.load(std::memory_order_relaxed);
		while (us > m && !lat_max_.compare_exchange_weak(m, us)) {
		}
	}
	const Safety::Result res = player_.render(l, r, n);
	atomic_max(peak_l_, res.peak_l);
	atomic_max(peak_r_, res.peak_r);
	atomic_min(min_gain_, res.min_gain);
	if (res.fault) faults_.fetch_add(1);
	stopped_.store(player_.stopped ? 1 : 0);
	voices_.store(player_.inst ? player_.inst->c.active_voices(player_.inst->state) : 0);
	const float load = static_cast<float>(static_cast<double>(now_ns() - t0) * rate_ / (1e9 * n));
	cpu_.store(std::max(load, cpu_.load() * 0.995f));
	if (last_host_ns_ && host_ns - last_host_ns_ > static_cast<uint64_t>(2.5e9 * n / rate_)) xruns_.fetch_add(1);
	last_host_ns_ = host_ns;
}
void Core::apply(const Cmd& c) {
	Instance* in = player_.inst;
	switch (c.t) {
	case Cmd::Swap: {
		player_.strict = c.i != 0;
		Instance* old = player_.swap(static_cast<Instance*>(c.p));
		spread_.reset(player_.inst ? player_.inst->frame() : 0);
		if (old) (void)rets_.push({Ret::Inst, old});
		generation_.fetch_add(1);
		return;
	}
	case Cmd::Master: player_.safety.set_target_db(c.v); return;
	case Cmd::LoadText: {
		auto* t = static_cast<std::string*>(c.p);
		if (in) (void)in->c.load_preset(in->state, t->data(), static_cast<uint32_t>(t->size()), nullptr);
		(void)rets_.push({Ret::Text, t});
		generation_.fetch_add(1);
		return;
	}
	default: break;
	}
	if (!in) return;
	switch (c.t) {
	case Cmd::SetParam: (void)in->c.set_param(in->state, c.i, c.v); break;
	case Cmd::SetPreset:
		in->c.set_preset(in->state, c.i);
		generation_.fetch_add(1);
		break;
	case Cmd::Mono: in->set_mono(c.i != 0); break;
	case Cmd::Panic:
		in->c.reset(in->state);
		in->count = 0;
		break;
	default: break;
	}
}

} // namespace awva_app
