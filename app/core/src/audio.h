// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// Audio out: WASAPI (Windows; shared mode by default, the OS mixer and its volume in the path, or
// exclusive mode on request, the Windows volume then applied by the app), CoreAudio's default output
// (macOS), or a null sink (no device: a thread that asks for audio in real time). The callback is
// given stereo float buffers to fill and the host time of the buffer; after the backend's own last
// stage (the exclusive mode's volume) it is given the exact samples the device gets (tap()).
#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace awva_app {

struct AudioCallback {
	virtual ~AudioCallback() = default;
	// fill l and r (n frames, n <= kMaxFrames); host_ns: the buffer's time on now_ns()'s clock
	virtual void render(float* l, float* r, uint32_t n, uint64_t host_ns) = 0;
	// the samples as they go to the device (after the backend's own volume stage, if any)
	virtual void tap(const float*, const float*, uint32_t) {}
	static constexpr uint32_t kMaxFrames = 8192;
};

// a glitch, as the audio thread saw it: kind 1 a late wake-up (dt_us since the last), 2 a render that
// took most of the period (render_us), 3 no event for 200 ms, 4 the device failed
struct Glitch {
	uint64_t ns;
	int kind;
	int64_t dt_us, render_us;
	uint32_t pad;
};

struct AudioConfig {
	bool exclusive = false; // WASAPI exclusive mode (Windows), on request only
	int frames = 256;       // the exclusive buffer asked for, frames (the device aligns it; 0: its smallest)
	float simulate_db = NAN; // the null sink: apply an exclusive endpoint volume of this level (tests)
};

struct AudioInfo {
	std::string backend, device, message;
	std::string mode = "shared"; // "shared", "exclusive", "null"
	int state = 0;               // AWVA_AUDIO_*
	int rate = 48000;
	int buffer = 0;              // the device buffer, frames (reported by the driver)
	int period = 0;              // the frames the app writes per wake-up
	float stream_ms = -1;        // the device's own latency, ms, as the driver reports it (-1: unknown)
	float volume = -1;           // exclusive: the Windows volume the app applies (linear), -1 none
	bool exclusive_available = false;
	uint32_t glitches = 0;   // since the start
	uint32_t recoveries = 0; // streams opened again by themselves
};

class AudioOut {
public:
	virtual ~AudioOut() = default;
	virtual void configure(const AudioConfig& c) { config_ = c; }
	// opens the device and starts calling cb; false: info().message says why
	virtual bool start(AudioCallback* cb) = 0;
	virtual void stop() = 0;
	virtual AudioInfo info() const = 0;
	// the rate the device will run at, known before start (0: unknown)
	virtual int probe_rate() = 0;
	// the system's default output changed since the last call (the core then restarts on it)
	virtual bool take_default_changed() { return false; }
	// the glitches noted since the last call (the UI thread logs them)
	virtual int take_glitches(Glitch*, int) { return 0; }

protected:
	AudioConfig config_;
};

std::unique_ptr<AudioOut> make_null_audio(int rate);
std::unique_ptr<AudioOut> make_device_audio(); // WASAPI or CoreAudio

} // namespace awva_app
