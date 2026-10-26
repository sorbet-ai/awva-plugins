// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The core without the UI, for smoke runs, renders and CI. Never opens an audio device unless
// told to (it has no option for that: every mode here runs the null sink or renders offline).
//
//   awva_app_cli list [--engines DIR] [--builtin DIR]
//       the catalog (built-in engines and packages) as JSON
//   awva_app_cli smoke [--engine KEY] [--seconds S] [--probe OUT.wav] [--engines DIR] [--builtin DIR]
//       the live path with the null sink: the core started (no audio device, no MIDI ports), the
//       MPE controller stream sent in real time through awva_ump() (the same rings, timestamps and
//       frames as a port's messages), the output tapped to OUT.wav; checks that it sounds and that
//       no sample passes the limiter's ceiling
//   awva_app_cli render --engine KEY [--preset I] [--seconds S] [--rate R] [--master-db DB] --out OUT.wav
//       the MPE controller stream rendered offline through the same Player and safety chain
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "awva_app/awva_app.h"
#include "catalog.h"
#include "core.h"
#include "json.h"
#include "platform.h"
#include "safety.h"
#include "wav.h"

using namespace awva_app;

namespace {

std::string arg(int argc, char** argv, const char* name, const std::string& def = "") {
	for (int i = 2; i + 1 < argc; i++)
		if (!std::strcmp(argv[i], name)) return argv[i + 1];
	return def;
}

std::string options(int argc, char** argv, const std::string& probe) {
	std::string o = "{\"no_audio\": true, \"midi\": false, \"settings\": \"\"";
	if (!arg(argc, argv, "--engines").empty()) o += ", \"engines_dir\": " + jstr(arg(argc, argv, "--engines"));
	if (!arg(argc, argv, "--builtin").empty()) o += ", \"builtin_dir\": " + jstr(arg(argc, argv, "--builtin"));
	if (!probe.empty()) o += ", \"probe_wav\": " + jstr(probe);
	return o + "}";
}

int smoke(int argc, char** argv) {
	const std::string probe = arg(argc, argv, "--probe", join(cache_dir(), "smoke.wav"));
	const double seconds = std::atof(arg(argc, argv, "--seconds", "4").c_str());
	if (awva_start(options(argc, argv, probe).c_str()) != 0) return 2;
	const std::string key = arg(argc, argv, "--engine");
	if (!key.empty() && awva_select_engine(key.c_str()) != 0) {
		std::fprintf(stderr, "smoke: %s\n", awva_last_error());
		awva_stop();
		return 2;
	}
	awva_set_master_db(0.0f);
	std::printf("smoke: %s\n", awva_audio());
	// the controller's stream, each message at its time (48 kHz frames on the core's clock)
	const std::vector<TimedUmp> ev = mpe_controller_performance(48000, seconds);
	std::this_thread::sleep_for(std::chrono::milliseconds(400)); // (past the fade-in)
	const auto t0 = std::chrono::steady_clock::now();
	for (const TimedUmp& e : ev) {
		std::this_thread::sleep_until(t0 + std::chrono::nanoseconds(static_cast<int64_t>(e.frame * 1e9 / 48000)));
		awva_ump(e.w, 1);
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(800));
	awva_status st;
	awva_status_get(&st);
	std::printf("smoke: %u MIDI messages, %u xruns, %u faults, cpu %.3f\n", st.midi_events, st.xruns, st.faults, st.cpu);
	awva_stop();
	// the tap: it sounds, and nothing above the ceiling
	std::string wav;
	if (!read_file(probe, &wav, 1u << 30) || wav.size() < 44) {
		std::fprintf(stderr, "smoke: no probe file %s\n", probe.c_str());
		return 1;
	}
	const float* s = reinterpret_cast<const float*>(wav.data() + 44);
	const size_t n = (wav.size() - 44) / 4;
	double peak = 0, sq = 0;
	for (size_t i = 0; i < n; i++) {
		peak = std::max(peak, static_cast<double>(std::fabs(s[i])));
		sq += static_cast<double>(s[i]) * s[i];
	}
	const double rms = n ? std::sqrt(sq / n) : 0;
	std::printf("smoke: probe %s: %zu frames, peak %.4f, rms %.4f\n", probe.c_str(), n / 2, peak, rms);
	const bool ok = n > 48000 && rms > 1e-4 && peak <= Limiter::kCeiling * 1.0001 && st.faults == 0;
	std::printf("smoke: %s\n", ok ? "ok" : "FAILED");
	return ok ? 0 : 1;
}

int render(int argc, char** argv) {
	Catalog cat;
	std::string builtin = arg(argc, argv, "--builtin", join(core_dir(), "engines"));
	cat.scan(builtin, arg(argc, argv, "--engines", join(app_data_dir(), "engines")));
	const Engine* e = cat.find(arg(argc, argv, "--engine"));
	const std::string out = arg(argc, argv, "--out");
	if (!e || !e->ok || out.empty()) {
		std::fprintf(stderr, "render: --engine KEY (a catalog key, e.g. builtin:va) and --out OUT.wav\n");
		return 2;
	}
	const int rate = std::atoi(arg(argc, argv, "--rate", "48000").c_str());
	const double seconds = std::atof(arg(argc, argv, "--seconds", "4").c_str());
	const int pi = std::atoi(arg(argc, argv, "--preset", "-1").c_str());
	const PresetEntry* p = pi >= 0 && pi < static_cast<int>(e->presets.size()) ? &e->presets[static_cast<size_t>(pi)] : nullptr;
	std::vector<float> l, r;
	if (!render_offline(*e->c, p, false, mpe_controller_performance(rate, seconds), static_cast<uint64_t>(rate * seconds), rate,
	        static_cast<float>(std::atof(arg(argc, argv, "--master-db", "0").c_str())), &l, &r))
		return 1;
	WavWriter w;
	if (!w.open(out, rate)) return 1;
	w.write(l.data(), r.data(), static_cast<uint32_t>(l.size()));
	w.close();
	std::printf("render: %s, %zu frames\n", out.c_str(), l.size());
	return 0;
}

} // namespace

int main(int argc, char** argv) {
	const std::string mode = argc > 1 ? argv[1] : "";
	if (mode == "list") {
		if (awva_start(options(argc, argv, "").c_str()) != 0) return 2;
		std::printf("%s\n", awva_catalog());
		awva_stop();
		return 0;
	}
	if (mode == "smoke") return smoke(argc, argv);
	if (mode == "render") return render(argc, argv);
	std::fprintf(stderr, "usage: awva_app_cli list|smoke|render [options] (see the source's header)\n");
	return 2;
}
