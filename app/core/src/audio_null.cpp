// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The null sink: no device; a thread asks for 256 frames every 256 / rate seconds, in real time,
// so MIDI, the meters and the probe tap behave as with a device.
#include <chrono>
#include <thread>
#include <vector>

#include "audio.h"
#include "awva_app/awva_app.h"
#include "platform.h"

namespace awva_app {

namespace {

class NullAudio : public AudioOut {
public:
	explicit NullAudio(int rate) : rate_(rate) {}
	~NullAudio() override { stop(); }
	bool start(AudioCallback* cb) override {
		stop();
		run_ = true;
		th_ = std::thread([this, cb] {
			constexpr uint32_t n = 256;
			std::vector<float> l(n), r(n);
			// every buffer at its scheduled time (the OS's sleep is coarse: late wakes catch up), its
			// host time the schedule's, so MIDI timestamps land on the frames they would with a device
			const auto t0 = std::chrono::steady_clock::now();
			const uint64_t h0 = now_ns();
			for (uint64_t k = 0; run_; k++) {
				const uint64_t at = static_cast<uint64_t>(1e9 * static_cast<double>(k * n) / rate_);
				std::this_thread::sleep_until(t0 + std::chrono::nanoseconds(at));
				cb->render(l.data(), r.data(), n, h0 + at);
			}
		});
		return true;
	}
	void stop() override {
		run_ = false;
		if (th_.joinable()) th_.join();
	}
	AudioInfo info() const override {
		AudioInfo i;
		i.backend = "null";
		i.device = "no audio device";
		i.state = AWVA_AUDIO_NULL;
		i.rate = rate_;
		i.buffer = 256;
		return i;
	}
	int probe_rate() override { return rate_; }

private:
	int rate_;
	std::atomic<bool> run_{false};
	std::thread th_;
};

} // namespace

std::unique_ptr<AudioOut> make_null_audio(int rate) { return std::make_unique<NullAudio>(rate); }

} // namespace awva_app
