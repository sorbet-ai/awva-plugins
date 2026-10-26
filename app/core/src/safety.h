// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The output's safety chain, after the engine (stereo, in place):
//   1. non-finite samples: a block with a NaN or an infinity is muted and counted (the caller
//      resets the engine);
//   2. the master level: linear ramps to the target (20 ms; the start fades in over 300 ms), the
//      gain exactly the target's once there;
//   3. a true-peak limiter at -1 dBTP: the inter-sample peaks estimated by 4x oversampling, the gain
//      the minimum over a look-ahead window, released over 80 ms, then averaged over the window, so
//      the output never exceeds the ceiling; the signal is delayed by kDelay frames. Below the
//      ceiling the gain is exactly 1 and the samples pass bit for bit (delayed);
//   4. a hard ceiling at 0 dBFS, should anything get past.
// The OS's own volume comes after all of this (WASAPI shared mode, CoreAudio).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace awva_app {

constexpr float kMasterMinDb = -60.0f, kMasterMaxDb = 6.0f;
constexpr float kStartMaxDb = -12.0f; // the most the app starts at, whatever was saved
constexpr float kStartDefaultDb = -18.0f;

inline float db_to_gain(float db) { return db <= kMasterMinDb ? 0.0f : std::pow(10.0f, db / 20.0f); }

class Limiter {
public:
	static constexpr int kWindow = 64;              // the look-ahead window (1.3 ms at 48 kHz)
	static constexpr int kTaps = 8;                 // per phase of the 4x interpolator
	static constexpr int kDelay = kWindow - 1 + kTaps / 2; // frames from input to output
	static constexpr float kCeiling = 0.89125094f;  // -1 dBTP

	void prepare(int rate) {
		release_ = 1.0f - std::exp(-1.0f / (0.080f * static_cast<float>(rate)));
		reset();
	}
	void reset() {
		std::memset(x_, 0, sizeof x_);
		for (float& g : req_) g = 1.0f;
		for (float& g : g2_) g = 1.0f;
		g2_last_ = 1.0f;
		n_ = 0;
	}
	// one frame in, one frame out (kDelay frames later); returns the gain applied
	float process(float* l, float* r) {
		const int i = n_ & (kRing - 1);
		x_[0][i] = *l;
		x_[1][i] = *r;
		// the true peak of the frame kTaps/2 back (its interpolated neighbours need the frames after it)
		const int c = (n_ - kTaps / 2) & (kRing - 1);
		float peak = std::max(std::fabs(x_[0][c]), std::fabs(x_[1][c]));
		for (int ch = 0; ch < 2; ch++)
			for (int ph = 1; ph < 4; ph++) {
				float s = 0;
				for (int k = 0; k < kTaps; k++) s += kFir[ph][k] * x_[ch][(n_ - kTaps / 2 - kTaps / 2 + 1 + k) & (kRing - 1)];
				peak = std::max(peak, std::fabs(s));
			}
		req_[c & (kWindow - 1)] = peak > kCeiling ? kCeiling / peak : 1.0f;
		// g1: the least gain the window ahead needs (the window: frames c-kWindow+1 .. c)
		float g1 = 1.0f;
		for (int k = 0; k < kWindow; k++) g1 = std::min(g1, req_[k]);
		// g2: drops at once, released slowly; exactly 1 once close
		float g2 = g1 < g2_last_ ? g1 : g2_last_ + (g1 - g2_last_) * release_;
		if (g2 > 0.999999f) g2 = 1.0f;
		g2_last_ = g2;
		g2_[c & (kWindow - 1)] = g2;
		// the gain: g2 averaged over the window (exactly 1 when all are 1)
		bool all = true;
		float sum = 0;
		for (int k = 0; k < kWindow; k++) {
			sum += g2_[k];
			all = all && g2_[k] == 1.0f;
		}
		const float g = all ? 1.0f : sum / kWindow;
		const int o = (n_ - kDelay) & (kRing - 1);
		*l = g == 1.0f ? x_[0][o] : x_[0][o] * g;
		*r = g == 1.0f ? x_[1][o] : x_[1][o] * g;
		n_++;
		return g;
	}

private:
	static constexpr int kRing = 128; // > kDelay + kTaps, a power of 2
	// the 4x interpolator's phases 1..3 (phase 0 is the sample itself): a Kaiser-windowed sinc
	static constexpr float kFir[4][kTaps] = {
	    {0, 0, 0, 1, 0, 0, 0, 0},
	    {-0.0094f, 0.0418f, -0.1308f, 0.9067f, 0.2363f, -0.0644f, 0.0186f, -0.0040f},
	    {-0.0120f, 0.0598f, -0.1781f, 0.6303f, 0.6303f, -0.1781f, 0.0598f, -0.0120f},
	    {-0.0040f, 0.0186f, -0.0644f, 0.2363f, 0.9067f, -0.1308f, 0.0418f, -0.0094f},
	};
	float x_[2][kRing];
	float req_[kWindow];
	float g2_[kWindow];
	float g2_last_ = 1.0f;
	float release_ = 0.0003f;
	int n_ = 0;
};

class Safety {
public:
	void prepare(int rate) {
		rate_ = rate;
		lim_.prepare(rate);
		ramp_left_ = 0;
	}
	// start from silence, fading in to the target (the start, a new engine, after a fault)
	void fade_in(float seconds) {
		gain_ = 0.0f;
		start_ramp(seconds);
	}
	void set_target_db(float db) {
		target_db_ = std::clamp(db, kMasterMinDb, kMasterMaxDb);
		start_ramp(0.020f);
	}
	// the level at once, no ramp (offline renders)
	void set_now_db(float db) {
		target_db_ = std::clamp(db, kMasterMinDb, kMasterMaxDb);
		gain_ = db_to_gain(target_db_);
		ramp_left_ = 0;
	}
	float target_db() const { return target_db_; }
	void reset() { lim_.reset(); }

	struct Result {
		float peak_l = 0, peak_r = 0, min_gain = 1;
		bool fault = false;
	};
	// in place; a fault (a non-finite sample in the block): the block muted, the caller resets the engine
	Result process(float* l, float* r, uint32_t n) {
		Result res;
		for (uint32_t i = 0; i < n; i++)
			if (!std::isfinite(l[i]) || !std::isfinite(r[i])) {
				res.fault = true;
				break;
			}
		if (res.fault) {
			std::memset(l, 0, n * sizeof(float));
			std::memset(r, 0, n * sizeof(float));
			fade_in(0.300f);
		}
		for (uint32_t i = 0; i < n; i++) {
			if (ramp_left_ > 0) {
				gain_ += step_;
				if (--ramp_left_ == 0) gain_ = db_to_gain(target_db_);
			}
			float a = gain_ == 1.0f ? l[i] : l[i] * gain_;
			float b = gain_ == 1.0f ? r[i] : r[i] * gain_;
			res.min_gain = std::min(res.min_gain, lim_.process(&a, &b));
			a = std::clamp(a, -1.0f, 1.0f);
			b = std::clamp(b, -1.0f, 1.0f);
			res.peak_l = std::max(res.peak_l, std::fabs(a));
			res.peak_r = std::max(res.peak_r, std::fabs(b));
			l[i] = a;
			r[i] = b;
		}
		return res;
	}

private:
	void start_ramp(float seconds) {
		const float to = db_to_gain(target_db_);
		ramp_left_ = std::max(1, static_cast<int>(seconds * static_cast<float>(rate_)));
		step_ = (to - gain_) / static_cast<float>(ramp_left_);
	}
	Limiter lim_;
	int rate_ = 48000;
	float target_db_ = kStartDefaultDb;
	float gain_ = 0.0f;
	float step_ = 0.0f;
	int ramp_left_ = 0;
};

} // namespace awva_app
