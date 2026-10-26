// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The Windows volume, applied by the app where the OS's mixer doesn't (WASAPI exclusive mode
// bypasses it): the endpoint's master level in dB as a linear gain, 0 at the slider's 0 or muted,
// 1 for what the device applies itself in hardware; ramped over a buffer when it moves. The same
// mapping the shared mixer applies, so exclusive at a volume plays at the level shared does.
#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>

namespace awva_app {

// the gain a shared stream gets from the mixer at this endpoint setting
inline float endpoint_gain(float level_db, float scalar, bool muted, bool hw_volume, bool hw_mute) {
	float g = 1.0f;
	if (!hw_volume) g = scalar <= 0.0f ? 0.0f : (level_db >= 0.0f ? 1.0f : std::pow(10.0f, level_db / 20.0f));
	if (!hw_mute && muted) g = 0.0f;
	return g;
}

class EndpointGain {
public:
	void set(float g) { target_.store(g); } // any thread
	float applied() const { return cur_; }
	// in place, the audio thread: a ramp over this buffer when the target moved
	void apply(float* l, float* r, uint32_t n) {
		const float t = target_.load();
		if (cur_ == t) {
			if (t == 1.0f) return;
			for (uint32_t i = 0; i < n; i++) l[i] *= t, r[i] *= t;
			return;
		}
		const float from = cur_, span = t - cur_, len = static_cast<float>(n ? n : 1);
		for (uint32_t i = 0; i < n; i++) { // (each step from the start: the ramp ends on the target exactly)
			const float g = i + 1 == n ? t : from + span * (static_cast<float>(i + 1) / len);
			l[i] *= g;
			r[i] *= g;
		}
		cur_ = t;
	}
	void reset(float g) {
		target_.store(g);
		cur_ = g;
	}

private:
	std::atomic<float> target_{1.0f};
	float cur_ = 1.0f;
};

} // namespace awva_app
