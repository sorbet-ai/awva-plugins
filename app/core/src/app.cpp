// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The core (awva_app.h): the catalog, the sound, MIDI in, audio out and the settings, behind the C
// ABI the UI calls. Threads:
//   UI thread    every awva_* call (one thread): it builds instances, reads parameters (param() is
//                any-thread), and sends the audio thread commands through a ring
//   audio thread the backend's callback: it applies the commands, takes the MIDI rings' messages
//                at their frames, renders through the Player, and returns retired instances and
//                texts to the UI thread through another ring (no allocation, no locks)
//   MIDI threads the OS's: each port's messages into its own ring, timestamped
// A MIDI message plays from the next buffer rendered, at its first frame: the least delay the output
// allows (under one period from its arrival); the engine takes it at its next 2 ms step. A burst (more
// than StepSpread::kPerStep in a step: a late wake-up, a dense controller) goes on over the next
// steps, in order, so no step gets more than the engine's tables take (the board's sizes: 256 a
// step), however the buffers split it (step_spread.h).#include <algorithm>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <thread>

#include "app_core.h"

namespace awva_app {

int Core::start(const Json& o) {
	settings_path_ = o.has("settings") ? o.str("settings") : join(app_data_dir(), "settings.json");
	load_settings();
	builtin_dir_ = o.str("builtin_dir");
	if (builtin_dir_.empty()) {
		builtin_dir_ = join(core_dir(), "engines");
		if (list_dir(builtin_dir_, false).empty()) builtin_dir_ = join(join(core_dir(), ".."), "Resources/engines");
	}
	user_dir_ = o.str("engines_dir", join(app_data_dir(), "engines"));
	make_dirs(user_dir_);
	catalog_.scan(builtin_dir_, user_dir_);
	folder_ = Catalog::signature(user_dir_);

	no_audio_ = o.flag("no_audio", false);
	audio_ = no_audio_ ? make_null_audio(48000) : make_device_audio();
	AudioConfig cfg;
	cfg.device = setting("output_device"); // ("" the system's default)
	cfg.exclusive = setting("audio_mode") == "exclusive";
	cfg.frames = 256; // (exclusive: 256 frames, about 5 ms, unless chosen; 0 is the device's smallest)
	if (!setting("exclusive_buffer").empty()) cfg.frames = std::max(0, std::atoi(setting("exclusive_buffer").c_str()));
	if (o.has("simulate_volume_db")) cfg.simulate_db = static_cast<float>(o.num("simulate_volume_db", 0));
	if (no_audio_) { // (the null sink as a fake endpoint, tests: buffer_policy.h)
		cfg.simulate_bus = o.str("simulate_bus", "");
		cfg.simulate_wake_ms = o.num("simulate_wake_ms", 0);
		if (const Json* d = o.get("simulate_devices"))
			for (const Json& x : d->a) cfg.simulate_devices.push_back({x.str("id"), x.str("name"), x.str("bus", "other"), false});
	}
	// what earlier streams learned per endpoint: the exclusive buffer it sustains, or shared only
	for (const auto& [k, v] : settings_)
		if (k.rfind(kLearned, 0) == 0 && !v.empty()) cfg.learned[k.substr(std::strlen(kLearned))] = v == "shared" ? -1 : std::atoi(v.c_str());
	if (no_audio_ && o.has("null_period")) cfg.frames = static_cast<int>(o.num("null_period", 256)); // (the null sink's period, tests)
	audio_->configure(cfg);
	config_ = cfg;
	rate_ = audio_->probe_rate();
	if (rate_ <= 0) rate_ = 48000;
	player_.prepare(rate_);
	float db = kStartDefaultDb;
	if (!setting("master_db").empty()) db = std::min(static_cast<float>(std::atof(setting("master_db").c_str())), kStartMaxDb);
	player_.safety.set_target_db(db);
	player_.safety.fade_in(0.300f);
	master_db_ = db;

	// the sound: the last engine and preset, else the first engine
	std::string key = setting("engine");
	const Engine* e = catalog_.find(key);
	if (!e || !e->ok) {
		e = nullptr;
		for (const Engine& x : catalog_.engines())
			if (x.ok && !e) e = &x;
	}
	if (e) {
		select_engine(e->key);
		const int p = std::atoi(setting("preset").c_str());
		if (!setting("preset").empty() && setting("engine") == e->key && p >= 0 && p < static_cast<int>(e->presets.size())) select_preset(p);
	}

	const std::string probe = o.str("probe_wav");
	if (!probe.empty() && wav_.open(probe, rate_)) {
		probe_run_ = true;
		probe_th_ = std::thread([this] { probe_loop(); });
	}
	start_audio();
	if (o.flag("midi", true)) {
		midi_ = make_midi_in();
		std::set<std::string> off;
		for (const std::string& n : lines(setting("midi_off"))) off.insert(n);
		midi_->set_disabled(off);
		midi_->start([this](int slot, const uint32_t* w, int count, uint64_t ns) {
			MidiEv e{ns, {0, 0, 0, 0}};
			std::memcpy(e.w, w, sizeof(uint32_t) * static_cast<size_t>(std::min(count, 4)));
			(void)rings_[slot].push(e);
			midi_count_.fetch_add(1, std::memory_order_relaxed);
		});
	}
	return 0;
}
void Core::stop() {
	if (shut_) return;
	shut_ = true;
	if (midi_) midi_->stop();
	midi_.reset();
	stop_audio();
	apply_all();
	if (probe_run_) {
		probe_run_ = false;
		probe_th_.join();
		drain_probe();
		wav_.close();
	}
	drain_rets();
	delete player_.inst;
	player_.inst = nullptr;
}
std::string Core::setting(const std::string& k) const {
	auto it = settings_.find(k);
	return it == settings_.end() ? "" : it->second;
}
void Core::set_setting(const std::string& k, const std::string& v) {
	settings_[k] = v;
	save_settings();
}
void Core::erase_setting(const std::string& k) {
	if (settings_.erase(k)) save_settings();
}
void Core::save_settings() {
	if (settings_path_.empty()) return;
	std::string s = "{";
	for (const auto& [a, b] : settings_) s += std::string(s.size() > 1 ? "," : "") + "\n  " + jstr(a) + ": " + jstr(b);
	write_file(settings_path_, s + "\n}\n");
}
int Core::fail(const std::string& why) {
	error_ = why;
	return -1;
}
void Core::load_settings() {
	std::string t;
	Json j;
	if (settings_path_.empty() || !read_file(settings_path_, &t) || !Json::parse(t, &j)) return;
	for (const auto& [k, v] : j.o)
		if (v.type == Json::String) settings_[k] = v.s;
}
std::vector<std::string> Core::lines(const std::string& s) {
	std::vector<std::string> v;
	size_t a = 0;
	while (a < s.size()) {
		size_t b = s.find('\n', a);
		if (b == std::string::npos) b = s.size();
		if (b > a) v.push_back(s.substr(a, b - a));
		a = b + 1;
	}
	return v;
}

namespace {

std::unique_ptr<Core> g_core;
std::string g_text;

} // namespace

} // namespace awva_app

using awva_app::g_core;
using awva_app::g_text;

extern "C" {

AWVA_APP_API int32_t awva_abi(void) { return AWVA_APP_ABI; }

AWVA_APP_API int32_t awva_start(const char* options_json) {
	if (g_core) return -1;
	awva_app::Json o;
	if (options_json && *options_json) (void)awva_app::Json::parse(options_json, &o);
	g_core = std::make_unique<awva_app::Core>();
	return g_core->start(o);
}
AWVA_APP_API void awva_stop(void) {
	if (!g_core) return;
	g_core->stop();
	g_core.reset();
}
AWVA_APP_API const char* awva_catalog(void) {
	g_text = g_core ? g_core->catalog().json() : "[]";
	return g_text.c_str();
}
AWVA_APP_API const char* awva_engines_dir(void) {
	static std::string s;
	s = g_core ? g_core->engines_dir() : "";
	return s.c_str();
}
AWVA_APP_API int32_t awva_rescan(void) { return g_core ? g_core->rescan() : 0; }
AWVA_APP_API int32_t awva_select_engine(const char* key) { return g_core && key ? g_core->select_engine(key) : -1; }
AWVA_APP_API const char* awva_sound(void) {
	static std::string s;
	s = g_core ? g_core->sound_json() : "{}";
	return s.c_str();
}
AWVA_APP_API int32_t awva_select_preset(int32_t index) { return g_core ? g_core->select_preset(index) : -1; }
AWVA_APP_API int32_t awva_set_param(int32_t index, float value) { return g_core ? g_core->set_param(index, value) : -1; }
AWVA_APP_API float awva_param(int32_t index) { return g_core ? g_core->param(index) : 0.0f; }
AWVA_APP_API void awva_set_mono(int32_t on) {
	if (g_core) g_core->set_mono(on != 0);
}
AWVA_APP_API void awva_panic(void) {
	if (g_core) g_core->panic();
}
AWVA_APP_API void awva_set_master_db(float db) {
	if (g_core) g_core->set_master(db);
}
AWVA_APP_API void awva_status_get(awva_status* out) {
	if (!out) return;
	std::memset(out, 0, sizeof *out);
	if (g_core) g_core->status(out);
}
AWVA_APP_API const char* awva_midi_ports(void) {
	static std::string s;
	s = g_core ? g_core->midi_json() : "[]";
	return s.c_str();
}
AWVA_APP_API void awva_midi_refresh(void) {
	if (g_core) g_core->midi_refresh();
}
AWVA_APP_API void awva_midi_enable(const char* name, int32_t on) {
	if (g_core && name) g_core->midi_enable(name, on != 0);
}
AWVA_APP_API void awva_ump(const uint32_t* words, int32_t count) {
	if (g_core && words && count > 0) g_core->inject(words, count);
}
AWVA_APP_API void awva_midi1(const uint8_t* bytes, int32_t count) {
	if (!g_core || !bytes || count <= 0) return;
	const std::vector<uint32_t> w = awva_app::midi1_to_ump(bytes, static_cast<size_t>(count));
	if (!w.empty()) g_core->inject(w.data(), static_cast<int>(w.size()));
}
AWVA_APP_API const char* awva_audio(void) {
	static std::string s;
	s = g_core ? g_core->audio_json() : "{}";
	return s.c_str();
}
AWVA_APP_API int32_t awva_audio_restart(void) { return g_core ? g_core->restart_audio() : -1; }
AWVA_APP_API int32_t awva_set_audio_mode(const char* mode, int32_t frames) { return g_core && mode ? g_core->set_audio_mode(mode, frames) : -1; }
AWVA_APP_API const char* awva_audio_devices(void) {
	static std::string s;
	s = g_core ? g_core->devices_json() : "[]";
	return s.c_str();
}
AWVA_APP_API int32_t awva_set_audio_device(const char* id) { return g_core && id ? g_core->set_audio_device(id) : -1; }
AWVA_APP_API const char* awva_latency(void) {
	static std::string s;
	s = g_core ? g_core->latency_json() : "{}";
	return s.c_str();
}
AWVA_APP_API const char* awva_setting(const char* key) {
	static std::string s;
	s = g_core && key ? g_core->setting(key) : "";
	return s.c_str();
}
AWVA_APP_API void awva_set_setting(const char* key, const char* value) {
	if (g_core && key) g_core->set_setting(key, value ? value : "");
}
AWVA_APP_API const char* awva_last_error(void) {
	static std::string s;
	s = g_core ? g_core->error_ : "not started";
	return s.c_str();
}

} // extern "C"
