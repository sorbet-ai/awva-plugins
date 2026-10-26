// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// Audio out on Linux: ALSA's "default" PCM (on a desktop, PulseAudio's or PipeWire's: the system's
// mixer and volume stay in the path), float stereo at 48 kHz (the plug layer converts when the device
// wants otherwise), 128-frame periods, two of them buffered. An underrun is a glitch: counted, noted
// for the log, and the stream prepared again. Without a device the thread asks for audio in real
// time, as the null sink does.
#if defined(__linux__)
#include <alsa/asoundlib.h>
#include <pthread.h>
#include <sched.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "audio.h"
#include "awva_app/awva_app.h"
#include "platform.h"

namespace awva_app {

namespace {

class AlsaOut : public AudioOut {
public:
	~AlsaOut() override { stop(); }
	int probe_rate() override { return kRate; } // (asked for exactly: the plug layer converts)

	bool start(AudioCallback* cb) override {
		stop();
		run_ = true;
		th_ = std::thread([this, cb] { thread(cb); });
		for (int k = 0; k < 400 && !opened_.load(); k++) std::this_thread::sleep_for(std::chrono::milliseconds(5));
		return true;
	}
	void stop() override {
		run_ = false;
		if (th_.joinable()) th_.join();
		opened_ = false;
	}
	AudioInfo info() const override {
		std::lock_guard<std::mutex> g(m_);
		AudioInfo i = info_;
		i.glitches = glitches_.load();
		return i;
	}
	int take_glitches(Glitch* out, int max) override {
		int k = 0;
		for (size_t r = gr_.load(std::memory_order_relaxed); k < max && r != gw_.load(std::memory_order_acquire); r++) {
			out[k++] = gring_[r % kRing];
			gr_.store(r + 1, std::memory_order_release);
		}
		return k;
	}

private:
	static constexpr int kRate = 48000;
	static constexpr snd_pcm_uframes_t kPeriod = 128;

	void glitch(int kind, int64_t dt_us) {
		glitches_.fetch_add(1);
		const size_t w = gw_.load(std::memory_order_relaxed);
		if (w - gr_.load(std::memory_order_acquire) < kRing) {
			gring_[w % kRing] = {now_ns(), kind, dt_us, 0, 0};
			gw_.store(w + 1, std::memory_order_release);
		}
	}

	void thread(AudioCallback* cb) {
		// (real-time where the system allows it; else the normal priority)
		sched_param sp{};
		sp.sched_priority = std::max(1, sched_get_priority_max(SCHED_FIFO) - 10);
		(void)pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
		{
			std::lock_guard<std::mutex> g(m_);
			info_ = AudioInfo{};
			info_.backend = "ALSA";
			info_.device = "default";
			info_.mode = "shared";
		}
		snd_pcm_t* pcm = nullptr;
		snd_pcm_uframes_t period = kPeriod, buffer = 2 * kPeriod;
		int err = snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
		if (err == 0) {
			snd_pcm_hw_params_t* hw = nullptr;
			snd_pcm_hw_params_malloc(&hw);
			snd_pcm_hw_params_any(pcm, hw);
			if ((err = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) == 0 &&
			    (err = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_FLOAT_LE)) == 0 && (err = snd_pcm_hw_params_set_channels(pcm, hw, 2)) == 0 &&
			    (err = snd_pcm_hw_params_set_rate(pcm, hw, kRate, 0)) == 0) {
				int dir = 0;
				snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, &dir);
				snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer);
				err = snd_pcm_hw_params(pcm, hw);
			}
			snd_pcm_hw_params_free(hw);
			if (err == 0) err = snd_pcm_prepare(pcm);
		}
		{
			std::lock_guard<std::mutex> g(m_);
			info_.rate = kRate;
			info_.buffer = static_cast<int>(buffer);
			info_.period = static_cast<int>(period);
			info_.state = err == 0 ? AWVA_AUDIO_RUNNING : AWVA_AUDIO_FAILED;
			info_.message = err == 0 ? "" : std::string("the audio device can't be opened: ") + snd_strerror(err);
		}
		opened_ = true;
		std::vector<float> l(AudioCallback::kMaxFrames), r(AudioCallback::kMaxFrames), io(2 * AudioCallback::kMaxFrames);
		const uint32_t n = static_cast<uint32_t>(std::min<snd_pcm_uframes_t>(period, AudioCallback::kMaxFrames));
		bool device = err == 0;
		auto next = std::chrono::steady_clock::now();
		while (run_) {
			cb->render(l.data(), r.data(), n, now_ns());
			cb->tap(l.data(), r.data(), n);
			if (!device) {
				next += std::chrono::nanoseconds(static_cast<int64_t>(1e9 * n / kRate));
				std::this_thread::sleep_until(next);
				continue;
			}
			for (uint32_t i = 0; i < n; i++) io[2 * i] = l[i], io[2 * i + 1] = r[i];
			snd_pcm_sframes_t w = snd_pcm_writei(pcm, io.data(), n); // (blocks until there is room: the clock)
			if (w == -EPIPE || w == -ESTRPIPE) { // (an underrun, or a suspend: counted, prepared again)
				glitch(w == -EPIPE ? 1 : 4, 0);
				if (w == -ESTRPIPE)
					while (snd_pcm_resume(pcm) == -EAGAIN) std::this_thread::sleep_for(std::chrono::milliseconds(10));
				snd_pcm_prepare(pcm);
				w = snd_pcm_writei(pcm, io.data(), n);
			}
			if (w < 0) {
				device = false; // (the device went away: on without it)
				std::lock_guard<std::mutex> g(m_);
				info_.state = AWVA_AUDIO_FAILED;
				info_.message = std::string("the audio device stopped: ") + snd_strerror(static_cast<int>(w));
				next = std::chrono::steady_clock::now();
			}
		}
		if (pcm) {
			snd_pcm_drop(pcm);
			snd_pcm_close(pcm);
		}
	}

	mutable std::mutex m_;
	AudioInfo info_;
	std::atomic<bool> run_{false}, opened_{false};
	std::thread th_;
	static constexpr size_t kRing = 64;
	Glitch gring_[kRing];
	std::atomic<size_t> gw_{0}, gr_{0};
	std::atomic<uint32_t> glitches_{0};
};

} // namespace

std::unique_ptr<AudioOut> make_device_audio() { return std::make_unique<AlsaOut>(); }

} // namespace awva_app
#endif
