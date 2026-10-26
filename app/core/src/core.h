// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The core's C++ side, for the CLI and the tests: the offline renderer (the same Player the
// live callback plays through) and a synthetic MPE controller stream, for tests and smoke runs.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "catalog.h"

namespace awva_app {

struct TimedUmp {
	uint64_t frame;
	uint32_t w[4];
};

// an engine's preset entry (factory or file) played with these events, through the Player: the
// instance at the rate, the master level at master_db at once (no fade), the safety chain. The
// output is delayed by Limiter::kDelay frames (removed here: out[0] is frame 0's sample).
// safety false: the samples the Player gives the safety chain (the engine's, as the plugin's)
bool render_offline(const nsc_component& c, const PresetEntry* preset, bool legato, const std::vector<TimedUmp>& events,
    uint64_t frames, int rate, float master_db, std::vector<float>* l, std::vector<float>* r, bool safety = true);

// the MPE stream a typical expressive MPE controller sends over USB MIDI: an MPE Configuration
// Message on channel 1 (RPN 6, the voices), the bend range on every member channel (RPN 0, 48 semitones), then per note on its own
// member channel from channel 2: the pitch bend first, the Note On, then pitch bend (x as bend:
// glides and vibrato), channel pressure and CC74 (tilt y: 64 + 64 y) as the finger moves, the
// Note Off. MIDI 1.0 in UMP (type 2), as WinMM gives it to the core.
std::vector<TimedUmp> mpe_controller_performance(int rate, double seconds);

// MIDI 1.0 bytes (whole messages) as MIDI 1.0 in UMP (type 2); system messages skipped
std::vector<uint32_t> midi1_to_ump(const uint8_t* bytes, size_t n);

} // namespace awva_app
