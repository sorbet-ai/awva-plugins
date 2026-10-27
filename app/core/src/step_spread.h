// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// MIDI on the engine's steps: the frame each message is played at, so that no step (the engine's
// 2 ms) gets more than kPerStep messages, whatever the buffers the messages came in. The engine's
// tables take a few hundred a block (the board's sizes: 256); a burst beyond goes on over the next
// steps, in order.
//
// The audio thread places every message it takes, in time order, at the frame it is due (the
// buffer's first): a step that is full sends it to the next. The count is kept across buffers: a
// burst taken in two buffers (the MIDI came while the audio thread was taking it) goes on where the
// first part ended, instead of piling the second part onto the first part's last step.
#pragma once

#include <algorithm>
#include <cstdint>

namespace awva_app {

class StepSpread {
public:
	static constexpr int kPerStep = 64; // MIDI messages placed in one engine step at most

	// a new instance at its frame: nothing placed yet
	void reset(uint64_t frame) {
		slot_ = last_ = frame;
		n_ = 0;
	}
	// the frame of the next message, due at frame `at` (messages in time order), on steps of `step`
	// frames
	uint64_t place(uint64_t at, uint64_t step) {
		uint64_t f = std::max(at, last_);
		if (f >= slot_ + step) { // a step of its own
			slot_ = f;
			n_ = 0;
		} else if (n_ == kPerStep) { // this step is full: the next one
			slot_ += step;
			f = std::max(f, slot_);
			n_ = 0;
		}
		n_++;
		last_ = f;
		return f;
	}

private:
	uint64_t slot_ = 0, last_ = 0; // the step being filled (its first frame), the last frame placed
	int n_ = 0;                    // messages in that step
};

} // namespace awva_app
