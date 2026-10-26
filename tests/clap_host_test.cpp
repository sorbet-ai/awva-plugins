// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// A minimal CLAP host for the awva plugins: loads a plugin (a .clap), plays it, checks what it
// gives. Never opens an audio device: it only calls process().
//
//   clap_host_test info PLUGIN.clap COMPONENT_LIB [PRESET_DIR]
//     The plugin is its component's: id, name and summary, the parameters (ids, ranges, kinds),
//     the presets (preset-discovery, preset-load), the voices; the ports; a state round trip.
//   clap_host_test voices PLUGIN.clap
//     Prints the plugin's voice count (for the reference render's --nv).
//   clap_host_test perf PLUGIN.clap REF.f32
//     The SDK's default scripted performance through the plugin (its voice-controls test
//     extension: the controls of every block, as awva_render gives them to the engine), in host
//     blocks of 256, 1, 33 and random sizes: the output must be awva_render's REF (mono, unscaled)
//     x 0.25, subnormals flushed, bit for bit, on both channels.
//   clap_host_test midifile PLUGIN.clap FILE.mid REF.f32 [RATE]
//     A Standard MIDI File in the MIDI dialect, each event at its frame (its time rounded to the
//     nearest frame, as awva_render places it), in host blocks of 256, 1, 33 and random sizes:
//     the output must be awva_render --midi's REF (mono, unscaled) x 0.25, bit for bit.
//   clap_host_test ump PLUGIN.clap COMPONENT_LIB
//     A MIDI 2.0 performance (MIDI2 dialect) and a MIDI 1.0 one (MIDI dialect), polyphonic and
//     legato, in host blocks of 256, 1, 33 and random sizes at 48 and 44.1 kHz, against the engine's
//     own component library given the same UMP at the same frames, block by block: bit for bit
//     (an event after its block was rendered goes to the next as a late event: plugin.cpp).
//   clap_host_test notes PLUGIN.clap
//     (and:) 20 overlapping CLAP notes on 20 keys, each with its own brightness: every one sounds
//     (without it the output differs), every one's brightness is its own (changing it changes the
//     output, for an engine that reads y), and each gets exactly one NOTE_END, with its id.
//     A CLAP-dialect performance (notes with ids, pressure, tuning, brightness expressions):
//     the same as its MIDI 2.0 translation, bit for bit; the same for every host block size; it
//     sounds, the expressions change it, and a NOTE_END comes for every note.
#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX // (nsc/load.h includes windows.h)
#endif
#include <clap/clap.h>

#include <algorithm>
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
#include "nsc/load.h"

namespace {

int failures = 0;
void check(bool ok, const std::string& what) {
	std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
	std::fflush(stdout);
	failures += !ok;
}
float flush(float x) { return x != 0.0f && std::fabs(x) < 1.17549435e-38f ? std::copysign(0.0f, x) : x; }

// --- loading -----------------------------------------------------------------------------------
const clap_plugin_entry* load_entry(const char* path) {
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
int rescans = 0, callbacks = 0, loaded_presets = 0;
const void* host_ext(const clap_host*, const char* id) {
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
int note_ends = 0;
std::vector<int32_t> ended_ids;
bool out_sorted = true;
uint32_t last_out = 0;
const clap_output_events kOut = {nullptr, [](const clap_output_events*, const clap_event_header* h) {
	                                 if (h->type == CLAP_EVENT_NOTE_END) {
		                                 note_ends++;
		                                 ended_ids.push_back(reinterpret_cast<const clap_event_note*>(h)->note_id);
	                                 }
	                                 out_sorted = out_sorted && h->time >= last_out;
	                                 last_out = h->time;
	                                 return true;
                                 }};

const clap_plugin_factory* factory = nullptr;
const clap_plugin_entry* entry = nullptr;

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
constexpr clap_id kLegatoId = 1;

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
std::vector<float> play(const std::vector<Timed>& events, uint64_t frames, int block, double rate = 48000, bool legato = false,
    const awva_voice_source* source = nullptr, uint32_t seed = 1) {
	Instance in;
	if (!in.p) return {};
	if (legato) in.set_param(kLegatoId, 1.0);
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
std::string blocks_name(int b) { return b > 0 ? std::to_string(b) : std::string("random 1..1024"); }

// a plugin's stereo output against a mono reference x 0.25, flushed: frames that differ
size_t differ(const std::vector<float>& out, const std::vector<float>& ref, size_t* first, double* peak) {
	size_t bad = 0;
	*peak = 0;
	for (size_t i = 0; i < ref.size() && 2 * i + 1 < out.size(); i++) {
		const float want = flush(ref[i] * 0.25f);
		*peak = std::fmax(*peak, std::fabs(want));
		if (std::memcmp(&out[2 * i], &want, 4) != 0 || std::memcmp(&out[2 * i + 1], &want, 4) != 0) {
			if (!bad) *first = i;
			bad++;
		}
	}
	if (out.size() != 2 * ref.size()) bad += ref.size();
	return bad;
}

uint32_t plugin_voices() {
	Instance in;
	const auto* vi = in.ext<clap_plugin_voice_info>(CLAP_EXT_VOICE_INFO);
	clap_voice_info v{};
	return vi && vi->get(in.p, &v) ? v.voice_capacity : 0;
}

// --- perf: the default scripted performance against awva_render ---------------------------------------
struct PerfSource {
	std::unique_ptr<awva::sdk::Player> player;
	std::vector<awva::VoiceControls> vc;
	awva::GlobalControls g = awva::globals_at_rest;
	// as awva_render: the player's key signals, then track_key, then the engine
	static void block(void* ctx, uint64_t b, nsc_voice* out, int32_t nv, nsc_globals* g) {
		auto& s = *static_cast<PerfSource*>(ctx);
		s.player->block(static_cast<int>(b), s.vc.data(), s.g);
		for (awva::VoiceControls& c : s.vc) awva::track_key(c);
		std::memcpy(out, s.vc.data(), sizeof(nsc_voice) * static_cast<size_t>(std::min<int32_t>(nv, static_cast<int32_t>(s.vc.size()))));
		std::memcpy(g, &s.g, sizeof *g);
	}
};
static_assert(sizeof(nsc_voice) == sizeof(awva::VoiceControls) && sizeof(nsc_globals) == sizeof(awva::GlobalControls));

int perf_test(const char* ref_path) {
	std::vector<float> ref;
	if (FILE* rf = std::fopen(ref_path, "rb")) {
		float buf[4096];
		size_t got;
		while ((got = std::fread(buf, 4, 4096, rf)) > 0) ref.insert(ref.end(), buf, buf + got);
		std::fclose(rf);
	}
	const int nv = static_cast<int>(plugin_voices());
	for (int block : {256, 1, 33, 0}) {
		PerfSource s;
		std::string err;
		s.player = awva::sdk::scripted_player("default", nv, 48000, &err);
		if (!s.player) {
			std::fprintf(stderr, "%s\n", err.c_str());
			return 2;
		}
		s.vc.assign(static_cast<size_t>(nv), awva::voice_at_rest);
		const awva_voice_source src = {&s, PerfSource::block};
		const std::vector<float> out = play({}, ref.size(), block, 48000, false, &src);
		size_t first = 0;
		double peak = 0;
		const size_t bad = differ(out, ref, &first, &peak);
		char what[256];
		std::snprintf(what, sizeof what, "default performance, %d voices, host blocks %s: %zu frames (peak %.3f) bit for bit awva_render's x 0.25",
		    nv, blocks_name(block).c_str(), ref.size(), peak);
		check(!ref.empty() && bad == 0 && peak > 0.001, what);
		if (bad) std::printf("     %zu frames differ, the first at %zu\n", bad, first);
	}
	return 0;
}

// --- midifile: a MIDI file against awva_render --midi -------------------------------------------------
int midifile_test(const char* mid, const char* ref_path, int rate) {
	const awva::sdk::MidiFile m = awva::sdk::read_midi_file(mid);
	if (!m.error.empty()) {
		std::fprintf(stderr, "%s\n", m.error.c_str());
		return 2;
	}
	std::vector<float> ref;
	if (FILE* rf = std::fopen(ref_path, "rb")) {
		float buf[4096];
		size_t got;
		while ((got = std::fread(buf, 4, 4096, rf)) > 0) ref.insert(ref.end(), buf, buf + got);
		std::fclose(rf);
	}
	std::vector<Timed> events;
	for (const awva::sdk::MidiEvent& e : m.events) {
		const uint64_t frame = e.t <= 0 ? 0u : static_cast<uint64_t>(std::llround(e.t * rate)); // (awva_render's frame_of)
		clap_event_midi x = {{sizeof x, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0}, 0,
		    {e.bytes[0], e.len > 1 ? e.bytes[1] : uint8_t{0}, e.len > 2 ? e.bytes[2] : uint8_t{0}}};
		push(events, frame, x);
	}
	for (const int block : {256, 1, 33, 0}) {
		const std::vector<float> out = play(events, ref.size(), block, rate);
		size_t first = 0;
		double peak = 0;
		const size_t bad = differ(out, ref, &first, &peak);
		char what[256];
		std::snprintf(what, sizeof what, "MIDI file (%zu events), %d Hz, host blocks %s: %zu frames (peak %.3f) bit for bit awva_render --midi's x 0.25",
		    m.events.size(), rate, blocks_name(block).c_str(), ref.size(), peak);
		check(!ref.empty() && bad == 0 && peak > 0.001, what);
		if (bad) std::printf("     %zu frames differ, the first at %zu\n", bad, first);
	}
	return 0;
}

// --- ump: MIDI 2.0 and MIDI 1.0 against the component library ---------------------------------------
struct Ev {
	uint64_t frame;
	uint32_t w0, w1;
};
// notes with velocities, pressure, bends, CC74, releases (the component tests' performance), and
// four more notes that overlap, for legato
std::vector<Ev> ump_performance(int rate) {
	std::vector<Ev> v;
	const int keys[6] = {60, 64, 67, 72, 62, 65};
	const double s = rate / 48000.0;
	for (int n = 0; n < 6; n++) {
		const uint8_t ch = static_cast<uint8_t>(1 + n);
		const uint64_t on = static_cast<uint64_t>((2400.0 * n + 13.0 * n) * s);
		v.push_back({on, 0x40900000u | ch << 16 | static_cast<uint32_t>(keys[n]) << 8, (0x6000u + 0x1000u * static_cast<uint32_t>(n)) << 16});
		for (int k = 1; k < 40; k++) {
			const uint64_t f = on + static_cast<uint64_t>(480.0 * k * s) + 7u;
			v.push_back({f, 0x40d00000u | ch << 16, static_cast<uint32_t>(0x20000000u + 0x02000000u * static_cast<uint32_t>(k % 20))});
			v.push_back({f + 3, 0x40e00000u | ch << 16, static_cast<uint32_t>(0x80000000u + 0x00400000u * static_cast<uint32_t>(k))});
			v.push_back({f + 5, 0x40b04a00u | ch << 16, static_cast<uint32_t>(0x10000000u * static_cast<uint32_t>(k % 16))});
		}
		v.push_back({on + static_cast<uint64_t>(24000.0 * s), 0x40800000u | ch << 16 | static_cast<uint32_t>(keys[n]) << 8, 0x40000000u});
	}
	std::stable_sort(v.begin(), v.end(), [](const Ev& a, const Ev& b) { return a.frame < b.frame; });
	return v;
}
// the same as MIDI 1.0 (in UMP, type 2): 7-bit velocity, channel pressure, pitch bend, CC74
std::vector<Ev> midi1_performance(int rate) {
	std::vector<Ev> v = ump_performance(rate);
	for (Ev& e : v) {
		const uint32_t status = (e.w0 >> 16) & 0xffu, d1 = (e.w0 >> 8) & 0x7fu;
		uint32_t a = d1, b = 0;
		switch (status & 0xf0u) {
		case 0x90:
		case 0x80: b = std::max<uint32_t>(e.w1 >> 25, (status & 0xf0u) == 0x90 ? 1u : 0u); break;
		case 0xd0: a = e.w1 >> 25; break;
		case 0xe0: a = (e.w1 >> 18) & 0x7fu, b = e.w1 >> 25; break;
		case 0xb0: b = e.w1 >> 25; break;
		default: break;
		}
		e.w0 = 0x20000000u | status << 16 | a << 8 | b;
		e.w1 = 0;
	}
	return v;
}
std::vector<Timed> as_clap(const std::vector<Ev>& perf, bool midi1) {
	std::vector<Timed> v;
	for (const Ev& e : perf) {
		if (midi1) {
			clap_event_midi x = {{sizeof x, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0}, 0,
			    {static_cast<uint8_t>(e.w0 >> 16), static_cast<uint8_t>(e.w0 >> 8), static_cast<uint8_t>(e.w0)}};
			push(v, e.frame, x);
		} else {
			clap_event_midi2 x = {{sizeof x, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI2, 0}, 0, {e.w0, e.w1, 0, 0}};
			push(v, e.frame, x);
		}
	}
	return v;
}
// the component library, block by block, each event at its frame
std::vector<float> render_component(const nsc_component& c, const std::vector<Ev>& perf, uint64_t frames, int rate, int nv, bool legato) {
	nsc_instance in;
	if (nsc_instance_new(&c, &in) != 0) return {};
	if (c.init(in.state, in.fast, in.large, rate, nv) != 0) return {};
	if (legato) (void)c.mode(in.state, NSC_MODE_LEGATO, 1);
	std::vector<float> out;
	size_t k = 0;
	float buf[2][NSC_BLOCK];
	for (uint64_t b0 = 0; b0 < frames; b0 += NSC_BLOCK) {
		std::vector<nsc_event> ev;
		for (; k < perf.size() && perf[k].frame < b0 + NSC_BLOCK; k++)
			ev.push_back({static_cast<int32_t>(perf[k].frame - b0), -1, {perf[k].w0, perf[k].w1, 0, 0}});
		float* o[2] = {buf[0], buf[1]};
		c.process(in.state, ev.data(), static_cast<int32_t>(ev.size()), o);
		out.insert(out.end(), buf[0], buf[0] + NSC_BLOCK);
	}
	nsc_instance_free(&in);
	out.resize(frames);
	return out;
}

int ump_test(const char* lib_path) {
	nsc_library lib;
	if (nsc_load(lib_path, &lib) != 0) {
		std::fprintf(stderr, "clap_host_test: cannot load the component %s\n", lib_path);
		return 2;
	}
	const nsc_component& c = *lib.c;
	const int nv = static_cast<int>(plugin_voices());
	for (const int rate : {48000, 44100}) {
		const uint64_t frames = static_cast<uint64_t>(rate) * 3;
		for (const bool midi1 : {false, true}) {
			const std::vector<Ev> perf = midi1 ? midi1_performance(rate) : ump_performance(rate);
			const std::vector<Timed> events = as_clap(perf, midi1);
			for (const bool legato : {false, true}) {
				const std::vector<float> ref = render_component(c, perf, frames, rate, nv, legato);
				for (const int block : {256, 1, 33, 0}) {
					const std::vector<float> out = play(events, frames, block, rate, legato);
					size_t first = 0;
					double peak = 0;
					const size_t bad = differ(out, ref, &first, &peak);
					char what[256];
					std::snprintf(what, sizeof what, "%s dialect, %s, %d Hz, host blocks %s: %llu frames (peak %.3f) bit for bit the component's x 0.25",
					    midi1 ? "MIDI (1.0)" : "MIDI2 (MIDI 2.0)", legato ? "legato" : "polyphonic", rate, blocks_name(block).c_str(),
					    static_cast<unsigned long long>(frames), peak);
					check(bad == 0 && peak > 0.001, what);
					if (bad) std::printf("     %zu frames differ, the first at %zu\n", bad, first);
				}
			}
		}
	}
	nsc_unload(&lib);
	return 0;
}

// --- notes: the CLAP dialect ---------------------------------------------------------------------------
std::vector<Timed> clap_performance(bool midi2, bool expressions) {
	std::vector<Timed> v;
	const int keys[3] = {55, 59, 62};
	for (int n = 0; n < 3; n++) {
		const uint64_t on = 4800 + 21000 * n + 37 * n, off = on + 60000;
		const uint8_t ch = 1; // the plugin's one channel for CLAP notes: MIDI channel 2
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
						if (x == 2) w0 = 0x40000000u | (0x10u | ch) << 16 | static_cast<uint32_t>(keys[n]) << 8 | 4, w1 = static_cast<uint32_t>(c);
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

bool same_bits(const std::vector<float>& a, const std::vector<float>& b, size_t* first = nullptr) {
	if (a.size() != b.size()) return false;
	for (size_t i = 0; i < a.size(); i++)
		if (std::memcmp(&a[i], &b[i], 4) != 0) {
			if (first) *first = i;
			return false;
		}
	return true;
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
	const bool reads_y = (lib.c->desc->inputs & (1u << 3)) != 0; // (engine.h in_tilt_y)
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

// --- info ------------------------------------------------------------------------------------------------
struct Buffer {
	std::string data;
	size_t at = 0;
};
int64_t buf_write(const clap_ostream* s, const void* b, uint64_t n) {
	static_cast<Buffer*>(s->ctx)->data.append(static_cast<const char*>(b), n);
	return static_cast<int64_t>(n);
}
int64_t buf_read(const clap_istream* s, void* b, uint64_t n) {
	Buffer& buf = *static_cast<Buffer*>(s->ctx);
	const size_t k = std::min<size_t>(n, buf.data.size() - buf.at);
	std::memcpy(b, buf.data.data() + buf.at, k);
	buf.at += k;
	return static_cast<int64_t>(k);
}

int list_presets(std::vector<std::string>* names) {
	const auto* pf = static_cast<const clap_preset_discovery_factory*>(entry->get_factory(CLAP_PRESET_DISCOVERY_FACTORY_ID));
	if (!pf || pf->count(pf) == 0) return 0;
	const clap_preset_discovery_indexer ix = {CLAP_VERSION_INIT, "clap_host_test", "", "", "", nullptr,
	    [](const clap_preset_discovery_indexer*, const clap_preset_discovery_filetype*) { return true; },
	    [](const clap_preset_discovery_indexer*, const clap_preset_discovery_location* l) {
		    return l->kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN && l->location == nullptr;
	    },
	    [](const clap_preset_discovery_indexer*, const clap_preset_discovery_soundpack*) { return true; },
	    [](const clap_preset_discovery_indexer*, const char*) -> const void* { return nullptr; }};
	const clap_preset_discovery_provider* prov = pf->create(pf, &ix, pf->get_descriptor(pf, 0)->id);
	clap_preset_discovery_metadata_receiver rcv = {names, [](const clap_preset_discovery_metadata_receiver*, int32_t, const char*) {},
	    [](const clap_preset_discovery_metadata_receiver* r, const char* name, const char*) {
		    static_cast<std::vector<std::string>*>(r->receiver_data)->push_back(name ? name : "");
		    return true;
	    },
	    [](const clap_preset_discovery_metadata_receiver*, const clap_universal_plugin_id*) {},
	    [](const clap_preset_discovery_metadata_receiver*, const char*) {},
	    [](const clap_preset_discovery_metadata_receiver*, uint32_t) {},
	    [](const clap_preset_discovery_metadata_receiver*, const char*) {},
	    [](const clap_preset_discovery_metadata_receiver*, const char*) {},
	    [](const clap_preset_discovery_metadata_receiver*, clap_timestamp, clap_timestamp) {},
	    [](const clap_preset_discovery_metadata_receiver*, const char*) {},
	    [](const clap_preset_discovery_metadata_receiver*, const char*, const char*) {}};
	if (prov && prov->init(prov)) prov->get_metadata(prov, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, &rcv);
	if (prov) prov->destroy(prov);
	return static_cast<int>(names->size());
}

// a short phrase (three notes with pressure and brightness), its output from an instance set up
// by `setup` (inactive) at 48 kHz
std::vector<float> phrase(const std::function<void(Instance&)>& setup, Buffer* state_out = nullptr, const Buffer* state_in = nullptr) {
	Instance in;
	if (state_in) {
		Buffer b{state_in->data};
		const clap_istream is = {&b, buf_read};
		if (!in.ext<clap_plugin_state>(CLAP_EXT_STATE)->load(in.p, &is)) return {};
	}
	if (setup) setup(in);
	// a second of silence first, as a project has before its first note: a preset and the values
	// set after it settle (an engine glides into a preset's values), then the state is saved
	in.start(48000);
	{
		std::vector<float> l(256), r(256);
		for (int k = 0; k < 48000 / 256; k++) {
			Events none;
			in.process(0, 256, none, l.data(), r.data());
		}
	}
	if (state_out) {
		const clap_ostream os = {state_out, buf_write};
		if (!in.ext<clap_plugin_state>(CLAP_EXT_STATE)->save(in.p, &os)) return {};
	}
	std::vector<Timed> ev;
	const int keys[3] = {57, 60, 64};
	for (int n = 0; n < 3; n++) {
		const uint64_t on = 960 + 7200 * static_cast<uint64_t>(n);
		clap_event_note x = {{sizeof x, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_ON, 0}, 300 + n, 0, 0, static_cast<int16_t>(keys[n]), 0.7};
		push(ev, on, x);
		clap_event_note_expression pr = {{sizeof pr, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_EXPRESSION, 0}, CLAP_NOTE_EXPRESSION_PRESSURE,
		    300 + n, 0, 0, static_cast<int16_t>(keys[n]), 0.6};
		push(ev, on, pr);
		clap_event_note y = {{sizeof y, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_OFF, 0}, 300 + n, 0, 0, static_cast<int16_t>(keys[n]), 0.0};
		push(ev, on + 24000, y);
	}
	std::vector<float> out(48000 * 2 * 2);
	std::vector<float> l(256), r(256);
	size_t k = 0;
	for (uint64_t at = 0; at < 48000 * 2; at += 256) {
		Events e;
		for (; k < ev.size() && ev[k].frame < at + 256; k++) {
			e.store.push_back(ev[k].bytes);
			reinterpret_cast<clap_event_header*>(e.store.back().data())->time = static_cast<uint32_t>(ev[k].frame - at);
		}
		in.process(at, 256, e, l.data(), r.data());
		for (int i = 0; i < 256; i++) {
			out[(at + i) * 2] = l[i];
			out[(at + i) * 2 + 1] = r[i];
		}
	}
	return out;
}

// random values for every parameter (inactive: through flush), legato among them
void randomize(Instance& in, uint32_t seed) {
	const auto* pa = in.ext<clap_plugin_params>(CLAP_EXT_PARAMS);
	std::mt19937 rng(seed);
	for (uint32_t i = 0; i < pa->count(in.p); i++) {
		clap_param_info pi;
		pa->get_info(in.p, i, &pi);
		in.set_param(pi.id, pi.min_value + (pi.max_value - pi.min_value) * (rng() % 1000) / 999.0);
	}
}

// a project's state restores the sound exactly: the same phrase from the saved instance and from a
// new one that loaded its state, bit for bit, and the restored one saves the same text again
void exact_restore(const nsc_desc& nd) {
	struct Case {
		const char* name;
		std::function<void(Instance&)> setup;
	};
	std::vector<Case> cases = {{"defaults", nullptr}, {"random parameters", [](Instance& in) { randomize(in, 11); }}};
	if (nd.num_presets > 1)
		cases.push_back({"the last factory preset", [n = nd.num_presets](Instance& in) {
			                 const auto* p = in.ext<clap_plugin_preset_load>(CLAP_EXT_PRESET_LOAD);
			                 (void)p->from_location(in.p, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, std::to_string(n - 1).c_str());
		                 }});
	if (nd.num_presets > 1)
		cases.push_back({"the last factory preset, then random parameters", [n = nd.num_presets](Instance& in) {
			                 const auto* p = in.ext<clap_plugin_preset_load>(CLAP_EXT_PRESET_LOAD);
			                 (void)p->from_location(in.p, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, std::to_string(n - 1).c_str());
			                 randomize(in, 12);
		                 }});
	for (const Case& c : cases) {
		Buffer saved, again;
		const std::vector<float> a = phrase(c.setup, &saved);
		const std::vector<float> b = phrase(nullptr, &again, &saved);
		double peak = 0;
		for (float x : a) peak = std::fmax(peak, std::fabs(x));
		if (!same_bits(a, b)) {
			size_t first = 0, n = 0;
			double md = 0;
			for (size_t i = 0; i < a.size() && i < b.size(); i++)
				if (std::memcmp(&a[i], &b[i], 4) != 0) {
					if (!n++) first = i;
					md = std::fmax(md, std::fabs(static_cast<double>(a[i]) - b[i]));
				}
			std::printf("     the sound differs: %zu samples, the first at frame %zu, at most %g\n", n, first / 2, md);
		}
		if (saved.data != again.data) std::printf("     the state text differs:\n%s\n---\n%s\n", saved.data.c_str(), again.data.c_str());
		check(!a.empty() && same_bits(a, b) && saved.data == again.data && peak > 0.0,
		    std::string("a project restores exactly (") + c.name + "): the same sound, bit for bit (peak " + std::to_string(peak) +
		        "), and the same state text again (" + std::to_string(saved.data.size()) + " bytes)");
	}
}

// the engine's preset files (awva's presets/<engine>/NN-<name>.preset.json, in name order) through
// preset-load: each gives the values its factory preset gives
void preset_files(const nsc_desc& nd, const char* dir) {
	std::vector<std::string> files;
	std::error_code ec;
	for (const auto& f : std::filesystem::directory_iterator(dir, ec))
		if (f.path().string().size() > 12 && f.path().string().rfind(".preset.json") == f.path().string().size() - 12) files.push_back(f.path().string());
	std::sort(files.begin(), files.end());
	if (nd.num_presets == 0) {
		check(files.empty(), "no preset files, no factory presets");
		return;
	}
	int same = 0;
	for (size_t k = 0; k < files.size(); k++) {
		Instance a, b;
		const auto* pa = a.ext<clap_plugin_preset_load>(CLAP_EXT_PRESET_LOAD);
		const auto* pb = b.ext<clap_plugin_preset_load>(CLAP_EXT_PRESET_LOAD);
		const bool la = pa->from_location(a.p, CLAP_PRESET_DISCOVERY_LOCATION_FILE, files[k].c_str(), nullptr);
		const bool lb = pb->from_location(b.p, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, std::to_string(k).c_str());
		const auto* qa = a.ext<clap_plugin_params>(CLAP_EXT_PARAMS);
		const auto* qb = b.ext<clap_plugin_params>(CLAP_EXT_PARAMS);
		bool eq = la && lb;
		for (uint32_t i = 0; eq && i < qa->count(a.p); i++) {
			clap_param_info pi;
			qa->get_info(a.p, i, &pi);
			double va = 0, vb = 0;
			qa->get_value(a.p, pi.id, &va);
			qb->get_value(b.p, pi.id, &vb);
			eq = std::memcmp(&va, &vb, sizeof va) == 0;
		}
		same += eq;
	}
	check(static_cast<int>(files.size()) == nd.num_presets && same == nd.num_presets,
	    std::to_string(files.size()) + " preset files through preset-load: each the values of its factory preset, bit for bit (" + std::to_string(same) + ")");
}

int info_test(const char* lib_path, const char* preset_dir) {
	nsc_library lib;
	if (nsc_load(lib_path, &lib) != 0) {
		std::fprintf(stderr, "clap_host_test: cannot load the component %s\n", lib_path);
		return 2;
	}
	const nsc_desc& nd = *lib.c->desc;
	const clap_plugin_descriptor* d = factory->get_plugin_descriptor(factory, 0);
	std::printf("plugin %s: \"%s\" by %s, %s; %s\n", d->id, d->name, d->vendor, d->version, d->description);
	check(factory->get_plugin_count(factory) == 1, "one plugin in the factory");
	check(std::string(d->id) == std::string("com.sorbet-ai.awva.") + nd.id && std::string(d->id) == nd.uid,
	    std::string("id com.sorbet-ai.awva.") + nd.id + ", the component's uid");
	check(std::string(d->name) == nd.name && std::string(d->vendor) == "Sorbet AI", "name from the component (\"" + std::string(nd.name) + "\"), vendor Sorbet AI");
	check(std::string(d->description).rfind(nd.summary, 0) == 0, "description: the component's summary");
	const std::string version = nd.version ? nd.version : "";
	check(version.size() >= 7 && version != "unknown" && std::string(d->description).find("(awva " + version + ")") != std::string::npos,
	    "the component's version, the awva commit (" + version + "), in the description");
	Instance in;
	check(in.p != nullptr, "created and initialised");
	if (!in.p) return 1;
	const auto* ap = in.ext<clap_plugin_audio_ports>(CLAP_EXT_AUDIO_PORTS);
	clap_audio_port_info ai;
	check(ap && ap->count(in.p, false) == 1 && ap->get(in.p, 0, false, &ai) && ai.channel_count == 2 && ap->count(in.p, true) == 0,
	    "one stereo output, no inputs");
	const auto* np = in.ext<clap_plugin_note_ports>(CLAP_EXT_NOTE_PORTS);
	clap_note_port_info ni;
	const uint32_t all = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI | CLAP_NOTE_DIALECT_MIDI_MPE | CLAP_NOTE_DIALECT_MIDI2;
	check(np && np->count(in.p, true) == 1 && np->get(in.p, 0, true, &ni) && ni.supported_dialects == all, "a note input: CLAP, MIDI, MIDI-MPE, MIDI2");
	{ // the sample rates: the component's min_rate .. max_rate, no other
		Instance r;
		const bool lo = r.p->activate(r.p, nd.min_rate, 1, 4096);
		if (lo) r.p->deactivate(r.p);
		const bool hi = r.p->activate(r.p, nd.max_rate, 1, 4096);
		if (hi) r.p->deactivate(r.p);
		const bool below = r.p->activate(r.p, nd.min_rate - 1, 1, 4096);
		if (below) r.p->deactivate(r.p);
		const bool above = r.p->activate(r.p, nd.max_rate + 1, 1, 4096);
		if (above) r.p->deactivate(r.p);
		check(lo && hi && !below && !above, "activates at " + std::to_string(nd.min_rate) + " .. " + std::to_string(nd.max_rate) + " Hz, the component's rates, and refuses others");
	}
	const uint32_t want_voices = static_cast<uint32_t>(std::min(10, nd.max_voices));
	check(plugin_voices() == want_voices, "voice info: " + std::to_string(want_voices) + " voices");
	const auto* params = in.ext<clap_plugin_params>(CLAP_EXT_PARAMS);
	const uint32_t count = params ? params->count(in.p) : 0;
	std::vector<clap_id> ids;
	bool match = count == static_cast<uint32_t>(nd.num_params) + 1;
	for (uint32_t i = 0; i < count; i++) {
		clap_param_info pi;
		params->get_info(in.p, i, &pi);
		ids.push_back(pi.id);
		if (i < static_cast<uint32_t>(nd.num_params)) {
			const nsc_param& q = nd.params[i];
			double v = 0; // (the value after init: the declared default)
			params->get_value(in.p, pi.id, &v);
			match = match && pi.default_value == static_cast<double>(q.def) && static_cast<float>(v) == q.def;
			match = match && pi.id == q.id && std::string(pi.name) == q.name && pi.min_value == q.min && pi.max_value == q.max &&
			    ((pi.flags & CLAP_PARAM_IS_STEPPED) != 0) == (q.kind != NSC_PARAM_CONTINUOUS) && pi.id != kLegatoId && pi.id != CLAP_INVALID_ID;
		} else {
			match = match && pi.id == kLegatoId && std::string(pi.name) == "legato";
		}
	}
	bool unique = true;
	for (size_t i = 0; i < ids.size(); i++)
		for (size_t j = 0; j < i; j++) unique = unique && ids[i] != ids[j];
	check(match && unique, std::to_string(nd.num_params) + " parameters as the component's (ids, names, ranges, kinds, defaults: def, the value after init), and legato; ids unique");
	std::vector<std::string> presets;
	const int listed = list_presets(&presets);
	bool names = listed == nd.num_presets;
	for (int i = 0; names && i < listed; i++) names = presets[static_cast<size_t>(i)] == nd.presets[i].name;
	check(names, "preset discovery: the " + std::to_string(nd.num_presets) + " factory presets, by name");
	const auto* pl = in.ext<clap_plugin_preset_load>(CLAP_EXT_PRESET_LOAD);
	if (nd.num_presets > 1) {
		rescans = loaded_presets = 0;
		const std::string key = std::to_string(nd.num_presets - 1);
		const bool loaded = pl && pl->from_location(in.p, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, key.c_str());
		check(loaded && rescans == 1 && loaded_presets == 1, "preset-load: preset " + key + " loaded, a rescan asked, the host told");
		in.start(48000);
		callbacks = rescans = 0;
		(void)pl->from_location(in.p, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, "0"); // active: the audio thread loads it
		Events ev;
		float l[64], r[64];
		in.process(0, 64, ev, l, r);
		in.p->on_main_thread(in.p);
		check(callbacks == 1 && rescans == 1, "preset-load while active: applied by the audio thread, then a callback and a rescan");
	}
	check(pl && !pl->from_location(in.p, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, "999"), "preset-load: no preset 999");

	// state: random values sent through process() at 44.1 kHz, saved, loaded into a new instance,
	// read back (the validator's state-reproducibility)
	{
		Instance a;
		a.start(44100);
		const auto* pa = a.ext<clap_plugin_params>(CLAP_EXT_PARAMS);
		std::mt19937 rng(3);
		Events ev;
		for (uint32_t i = 0; i < pa->count(a.p); i++) {
			clap_param_info pi;
			pa->get_info(a.p, i, &pi);
			const double v = pi.min_value + (pi.max_value - pi.min_value) * (rng() % 1000) / 999.0;
			clap_event_param_value e = {{sizeof e, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0}, pi.id, nullptr, -1, -1, -1, -1, v};
			ev.add(e);
		}
		float l[64], r[64];
		a.process(0, 64, ev, l, r);
		std::vector<double> want(ids.size());
		for (size_t i = 0; i < ids.size(); i++) pa->get_value(a.p, ids[i], &want[i]);
		Buffer st;
		const clap_ostream os = {&st, buf_write};
		const bool saved = a.ext<clap_plugin_state>(CLAP_EXT_STATE)->save(a.p, &os);
		for (int active = 0; active < 2; active++) {
			Instance b;
			if (active) b.start(48000);
			st.at = 0;
			const clap_istream is = {&st, buf_read};
			rescans = 0;
			const bool ok = b.ext<clap_plugin_state>(CLAP_EXT_STATE)->load(b.p, &is);
			if (active) { // the audio thread applies it
				Events none;
				b.process(0, 64, none, l, r);
				b.p->on_main_thread(b.p);
			}
			const auto* pb = b.ext<clap_plugin_params>(CLAP_EXT_PARAMS);
			int diff = 0;
			for (size_t i = 0; i < ids.size(); i++) {
				double got = 0;
				pb->get_value(b.p, ids[i], &got);
				if (std::memcmp(&got, &want[i], sizeof got) != 0) {
					if (diff < 3) std::printf("     param %zu: %g saved, %g loaded\n", i, want[i], got);
					diff++;
				}
			}
			check(saved && ok && diff == 0 && rescans == 1, std::string("state round trip into a new instance, ") + (active ? "active" : "inactive") +
			        ": every value the same, bit for bit (" + std::to_string(diff) + " differ), a rescan");
		}
		check(st.data.rfind("{", 0) == 0 && st.data.find("\"format\": \"awva-preset/1\"") != std::string::npos &&
		          st.data.find("\"awva-plugin\": {") != std::string::npos,
		    "the state is an awva-preset/1 file, with the plugin's own key");
		for (const char* text : {"{\"format\": \"awva-preset/1\", \"engine\": {\"id\": \"no-such-engine\"}, \"params\": {}}",
		         "awva-plugin 1\nengine x\n", ""}) {
			Buffer bad{text};
			const clap_istream is = {&bad, buf_read};
			Instance c;
			check(!c.ext<clap_plugin_state>(CLAP_EXT_STATE)->load(c.p, &is), std::string("refused as a state: ") + (*text ? std::string(text).substr(0, std::min<size_t>(40, std::string(text).find('\n'))) : "nothing"));
		}
	}
	exact_restore(nd);
	if (preset_dir) preset_files(nd, preset_dir);
	nsc_unload(&lib);
	return 0;
}

} // namespace

int main(int argc, char** argv) {
	if (argc < 3) {
		std::fprintf(stderr, "usage: clap_host_test info|voices|perf|ump|notes PLUGIN.clap [REF.f32 | COMPONENT_LIB]\n");
		return 2;
	}
	const std::string mode = argv[1];
	entry = load_entry(argv[2]);
	if (!entry || !entry->init(argv[2])) {
		std::fprintf(stderr, "clap_host_test: cannot load %s\n", argv[2]);
		return 2;
	}
	factory = static_cast<const clap_plugin_factory*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
	int rc = 0;
	if (mode == "voices") {
		std::printf("%u\n", plugin_voices());
		entry->deinit();
		return 0;
	}
	if (mode == "info" && argc >= 4) rc = info_test(argv[3], argc >= 5 ? argv[4] : nullptr);
	else if (mode == "perf" && argc >= 4) rc = perf_test(argv[3]);
	else if (mode == "ump" && argc >= 4) rc = ump_test(argv[3]);
	else if (mode == "midifile" && argc >= 5) rc = midifile_test(argv[3], argv[4], argc >= 6 ? std::atoi(argv[5]) : 48000);
	else if (mode == "notes") rc = notes_test();
	else if (mode == "many" && argc >= 4) rc = many_test(argv[3]);
	else {
		std::fprintf(stderr, "clap_host_test: unknown mode %s, or arguments missing\n", mode.c_str());
		return 2;
	}
	entry->deinit();
	if (rc) return rc;
	std::printf(failures ? "clap_host_test: %d FAILED\n" : "clap_host_test: all passed\n", failures);
	return failures ? 1 : 0;
}
