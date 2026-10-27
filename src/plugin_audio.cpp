// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The generic plugin's audio (plugin.h): the component's 32-frame blocks, played out at any host
// block size (a late event to the next block), subnormals flushed; process(), a parameter applied
// at its time.
#include <cstring>
#include <iterator>

#include "plugin.h"

namespace awva_plugin {

void Plugin::render_block() {
	const uint64_t b0 = blocks * NSC_BLOCK;
	int n = 0;
	while (count > 0 && n < static_cast<int>(std::size(block_ev))) {
		const Ev& e = queue[head];
		if (e.frame >= b0 + NSC_BLOCK) break;
		// (an event whose block was rendered before it came: at this block's frame 0)
		// (an event that came after its block was rendered: before this block, a late event)
		const int64_t f = static_cast<int64_t>(e.frame) - static_cast<int64_t>(b0);
		block_ev[n].frame = static_cast<int32_t>(f); // (a late event: -31 .. -1)
		block_ev[n].note_id = e.note_id;
		std::memcpy(block_ev[n].words, e.w, sizeof e.w);
		n++;
		head = (head + 1) % kQueue;
		count--;
	}
	float* out[2] = {buf[0], buf[1]};
	if (use_source) {
		source.block(source.ctx, blocks, vc.data(), nv, &g);
		c.process_voices(state, vc.data(), &g, out);
	} else {
		c.process(state, NSC_BLOCK, block_ev, n, out); // (a block a call: the engine's own, as the events are queued)
		ended_notes(call_frame);
	}
	if (d.channels < 2) std::memcpy(buf[1], buf[0], sizeof buf[0]);
	blocks++;
	pos = 0;
}
void Plugin::render(float* l, float* r, uint32_t from, uint32_t to) {
	for (uint32_t i = from; i < to; i++) {
		if (pos == NSC_BLOCK) {
			call_frame = i;
			render_block();
		}
		l[i] = flush(buf[0][pos]);
		r[i] = flush(buf[1][pos]);
		pos++;
	}
}
clap_process_status Plugin::process(const clap_process* p) {
	const uint32_t n = p->frames_count;
	if (p->audio_outputs_count < 1 || p->audio_outputs[0].channel_count < 2 || !p->audio_outputs[0].data32) return CLAP_PROCESS_ERROR;
	float* l = p->audio_outputs[0].data32[0];
	float* r = p->audio_outputs[0].data32[1];
	const uint64_t start = frame; // the call's first frame: its events' times count from it
	out_events = p->out_events;
	apply_pending();
	gui_drain(p->out_events);
	// the notes and MIDI of the whole call first (each waits for its block), then the audio,
	// a parameter applied at its time
	const uint32_t ne = p->in_events->size(p->in_events);
	for (uint32_t k = 0; k < ne; k++) {
		const clap_event_header* h = p->in_events->get(p->in_events, k);
		if (h->space_id != CLAP_CORE_EVENT_SPACE_ID || h->type != CLAP_EVENT_PARAM_VALUE) event(start, h);
	}
	uint32_t done = 0;
	for (uint32_t k = 0; k < ne; k++) {
		const clap_event_header* h = p->in_events->get(p->in_events, k);
		if (h->space_id != CLAP_CORE_EVENT_SPACE_ID || h->type != CLAP_EVENT_PARAM_VALUE) continue;
		const uint32_t t = std::min(h->time, n);
		if (t > done) {
			render(l, r, done, t);
			done = t;
		}
		param(*reinterpret_cast<const clap_event_param_value*>(h));
	}
	render(l, r, done, n);
	frame = start + n;
	out_events = nullptr;
	return CLAP_PROCESS_CONTINUE;
}

} // namespace awva_plugin
