// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The null sink: no device; a thread asks for 256 frames every 256 / rate seconds, in real time,
// so MIDI, the meters and the probe tap behave as with a device. With AudioConfig::simulate_db it
// applies an exclusive output's Windows volume at that level (the stage WASAPI exclusive mode
// applies), so the level the tap measures can be compared with shared's (tests).
// With AudioConfig::simulate_bus it is a fake endpoint (tests): exclusive (on request) or shared,
// smallest period 3 ms, default period 10 ms, on that bus, its buffer chosen by buffer_policy.h as
// WASAPI's is; with simulate_wake_ms it wakes at most that often (the times on a grid of that step,
// as a Bluetooth radio's bursts): a period shorter than that comes late, a late wake-up as WASAPI
// sees one (more than a period and a half after the last), on the schedule's own clock, so a test
// sees the same glitches on any machine. With AudioConfig::simulate_devices it lists that fake
// endpoint list (the first the default) and plays the one chosen as the fake endpoint, on its bus
// (the default, info().missing, when the chosen one isn't listed).
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <timeapi.h>
#endif
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>
#include <vector>

#include "audio.h"
#include "awva_app/awva_app.h"
#include "buffer_policy.h"
#include "endpoint_gain.h"
#include "platform.h"

namespace awva_app {

namespace {

class NullAudio : public AudioOut {
public:
	static constexpr const char* kFakeId = "fake-endpoint";

	explicit NullAudio(int rate) : rate_(rate) {}
	~NullAudio() override { stop(); }
	bool start(AudioCallback* cb) override {
		stop();
		run_ = true;
		const bool sim = !std::isnan(config_.simulate_db);
		if (sim) gain_.reset(endpoint_gain(config_.simulate_db, 1.0f, false, false, false));
		const bool fake = !config_.simulate_bus.empty() || !config_.simulate_devices.empty();
		policy_ = EndpointBuffer{};
		{
			std::lock_guard<std::mutex> g(m_);
			id_ = kFakeId;
			name_.clear();
			missing_ = false;
			Bus bus = bus_from_name(config_.simulate_bus);
			if (!config_.simulate_devices.empty()) {
				const OutputDevice* d = &config_.simulate_devices.front();
				missing_ = !config_.device.empty();
				for (const OutputDevice& x : config_.simulate_devices)
					if (!config_.device.empty() && x.id == config_.device) d = &x, missing_ = false;
				id_ = d->id;
				name_ = d->name;
				bus = bus_from_name(d->bus);
			}
			if (fake) {
				const auto it = config_.learned.find(id_);
				policy_.endpoint(id_, bus, it == config_.learned.end() ? 0 : it->second);
			}
		}
		open_stream(fake);
		th_ = std::thread([this, cb, sim, fake] {
#if defined(_WIN32)
			timeBeginPeriod(1); // (the sleeps to the millisecond, as a device's events)
#endif
			while (run_) {
				if (!play(cb, sim, fake)) break;
				recoveries_.fetch_add(1); // (the fake endpoint's stream opened again: a raised buffer, or shared)
				open_stream(fake);
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
		std::lock_guard<std::mutex> g(m_);
		AudioInfo i;
		i.backend = "null";
		i.device = !name_.empty() ? name_ : (fake_ ? std::string("fake endpoint (") + bus_name(policy_.bus()) + ")" : "no audio device");
		i.missing = missing_;
		i.state = AWVA_AUDIO_NULL;
		i.rate = rate_;
		i.buffer = period_;
		i.period = period_;
		i.mode = mode_;
		i.message = note_;
		i.exclusive_available = fake_;
		if (fake_) {
			i.endpoint = id_;
			i.bus = bus_name(policy_.bus());
			i.learned = policy_.learned();
		}
		if (!std::isnan(config_.simulate_db)) i.volume = gain_.applied();
		i.glitches = glitches_.load();
		i.recoveries = recoveries_.load();
		return i;
	}
	int probe_rate() override { return rate_; }
	std::vector<OutputDevice> devices() override {
		std::vector<OutputDevice> v = config_.simulate_devices;
		for (size_t k = 0; k < v.size(); k++) v[k].is_default = k == 0;
		return v;
	}

private:
	// a stream's buffer and mode: the null sink's own, or the fake endpoint's (exclusive by the policy)
	void open_stream(bool fake) {
		std::lock_guard<std::mutex> g(m_);
		fake_ = fake;
		const int dev_min = rate_ * 3 / 1000, dev_default = rate_ / 100;
		exclusive_ = fake && config_.exclusive && !policy_.shared_only();
		if (!fake) {
			period_ = config_.frames > 0 ? config_.frames : 256;
			mode_ = "null";
		} else {
			const int f = policy_.frames(config_.frames, dev_min, dev_default); // (also the policy's ceiling)
			period_ = exclusive_ ? f : dev_default;
			mode_ = exclusive_ ? "exclusive" : "shared";
		}
		note_ = fake ? policy_.note(config_.exclusive) : "";
	}

	// one stream, until stopped (false) or opened again (true)
	bool play(AudioCallback* cb, bool sim, bool fake) {
		const uint32_t n = static_cast<uint32_t>(period_);
		std::vector<float> l(n), r(n);
		const double wake_ns = fake ? config_.simulate_wake_ms * 1e6 : 0;
		const uint64_t period_ns = static_cast<uint64_t>(1e9 * n / rate_);
		// every buffer at its scheduled time (the OS's sleep is coarse: late wakes catch up), its
		// host time the wake's, so MIDI timestamps land on the frames they would with a device
		const auto t0 = std::chrono::steady_clock::now();
		const uint64_t h0 = now_ns();
		uint64_t last = 0;
		for (uint64_t k = 0; run_; k++) {
			uint64_t at = static_cast<uint64_t>(1e9 * static_cast<double>(k * n) / rate_);
			if (wake_ns > 0) at = static_cast<uint64_t>(std::ceil(static_cast<double>(at) / wake_ns) * wake_ns); // (the endpoint's grid)
			std::this_thread::sleep_until(t0 + std::chrono::nanoseconds(at));
			if (fake && k > 0 && at - last > period_ns + period_ns / 2 + 1000000ull) {
				glitches_.fetch_add(1);
				std::lock_guard<std::mutex> g(m_); // (info() reads the policy)
				if (exclusive_ && policy_.glitch(h0 + at, static_cast<int>(n)) != EndpointBuffer::Action::None) return true;
			}
			last = at;
			cb->render(l.data(), r.data(), n, now_ns()); // (the wake's time, as a device's callback has)
			if (sim) gain_.apply(l.data(), r.data(), n);
			cb->tap(l.data(), r.data(), n);
		}
		return false;
	}

	int rate_, period_ = 256;
	mutable std::mutex m_;
	bool fake_ = false, exclusive_ = false, missing_ = false;
	std::string mode_ = "null", note_, id_ = kFakeId, name_;
	EndpointBuffer policy_;
	EndpointGain gain_;
	std::atomic<bool> run_{false};
	std::atomic<uint32_t> glitches_{0}, recoveries_{0};
	std::thread th_;
};

} // namespace

std::unique_ptr<AudioOut> make_null_audio(int rate) { return std::make_unique<NullAudio>(rate); }

} // namespace awva_app
