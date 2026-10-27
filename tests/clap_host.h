// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The CLAP host the plugin tests share (clap_host_test.cpp: the modes): loading a .clap, the host,
// an instance, events and timed performances, playing in host blocks, the checks.
#pragma once

#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX // (nsc/load.h includes windows.h)
#endif
#include <clap/clap.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "awva_plugin/voice_controls.h"
#include "awva_sdk/performance.h"
#include "awva_sdk/smf.h"
#include "expressive.h"
#include "nsc/load.h"

namespace host_test {

inline int failures = 0;
inline void check(bool ok, const std::string& what) {
	std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
	std::fflush(stdout);
	failures += !ok;
}
inline float flush(float x) { return x != 0.0f && std::fabs(x) < 1.17549435e-38f ? std::copysign(0.0f, x) : x; }

// --- loading -----------------------------------------------------------------------------------
inline const clap_plugin_entry* load_entry(const char* path) {
#ifdef _WIN32
	HMODULE m = LoadLibraryA(path);
	if (!m) return nullptr;
	return reinterpret_cast<const clap_plugin_entry*>(GetProcAddress(m, "clap_entry"));
#else
	std::string p = path;
	if (p.size() > 5 && p.substr(p.size() - 5) == ".clap") { // a macOS bundle: its binary
		const size_t slash = p.find_last_of('/');
		const std::string name = p.substr(slash + 1, p.size() - slash - 6);
		const std::string inner = p + "/Contents/MacOS/" + name;
		if (FILE* f = std::fopen(inner.c_str(), "rb")) {
			std::fclose(f);
			p = inner;
		}
	}
	void* h = dlopen(p.c_str(), RTLD_NOW | RTLD_LOCAL);
	if (!h) return nullptr;
	return static_cast<const clap_plugin_entry*>(dlsym(h, "clap_entry"));
#endif
}

// --- the host ------------------------------------------------------------------------------------
inline int rescans = 0, callbacks = 0, loaded_presets = 0;
inline const void* host_ext(const clap_host*, const char* id) {
	static const clap_host_params params = {[](const clap_host*, clap_param_rescan_flags) { rescans++; },
	    [](const clap_host*, clap_id, clap_param_clear_flags) {}, [](const clap_host*) {}};
	static const clap_host_preset_load pl = {[](const clap_host*, uint32_t, const char*, const char*, int32_t, const char*) {},
	    [](const clap_host*, uint32_t, const char*, const char*) { loaded_presets++; }};
	if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &params;
	if (!std::strcmp(id, CLAP_EXT_PRESET_LOAD)) return &pl;
	return nullptr;
}
const clap_host kHost = {CLAP_VERSION_INIT, nullptr, "clap_host_test", "Sorbet AI", "", "0.1", host_ext, [](const clap_host*) {},
    [](const clap_host*) {}, [](const clap_host*) { callbacks++; }};

// an event list of one process() call
struct Events {
	std::vector<std::vector<uint8_t>> store;
	std::vector<const clap_event_header*> list;
	void clear() {
		store.clear();
		list.clear();
	}
	template <class E> void add(const E& e) {
		store.emplace_back(sizeof(E));
		std::memcpy(store.back().data(), &e, sizeof(E));
	}
	void seal() {
		list.clear();
		for (auto& s : store) list.push_back(reinterpret_cast<const clap_event_header*>(s.data()));
	}
	clap_input_events in() {
		return {this, [](const clap_input_events* l) { return static_cast<uint32_t>(static_cast<Events*>(l->ctx)->list.size()); },
		    [](const clap_input_events* l, uint32_t i) { return static_cast<Events*>(l->ctx)->list[i]; }};
	}
};
inline int note_ends = 0;
inline std::vector<int32_t> ended_ids;
inline bool out_sorted = true;
inline uint32_t last_out = 0;
const clap_output_events kOut = {nullptr, [](const clap_output_events*, const clap_event_header* h) {
	                                 if (h->type == CLAP_EVENT_NOTE_END) {
		                                 note_ends++;
		                                 ended_ids.push_back(reinterpret_cast<const clap_event_note*>(h)->note_id);
	                                 }
	                                 out_sorted = out_sorted && h->time >= last_out;
	                                 last_out = h->time;
	                                 return true;
                                 }};

inline const clap_plugin_factory* factory = nullptr;
inline const clap_plugin_entry* entry = nullptr;

struct Instance {
	const clap_plugin* p = nullptr;
	bool active = false;
	Instance() {
		p = factory->create_plugin(factory, &kHost, factory->get_plugin_descriptor(factory, 0)->id);
		if (p && !p->init(p)) p = nullptr;
	}
	~Instance() {
		if (!p) return;
		if (active) {
			p->stop_processing(p);
			p->deactivate(p);
		}
		p->destroy(p);
	}
	template <class X> const X* ext(const char* id) const { return static_cast<const X*>(p->get_extension(p, id)); }
	void start(double rate) {
		active = p->activate(p, rate, 1, 4096);
		p->start_processing(p);
	}
	bool set_param(clap_id id, double v) { // (inactive: through flush)
		clap_event_param_value e = {{sizeof e, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0}, id, nullptr, -1, -1, -1, -1, v};
		Events ev;
		ev.add(e);
		ev.seal();
		clap_input_events in = ev.in();
		const auto* params = ext<clap_plugin_params>(CLAP_EXT_PARAMS);
		params->flush(p, &in, &kOut);
		return true;
	}
	void process(uint64_t at, uint32_t n, Events& ev, float* l, float* r) {
		ev.seal();
		clap_input_events ie = ev.in();
		float* chans[2] = {l, r};
		clap_audio_buffer ob = {chans, nullptr, 2, 0, 0};
		clap_process pr = {static_cast<int64_t>(at), n, nullptr, nullptr, &ob, 0, 1, &ie, &kOut};
		last_out = 0;
		p->process(p, &pr);
	}
};
constexpr clap_id kMonoId = 1;

// timed events of a whole performance (any CLAP event type), played in host blocks
struct Timed {
	uint64_t frame;
	std::vector<uint8_t> bytes; // the event, its header's time set per block
};
template <class E> void push(std::vector<Timed>& v, uint64_t frame, const E& e) {
	Timed t{frame, std::vector<uint8_t>(sizeof(E))};
	std::memcpy(t.bytes.data(), &e, sizeof(E));
	v.push_back(std::move(t));
}

// host blocks: n > 0 that many frames, 0 random 1..1024 (seeded)
inline std::vector<float> play(const std::vector<Timed>& events, uint64_t frames, int block, double rate = 48000, bool mono = false,
    const awva_voice_source* source = nullptr, uint32_t seed = 1) {
	Instance in;
	if (!in.p) return {};
	if (mono) in.set_param(kMonoId, 1.0);
	if (source) {
		const auto* vx = in.ext<awva_plugin_voice_controls>(AWVA_EXT_VOICE_CONTROLS);
		if (!vx || !vx->set_source(in.p, source)) return {};
	}
	in.start(rate);
	std::mt19937 rng(seed);
	std::vector<float> out(frames * 2);
	std::vector<float> l(4096), r(4096);
	size_t k = 0;
	uint64_t at = 0;
	Events ev;
	while (at < frames) {
		const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(block > 0 ? static_cast<uint64_t>(block) : 1 + rng() % 1024, frames - at));
		ev.clear();
		for (; k < events.size() && events[k].frame < at + n; k++) {
			ev.store.push_back(events[k].bytes);
			reinterpret_cast<clap_event_header*>(ev.store.back().data())->time = static_cast<uint32_t>(events[k].frame - at);
		}
		in.process(at, n, ev, l.data(), r.data());
		for (uint32_t i = 0; i < n; i++) {
			out[(at + i) * 2] = l[i];
			out[(at + i) * 2 + 1] = r[i];
		}
		at += n;
	}
	return out;
}
inline std::string blocks_name(int b) { return b > 0 ? std::to_string(b) : std::string("random 1..1024"); }

// a plugin's stereo output against a mono reference, flushed: frames that differ
inline size_t differ(const std::vector<float>& out, const std::vector<float>& ref, size_t* first, double* peak) {
	size_t bad = 0;
	*peak = 0;
	for (size_t i = 0; i < ref.size() && 2 * i + 1 < out.size(); i++) {
		const float want = flush(ref[i]);
		*peak = std::fmax(*peak, std::fabs(want));
		if (std::memcmp(&out[2 * i], &want, 4) != 0 || std::memcmp(&out[2 * i + 1], &want, 4) != 0) {
			if (!bad) *first = i;
			bad++;
		}
	}
	if (out.size() != 2 * ref.size()) bad += ref.size();
	return bad;
}

inline uint32_t plugin_voices() {
	Instance in;
	const auto* vi = in.ext<clap_plugin_voice_info>(CLAP_EXT_VOICE_INFO);
	clap_voice_info v{};
	return vi && vi->get(in.p, &v) ? v.voice_capacity : 0;
}

inline bool same_bits(const std::vector<float>& a, const std::vector<float>& b, size_t* first = nullptr) {
	if (a.size() != b.size()) return false;
	for (size_t i = 0; i < a.size(); i++)
		if (std::memcmp(&a[i], &b[i], 4) != 0) {
			if (first) *first = i;
			return false;
		}
	return true;
}

struct Buffer {
	std::string data;
	size_t at = 0;
};
inline int64_t buf_write(const clap_ostream* s, const void* b, uint64_t n) {
	static_cast<Buffer*>(s->ctx)->data.append(static_cast<const char*>(b), n);
	return static_cast<int64_t>(n);
}
inline int64_t buf_read(const clap_istream* s, void* b, uint64_t n) {
	Buffer& buf = *static_cast<Buffer*>(s->ctx);
	const size_t k = std::min<size_t>(n, buf.data.size() - buf.at);
	std::memcpy(b, buf.data.data() + buf.at, k);
	buf.at += k;
	return static_cast<int64_t>(k);
}

// as CLAP notes with expressions (the channel messages, of mpe, as MIDI)
inline std::vector<Timed> clap_notes(const std::vector<expressive::Event>& perf) {
	using namespace expressive;
	std::vector<Timed> v;
	for (const Event& e : perf) {
		const int16_t ch = e.channel, key = e.key;
		switch (e.kind) {
		case kNoteOn:
		case kNoteOff: {
			clap_event_note x = {{sizeof x, 0, CLAP_CORE_EVENT_SPACE_ID, static_cast<uint16_t>(e.kind == kNoteOn ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF), 0},
			    e.id, 0, ch, key, velocity(e)};
			push(v, e.frame, x);
			break;
		}
		case kPressure:
		case kTuning:
		case kBrightness: {
			const clap_note_expression id = e.kind == kPressure ? CLAP_NOTE_EXPRESSION_PRESSURE
			    : e.kind == kTuning                         ? CLAP_NOTE_EXPRESSION_TUNING
			                                                : CLAP_NOTE_EXPRESSION_BRIGHTNESS;
			clap_event_note_expression x = {{sizeof x, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_EXPRESSION, 0}, id, e.id, 0, ch, key,
			    e.kind == kTuning ? semitones(e) : unit(e)};
			push(v, e.frame, x);
			break;
		}
		default: {
			clap_event_midi x = {{sizeof x, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0}, 0, {}};
			(void)bytes(e, x.data);
			push(v, e.frame, x);
			break;
		}
		}
	}
	return v;
}

// the modes (clap_host_test.cpp's header)
int perf_test(const char* ref_path);                                 // clap_host_proofs.cpp
int midifile_test(const char* mid, const char* ref_path, int rate);  // clap_host_proofs.cpp
int ump_test(const char* lib_path);                                  // clap_host_proofs.cpp
int expressive_test(const char* lib_path, const std::string& dir, int rate); // clap_host_proofs.cpp
int notes_test();                                                    // clap_host_notes.cpp
int many_test(const char* lib_path);                                 // clap_host_notes.cpp
int bench_test(const char* lib_path);                                // clap_host_bench.cpp
int info_test(const char* lib_path, const char* preset_dir);         // clap_host_info.cpp
int stress_test(double seconds, uint32_t seed);                      // clap_host_stress.cpp

} // namespace host_test
