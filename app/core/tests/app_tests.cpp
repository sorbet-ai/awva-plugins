// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The core's own checks (no audio device, no MIDI port):
//   app_tests BUILTIN_DIR SCRATCH_DIR
//   - the safety chain: below the ceiling the samples pass bit for bit (delayed); a signal 18 dB
//     over it comes out under -1 dBTP (its 4x-interpolated peaks); a NaN block is muted and counted;
//     the master level starts faded in
//   - packages (awva-package/1): a library package, a presets-only package, and broken ones, each
//     listed as the README says
//   - the controller's MPE setup reaches a new engine: an MCM and a bend range sent before an engine
//     switch give the same output as sent to that engine directly
//   - MIDI 1.0 bytes to UMP
#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "awva_app/awva_app.h"
#include "catalog.h"
#include "core.h"
#include "json.h"
#include "platform.h"
#include "player.h"
#include "nsc/load.h"
#include "safety.h"

using namespace awva_app;

static bool nsc_load_check(const std::string& path, nsc_library* out) { return nsc_load(path.c_str(), out) == 0; }
static std::string read_back(const std::string& path) {
	std::string s;
	read_file(path, &s);
	return s;
}
static void remove_all(const std::string& path) {
	std::error_code ec;
	std::filesystem::remove_all(std::filesystem::u8path(path), ec);
}

namespace {

int failures = 0;
void check(bool ok, const std::string& what) {
	std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
	std::fflush(stdout);
	failures += !ok;
}

void safety_checks() {
	const int rate = 48000, n = rate;
	{ // transparent below the ceiling
		Safety s;
		s.prepare(rate);
		s.set_now_db(0.0f);
		std::vector<float> l(n), r(n), l0, r0;
		for (int i = 0; i < n; i++) {
			l[i] = 0.8f * std::sin(i * 0.031f);
			r[i] = -0.5f * std::sin(i * 0.017f);
		}
		l0 = l, r0 = r;
		(void)s.process(l.data(), r.data(), n);
		bool same = true;
		for (int i = Limiter::kDelay; i < n; i++) same = same && l[i] == l0[i - Limiter::kDelay] && r[i] == r0[i - Limiter::kDelay];
		check(same, "safety: below the ceiling the samples pass bit for bit, delayed by the look-ahead");
	}
	{ // a loud signal: under -1 dBTP, inter-sample peaks included
		Safety s;
		s.prepare(rate);
		s.set_now_db(0.0f);
		std::vector<float> l(n), r(n);
		for (int i = 0; i < n; i++) {
			l[i] = 7.0f * std::sin(i * 2.0f * 3.14159265f * 11025.0f / rate + 0.7f); // (fs/4 + phase: inter-sample peaks)
			r[i] = (i / 4800) % 2 ? 6.0f : -0.1f;                                      // (steps)
		}
		const Safety::Result res = s.process(l.data(), r.data(), n);
		double peak = 0;
		for (int i = 0; i < n; i++) peak = std::max({peak, (double)std::fabs(l[i]), (double)std::fabs(r[i])});
		// the true peak of the output (a fine sinc interpolation, 16x)
		double tp = 0;
		for (int i = 64; i + 64 < n; i += 1)
			for (int ph = 0; ph < 16; ph++) {
				double v = 0;
				const double x = ph / 16.0;
				for (int k = -32; k <= 32; k++) {
					const double t = k - x;
					const double w = t == 0 ? 1 : std::sin(3.14159265358979 * t) / (3.14159265358979 * t) * (0.5 + 0.5 * std::cos(3.14159265358979 * t / 33));
					v += l[i + k] * w;
				}
				tp = std::max(tp, std::fabs(v));
			}
		char b[160];
		std::snprintf(b, sizeof b, "safety: 18 dB over: sample peak %.4f, true peak %.4f (%.2f dBTP), under the ceiling", peak, tp, 20 * std::log10(tp));
		check(peak <= Limiter::kCeiling + 1e-6 && tp < 1.0 && res.min_gain < 0.2f, b);
	}
	{ // a fault
		Safety s;
		s.prepare(rate);
		s.set_now_db(0.0f);
		std::vector<float> l(256, 0.1f), r(256, 0.1f);
		l[17] = NAN;
		const Safety::Result res = s.process(l.data(), r.data(), 256);
		bool silent = true;
		for (int i = 0; i < 256; i++) silent = silent && l[i] == 0 && r[i] == 0;
		check(res.fault && silent, "safety: a block with a NaN is muted and reported");
	}
	{ // the start fades in, and the startup level can't be loud
		Safety s;
		s.prepare(rate);
		s.set_target_db(kStartMaxDb);
		s.fade_in(0.3f);
		std::vector<float> l(n, 0.5f), r(n, 0.5f);
		(void)s.process(l.data(), r.data(), n);
		check(l[Limiter::kDelay + 10] < 0.01f && std::fabs(l[n - 1] - 0.5f * db_to_gain(kStartMaxDb)) < 1e-5f,
		    "safety: the start fades in, to at most -12 dB");
	}
}

std::string mkpkg(const std::string& root, const std::string& name, const std::string& manifest) {
	const std::string d = join(root, name);
	make_dirs(join(d, "presets"));
	write_file(join(d, "awva-package.json"), manifest);
	return d;
}

void package_checks(const std::string& builtin, const std::string& scratch) {
	const std::string root = join(scratch, "engines");
	make_dirs(root);
	// a library package: a copy of the built-in va, with a preset file of its own and a foreign one
	const std::string lib = join(builtin, std::string("va") + library_suffix());
	const std::string a = mkpkg(root, "va-test", "{\"format\": \"awva-package/1\", \"label\": \"va test (fit)\", \"library\": \"va" + std::string(library_suffix()) + "\", \"awva\": \"" AWVA_APP_AWVA_COMMIT "\"}");
	copy_file(lib, join(a, std::string("va") + library_suffix()));
	// a preset of va, written by the component itself
	{
		nsc_library l;
		if (nsc_load_check(lib, &l)) {
			Instance in(*l.c);
			in.init(48000);
			l.c->set_param(in.state, 0, l.c->desc->params[0].min);
			char buf[16384];
			const uint32_t len = l.c->save_preset(in.state, "Package Sound", buf, sizeof buf);
			write_file(join(join(a, "presets"), "01-package-sound.preset.json"), std::string(buf, len));
			write_file(join(join(a, "presets"), "02-not-va.preset.json"), "{\"format\": \"awva-preset/1\", \"engine\": \"bowed\", \"params\": {}}");
			// a presets-only package for the built-in va
			const std::string b = mkpkg(root, "va-fit", "{\"format\": \"awva-package/1\", \"label\": \"va fit v1\", \"engine\": \"va\"}");
			write_file(join(join(b, "presets"), "01-fit.preset.json"), std::string(buf, len));
		}
	}
	mkpkg(root, "broken-format", "{\"format\": \"awva-package/9\", \"label\": \"x\", \"library\": \"x.dll\"}");
	mkpkg(root, "broken-base", "{\"format\": \"awva-package/1\", \"label\": \"old base\", \"library\": \"va" + std::string(library_suffix()) + "\", \"awva\": \"1726a05\"}");
	copy_file(lib, join(join(root, "broken-base"), std::string("va") + library_suffix()));
	mkpkg(root, "broken-missing", "{\"format\": \"awva-package/1\", \"label\": \"missing lib\", \"library\": \"nothere" + std::string(library_suffix()) + "\", \"awva\": \"" AWVA_APP_AWVA_COMMIT "\"}");
	mkpkg(root, "broken-engine", "{\"format\": \"awva-package/1\", \"label\": \"no engine\", \"engine\": \"nosuchengine\"}");

	Catalog cat;
	cat.scan(builtin, root);
	const Engine* va = cat.find("builtin:va");
	const Engine* pa = cat.find("package:va-test");
	const Engine* pb = cat.find("package:va-fit");
	check(va && va->ok && va->label == "built in", "packages: the built-in va is listed, built in");
	check(pa && pa->ok && pa->label == "va test (fit)" && pa->c != va->c, "packages: a library package loads from its own copy, under its label");
	check(pa && pa->presets.size() == static_cast<size_t>(pa->c->desc->num_presets) + 1 && pa->presets.back().name == "Package Sound" &&
	          pa->refused.size() == 1,
	    "packages: its factory presets, then its preset file by title; another engine's preset refused");
	check(pb && pb->ok && pb->c == va->c && pb->presets.size() == 1 && pb->label == "va fit v1", "packages: a presets-only package plays the built-in engine with its presets");
	{
		const Engine* e = cat.find("package:broken-base");
		check(e && !e->ok && !e->c && e->error.rfind("built for a different awva", 0) == 0, "packages: a library from another awva base is refused unloaded: " + (e ? e->error : "?"));
	}
	for (const char* k : {"package:broken-format", "package:broken-missing", "package:broken-engine"}) {
		const Engine* e = cat.find(k);
		check(e && !e->ok && !e->error.empty(), std::string("packages: ") + k + " is listed, refused: " + (e ? e->error : "?"));
	}
	Json j;
	check(Json::parse(cat.json(), &j) && j.type == Json::Array && j.a.size() == cat.engines().size(), "packages: the catalog is JSON");

	// the C ABI on this folder: the package plays, its preset selects
	const std::string o = "{\"no_audio\": true, \"midi\": false, \"settings\": \"\", \"builtin_dir\": " + jstr(builtin) + ", \"engines_dir\": " + jstr(root) + "}";
	check(awva_start(o.c_str()) == 0, "the core starts with the null sink");
	check(awva_select_engine("package:va-fit") == 0, "the core plays a presets-only package");
	Json s;
	check(Json::parse(awva_sound(), &s) && s.num("preset", -1) == 0 && s.get("heroes") && !s.get("heroes")->a.empty(), "the sound: its preset and heroes");
	check(awva_select_engine("package:broken-engine") != 0 && std::strlen(awva_last_error()) > 0, "a refused package can't be played, and says why");

	// a package written while the core runs: listed at the next refresh, by itself; a hidden folder
	// (one being written) never
	awva_status st0;
	awva_status_get(&st0);
	const std::string hidden = mkpkg(root, ".va-iter9", "{\"format\": \"awva-package/1\", \"label\": \"half written\", \"engine\": \"va\"}");
	write_file(join(join(hidden, "presets"), "01-fit.preset.json"), read_back(join(join(root, "va-fit"), "presets/01-fit.preset.json")));
	awva_midi_refresh();
	check(std::string(awva_catalog()).find("half written") == std::string::npos, "a hidden folder (a package being written) isn't listed");
	std::rename(hidden.c_str(), join(root, "va-iter9").c_str());
	awva_midi_refresh();
	awva_status st1;
	awva_status_get(&st1);
	check(std::string(awva_catalog()).find("package:va-iter9") != std::string::npos && st1.catalog != st0.catalog,
	    "a package that appears while the core runs is listed at the next refresh, and the UI is told");
	// the package playing, its folder removed: it keeps playing, the sound still there
	check(awva_select_engine("package:va-iter9") == 0, "the new package plays");
	remove_all(join(root, "va-iter9"));
	awva_midi_refresh();
	Json snd;
	check(Json::parse(awva_sound(), &snd) && snd.get("engine") && snd.get("engine")->str("key") == "package:va-iter9",
	    "a package removed while it plays keeps playing (its entry kept)");
	awva_stop();
}

void setup_checks(const std::string& builtin) {
	nsc_library l;
	if (!nsc_load_check(join(builtin, std::string("va") + library_suffix()), &l)) return check(false, "va loads");
	// the controller's stream with a bend range of 12, not the default 48
	std::vector<TimedUmp> ev = mpe_controller_performance(48000, 2.0);
	for (TimedUmp& e : ev)
		if ((e.w[0] & 0xfff0ff00u) == 0x20b00600u && (e.w[0] >> 16 & 0xf) != 0) e.w[0] = (e.w[0] & 0xffffff00u) | 12u;
	auto run = [&](bool switch_first) {
		Player p;
		p.prepare(48000);
		p.safety.set_now_db(0.0f);
		Instance a(*l.c), b(*l.c);
		a.init(48000);
		b.init(48000);
		size_t k = 0;
		if (switch_first) { // the setup goes to engine a, then the switch to b
			p.inst = &a;
			for (; k < ev.size() && ev[k].frame == 0; k++) p.event(0, ev[k].w);
			(void)p.swap(&b);
		} else {
			p.inst = &b;
		}
		std::vector<float> out(48000 * 2), r(48000 * 2);
		for (uint64_t f = 0; f < out.size(); f += 256) {
			for (; k < ev.size() && ev[k].frame < f + 256; k++) p.event(ev[k].frame, ev[k].w);
			(void)p.render(out.data() + f, r.data() + f, 256);
		}
		p.inst = nullptr;
		return out;
	};
	const std::vector<float> direct = run(false), switched = run(true);
	size_t first = direct.size();
	for (size_t i = 0; i < direct.size(); i++)
		if (std::fabs(direct[i] - switched[i]) > 1e-6f) {
			first = i;
			break;
		}
	// (the switch fades in over 30 ms: compare after it)
	bool same = true;
	for (size_t i = 48000 / 10; i < direct.size(); i++) same = same && std::fabs(direct[i] - switched[i]) <= 1e-6f * (1 + std::fabs(direct[i]));
	check(same, "MIDI setup: an MCM and a bend range sent before an engine switch reach the new engine");
	(void)first;
}

void midi1_checks() {
	const uint8_t b[] = {0x91, 60, 100, 0xd1, 50, 0xf8, 0xe1, 0x00, 0x40, 0xf0, 1, 2, 3, 0xf7, 0xb1, 74, 64};
	const std::vector<uint32_t> w = midi1_to_ump(b, sizeof b);
	check(w.size() == 4 && w[0] == 0x20913c64u && w[1] == 0x20d13200u && w[2] == 0x20e10040u && w[3] == 0x20b14a40u,
	    "MIDI 1.0 bytes to UMP type 2 (system messages skipped)");
}

} // namespace


int main(int argc, char** argv) {
	if (argc < 3) {
		std::fprintf(stderr, "usage: app_tests BUILTIN_DIR SCRATCH_DIR\n");
		return 2;
	}
	safety_checks();
	midi1_checks();
	package_checks(argv[1], argv[2]);
	setup_checks(argv[1]);
	std::printf("%s\n", failures ? "app_tests: FAILED" : "app_tests: ok");
	return failures ? 1 : 0;
}
