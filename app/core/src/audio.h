// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// Audio out: WASAPI shared mode (Windows; the OS mixer and its volume stay in the path), CoreAudio's
// default output (macOS), or a null sink (no device: a thread that asks for audio in real time).
// The callback is given stereo float buffers to fill and the host time of the buffer.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace awva_app {

struct AudioCallback {
	virtual ~AudioCallback() = default;
	// fill l and r (n frames, n <= kMaxFrames); host_ns: the buffer's time on now_ns()'s clock
	virtual void render(float* l, float* r, uint32_t n, uint64_t host_ns) = 0;
	static constexpr uint32_t kMaxFrames = 8192;
};

struct AudioInfo {
	std::string backend, device, message;
	int state = 0; // AWVA_AUDIO_*
	int rate = 48000;
	int buffer = 0;
};

class AudioOut {
public:
	virtual ~AudioOut() = default;
	// opens the device and starts calling cb; false: info().message says why
	virtual bool start(AudioCallback* cb) = 0;
	virtual void stop() = 0;
	virtual AudioInfo info() const = 0;
	// the device's rate, known before start (0: unknown)
	virtual int probe_rate() = 0;
	// the system's default output changed since the last call (the core then restarts on it)
	virtual bool take_default_changed() { return false; }
};

std::unique_ptr<AudioOut> make_null_audio(int rate);
std::unique_ptr<AudioOut> make_device_audio(); // WASAPI shared or CoreAudio

} // namespace awva_app
