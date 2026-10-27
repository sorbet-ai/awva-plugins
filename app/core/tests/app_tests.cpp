// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The core's own checks (no audio device, no MIDI port):
//   app_tests BUILTIN_DIR SCRATCH_DIR
//   - the safety chain: below the ceiling the samples pass bit for bit (delayed); a signal 18 dB
//     over it comes out under -1 dBTP (its 4x-interpolated peaks); a NaN block is muted and counted;
//     the master level starts faded in
//   - packages (.awvapkg, awva-package/2): an image package, a presets-only package, and broken
//     ones (another awva, another computer, no such engine, a damaged section, the old format),
//     each listed as the README says
//   - the controller's MPE setup reaches a new engine: an MCM and a bend range sent before an engine
//     switch give the same output as sent to that engine directly
//   - MIDI 1.0 bytes to UMP
//   - the output chosen, on a fake endpoint list: listed with its bus, kept, missing at the start
//     (the default plays, a line says so), a switch faded in at -12 dB at most, the buffer its bus allows
#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <thread>
#include <string>
#include <vector>

#include "awva_app/awva_app.h"
#include "catalog.h"
#include "core.h"
#include "json.h"
#include "platform.h"
#include "player.h"
#include "nsc/load.h"
#include "nsc/package.h"
#include "buffer_policy.h"
#include "endpoint_gain.h"
#include "safety.h"
#include "step_spread.h"

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

// an .awvapkg (awva-package/2), written here as awva's packager writes one (nsc/package.h): the
// manifest, an image for each (tag, library) given, the presets (tag, text)
struct Image {
	std::string tag, path;
};
bool write_awvapkg(const std::string& out, const std::string& manifest, const std::vector<Image>& images,
    const std::vector<std::pair<std::string, std::string>>& presets) {
	struct Sec {
		uint32_t kind;
		std::string tag, bytes;
	};
	std::vector<Sec> secs = {{nscpkg::KIND_MANIFEST, "manifest", manifest}};
	for (const Image& im : images) {
		std::string lib;
		nsc_library l;
		if (!read_file(im.path, &lib, 64u << 20) || !nsc_load_check(im.path, &l)) return false;
		const nsc_desc& d = *l.c->desc;
		nscpkg::ModuleHeader h{};
		h.magic = nscpkg::MODULE_MAGIC;
		h.format = nscpkg::FORMAT;
		h.header_size = sizeof h;
		h.abi = d.abi;
		std::snprintf(h.arch, sizeof h.arch, "%s", im.tag.c_str());
		std::snprintf(h.toolchain, sizeof h.toolchain, "app_tests");
		h.image_size = static_cast<uint32_t>(lib.size());
		h.hot_bytes = d.hot_bytes, h.stream_bytes = d.stream_bytes, h.bulk_bytes = d.bulk_bytes;
		h.max_voices = d.max_voices, h.channels = d.channels, h.colour = d.colour, h.schema_hash = d.schema_hash;
		std::snprintf(h.id, sizeof h.id, "%s", d.id);
		std::snprintf(h.uid, sizeof h.uid, "%s", d.uid);
		std::snprintf(h.version, sizeof h.version, "%s", d.version ? d.version : "");
		h.payload_crc = nscpkg::crc32(0, lib.data(), lib.size());
		h.header_crc = nscpkg::crc32(0, &h, 252);
		secs.push_back({nscpkg::KIND_IMAGE, im.tag, std::string(reinterpret_cast<const char*>(&h), sizeof h) + lib});
	}
	for (const auto& [tag, text] : presets) secs.push_back({nscpkg::KIND_PRESET, tag, text});
	const uint32_t n = static_cast<uint32_t>(secs.size());
	std::string table(n * sizeof(nscpkg::Section), '\0'), body;
	uint32_t at = static_cast<uint32_t>(sizeof(nscpkg::PackageHeader) + table.size());
	for (uint32_t i = 0; i < n; i++) {
		while ((at + body.size()) % 4) body += '\0';
		nscpkg::Section s{};
		s.kind = secs[i].kind;
		std::snprintf(s.tag, sizeof s.tag, "%s", secs[i].tag.c_str());
		s.offset = at + static_cast<uint32_t>(body.size());
		s.size = static_cast<uint32_t>(secs[i].bytes.size());
		s.crc = nscpkg::crc32(0, secs[i].bytes.data(), secs[i].bytes.size());
		std::memcpy(&table[i * sizeof s], &s, sizeof s);
		body += secs[i].bytes;
	}
	nscpkg::PackageHeader ph{};
	ph.magic = nscpkg::PACKAGE_MAGIC;
	ph.format = nscpkg::FORMAT;
	ph.header_size = sizeof ph;
	ph.abi = NSC_ABI;
	ph.n_sections = n;
	ph.table_offset = sizeof ph;
	ph.file_size = at + static_cast<uint32_t>(body.size());
	ph.table_crc = nscpkg::crc32(0, table.data(), table.size());
	ph.header_crc = nscpkg::crc32(0, &ph, 60);
	return write_file(out, std::string(reinterpret_cast<const char*>(&ph), sizeof ph) + table + body);
}

std::string manifest(const std::string& label, const std::string& id, const std::string& awva, bool image) {
	return "{\"format\": \"awva-package/2\", \"label\": " + jstr(label) + ", \"id\": " + jstr(id) + ", \"awva\": " + jstr(awva) +
	    ", \"abi\": " + std::to_string(NSC_ABI) + ", \"images\": [" + (image ? jstr(host_tag()) : std::string()) + "]}";
}

void package_checks(const std::string& builtin, const std::string& scratch) {
	const std::string root = join(scratch, "engines");
	make_dirs(root);
	const std::string lib = join(builtin, std::string("va") + library_suffix());
	// a preset of va, written by the component itself; and va's factory preset 0 as its file (as a
	// package's image carries its presets/<id> files)
	std::string preset, factory0;
	{
		nsc_library l;
		if (nsc_load_check(lib, &l)) {
			Instance in(*l.c);
			in.init(48000);
			l.c->set_param(in.state, 0, l.c->desc->params[0].min);
			char buf[16384];
			preset.assign(buf, l.c->save_preset(in.state, "Package Sound", buf, sizeof buf));
			l.c->set_preset(in.state, 0);
			factory0.assign(buf, l.c->save_preset(in.state, l.c->desc->presets[0].name, buf, sizeof buf));
		}
	}
	const std::string pin = AWVA_APP_AWVA_COMMIT;
	// a library package: the built-in va as a package, its preset and another engine's
	write_awvapkg(join(root, "va-test.awvapkg"), manifest("va test (fit)", "va", pin.substr(0, 7), true), {{host_tag(), lib}},
	    {{"00-factory", factory0}, {"01-package-sound", preset}, {"02-not-va", "{\"format\": \"awva-preset/1\", \"engine\": \"bowed\", \"params\": {}}"}});
	// a presets-only package for the built-in va, and one whose file name comes after the library
	// package's (it binds to the built-in all the same, never to another package's library)
	write_awvapkg(join(root, "va-fit.awvapkg"), manifest("va fit v1", "va", "", false), {}, {{"01-fit", preset}});
	write_awvapkg(join(root, "va-zfit.awvapkg"), manifest("va fit v2", "va", "", false), {}, {{"01-fit", preset}});
	// refused ones
	write_awvapkg(join(root, "broken-base.awvapkg"), manifest("old base", "va", "1726a05", true), {{host_tag(), lib}}, {});
	write_awvapkg(join(root, "broken-target.awvapkg"), manifest("another computer", "va", pin, true), {{"armv7em-fpv5-d16-softfp", lib}}, {});
	write_awvapkg(join(root, "broken-engine.awvapkg"), manifest("no engine", "nosuchengine", "", false), {}, {{"01-fit", preset}});
	{
		std::string b;
		write_awvapkg(join(root, "broken-crc.awvapkg"), manifest("damaged", "va", pin, true), {{host_tag(), lib}}, {});
		read_file(join(root, "broken-crc.awvapkg"), &b, 64u << 20);
		b[b.size() / 2] ^= 0x5a; // (a byte of the image)
		write_file(join(root, "broken-crc.awvapkg"), b);
		write_file(join(root, "broken-format.awvapkg"), "{\"format\": \"awva-package/1\"}");
	}

	Catalog cat;
	cat.scan(builtin, root);
	const Engine* va = cat.find("builtin:va");
	const Engine* pa = cat.find("package:va-test");
	const Engine* pb = cat.find("package:va-fit");
	check(va && va->ok && va->label == "built in", "packages: the built-in va is listed, built in");
	check(pa && pa->ok && pa->label == "va test (fit)" && pa->c != va->c, "packages: an .awvapkg's image for this computer loads from its own copy, under its label");
	check(pa && pa->presets.size() == static_cast<size_t>(pa->c->desc->num_presets) + 1 && pa->presets.back().name == "Package Sound" &&
	          pa->refused.size() == 1,
	    "packages: its factory presets, then its preset sections by title (a factory preset's own file listed once); another engine's preset refused");
	check(pb && pb->ok && pb->c == va->c && pb->presets.size() == 1 && pb->label == "va fit v1", "packages: a presets-only package plays the built-in engine with its presets");
	const Engine* pz = cat.find("package:va-zfit");
	check(pz && pz->ok && pz->c == va->c && pz->presets.size() == 1,
	    "packages: a presets-only package after a library package of its engine binds to the built-in, its presets once");
	{
		const Engine* e = cat.find("package:broken-base");
		check(e && !e->ok && !e->c && e->error.rfind("built for a different awva", 0) == 0, "packages: an image built on another awva is refused unloaded: " + (e ? e->error : "?"));
	}
	for (const char* k : {"package:broken-target", "package:broken-engine", "package:broken-crc", "package:broken-format"}) {
		const Engine* e = cat.find(k);
		check(e && !e->ok && !e->c && !e->error.empty(), std::string("packages: ") + k + " is listed, refused: " + (e ? e->error : "?"));
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

	// a package written while the core runs: listed at the next refresh, by itself; one being
	// written (<name>.awvapkg.tmp) never
	awva_status st0;
	awva_status_get(&st0);
	const std::string tmp = join(root, "va-iter9.awvapkg.tmp");
	write_awvapkg(tmp, manifest("half written", "va", "", false), {}, {{"01-fit", preset}});
	awva_midi_refresh();
	check(std::string(awva_catalog()).find("half written") == std::string::npos, "a package being written (.tmp) isn't listed");
	std::rename(tmp.c_str(), join(root, "va-iter9.awvapkg").c_str());
	awva_midi_refresh();
	awva_status st1;
	awva_status_get(&st1);
	check(std::string(awva_catalog()).find("package:va-iter9") != std::string::npos && st1.catalog != st0.catalog,
	    "a package that appears while the core runs is listed at the next refresh, and the UI is told");
	// the package playing, its file removed: it keeps playing, the sound still there
	check(awva_select_engine("package:va-iter9") == 0, "the new package plays");
	std::remove(join(root, "va-iter9.awvapkg").c_str());
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

// exclusive mode's Windows volume (the null sink simulating it) against shared at that volume: a held
// sine note through the live path, the tapped level (shared: the mixer's volume comes after the tap)
double held_note_rms(const std::string& builtin, const std::string& scratch, const char* sim_db, const std::string& name) {
	const std::string wav = join(scratch, name);
	std::string o = "{\"no_audio\": true, \"midi\": false, \"settings\": \"\", \"builtin_dir\": " + jstr(builtin) + ", \"engines_dir\": " +
	    jstr(join(scratch, "none")) + ", \"probe_wav\": " + jstr(wav);
	if (sim_db) o += std::string(", \"simulate_volume_db\": ") + sim_db;
	o += "}";
	if (awva_start(o.c_str()) != 0 || awva_select_engine("builtin:sine") != 0) return -1;
	awva_set_master_db(-6.0f);
	const uint32_t on[2] = {0x40903c00u, 0xc0000000u}; // (MIDI 2.0 Note On, key 60)
	awva_ump(on, 2);
	std::this_thread::sleep_for(std::chrono::milliseconds(1500));
	awva_stop();
	std::string d;
	if (!read_file(wav, &d, 1u << 26) || d.size() < 44 + 8 * 24000) return -1;
	const float* s = reinterpret_cast<const float*>(d.data() + 44);
	const size_t frames = (d.size() - 44) / 8;
	double sq = 0;
	for (size_t i = frames - 24000; i < frames; i++) sq += static_cast<double>(s[2 * i]) * s[2 * i];
	return std::sqrt(sq / 24000);
}

void exclusive_volume_checks(const std::string& builtin, const std::string& scratch) {
	{
		EndpointGain g;
		g.reset(1.0f);
		g.set(endpoint_gain(-10.0f, 0.4f, false, false, false));
		std::vector<float> l(256, 1.0f), r(256, 1.0f);
		g.apply(l.data(), r.data(), 256);
		const float want = std::pow(10.0f, -10.0f / 20.0f);
		check(std::fabs(l[255] - want) < 1e-6f && l[0] > want && l[0] < 1.0f, "exclusive volume: a change is ramped over a buffer, to the mixer's gain");
		check(endpoint_gain(-10.0f, 0.0f, false, false, false) == 0.0f && endpoint_gain(-10.0f, 0.5f, true, false, false) == 0.0f &&
		          endpoint_gain(-10.0f, 0.5f, true, true, true) == 1.0f,
		    "exclusive volume: the slider at 0 and mute are silence; a device that does it in hardware is left alone");
	}
	const double shared = held_note_rms(builtin, scratch, nullptr, "shared.wav");
	const double excl = held_note_rms(builtin, scratch, "-10.46", "exclusive.wav");
	const double diff = 20 * std::log10(excl / shared), want = -10.46;
	char b[200];
	std::snprintf(b, sizeof b, "exclusive at -10.46 dB (Windows slider 0.3) measures as shared at that volume: shared %.2f dBFS - 10.46 dB vs exclusive %.2f dBFS (%.3f dB apart)",
	    20 * std::log10(shared), 20 * std::log10(excl), diff - want);
	check(shared > 1e-3 && std::fabs(diff - want) < 0.01, b);
}

// a burst of MIDI (a late wake-up after a dense controller: 10 notes, 880 pressures, their Note
// Offs) as the audio thread takes it, placed on the steps (step_spread.h) and played by the built-in
// va through the Player, block by block: no sleeps, no threads. It comes in one buffer, and split
// over buffers as when it arrives while the audio thread is taking it (the first part spread past
// the next buffers' starts: the second part used to pile onto its last step, past the 256 a block
// the engine's tables take, and a Note Off there was dropped). No step gets more than kPerStep,
// the order is kept, every Note Off is taken.
void burst_checks(const std::string& builtin) {
	nsc_library l;
	if (!nsc_load_check(join(builtin, std::string("va") + library_suffix()), &l)) return check(false, "burst: va loads");
	std::vector<uint32_t> w;
	for (int n = 0; n < 10; n++) w.push_back(0x20900000u | (1u + n) << 16 | static_cast<uint32_t>(50 + n) << 8 | 100u); // (Note On, channels 2..11)
	for (int k = 0; k < 88; k++)
		for (int n = 0; n < 10; n++) w.push_back(0x20d00000u | (1u + n) << 16 | static_cast<uint32_t>(40 + k % 60) << 8); // (pressure)
	for (int n = 0; n < 10; n++) w.push_back(0x20800000u | (1u + n) << 16 | static_cast<uint32_t>(50 + n) << 8 | 64u); // (Note Off)
	const int total = static_cast<int>(w.size());
	struct Split {
		const char* what;
		std::vector<int> parts; // messages taken by each buffer, in turn
	};
	const Split splits[] = {
	    {"in one buffer", {total}},
	    {"in two buffers", {600, total - 600}},
	    {"700, then 70 a buffer", {700, 70, 70, total - 840}},
	    {"64 a buffer", std::vector<int>((total + 63) / 64, 64)},
	};
	const uint64_t step = 96, period = 256; // (48 kHz: the 2 ms step; the null sink's buffer)
	for (const Split& sp : splits) {
		Player p;
		p.prepare(48000);
		p.safety.set_now_db(0.0f);
		Instance in(*l.c);
		in.init(48000);
		p.inst = &in;
		StepSpread spread;
		spread.reset(in.frame());
		std::vector<uint64_t> frames;
		std::vector<float> outl(period), outr(period);
		int at = 0, most = 0;
		bool ordered = true;
		for (size_t b = 0; b < 48000 * 3 / period; b++) {
			const int take = b < sp.parts.size() ? std::min(sp.parts[b], total - at) : 0;
			for (int i = 0; i < take; i++, at++) {
				const uint64_t f = spread.place(in.frame(), step);
				ordered = ordered && (frames.empty() || f >= frames.back());
				frames.push_back(f);
				const uint32_t m[4] = {w[static_cast<size_t>(at)], 0, 0, 0};
				p.event(f, m);
			}
			(void)p.render(outl.data(), outr.data(), static_cast<uint32_t>(period));
		}
		for (size_t i = 0; i < frames.size();) { // the most messages in one step's frames
			size_t j = i;
			while (j < frames.size() && frames[j] < frames[i] + step) j++;
			most = std::max(most, static_cast<int>(j - i));
			i = j;
		}
		const int left = in.c.active_voices(in.state);
		p.inst = nullptr;
		char s[220];
		std::snprintf(s, sizeof s, "burst of %d messages %s: at most %d a step (%d), in order, every Note Off taken (voices %d after 3 s)", total,
		    sp.what, most, StepSpread::kPerStep, left);
		check(at == total && ordered && most <= StepSpread::kPerStep && left == 0, s);
	}
	nsc_unload(&l);
}

// a Bluetooth endpoint's exclusive buffer (buffer_policy.h): the bus from the endpoint's devnode and
// topology; the buffer raised on its glitches, or shared; a wired or USB endpoint's exactly as asked
void bluetooth_policy_checks() {
	check(bus_of("BTHENUM", "{2}.\\\\?\\bthenum#{0000110b-0000-1000-8000-00805f9b34fb}_localmfg&0002#7&1a2b3c4d&0&001122334455_c00000000#{6994ad04-93ef-11d0-a3cc-00a0c9223196}\\src") ==
	              Bus::Bluetooth &&
	          bus_of("", "{2}.\\\\?\\bthhfenum#bthhfpaudio#8&2b8a0b4&0&97#{6994ad04-93ef-11d0-a3cc-00a0c9223196}\\singlelinewaveout") == Bus::Bluetooth &&
	          bus_of("BTHLEDevice", "") == Bus::Bluetooth && bus_of("USB", "{2}.\\\\?\\usb#vid_1235&pid_8211&mi_00#9&1c2b3d4&0&0000#{6994ad04-93ef-11d0-a3cc-00a0c9223196}\\speaker") == Bus::Usb &&
	          bus_of("HDAUDIO", "{2}.\\\\?\\hdaudio#func_01&ven_10ec&dev_0257#4&2fd1ab66&0&0001#{6994ad04-93ef-11d0-a3cc-00a0c9223196}\\elineoutwave") == Bus::Other &&
	          bus_of("", "") == Bus::Other,
	    "endpoints: Bluetooth (A2DP, hands-free, LE), USB and wired told apart by their bus drivers' names");
	{
		EndpointBuffer w;
		w.endpoint("usb", Bus::Usb, 0);
		bool none = w.frames(144, 144, 480) == 144 && w.frames(0, 144, 480) == 144 && w.frames(64, 144, 480) == 144 && w.frames(256, 144, 480) == 256;
		for (uint64_t t = 0; t < 10; t++) none = none && w.glitch(t * 1000000ull, 144) == EndpointBuffer::Action::None;
		check(none && !w.shared_only() && w.learned() == 0 && w.note(true).empty(),
		    "endpoints: a wired or USB endpoint's exclusive buffer is the one asked for (the device's smallest at least), glitches or not");
	}
	{
		EndpointBuffer bt;
		bt.endpoint("bt", Bus::Bluetooth, 0);
		const bool start = bt.frames(144, 144, 480) == 480 && bt.frames(0, 144, 480) == 480 && bt.frames(1024, 144, 480) == 1024;
		bt.frames(144, 144, 480);
		const auto a1 = bt.glitch(1000000000ull, 480), a2 = bt.glitch(1500000000ull, 480);
		const bool raised = a1 == EndpointBuffer::Action::None && a2 == EndpointBuffer::Action::StepUp && bt.learned() == 960 && bt.frames(144, 144, 480) == 960;
		bt.endpoint("bt", Bus::Bluetooth, 0); // (the stream opened again: the count starts over, what was learned stays)
		const auto b1 = bt.glitch(100000000000ull, 960), b2 = bt.glitch(131000000000ull, 960); // (31 s apart: not a rate)
		const auto b3 = bt.glitch(132000000000ull, 960);
		const bool shared = b1 == EndpointBuffer::Action::None && b2 == EndpointBuffer::Action::None && b3 == EndpointBuffer::Action::Shared && bt.shared_only() &&
		    bt.note(true).find("playing shared") != std::string::npos && bt.note(false).empty();
		EndpointBuffer again;
		again.endpoint("bt", Bus::Bluetooth, -1);
		check(start && raised && shared && again.shared_only(),
		    "endpoints: a Bluetooth endpoint starts at its default period, 2 glitches in 30 s double the buffer, past twice the period it plays shared; "
		    "what it learned is taken by the next start");
	}
}

// the null sink as a fake endpoint on a bus, waking at most every wake_ms; exclusive at 144 frames
// asked for; the audio state after a second
Json fake_endpoint(const std::string& builtin, const std::string& scratch, const char* bus, int wake_ms, const std::string& settings, bool ask) {
	const std::string o = "{\"no_audio\": true, \"midi\": false, \"settings\": " + jstr(settings) + ", \"builtin_dir\": " + jstr(builtin) +
	    ", \"engines_dir\": " + jstr(join(scratch, "none")) + ", \"simulate_bus\": " + jstr(bus) + ", \"simulate_wake_ms\": " + std::to_string(wake_ms) + "}";
	Json j;
	if (awva_start(o.c_str()) != 0 || awva_select_engine("builtin:sine") != 0) return j;
	if (ask) awva_set_audio_mode("exclusive", 144);
	std::this_thread::sleep_for(std::chrono::milliseconds(1000));
	Json lat;
	if (!Json::parse(awva_latency(), &lat) || !Json::parse(awva_audio(), &j)) j = Json{};
	if (j.type == Json::Object) {
		j.o["latency_buffer_frames"] = lat.o["buffer_frames"];
		j.o["glitches"] = lat.o["glitches"];
		j.o["recoveries"] = lat.o["recoveries"];
	}
	awva_stop();
	return j;
}

void bluetooth_checks(const std::string& builtin, const std::string& scratch) {
	const std::string settings = join(scratch, "bt-settings.json");
	std::remove(settings.c_str());
	// a Bluetooth endpoint waking every 20 ms: 480 frames (10 ms) come late, 960 (20 ms) play
	Json a = fake_endpoint(builtin, scratch, "bluetooth", 20, settings, true);
	char s[400];
	std::snprintf(s, sizeof s, "Bluetooth, wakes every 20 ms: exclusive asked at 144 frames plays %s at %d frames (latency's buffer %d), learned %d, \"%s\"",
	    a.str("mode").c_str(), static_cast<int>(a.num("buffer", 0)), static_cast<int>(a.num("latency_buffer_frames", 0)), static_cast<int>(a.num("learned", 0)),
	    a.str("message").c_str());
	check(a.str("mode") == "exclusive" && a.num("buffer", 0) == 960 && a.num("latency_buffer_frames", 0) == 960 && a.num("learned", 0) == 960 &&
	          a.str("bus") == "bluetooth" && a.str("message").empty() && a.num("recoveries", 0) == 1,
	    s);
	std::string saved;
	read_file(settings, &saved, 1u << 16);
	check(saved.find("\"exclusive_buffer_learned:fake-endpoint\": \"960\"") != std::string::npos, "Bluetooth: the buffer it sustains is kept in the settings");
	// the next start: at 960 at once, no glitch
	Json b = fake_endpoint(builtin, scratch, "bluetooth", 20, settings, false);
	std::snprintf(s, sizeof s, "Bluetooth: the next start plays %s at %d frames at once (%d glitches)", b.str("mode").c_str(), static_cast<int>(b.num("buffer", 0)),
	    static_cast<int>(b.num("glitches", -1)));
	check(b.str("mode") == "exclusive" && b.num("buffer", 0) == 960 && b.num("glitches", -1) == 0, s);
	// one waking every 40 ms: no buffer up to twice its period plays: shared, with the notice
	std::remove(settings.c_str());
	Json c = fake_endpoint(builtin, scratch, "bluetooth", 40, settings, true);
	std::snprintf(s, sizeof s, "Bluetooth, wakes every 40 ms: plays %s at %d frames, wanted %s, learned %d, \"%s\"", c.str("mode").c_str(),
	    static_cast<int>(c.num("buffer", 0)), c.str("wanted").c_str(), static_cast<int>(c.num("learned", 0)), c.str("message").c_str());
	check(c.str("mode") == "shared" && c.str("wanted") == "exclusive" && c.num("buffer", 0) == 480 && c.num("learned", 0) == -1 &&
	          c.str("message") == "Bluetooth output: exclusive mode glitched up to 960 frames, playing shared",
	    s);
	read_file(settings, &saved, 1u << 16);
	check(saved.find("\"exclusive_buffer_learned:fake-endpoint\": \"shared\"") != std::string::npos, "Bluetooth: shared only is kept in the settings");
	// a USB endpoint waking every 20 ms: exactly as asked (144 frames), its glitches counted, nothing raised
	std::remove(settings.c_str());
	Json d = fake_endpoint(builtin, scratch, "usb", 20, settings, true);
	std::snprintf(s, sizeof s, "USB, wakes every 20 ms: plays %s at %d frames as asked, %d glitches counted, %d re-opened, learned %d", d.str("mode").c_str(),
	    static_cast<int>(d.num("buffer", 0)), static_cast<int>(d.num("glitches", 0)), static_cast<int>(d.num("recoveries", -1)), static_cast<int>(d.num("learned", -9)));
	check(d.str("mode") == "exclusive" && d.num("buffer", 0) == 144 && d.num("glitches", 0) > 10 && d.num("recoveries", -1) == 0 && d.num("learned", -9) == 0 &&
	          d.str("message").empty(),
	    s);
	std::remove(settings.c_str());
}

// the output chosen (a fake endpoint list: no device opened): listed with its bus; the choice kept
// for the next start; a missing one plays the default and says so; a switch opens the stream again
// on the new output, faded in at -12 dB at most, its buffer as that output's bus allows
void output_device_checks(const std::string& builtin, const std::string& scratch) {
	const std::string settings = join(scratch, "device-settings.json");
	std::remove(settings.c_str());
	const std::string list = "[{\"id\": \"speakers\", \"name\": \"Speakers\", \"bus\": \"other\"}, {\"id\": \"usb-dac\", \"name\": \"USB DAC\", \"bus\": \"usb\"},"
	                         " {\"id\": \"buds\", \"name\": \"Buds\", \"bus\": \"bluetooth\"}]";
	auto start = [&](const std::string& devices) {
		const std::string o = "{\"no_audio\": true, \"midi\": false, \"settings\": " + jstr(settings) + ", \"builtin_dir\": " + jstr(builtin) +
		    ", \"engines_dir\": " + jstr(join(scratch, "none")) + ", \"simulate_devices\": " + devices + "}";
		return awva_start(o.c_str()) == 0 && awva_select_engine("builtin:sine") == 0;
	};
	auto audio = [] {
		Json j;
		if (!Json::parse(awva_audio(), &j)) j = Json{};
		return j;
	};
	if (!start(list)) return check(false, "outputs: the core starts");
	Json d;
	Json::parse(awva_audio_devices(), &d);
	const bool listed = d.a.size() == 3 && d.a[0].str("name") == "Speakers" && d.a[0].flag("default", false) && d.a[1].str("bus") == "usb" &&
	    d.a[2].str("bus") == "bluetooth" && !d.a[2].flag("default", true);
	Json a = audio();
	check(listed && a.str("device") == "Speakers" && a.str("chosen").empty() && !a.flag("missing", true),
	    "outputs: every endpoint listed with its bus and the default; the default plays when none is chosen");
	awva_set_master_db(0.0f);
	awva_set_audio_mode("exclusive", 144);
	const bool usb = awva_set_audio_device("usb-dac") == 0;
	awva_status st;
	awva_status_get(&st);
	a = audio();
	char s[300];
	std::snprintf(s, sizeof s, "outputs: the USB DAC chosen plays %s at %d frames on %s; the level %.1f dB", a.str("device").c_str(), static_cast<int>(a.num("buffer", 0)),
	    a.str("bus").c_str(), st.master_db);
	check(usb && a.str("device") == "USB DAC" && a.str("bus") == "usb" && a.str("mode") == "exclusive" && a.num("buffer", 0) == 144 && st.master_db == -12.0f, s);
	const bool bt = awva_set_audio_device("buds") == 0;
	a = audio();
	std::snprintf(s, sizeof s, "outputs: Bluetooth buds chosen: exclusive at %d frames (the device's period at least)", static_cast<int>(a.num("buffer", 0)));
	check(bt && a.str("device") == "Buds" && a.str("bus") == "bluetooth" && a.num("buffer", 0) == 480, s);
	check(awva_set_audio_device("nosuch") != 0 && audio().str("device") == "Buds", "outputs: an output not listed is refused");
	awva_stop();
	std::string saved;
	read_file(settings, &saved, 1u << 16);
	check(saved.find("\"output_device\": \"buds\"") != std::string::npos, "outputs: the choice is kept in the settings");
	// the next start, the buds gone: the default plays, and says so
	if (!start("[{\"id\": \"speakers\", \"name\": \"Speakers\", \"bus\": \"other\"}]")) return check(false, "outputs: the core starts again");
	a = audio();
	std::snprintf(s, sizeof s, "outputs: the saved output missing at the start: plays %s, \"%s\"", a.str("device").c_str(), a.str("notice").c_str());
	check(a.str("device") == "Speakers" && a.flag("missing", false) && a.str("chosen") == "buds" && a.str("notice") == "Buds isn't connected: playing the default", s);
	check(awva_set_audio_device("") == 0 && !audio().flag("missing", true) && audio().str("notice").empty(), "outputs: the system default chosen again");
	awva_stop();
	std::remove(settings.c_str());
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
	exclusive_volume_checks(argv[1], argv[2]);
	burst_checks(argv[1]);
	bluetooth_policy_checks();
	bluetooth_checks(argv[1], argv[2]);
	output_device_checks(argv[1], argv[2]);
	std::printf("%s\n", failures ? "app_tests: FAILED" : "app_tests: ok");
	return failures ? 1 : 0;
}
