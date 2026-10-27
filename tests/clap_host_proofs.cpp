// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// clap_host_test's bit-for-bit proofs (clap_host.h): perf and midifile against awva_render, ump against
// the component library, expressive against awva_render --ump.
#include "clap_host.h"

namespace host_test {

// --- perf: the default scripted performance against awva_render ---------------------------------------
struct PerfSource {
	std::unique_ptr<awva::sdk::Player> player;
	std::vector<awva::NoteControls> vc;
	awva::InstrumentControls g = awva::instrument_at_rest;
	// as awva_render: the player's controls, then the engine
	static void block(void* ctx, uint64_t b, nsc_voice* out, int32_t nv, nsc_globals* g) {
		auto& s = *static_cast<PerfSource*>(ctx);
		s.player->block(static_cast<int>(b), s.vc.data(), s.g);
		std::memcpy(out, s.vc.data(), sizeof(nsc_voice) * static_cast<size_t>(std::min<int32_t>(nv, static_cast<int32_t>(s.vc.size()))));
		std::memcpy(g, &s.g, sizeof *g);
	}
};
static_assert(sizeof(nsc_voice) == sizeof(awva::NoteControls) && sizeof(nsc_globals) == sizeof(awva::InstrumentControls));

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
		s.vc.assign(static_cast<size_t>(nv), awva::note_at_rest);
		const awva_voice_source src = {&s, PerfSource::block};
		const std::vector<float> out = play({}, ref.size(), block, 48000, false, &src);
		size_t first = 0;
		double peak = 0;
		const size_t bad = differ(out, ref, &first, &peak);
		char what[320];
		std::snprintf(what, sizeof what, "default performance, %d voices, host blocks %s: %zu frames (peak %.3f) bit for bit awva_render --ftz's",
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
		char what[320];
		std::snprintf(what, sizeof what, "MIDI file (%zu events), %d Hz, host blocks %s: %zu frames (peak %.3f) bit for bit awva_render --ftz --midi's",
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
// four more notes that overlap, for mono
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
std::vector<float> render_component(const nsc_component& c, const std::vector<Ev>& perf, uint64_t frames, int rate, int nv, bool mono) {
	nsc_instance in;
	const nsc_config cfg = {rate, 0, nv};
	if (nsc_instance_new(&c, &in) != 0) return {};
	if (c.init(in.state, in.stream, in.bulk, &cfg) != 0) return {};
	if (mono) (void)c.mode(in.state, NSC_MODE_MONO, 1);
	std::vector<float> out;
	size_t k = 0;
	float buf[2][NSC_BLOCK];
	for (uint64_t b0 = 0; b0 < frames; b0 += NSC_BLOCK) {
		std::vector<nsc_event> ev;
		for (; k < perf.size() && perf[k].frame < b0 + NSC_BLOCK; k++)
			ev.push_back({static_cast<int32_t>(perf[k].frame - b0), -1, {perf[k].w0, perf[k].w1, 0, 0}});
		float* o[2] = {buf[0], buf[1]};
		c.process(in.state, NSC_BLOCK, ev.data(), static_cast<int32_t>(ev.size()), o);
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
			for (const bool mono : {false, true}) {
				const std::vector<float> ref = render_component(c, perf, frames, rate, nv, mono);
				for (const int block : {256, 1, 33, 0}) {
					const std::vector<float> out = play(events, frames, block, rate, mono);
					size_t first = 0;
					double peak = 0;
					const size_t bad = differ(out, ref, &first, &peak);
					char what[256];
					std::snprintf(what, sizeof what, "%s dialect, %s, %d Hz, host blocks %s: %llu frames (peak %.3f) bit for bit the component's",
					    midi1 ? "MIDI (1.0)" : "MIDI2 (MIDI 2.0)", mono ? "mono" : "polyphonic", rate, blocks_name(block).c_str(),
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

// --- expressive: the performances of tests/expressive.h through the CLAP dialects ------------------------
// the UMP stream as MIDI 2.0 (MIDI2 dialect)
std::vector<Timed> clap_ump(const std::vector<expressive::Packet>& u) {
	std::vector<Timed> v;
	for (const expressive::Packet& p : u) {
		clap_event_midi2 x = {{sizeof x, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI2, 0}, 0, {p.w0, p.w1, 0, 0}};
		push(v, p.frame, x);
	}
	return v;
}
// a MIDI 1.0 performance as MIDI (MIDI dialect)
std::vector<Timed> clap_midi(const std::vector<expressive::Event>& perf) {
	std::vector<Timed> v;
	for (const expressive::Event& e : perf) {
		clap_event_midi x = {{sizeof x, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0}, 0, {}};
		if (expressive::bytes(e, x.data)) push(v, e.frame, x);
	}
	return v;
}
// one comparison: the output against the reference, bit for bit
void expect_ref(const std::vector<float>& out, const std::vector<float>& ref, const std::string& what) {
	const expressive::Diff d = expressive::differ(out, ref);
	char line[400];
	std::snprintf(line, sizeof line, "%s: %zu frames (peak %.3f) bit for bit awva_render --ftz --ump's", what.c_str(), ref.size(), d.peak);
	check(!ref.empty() && d.bad == 0 && d.peak > 0.001, line);
	if (d.bad) std::printf("     %zu frames differ, the first at %zu, by up to %.3g (%.1f dBFS)\n", d.bad, d.first, d.max_diff, 20 * std::log10(d.max_diff + 1e-300));
}
int expressive_test(const char* lib_path, const std::string& dir, int rate) {
	nsc_library lib;
	if (nsc_load(lib_path, &lib) != 0) {
		std::fprintf(stderr, "clap_host_test: cannot load the component %s\n", lib_path);
		return 2;
	}
	const bool reads_timbre = (lib.c->desc->inputs & (1u << 2)) != 0; // (engine.h in_timbre)
	nsc_unload(&lib);
	using namespace expressive;
	const std::string r = std::to_string(rate);
	const std::vector<float> ref_note = read_f32(dir + "/ref_per_note_" + r + ".f32"), ref_mpe = read_f32(dir + "/ref_mpe_" + r + ".f32");
	if (ref_note.empty() || ref_mpe.empty()) {
		std::fprintf(stderr, "clap_host_test: no references in %s (awva_render --ump)\n", dir.c_str());
		return 2;
	}
	const std::vector<Event> note = per_note(rate), mp = mpe(rate);
	const std::vector<Timed> as_notes = clap_notes(note), as_ump = clap_ump(ump(note)), as_midi = clap_midi(mp);
	for (const int block : {256, 1, 33, 0}) {
		const std::string b = std::to_string(rate) + " Hz, host blocks " + blocks_name(block);
		expect_ref(play(as_notes, ref_note.size(), block, rate), ref_note, "per-note: CLAP notes, pressure, tuning, brightness expressions, " + b);
		expect_ref(play(as_ump, ref_note.size(), block, rate), ref_note, "per-note: MIDI 2.0 (MIDI2 dialect), " + b);
		expect_ref(play(as_midi, ref_mpe.size(), block, rate), ref_mpe, "MPE: MIDI 1.0 (MIDI dialect), " + b);
	}
	// (each dimension reaches the engine: without it the output differs)
	for (const Kind k : {kPressure, kTuning, kBrightness}) {
		std::vector<Event> without;
		for (const Event& e : note)
			if (e.kind != k) without.push_back(e);
		const char* name = k == kPressure ? "pressure" : k == kTuning ? "tuning" : "brightness";
		const bool differs = !same_bits(play(clap_notes(without), ref_note.size(), 256, rate), play(as_notes, ref_note.size(), 256, rate));
		if (k != kBrightness || reads_timbre) check(differs, std::string("per-note: the ") + name + " expressions change the output");
		else std::printf("info the engine doesn't read timbre: the brightness expressions %s the output\n", differs ? "change" : "don't change");
	}
	return 0;
}

} // namespace host_test
