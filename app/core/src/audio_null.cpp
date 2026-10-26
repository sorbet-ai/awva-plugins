// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The null sink: no device; a thread asks for 256 frames every 256 / rate seconds, in real time,
// so MIDI, the meters and the probe tap behave as with a device. With AudioConfig::simulate_db it
// applies an exclusive output's Windows volume at that level (the stage WASAPI exclusive mode
// applies), so the level the tap measures can be compared with shared's (tests).
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <timeapi.h>
#endif
#include <chrono>
#include <thread>
#include <vector>

#include "audio.h"
#include "awva_app/awva_app.h"
#include "endpoint_gain.h"
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
		const bool sim = !std::isnan(config_.simulate_db);
		if (sim) gain_.reset(endpoint_gain(config_.simulate_db, 1.0f, false, false, false));
		const uint32_t n = config_.frames > 0 ? static_cast<uint32_t>(config_.frames) : 256;
		period_ = static_cast<int>(n);
		th_ = std::thread([this, cb, sim, n] {
#if defined(_WIN32)
			timeBeginPeriod(1); // (the sleeps to the millisecond, as a device's events)
#endif
			std::vector<float> l(n), r(n);
			// every buffer at its scheduled time (the OS's sleep is coarse: late wakes catch up), its
			// host time the schedule's, so MIDI timestamps land on the frames they would with a device
			const auto t0 = std::chrono::steady_clock::now();
			const uint64_t h0 = now_ns();
			for (uint64_t k = 0; run_; k++) {
				const uint64_t at = static_cast<uint64_t>(1e9 * static_cast<double>(k * n) / rate_);
				std::this_thread::sleep_until(t0 + std::chrono::nanoseconds(at));
				(void)h0;
				cb->render(l.data(), r.data(), n, now_ns()); // (the wake's time, as a device's callback has)
				if (sim) gain_.apply(l.data(), r.data(), n);
				cb->tap(l.data(), r.data(), n);
			}
#if defined(_WIN32)
			timeEndPeriod(1);
#endif
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
		i.buffer = period_;
		i.period = period_;
		i.mode = "null";
		if (!std::isnan(config_.simulate_db)) i.volume = gain_.applied();
		return i;
	}
	int probe_rate() override { return rate_; }

private:
	int rate_, period_ = 256;
	EndpointGain gain_;
	std::atomic<bool> run_{false};
	std::thread th_;
};

} // namespace

std::unique_ptr<AudioOut> make_null_audio(int rate) { return std::make_unique<NullAudio>(rate); }

} // namespace awva_app
