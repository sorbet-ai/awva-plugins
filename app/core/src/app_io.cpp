// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The core's MIDI ports and audio output (app_core.h): the ports, the output and its mode and
// device, its glitches logged, the delay figures, the stream opened again; the probe's WAV.
#include "app_core.h"

namespace awva_app {

std::string Core::midi_json() {
	std::string s = "[";
	if (midi_)
		for (const MidiPort& p : midi_->ports()) {
			if (s.size() > 1) s += ",";
			s += "{\"name\":" + jstr(p.name) + ",\"enabled\":" + (p.enabled ? "true" : "false") + ",\"state\":" + jstr(p.state) +
			    ",\"message\":" + jstr(p.message) + "}";
		}
	return s + "]";
}
void Core::midi_refresh() {
	drain_rets();
	if (midi_) midi_->refresh();
	rescan_if_changed(); // (a package written while the app runs is listed within two seconds)
	if (!audio_) return;
	// the system's default output changed: play on the new one, if it's the one playing (faded in,
	// at -12 dB at most, as at the start); an output came or went: the chosen one, if that's it
	const bool def = audio_->take_default_changed(), devs = audio_->take_devices_changed();
	if (!def && !devs) return;
	const AudioInfo a = audio_info();
	bool restart = def && (config_.device.empty() || a.missing);
	if (devs && !config_.device.empty()) {
		bool present = false;
		for (const OutputDevice& d : audio_->devices()) present = present || d.id == config_.device;
		restart = restart || present == a.missing || a.state == AWVA_AUDIO_FAILED;
	}
	if (restart) (void)restart_audio(true);
}
void Core::midi_enable(const std::string& name, bool on) {
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
void Core::inject(const uint32_t* w, int count) {
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
AudioInfo Core::audio_info() const {
	AudioInfo i = fallback_ ? fallback_->info() : (audio_ ? audio_->info() : AudioInfo{});
	i.glitches += glitches_base_;
	i.recoveries += recoveries_base_;
	if (fallback_) {
		const AudioInfo d = audio_->info();
		i.state = AWVA_AUDIO_FAILED;
		i.message = d.message;
		i.backend = d.backend;
	}
	return i;
}
std::string Core::audio_json() const {
	const AudioInfo i = audio_info();
	static const char* names[] = {"off", "running", "null", "failed"};
	return "{\"backend\":" + jstr(i.backend) + ",\"device\":" + jstr(i.device) + ",\"state\":" + jstr(names[std::clamp(i.state, 0, 3)]) +
	    ",\"message\":" + jstr(i.message) + ",\"rate\":" + std::to_string(i.rate) + ",\"buffer\":" + std::to_string(i.buffer) +
	    ",\"mode\":" + jstr(i.mode) + ",\"wanted\":" + jstr(config_.exclusive ? "exclusive" : "shared") + ",\"frames\":" + std::to_string(config_.frames) +
	    ",\"exclusive_available\":" + (i.exclusive_available ? "true" : "false") + ",\"volume\":" + jnum(i.volume) +
	    ",\"bus\":" + jstr(i.bus) + ",\"learned\":" + std::to_string(i.learned) + ",\"chosen\":" + jstr(config_.device) +
	    ",\"missing\":" + (i.missing ? "true" : "false") + ",\"notice\":" +
	    jstr(i.missing ? (setting("output_device_name").empty() ? "The output chosen" : setting("output_device_name")) + " isn't connected: playing the default" : "") +
	    "}";
}
// every output endpoint: [{"id","name","bus","default"}...]
std::string Core::devices_json() const {
	std::string s = "[";
	if (audio_)
		for (const OutputDevice& d : audio_->devices())
			s += std::string(s.size() > 1 ? "," : "") + "{\"id\":" + jstr(d.id) + ",\"name\":" + jstr(d.name) + ",\"bus\":" + jstr(d.bus) +
			    ",\"default\":" + (d.is_default ? "true" : "false") + "}";
	return s + "]";
}
// the output: an id devices() lists, or "" the system's default; kept for the next start (with its
// name, for the line when it's missing); the stream opened again on it, as at the start
int Core::set_audio_device(const std::string& id) {
	std::string name;
	if (!id.empty()) {
		for (const OutputDevice& d : audio_->devices())
			if (d.id == id) name = d.name;
		if (name.empty()) return fail("no such output");
	}
	config_.device = id;
	set_setting("output_device", id);
	set_setting("output_device_name", name);
	return restart_audio(true);
}
// the output mode: "shared" (the default) or "exclusive" with a buffer of frames; restarts the audio
int Core::set_audio_mode(const std::string& mode, int frames) {
	config_.exclusive = mode == "exclusive";
	if (config_.exclusive) { // (asked again: the endpoint's buffer measured again)
		const AudioInfo a = audio_info();
		if (!a.endpoint.empty()) {
			config_.learned.erase(a.endpoint);
			erase_setting(kLearned + a.endpoint);
		}
	}
	if (frames >= 0) config_.frames = frames == 0 ? 0 : std::clamp(frames, 16, 4096); // (0: the smallest)
	set_setting("audio_mode", config_.exclusive ? "exclusive" : "shared");
	set_setting("exclusive_buffer", std::to_string(config_.frames));
	audio_->configure(config_);
	return restart_audio();
}
// the audio thread's glitches into <app data>/awva/audio.log, with the stream's state (UI thread)
void Core::log_glitches() {
	if (!audio_) return;
	Glitch g[64];
	const int k = audio_->take_glitches(g, 64);
	if (k == 0) return;
	const AudioInfo a = audio_info();
	static const char* kinds[] = {"", "late wake-up", "slow render", "no event for 200 ms", "device failed"};
	std::string s;
	for (int i = 0; i < k; i++) {
		char b[256];
		std::snprintf(b, sizeof b, "%.3f s  %s  (since last %lld us, render %lld us, padding %u)  %s %s %d Hz, %d frames, period %d; %u glitches, %u re-opened\n",
		    (g[i].ns - start_ns_) / 1e9, kinds[std::clamp(g[i].kind, 0, 4)], static_cast<long long>(g[i].dt_us), static_cast<long long>(g[i].render_us), g[i].pad,
		    a.device.c_str(), a.mode.c_str(), a.rate, a.buffer, a.period, a.glitches, a.recoveries);
		s += b;
	}
	std::string old;
	const std::string path = join(app_data_dir(), "audio.log");
	read_file(path, &old, 1u << 20);
	if (old.size() > (1u << 19)) old = old.substr(old.size() - (1u << 18)); // (the last 256 KB kept)
	write_file(path, old + s);
}
// what the stream learned about its endpoint (a Bluetooth endpoint's buffer, or shared only) into
// the settings, for the next start (UI thread)
void Core::remember_endpoint() {
	if (!audio_) return;
	const AudioInfo a = audio_->info();
	if (a.endpoint.empty() || a.learned == 0) return;
	const std::string v = a.learned < 0 ? "shared" : std::to_string(a.learned);
	config_.learned[a.endpoint] = a.learned;
	if (setting(kLearned + a.endpoint) != v) set_setting(kLearned + a.endpoint, v);
}
// the delay from a key to the speaker, part by part: each figure measured or as the driver reports it
std::string Core::latency_json() {
	log_glitches();
	remember_endpoint();
	const AudioInfo a = audio_info();
	const int64_t n = lat_n_.exchange(0);
	const int64_t sum = lat_sum_.exchange(0), mn = lat_min_.exchange(INT64_MAX), mx = lat_max_.exchange(INT64_MIN);
	if (n > 0) lat_last_ = {n, mn / 1e3, sum / 1e3 / n, mx / 1e3};
	const double sr = std::max(1, a.rate);
	const double period_ms = a.period * 1e3 / sr, buffer_ms = a.buffer * 1e3 / sr;
	const double block_ms = NSC_BLOCK * 1e3 / sr, step_ms = 2.0;
	// the total: the MIDI message to its frame (measured), the device's buffer and its own latency
	// (reported); the engine's 2 ms step is inside the measured MIDI figure
	const double midi = lat_last_.n ? lat_last_.mean : period_ms / 2;
	const double out = period_ms + std::max(0.0f, a.stream_ms); // (one period queued, in both modes)
	std::string s = "{\"mode\":" + jstr(a.mode) + ",\"rate\":" + std::to_string(a.rate);
	s += ",\"period_frames\":" + std::to_string(a.period) + ",\"period_ms\":" + jnum(period_ms);
	s += ",\"buffer_frames\":" + std::to_string(a.buffer) + ",\"buffer_ms\":" + jnum(buffer_ms);
	s += ",\"queued_frames\":" + std::to_string(a.period) + ",\"queued_ms\":" + jnum(period_ms);
	s += ",\"stream_ms\":" + jnum(a.stream_ms);
	s += ",\"block_frames\":" + std::to_string(NSC_BLOCK) + ",\"block_ms\":" + jnum(block_ms) + ",\"step_ms\":" + jnum(step_ms);
	s += ",\"midi_count\":" + std::to_string(lat_last_.n) + ",\"midi_min_ms\":" + jnum(lat_last_.min) + ",\"midi_mean_ms\":" + jnum(lat_last_.mean) +
	    ",\"midi_max_ms\":" + jnum(lat_last_.max);
	s += ",\"total_ms\":" + jnum(midi + out);
	s += ",\"volume\":" + jnum(a.volume) + ",\"glitches\":" + std::to_string(a.glitches) + ",\"recoveries\":" + std::to_string(a.recoveries) + "}";
	return s;
}
// new_device: the output changed (chosen, or the default playing changed): the level at -12 dB at
// most, as at the start
int Core::restart_audio(bool new_device) {
	const AudioInfo before = audio_info(); // (the counts carry over a restart)
	glitches_base_ = before.glitches;
	recoveries_base_ = before.recoveries;
	remember_endpoint();
	stop_audio();
	audio_->configure(config_); // (with what the stream learned)
	apply_all(); // (the audio thread is stopped: the UI thread applies what it hadn't)
	drain_rets();
	const int r = audio_->probe_rate(); // (exclusive mode may run at another rate than the mixer)
	if (r > 0 && r != rate_) {
		if (player_.inst && player_.inst->reinit(r) != 0) return fail("the engine doesn't run at " + std::to_string(r) + " Hz");
		rate_ = r;
	}
	player_.prepare(rate_);
	if (new_device && master_db_ > kStartMaxDb) { // (the audio thread is stopped)
		master_db_ = kStartMaxDb;
		player_.safety.set_target_db(kStartMaxDb);
		set_setting("master_db", jnum(kStartMaxDb));
	}
	player_.safety.fade_in(0.300f);
	start_audio();
	return 0;
}
void Core::start_audio() {
	fallback_.reset();
	if (!audio_->start(this)) {
		fallback_ = make_null_audio(rate_); // (no device: the sound keeps running without one)
		fallback_->start(this);
	}
}
void Core::stop_audio() {
	if (fallback_) fallback_->stop();
	fallback_.reset();
	if (audio_) audio_->stop();
}
void Core::probe_loop() {
	while (probe_run_) {
		drain_probe();
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
}
void Core::drain_probe() {
	Frame f;
	while (probe_.pop(&f)) wav_.write(&f.l, &f.r, 1);
}

} // namespace awva_app
