// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The app plays what the plugin plays:
//   render_equal PLUGIN.clap COMPONENT_LIB
// The same UMP into the plugin (a minimal CLAP host, host blocks of 512 and of random sizes) and
// into the app's core (render_offline: its Player, the safety chain at 0 dB): the outputs must be
// equal, bit for bit, on both channels. Two performances: the MPE stream of an MPE controller (MIDI
// 1.0, the plugin's MIDI dialect) and a MIDI 2.0 one (its MIDI2 dialect); the engine's default
// sound and its last factory preset; 48 and 44.1 kHz. Opens no audio device.
#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <clap/clap.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "core.h"
#include "nsc/load.h"
#include "safety.h"

using namespace awva_app;

namespace {

int failures = 0;
void check(bool ok, const std::string& what) {
	std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
	std::fflush(stdout);
	failures += !ok;
}

const clap_plugin_entry* load_entry(const char* path) {
#ifdef _WIN32
	HMODULE m = LoadLibraryA(path);
	return m ? reinterpret_cast<const clap_plugin_entry*>(GetProcAddress(m, "clap_entry")) : nullptr;
#else
	std::string p = path;
	if (p.size() > 5 && p.substr(p.size() - 5) == ".clap") {
		const size_t slash = p.find_last_of('/');
		const std::string inner = p + "/Contents/MacOS/" + p.substr(slash + 1, p.size() - slash - 6);
		if (FILE* f = std::fopen(inner.c_str(), "rb")) {
			std::fclose(f);
			p = inner;
		}
	}
	void* h = dlopen(p.c_str(), RTLD_NOW | RTLD_LOCAL);
	return h ? static_cast<const clap_plugin_entry*>(dlsym(h, "clap_entry")) : nullptr;
#endif
}

const void* host_ext(const clap_host*, const char*) { return nullptr; }
const clap_host kHost = {CLAP_VERSION_INIT, nullptr, "render_equal", "Sorbet AI", "", "0.1", host_ext, [](const clap_host*) {},
    [](const clap_host*) {}, [](const clap_host*) {}};
const clap_output_events kOut = {nullptr, [](const clap_output_events*, const clap_event_header*) { return true; }};

struct EvList {
	std::vector<std::vector<uint8_t>> store;
	std::vector<const clap_event_header*> list;
	template <class E> void add(const E& e) {
		store.emplace_back(sizeof(E));
		std::memcpy(store.back().data(), &e, sizeof(E));
		list.push_back(reinterpret_cast<const clap_event_header*>(store.back().data()));
	}
	void seal() {
		list.clear();
		for (auto& s : store) list.push_back(reinterpret_cast<const clap_event_header*>(s.data()));
	}
	clap_input_events in() {
		return {this, [](const clap_input_events* l) { return static_cast<uint32_t>(static_cast<EvList*>(l->ctx)->list.size()); },
		    [](const clap_input_events* l, uint32_t i) { return static_cast<EvList*>(l->ctx)->list[i]; }};
	}
};

// the plugin: a factory preset by preset-load (-1: its default sound), the events in host blocks
// (block > 0: that size; 0: random 1 .. 1024)
std::vector<float> play_plugin(const clap_plugin_factory* f, int preset, bool midi1, const std::vector<TimedUmp>& ev, uint64_t frames,
    int rate, int block, std::vector<float>* right) {
	const clap_plugin* p = f->create_plugin(f, &kHost, f->get_plugin_descriptor(f, 0)->id);
	if (!p || !p->init(p)) return {};
	// active first, then the preset, as when it is chosen while playing (the plugin applies it before
	// its next block; the app's audio thread the same)
	p->activate(p, rate, 1, 4096);
	p->start_processing(p);
	if (preset >= 0) {
		const auto* pl = static_cast<const clap_plugin_preset_load*>(p->get_extension(p, CLAP_EXT_PRESET_LOAD));
		char key[16];
		std::snprintf(key, sizeof key, "%03d", preset);
		if (!pl || !pl->from_location(p, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, key)) return {};
	}
	std::vector<float> l(frames), r(frames);
	std::mt19937 rng(7);
	size_t k = 0;
	for (uint64_t at = 0; at < frames;) {
		const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(block > 0 ? block : 1 + rng() % 1024, frames - at));
		EvList list;
		for (; k < ev.size() && ev[k].frame < at + n; k++) {
			const uint32_t t = static_cast<uint32_t>(ev[k].frame - at);
			if (midi1) {
				const uint32_t w = ev[k].w[0];
				clap_event_midi e = {{sizeof e, t, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0}, 0,
				    {static_cast<uint8_t>(w >> 16), static_cast<uint8_t>(w >> 8), static_cast<uint8_t>(w)}};
				list.add(e);
			} else {
				clap_event_midi2 e = {{sizeof e, t, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI2, 0}, 0, {ev[k].w[0], ev[k].w[1], ev[k].w[2], ev[k].w[3]}};
				list.add(e);
			}
		}
		list.seal();
		clap_input_events ie = list.in();
		float* chans[2] = {l.data() + at, r.data() + at};
		clap_audio_buffer ob = {chans, nullptr, 2, 0, 0};
		clap_process pr = {static_cast<int64_t>(at), n, nullptr, nullptr, &ob, 0, 1, &ie, &kOut};
		p->process(p, &pr);
		at += n;
	}
	p->stop_processing(p);
	p->deactivate(p);
	p->destroy(p);
	*right = r;
	return l;
}

// MIDI 2.0: four notes with 16-bit velocities, per-note pitch (Pitch 7.25), poly pressure,
// per-note y, as a MIDI 2.0 controller sends them
std::vector<TimedUmp> midi2_performance(int rate) {
	std::vector<TimedUmp> v;
	const int keys[4] = {57, 61, 64, 69};
	for (int n = 0; n < 4; n++) {
		const uint64_t on = static_cast<uint64_t>((0.1 + 0.4 * n) * rate);
		const uint32_t key = static_cast<uint32_t>(keys[n]);
		v.push_back({on, {0x40910000u | key << 8, (0x5000u + 0x2000u * static_cast<uint32_t>(n)) << 16, 0, 0}});
		for (int k = 1; k < 120; k++) {
			const uint64_t f = on + static_cast<uint64_t>(k * 0.01 * rate) + 3;
			const double t = k * 0.01;
			v.push_back({f, {0x40a10000u | key << 8, static_cast<uint32_t>((0.4 + 0.3 * std::sin(3 * t)) * 4294967295.0), 0, 0}});
			v.push_back({f + 1, {0x40010004u | key << 8, static_cast<uint32_t>((0.5 + 0.3 * std::sin(2 * t + n)) * 4294967295.0), 0, 0}});
			const double pitch = keys[n] + (n == 3 ? 0.2 * std::sin(30 * t) : 0.0);
			v.push_back({f + 2, {0x40010003u | key << 8, static_cast<uint32_t>(pitch * 33554432.0), 0, 0}});
		}
		v.push_back({on + static_cast<uint64_t>(1.3 * rate), {0x40810000u | key << 8, 0x80000000u, 0, 0}});
	}
	std::stable_sort(v.begin(), v.end(), [](const TimedUmp& a, const TimedUmp& b) { return a.frame < b.frame; });
	return v;
}

} // namespace

int main(int argc, char** argv) {
	if (argc < 3) {
		std::fprintf(stderr, "usage: render_equal PLUGIN.clap COMPONENT_LIB\n");
		return 2;
	}
	const clap_plugin_entry* entry = load_entry(argv[1]);
	if (!entry || !entry->init(argv[1])) {
		std::fprintf(stderr, "render_equal: can't load %s\n", argv[1]);
		return 2;
	}
	const auto* f = static_cast<const clap_plugin_factory*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
	nsc_library lib;
	if (nsc_load(argv[2], &lib) != 0) {
		std::fprintf(stderr, "render_equal: can't load the component %s\n", argv[2]);
		return 2;
	}
	const nsc_component& c = *lib.c;
	const int last = c.desc->num_presets - 1;
	for (const int rate : {48000, 44100}) {
		const uint64_t frames = static_cast<uint64_t>(rate) * 4;
		for (const bool midi1 : {true, false}) {
			const std::vector<TimedUmp> ev = midi1 ? mpe_controller_performance(rate, 4.0) : midi2_performance(rate);
			std::vector<int> presets = {-1};
			if (last >= 0) presets.push_back(last);
			for (const int preset : presets) {
				PresetEntry pe;
				pe.factory = preset;
				// the engine path (what the safety chain is given), and the whole output at 0 dB
				std::vector<float> el, er, al, ar;
				if (!render_offline(c, preset >= 0 ? &pe : nullptr, false, ev, frames, rate, 0.0f, &el, &er, false) ||
				    !render_offline(c, preset >= 0 ? &pe : nullptr, false, ev, frames, rate, 0.0f, &al, &ar, true)) {
					check(false, "the app's render");
					continue;
				}
				for (const int block : {512, 0}) {
					std::vector<float> pr;
					const std::vector<float> pl = play_plugin(f, preset, midi1, ev, frames, rate, block, &pr);
					double peak = 0;
					size_t bad = 0, bad_out = 0, first = 0;
					for (size_t i = 0; i < frames && i < pl.size(); i++) {
						peak = std::max(peak, static_cast<double>(std::fabs(pl[i])));
						if (std::memcmp(&pl[i], &el[i], 4) != 0 || std::memcmp(&pr[i], &er[i], 4) != 0) {
							if (!bad) first = i;
							bad++;
						}
						if (std::memcmp(&pl[i], &al[i], 4) != 0 || std::memcmp(&pr[i], &ar[i], 4) != 0) bad_out++;
					}
					char what[320];
					std::snprintf(what, sizeof what, "%s: %s, %s, %d Hz, host blocks %s: the app's engine output is the plugin's, bit for bit (peak %.3f)",
					    c.desc->id, midi1 ? "an MPE controller stream (MIDI 1.0)" : "MIDI 2.0", preset < 0 ? "default sound" : "last factory preset", rate,
					    block ? "512" : "random", peak);
					check(pl.size() == frames && bad == 0 && peak > 0.001, what);
					if (bad) std::printf("     %zu frames differ, the first at %zu\n", bad, first);
					// the whole output, through the safety chain at 0 dB: the same while the plugin stays under
					// the limiter's ceiling (-1 dBTP); above it the limiter works, as it should
					if (peak < Limiter::kCeiling * 0.97) check(bad_out == 0, "     ... and through the safety chain at 0 dB, bit for bit");
					else std::printf("     (its peak passes the app's -1 dBTP ceiling: the limiter works there; %zu frames changed)\n", bad_out);
				}
			}
		}
	}
	entry->deinit();
	std::printf("%s\n", failures ? "render_equal: FAILED" : "render_equal: ok");
	return failures ? 1 : 0;
}
