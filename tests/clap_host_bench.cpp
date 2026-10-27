// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// clap_host_test's bench mode (clap_host.h): the CPU load and the latency.
#include "clap_host.h"

namespace host_test {

// --- bench: the CPU load and the latency -------------------------------------------------------------------
// seconds spent in process(), host blocks of `block`
double play_timed(const std::vector<Timed>& events, uint64_t frames, uint32_t block, double rate) {
	Instance in;
	if (!in.p) return -1;
	in.start(rate);
	std::vector<float> l(block), r(block);
	size_t k = 0;
	Events ev;
	double spent = 0;
	for (uint64_t at = 0; at < frames; at += block) {
		ev.clear();
		for (; k < events.size() && events[k].frame < at + block; k++) {
			ev.store.push_back(events[k].bytes);
			reinterpret_cast<clap_event_header*>(ev.store.back().data())->time = static_cast<uint32_t>(events[k].frame - at);
		}
		const auto t0 = std::chrono::steady_clock::now();
		in.process(at, block, ev, l.data(), r.data());
		spent += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	}
	return spent;
}
// the component alone, the same UMP in 32-frame blocks
double component_timed(const nsc_component& c, const std::vector<expressive::Packet>& u, uint64_t frames, int rate, int nv) {
	nsc_instance in;
	const nsc_config cfg = {rate, 0, nv};
	if (nsc_instance_new(&c, &in) != 0 || c.init(in.state, in.stream, in.bulk, &cfg) != 0) return -1;
	size_t k = 0;
	float buf[2][NSC_BLOCK];
	float* o[2] = {buf[0], buf[1]};
	std::vector<nsc_event> ev;
	double spent = 0;
	for (uint64_t b0 = 0; b0 < frames; b0 += NSC_BLOCK) {
		ev.clear();
		for (; k < u.size() && u[k].frame < b0 + NSC_BLOCK; k++) ev.push_back({static_cast<int32_t>(u[k].frame - b0), -1, {u[k].w0, u[k].w1, 0, 0}});
		const auto t0 = std::chrono::steady_clock::now();
		c.process(in.state, NSC_BLOCK, ev.data(), static_cast<int32_t>(ev.size()), o);
		spent += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	}
	nsc_instance_free(&in);
	return spent;
}
int bench_test(const char* lib_path) {
	nsc_library lib;
	if (nsc_load(lib_path, &lib) != 0) {
		std::fprintf(stderr, "clap_host_test: cannot load the component %s\n", lib_path);
		return 2;
	}
	constexpr int rate = 48000;
	constexpr double seconds = 4;
	const int nv = static_cast<int>(plugin_voices());
	const uint64_t frames = static_cast<uint64_t>(seconds * rate);
	for (const int voices : {1, 4, nv}) {
		const std::vector<expressive::Event> perf = expressive::bench(voices, rate, seconds);
		const std::vector<Timed> events = clap_notes(perf);
		const std::vector<expressive::Packet> u = expressive::ump(perf);
		double best = 1e9, best_c = 1e9;
		for (int run = 0; run < 5; run++) {
			best = std::fmin(best, play_timed(events, frames, 128, rate));
			best_c = std::fmin(best_c, component_timed(*lib.c, u, frames, rate, nv));
		}
		std::printf("bench clap %d %.2f\n", voices, 100 * best / seconds);
		std::printf("bench component %d %.2f\n", voices, 100 * best_c / seconds);
	}
	nsc_unload(&lib);
	// the latency: none reported (no latency extension), and the frames from a Note On to the sound
	int64_t lo = 1 << 30, hi = -1;
	for (int phase = 0; phase < 96; phase += 5) {
		const uint64_t on = 4800 + static_cast<uint64_t>(phase);
		const int64_t n = expressive::onset(play(clap_notes(expressive::one_note(on)), on + 4800, 128, rate), on);
		if (n < 0) {
			lo = n, hi = n;
			break;
		}
		lo = std::min(lo, n), hi = std::max(hi, n);
	}
	Instance in;
	const bool reports = in.ext<void>(CLAP_EXT_LATENCY) != nullptr;
	std::printf("latency clap %s %lld %lld\n", reports ? "reported" : "0", static_cast<long long>(lo), static_cast<long long>(hi));
	return 0;
}

} // namespace host_test
