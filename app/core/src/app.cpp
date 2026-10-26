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
// A MIDI message's frame: the buffer's first frame + one buffer + (its time - the buffer's time), so
// the timing between messages is kept to the frame, one buffer later, whatever the buffer size.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "audio.h"
#include "awva_app/awva_app.h"
#include "catalog.h"
#include "core.h"
#include "json.h"
#include "midi.h"
#include "platform.h"
#include "player.h"
#include "wav.h"

namespace awva_app {

namespace {

struct Cmd {
	enum T : int { SetParam, SetPreset, LoadText, Legato, Panic, Swap, Master } t;
	int i = 0;
	float v = 0;
	void* p = nullptr;
};
struct Ret {
	enum T : int { Inst, Text } t;
	void* p;
};
struct MidiEv {
	uint64_t ns;
	uint32_t w[4];
};
struct Frame {
	float l, r;
};

void atomic_max(std::atomic<float>& a, float v) {
	float cur = a.load(std::memory_order_relaxed);
	while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
	}
}
void atomic_min(std::atomic<float>& a, float v) {
	float cur = a.load(std::memory_order_relaxed);
	while (v < cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
	}
}

class Core : public AudioCallback {
public:
	// --- the UI thread ---------------------------------------------------------------------------
	int start(const Json& o) {
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

		no_audio_ = o.flag("no_audio", false);
		audio_ = no_audio_ ? make_null_audio(48000) : make_device_audio();
		rate_ = audio_->probe_rate();
		if (rate_ <= 0) rate_ = 48000;
		player_.prepare(rate_);
		float db = kStartDefaultDb;
		if (!setting("master_db").empty()) db = std::min(static_cast<float>(std::atof(setting("master_db").c_str())), kStartMaxDb);
		player_.safety.set_target_db(db);
		player_.safety.fade_in(0.300f);

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
	void stop() {
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

	const Catalog& catalog() const { return catalog_; }
	const std::string& engines_dir() const { return user_dir_; }
	int rescan() {
		catalog_.scan(builtin_dir_, user_dir_);
		// (the engine playing keeps playing; its entry is found again by key)
		engine_ = catalog_.find(engine_key_);
		if (engine_ && preset_ >= static_cast<int>(engine_->presets.size())) preset_ = -1;
		return static_cast<int>(catalog_.engines().size());
	}

	int select_engine(const std::string& key) {
		drain_rets();
		const Engine* e = catalog_.find(key);
		if (!e || !e->ok) return fail("no such engine, or it was refused: " + key);
		auto* in = new Instance(*e->c);
		if (in->init(rate_) != 0) {
			delete in;
			return fail("this engine doesn't run at " + std::to_string(rate_) + " Hz");
		}
		engine_ = e;
		engine_key_ = key;
		preset_ = -1;
		const int fp = in->c.preset(in->state);
		for (size_t i = 0; i < e->presets.size(); i++)
			if (e->presets[i].factory >= 0 && e->presets[i].factory == fp) preset_ = static_cast<int>(i);
		// a package of preset files only: its first preset (not live yet: set here)
		if (preset_ < 0 && !e->presets.empty() && e->presets[0].factory < 0) {
			const std::string& t = e->presets[0].text;
			(void)in->c.load_preset(in->state, t.data(), static_cast<uint32_t>(t.size()), nullptr);
			preset_ = 0;
		}
		ui_ = in;
		legato_ = false;
		send({Cmd::Swap, e->key.rfind("package:", 0) == 0 && e->c != builtin_of(e) ? 1 : 0, 0, in});
		set_setting("engine", key);
		set_setting("preset", std::to_string(preset_));
		return 0;
	}
	int select_preset(int i) {
		if (!engine_ || !ui_ || i < 0 || i >= static_cast<int>(engine_->presets.size())) return fail("no such preset");
		const PresetEntry& p = engine_->presets[static_cast<size_t>(i)];
		if (p.factory >= 0) send({Cmd::SetPreset, p.factory, 0, nullptr});
		else send({Cmd::LoadText, 0, 0, new std::string(p.text)});
		preset_ = i;
		set_setting("preset", std::to_string(i));
		return 0;
	}
	int set_param(int i, float v) {
		if (!ui_ || i < 0 || i >= ui_->d.num_params) return fail("no such parameter");
		send({Cmd::SetParam, i, v, nullptr});
		return 0;
	}
	float param(int i) const { return ui_ && i >= 0 && i < ui_->d.num_params ? ui_->c.param(ui_->state, i) : 0.0f; }
	void set_legato(bool on) {
		legato_ = on;
		send({Cmd::Legato, on ? 1 : 0, 0, nullptr});
	}
	void panic() { send({Cmd::Panic, 0, 0, nullptr}); }
	void set_master(float db) {
		db = std::clamp(db, kMasterMinDb, kMasterMaxDb);
		master_db_ = db;
		send({Cmd::Master, 0, db, nullptr});
		set_setting("master_db", jnum(db));
	}

	std::string sound_json() {
		drain_rets();
		if (!engine_ || !ui_) return "{}";
		const nsc_desc& d = ui_->d;
		std::string s = "{\"engine\":{\"key\":" + jstr(engine_->key) + ",\"label\":" + jstr(engine_->label) + ",\"id\":" + jstr(d.id) +
		    ",\"title\":" + jstr(d.name) + ",\"version\":" + jstr(d.version ? d.version : "") + ",\"summary\":" + jstr(d.summary ? d.summary : "") +
		    ",\"builtin\":" + (engine_->key.rfind("builtin:", 0) == 0 ? "true" : "false") + "}";
		s += ",\"presets\":[";
		for (size_t i = 0; i < engine_->presets.size(); i++)
			s += std::string(i ? "," : "") + "{\"name\":" + jstr(engine_->presets[i].name) + ",\"factory\":" + (engine_->presets[i].factory >= 0 ? "true" : "false") + "}";
		s += "],\"preset\":" + std::to_string(preset_) + ",\"heroes\":[";
		bool first = true;
		for (const std::string& h : heroes()) {
			const int k = ui_->param_index(h.c_str());
			if (k < 0) continue;
			s += (first ? "" : ",") + std::to_string(k);
			first = false;
		}
		s += "],\"params\":[";
		for (int i = 0; i < d.num_params; i++) {
			const nsc_param& p = d.params[i];
			s += std::string(i ? "," : "") + "{\"name\":" + jstr(p.name) + ",\"group\":" + jstr(p.group ? p.group : "") + ",\"min\":" + jnum(p.min) +
			    ",\"max\":" + jnum(p.max) + ",\"kind\":" + std::to_string(p.kind) + ",\"def\":" + jnum(p.def) + ",\"value\":" + jnum(param(i)) + "}";
		}
		s += "],\"legato\":" + std::string(legato_ ? "true" : "false") + "}";
		return s;
	}

	void status(awva_status* o) {
		o->peak_l = peak_l_.exchange(0.0f);
		o->peak_r = peak_r_.exchange(0.0f);
		const float g = min_gain_.exchange(1.0f);
		o->reduction_db = g < 1.0f ? -20.0f * std::log10(std::max(g, 1e-6f)) : 0.0f;
		o->master_db = master_db_;
		o->cpu = cpu_.load();
		o->voices = voices_.load();
		const AudioInfo i = audio_info();
		o->audio_state = i.state;
		o->sample_rate = i.rate;
		o->buffer_frames = i.buffer;
		o->midi_events = midi_count_.load();
		o->generation = generation_.load();
		o->xruns = xruns_.load();
		o->faults = faults_.load();
		o->engine_stopped = stopped_.load();
	}

	std::string midi_json() {
		std::string s = "[";
		if (midi_)
			for (const MidiPort& p : midi_->ports()) {
				if (s.size() > 1) s += ",";
				s += "{\"name\":" + jstr(p.name) + ",\"enabled\":" + (p.enabled ? "true" : "false") + ",\"state\":" + jstr(p.state) +
				    ",\"message\":" + jstr(p.message) + "}";
			}
		return s + "]";
	}
	void midi_refresh() {
		drain_rets();
		if (midi_) midi_->refresh();
	}
	void midi_enable(const std::string& name, bool on) {
		if (!midi_) return;
		std::set<std::string> off = midi_->disabled();
		if (on) off.erase(name);
		else off.insert(name);
		midi_->set_disabled(off);
		std::string v;
		for (const std::string& n : off) v += n + "\n";
		set_setting("midi_off", v);
		midi_->refresh();
	}
	void inject(const uint32_t* w, int count) {
		const uint64_t t = now_ns();
		for (int i = 0; i < count;) {
			const int n = std::min(ump_words(w[i]), count - i);
			MidiEv e{t, {0, 0, 0, 0}};
			std::memcpy(e.w, w + i, sizeof(uint32_t) * static_cast<size_t>(n));
			(void)rings_[MidiIn::kSlots].push(e);
			midi_count_.fetch_add(1, std::memory_order_relaxed);
			i += n;
		}
	}

	AudioInfo audio_info() const {
		AudioInfo i = fallback_ ? fallback_->info() : (audio_ ? audio_->info() : AudioInfo{});
		if (fallback_) {
			const AudioInfo d = audio_->info();
			i.state = AWVA_AUDIO_FAILED;
			i.message = d.message;
			i.backend = d.backend;
		}
		return i;
	}
	std::string audio_json() const {
		const AudioInfo i = audio_info();
		static const char* names[] = {"off", "running", "null", "failed"};
		return "{\"backend\":" + jstr(i.backend) + ",\"device\":" + jstr(i.device) + ",\"state\":" + jstr(names[std::clamp(i.state, 0, 3)]) +
		    ",\"message\":" + jstr(i.message) + ",\"rate\":" + std::to_string(i.rate) + ",\"buffer\":" + std::to_string(i.buffer) + "}";
	}
	int restart_audio() {
		stop_audio();
		apply_all(); // (the audio thread is stopped: the UI thread applies what it hadn't)
		drain_rets();
		const int r = audio_->probe_rate();
		if (r > 0 && r != rate_) {
			if (player_.inst && player_.inst->reinit(r) != 0) return fail("the engine doesn't run at " + std::to_string(r) + " Hz");
			rate_ = r;
		}
		player_.prepare(rate_);
		player_.safety.fade_in(0.300f);
		start_audio();
		return 0;
	}

	std::string setting(const std::string& k) const {
		auto it = settings_.find(k);
		return it == settings_.end() ? "" : it->second;
	}
	void set_setting(const std::string& k, const std::string& v) {
		settings_[k] = v;
		if (settings_path_.empty()) return;
		std::string s = "{";
		for (const auto& [a, b] : settings_) s += std::string(s.size() > 1 ? "," : "") + "\n  " + jstr(a) + ": " + jstr(b);
		write_file(settings_path_, s + "\n}\n");
	}

	int fail(const std::string& why) {
		error_ = why;
		return -1;
	}
	std::string error_;

	// --- the audio thread --------------------------------------------------------------------------
	void render(float* l, float* r, uint32_t n, uint64_t host_ns) override {
		const uint64_t t0 = now_ns();
		Cmd c;
		while (cmds_.pop(&c)) apply(c);
		// MIDI: every ring's messages at their frames, in time order
		const uint64_t base = player_.inst ? player_.inst->frame() : 0;
		int k = 0;
		for (auto& ring : rings_) {
			MidiEv e;
			while (k < kBatch && ring.pop(&e)) {
				const double dt = (static_cast<double>(e.ns) - static_cast<double>(host_ns)) * rate_ / 1e9;
				const int64_t f = static_cast<int64_t>(base) + n + static_cast<int64_t>(std::floor(dt));
				batch_[k].f = static_cast<uint64_t>(std::clamp<int64_t>(f, static_cast<int64_t>(base), static_cast<int64_t>(base) + 2 * n));
				std::memcpy(batch_[k].w, e.w, sizeof e.w);
				k++;
			}
		}
		std::stable_sort(batch_, batch_ + k, [](const Batched& a, const Batched& b) { return a.f < b.f; });
		for (int i = 0; i < k; i++) {
			const uint64_t f = std::max(batch_[i].f, last_frame_);
			last_frame_ = f;
			player_.event(f, batch_[i].w);
		}
		const Safety::Result res = player_.render(l, r, n);
		atomic_max(peak_l_, res.peak_l);
		atomic_max(peak_r_, res.peak_r);
		atomic_min(min_gain_, res.min_gain);
		if (res.fault) faults_.fetch_add(1);
		stopped_.store(player_.stopped ? 1 : 0);
		voices_.store(player_.inst ? player_.inst->c.active_voices(player_.inst->state) : 0);
		if (probe_run_)
			for (uint32_t i = 0; i < n; i++) (void)probe_.push({l[i], r[i]});
		const float load = static_cast<float>(static_cast<double>(now_ns() - t0) * rate_ / (1e9 * n));
		cpu_.store(std::max(load, cpu_.load() * 0.995f));
		if (last_host_ns_ && host_ns - last_host_ns_ > static_cast<uint64_t>(2.5e9 * n / rate_)) xruns_.fetch_add(1);
		last_host_ns_ = host_ns;
	}

private:
	void apply(const Cmd& c) {
		Instance* in = player_.inst;
		switch (c.t) {
		case Cmd::Swap: {
			player_.strict = c.i != 0;
			Instance* old = player_.swap(static_cast<Instance*>(c.p));
			last_frame_ = player_.inst ? player_.inst->frame() : 0;
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
		case Cmd::Legato: in->set_legato(c.i != 0); break;
		case Cmd::Panic:
			in->c.reset(in->state);
			in->count = 0;
			break;
		default: break;
		}
	}
	// the UI thread, with the audio thread stopped
	void apply_all() {
		Cmd c;
		while (cmds_.pop(&c)) apply(c);
	}
	void send(const Cmd& c) {
		for (int k = 0; !cmds_.push(c); k++) {
			if (k > 200) return; // (the audio thread isn't taking them: dropped)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}
	void drain_rets() {
		Ret r;
		while (rets_.pop(&r)) {
			if (r.t == Ret::Inst) delete static_cast<Instance*>(r.p);
			else delete static_cast<std::string*>(r.p);
		}
	}
	// the built-in engine's library a presets-only package plays (not stopped like a package's own)
	const nsc_component* builtin_of(const Engine* e) const {
		for (const Engine& b : catalog_.engines())
			if (b.key.rfind("builtin:", 0) == 0 && b.c == e->c) return b.c;
		return nullptr;
	}
	std::vector<std::string> heroes() const {
		std::vector<std::string> v;
		const nsc_desc& d = ui_->d;
		const PresetEntry* p = engine_ && preset_ >= 0 && preset_ < static_cast<int>(engine_->presets.size()) ? &engine_->presets[static_cast<size_t>(preset_)] : nullptr;
		if (p && p->factory < 0 && !p->heroes.empty()) return p->heroes;
		const char* const* h = d.heroes;
		if (p && p->factory >= 0 && d.presets[p->factory].heroes[0]) h = d.presets[p->factory].heroes;
		for (int i = 0; i < NSC_MAX_HEROES && h[i]; i++) v.push_back(h[i]);
		return v;
	}
	void start_audio() {
		fallback_.reset();
		if (!audio_->start(this)) {
			fallback_ = make_null_audio(rate_); // (no device: the sound keeps running without one)
			fallback_->start(this);
		}
	}
	void stop_audio() {
		if (fallback_) fallback_->stop();
		fallback_.reset();
		audio_->stop();
	}
	void probe_loop() {
		while (probe_run_) {
			drain_probe();
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
	}
	void drain_probe() {
		Frame f;
		while (probe_.pop(&f)) wav_.write(&f.l, &f.r, 1);
	}
	void load_settings() {
		std::string t;
		Json j;
		if (settings_path_.empty() || !read_file(settings_path_, &t) || !Json::parse(t, &j)) return;
		for (const auto& [k, v] : j.o)
			if (v.type == Json::String) settings_[k] = v.s;
	}
	static std::vector<std::string> lines(const std::string& s) {
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

	Catalog catalog_;
	std::string builtin_dir_, user_dir_, settings_path_;
	std::map<std::string, std::string> settings_;
	bool no_audio_ = false;
	std::unique_ptr<AudioOut> audio_, fallback_;
	std::unique_ptr<MidiIn> midi_;
	int rate_ = 48000;

	// the UI's view
	const Engine* engine_ = nullptr;
	std::string engine_key_;
	Instance* ui_ = nullptr;
	int preset_ = -1;
	bool legato_ = false;
	float master_db_ = kStartDefaultDb;

	// between the threads
	Ring<Cmd, 1024> cmds_;
	Ring<Ret, 1024> rets_;
	Ring<MidiEv, 1024> rings_[MidiIn::kSlots + 1]; // the ports', then awva_ump()'s
	Ring<Frame, 1u << 17> probe_;
	std::atomic<float> peak_l_{0}, peak_r_{0}, min_gain_{1}, cpu_{0};
	std::atomic<int> voices_{0}, stopped_{0};
	std::atomic<uint32_t> midi_count_{0}, generation_{0}, xruns_{0}, faults_{0};

	// the audio thread's
	Player player_;
	struct Batched {
		uint64_t f;
		uint32_t w[4];
	};
	static constexpr int kBatch = 2048;
	Batched batch_[kBatch];
	uint64_t last_frame_ = 0, last_host_ns_ = 0;

	WavWriter wav_;
	std::atomic<bool> probe_run_{false};
	std::thread probe_th_;
};

std::unique_ptr<Core> g_core;
std::string g_text;

} // namespace

// --- the C++ side (core.h) ---------------------------------------------------------------------------
bool render_offline(const nsc_component& c, const PresetEntry* preset, bool legato, const std::vector<TimedUmp>& events, uint64_t frames,
    int rate, float master_db, std::vector<float>* l, std::vector<float>* r, bool safety) {
	auto in = std::make_unique<Instance>(c);
	if (in->init(rate) != 0) return false;
	if (preset && preset->factory >= 0) c.set_preset(in->state, preset->factory);
	if (preset && preset->factory < 0 && c.load_preset(in->state, preset->text.data(), static_cast<uint32_t>(preset->text.size()), nullptr) != 1)
		return false;
	if (legato) in->set_legato(true);
	Player p;
	p.prepare(rate);
	p.safety.set_now_db(master_db);
	p.inst = in.get();
	if (!safety) { // (the instance alone: what the Player gives the safety chain)
		l->assign(static_cast<size_t>(frames), 0.0f);
		r->assign(static_cast<size_t>(frames), 0.0f);
		size_t k = 0;
		for (uint64_t f = 0; f < frames; f += 256) {
			const uint64_t n = std::min<uint64_t>(256, frames - f);
			for (; k < events.size() && events[k].frame < f + n; k++) p.event(events[k].frame, events[k].w);
			in->render(l->data() + f, r->data() + f, static_cast<uint32_t>(n));
		}
		p.inst = nullptr;
		return true;
	}
	const uint64_t total = frames + Limiter::kDelay;
	std::vector<float> a(static_cast<size_t>(total)), b(static_cast<size_t>(total));
	size_t k = 0;
	constexpr uint64_t kChunk = 256; // (any size gives the same samples: the instance's own block grid)
	for (uint64_t f = 0; f < total; f += kChunk) {
		const uint64_t n = std::min(kChunk, total - f);
		for (; k < events.size() && events[k].frame < f + n; k++) p.event(events[k].frame, events[k].w);
		(void)p.render(a.data() + f, b.data() + f, static_cast<uint32_t>(n));
	}
	p.inst = nullptr;
	l->assign(a.begin() + Limiter::kDelay, a.end());
	r->assign(b.begin() + Limiter::kDelay, b.end());
	return true;
}

std::vector<uint32_t> midi1_to_ump(const uint8_t* bytes, size_t n) {
	std::vector<uint32_t> out;
	size_t i = 0;
	while (i < n) {
		const uint8_t s = bytes[i];
		if (s < 0x80) {
			i++;
			continue;
		}
		if (s >= 0xf0) { // (system: skipped, a SysEx to its end)
			i++;
			if (s == 0xf0)
				while (i < n && bytes[i] != 0xf7) i++;
			continue;
		}
		const uint8_t hi = s & 0xf0;
		const size_t len = hi == 0xc0 || hi == 0xd0 ? 2 : 3;
		if (i + len > n) break;
		const uint32_t d1 = bytes[i + 1] & 0x7f, d2 = len == 3 ? bytes[i + 2] & 0x7f : 0;
		out.push_back(0x20000000u | static_cast<uint32_t>(s) << 16 | d1 << 8 | d2);
		i += len;
	}
	return out;
}

std::vector<TimedUmp> mpe_controller_performance(int rate, double seconds) {
	std::vector<TimedUmp> v;
	auto cc = [&](uint64_t f, int ch, int c, int val) { v.push_back({f, {0x20b00000u | static_cast<uint32_t>(ch) << 16 | static_cast<uint32_t>(c) << 8 | static_cast<uint32_t>(val), 0, 0, 0}}); };
	auto msg = [&](uint64_t f, uint32_t status, int ch, int d1, int d2) {
		v.push_back({f, {0x20000000u | (status | static_cast<uint32_t>(ch)) << 16 | static_cast<uint32_t>(d1) << 8 | static_cast<uint32_t>(d2), 0, 0, 0}});
	};
	const int voices = 4;
	const double range = 48.0;
	// midi_config(): the MCM on channel 1 (index 0), the bend range on each member channel
	cc(0, 0, 101, 0);
	cc(0, 0, 100, 6);
	cc(0, 0, 6, voices);
	cc(0, 0, 101, 127);
	cc(0, 0, 100, 127);
	for (int n = 0; n < voices; n++) {
		cc(0, 1 + n, 101, 0);
		cc(0, 1 + n, 100, 0);
		cc(0, 1 + n, 6, static_cast<int>(range));
		cc(0, 1 + n, 101, 127);
		cc(0, 1 + n, 100, 127);
	}
	// a phrase: four notes, each on its own member channel (voice n: channel 2 + n), overlapping,
	// with a glide on the third and vibrato on the last; the controller's update rate is about 1 kHz
	const int keys[4] = {62, 66, 69, 74};
	const double on_s[4] = {0.10, 0.55, 1.00, 1.45};
	const double len_s[4] = {1.20, 1.10, 1.00, std::max(1.2, seconds - 1.75)};
	for (int n = 0; n < voices; n++) {
		const int ch = 1 + n;
		const uint64_t on = static_cast<uint64_t>(on_s[n] * rate);
		const uint64_t off = static_cast<uint64_t>((on_s[n] + len_s[n]) * rate);
		auto bend14 = [&](double semis) { return std::clamp(static_cast<int>(semis * (8192.0 / range) + 8192.0 + 0.5), 0, 16383); };
		int b = bend14(0.0);
		msg(on, 0xe0, ch, b & 0x7f, b >> 7); // (the bend before the Note On: the tuning)
		msg(on, 0x90, ch, keys[n], 70 + 10 * n);
		int last_p = -1, last_y = -1, last_b = b;
		for (uint64_t f = on + rate / 1000; f < off; f += static_cast<uint64_t>(rate / 1000)) {
			const double t = static_cast<double>(f - on) / rate, T = static_cast<double>(off - on) / rate;
			const double env = std::min(1.0, t / 0.08) * std::min(1.0, (T - t) / 0.15);
			const int p = std::clamp(static_cast<int>((0.35 + 0.45 * env + 0.1 * std::sin(6.0 * t)) * 127.0 * env + 0.5), 0, 127);
			const int y = std::clamp(static_cast<int>(64 + 64 * 0.4 * std::sin(1.3 * t + n) + 0.5), 0, 127);
			double semis = 0.0;
			if (n == 2 && t > 0.4) semis = std::min(2.0, (t - 0.4) * 4.0); // a glide up a tone
			if (n == 3 && t > 0.3) semis = 0.25 * std::sin(2.0 * 3.14159265358979 * 5.5 * (t - 0.3)); // vibrato
			b = bend14(semis);
			if (b != last_b) msg(f, 0xe0, ch, b & 0x7f, b >> 7), last_b = b;
			if (p != last_p) msg(f, 0xd0, ch, p, 0), last_p = p;
			if (y != last_y) cc(f, ch, 74, y), last_y = y;
		}
		msg(off, 0x80, ch, keys[n], 64);
	}
	std::stable_sort(v.begin(), v.end(), [](const TimedUmp& a, const TimedUmp& b) { return a.frame < b.frame; });
	return v;
}

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
AWVA_APP_API void awva_set_legato(int32_t on) {
	if (g_core) g_core->set_legato(on != 0);
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
