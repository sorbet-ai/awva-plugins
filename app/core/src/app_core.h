// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The core behind the C ABI (awva_app.h; the design: app.cpp's header), in its parts:
//   app.cpp          its start and stop, the settings, the C ABI
//   app_sound.cpp    the catalog and the sound (the UI thread's side of the commands)
//   app_io.cpp       MIDI ports, the audio output and its figures, the probe
//   app_render.cpp   the audio thread: the commands, MIDI at its frames, the render
#pragma once

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
#include "step_spread.h"
#include "wav.h"

namespace awva_app {

struct Cmd {
	enum T : int { SetParam, SetPreset, LoadText, Mono, Panic, Swap, Master } t;
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

inline void atomic_max(std::atomic<float>& a, float v) {
	float cur = a.load(std::memory_order_relaxed);
	while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
	}
}
inline void atomic_min(std::atomic<float>& a, float v) {
	float cur = a.load(std::memory_order_relaxed);
	while (v < cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
	}
}

class Core : public AudioCallback {
public:
	// --- the UI thread ---------------------------------------------------------------------------
	int start(const Json& o);
	~Core() override { stop(); }
	void stop();

	const Catalog& catalog() const { return catalog_; }
	const std::string& engines_dir() const { return user_dir_; }
	int rescan();
	// the packages folder changed since the last scan: scan it again
	void rescan_if_changed();

	int select_engine(const std::string& key);
	int select_preset(int i);
	int set_param(int i, float v);
	float param(int i) const { return ui_ && i >= 0 && i < ui_->d.num_params ? ui_->c.param(ui_->state, i) : 0.0f; }
	void set_mono(bool on);
	void panic() { send({Cmd::Panic, 0, 0, nullptr}); }
	void set_master(float db);

	std::string sound_json();

	void status(awva_status* o);

	std::string midi_json();
	void midi_refresh();
	void midi_enable(const std::string& name, bool on);
	void inject(const uint32_t* w, int count);

	AudioInfo audio_info() const;
	std::string audio_json() const;
	// every output endpoint: [{"id","name","bus","default"}...]
	std::string devices_json() const;
	// the output: an id devices() lists, or "" the system's default; kept for the next start (with its
	// name, for the line when it's missing); the stream opened again on it, as at the start
	int set_audio_device(const std::string& id);
	// the output mode: "shared" (the default) or "exclusive" with a buffer of frames; restarts the audio
	int set_audio_mode(const std::string& mode, int frames);

	// the audio thread's glitches into <app data>/awva/audio.log, with the stream's state (UI thread)
	void log_glitches();

	// what the stream learned about its endpoint (a Bluetooth endpoint's buffer, or shared only) into
	// the settings, for the next start (UI thread)
	void remember_endpoint();

	// the delay from a key to the speaker, part by part: each figure measured or as the driver reports it
	std::string latency_json();

	// new_device: the output changed (chosen, or the default playing changed): the level at -12 dB at
	// most, as at the start
	int restart_audio(bool new_device = false);

	std::string setting(const std::string& k) const;
	void set_setting(const std::string& k, const std::string& v);
	void erase_setting(const std::string& k);
	void save_settings();

	int fail(const std::string& why);
	std::string error_;

	// --- the audio thread --------------------------------------------------------------------------
	// the samples as they go to the device: the probe tap
	void tap(const float* l, const float* r, uint32_t n) override;
	void render(float* l, float* r, uint32_t n, uint64_t host_ns) override;

private:
	void apply(const Cmd& c);
	// the UI thread, with the audio thread stopped
	void apply_all();
	void send(const Cmd& c);
	void drain_rets();
	// the built-in engine's library a presets-only package plays (not stopped like a package's own)
	const nsc_component* builtin_of(const Engine* e) const;
	std::vector<std::string> heroes() const;
	void start_audio();
	void stop_audio();
	void probe_loop();
	void drain_probe();
	void load_settings();
	static std::vector<std::string> lines(const std::string& s);

	uint64_t start_ns_ = now_ns();
	Catalog catalog_;
	Engine playing_;     // the playing engine's entry, should a rescan lose it
	std::string folder_; // the packages folder at the last scan (Catalog::signature)
	uint32_t catalog_gen_ = 0;
	std::string builtin_dir_, user_dir_, settings_path_;
	std::map<std::string, std::string> settings_;
	static constexpr const char* kLearned = "exclusive_buffer_learned:"; // + the endpoint's id
	bool no_audio_ = false, shut_ = false;
	AudioConfig config_;
	uint32_t glitches_base_ = 0, recoveries_base_ = 0;
	std::unique_ptr<AudioOut> audio_, fallback_;
	std::unique_ptr<MidiIn> midi_;
	int rate_ = 48000;

	// the UI's view
	const Engine* engine_ = nullptr;
	std::string engine_key_;
	Instance* ui_ = nullptr;
	int preset_ = -1;
	bool mono_ = false;
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
		uint64_t f, ns;
		uint32_t w[4];
	};
	std::atomic<int64_t> lat_n_{0}, lat_sum_{0}, lat_min_{INT64_MAX}, lat_max_{INT64_MIN};
	struct LatWindow {
		int64_t n = 0;
		double min = 0, mean = 0, max = 0;
	} lat_last_;
	static constexpr int kBatch = 2048;
	Batched batch_[kBatch];
	StepSpread spread_; // the frames MIDI is placed at, on the engine's steps
	uint64_t last_host_ns_ = 0;

	WavWriter wav_;
	std::atomic<bool> probe_run_{false};
	std::thread probe_th_;
};

} // namespace awva_app
