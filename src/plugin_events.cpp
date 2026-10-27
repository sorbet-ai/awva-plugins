// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The generic plugin's events (plugin.h): CLAP notes, their expressions, MIDI 1.0 and MIDI 2.0 as
// UMP into the component's queue (each waits for its 32-frame block), the NOTE_ENDs the component
// hands back, and parameter values.
#include <cstring>

#include "plugin.h"

namespace awva_plugin {

void Plugin::push(uint64_t f, uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3, int32_t note_id) {
	if (count == kQueue) return; // (4096 events ahead of the audio: dropped)
	queue[(head + count) % kQueue] = {f, {w0, w1, w2, w3}, note_id};
	count++;
}
void Plugin::midi1(uint64_t f, const uint8_t* m) {
	const uint8_t s = m[0] & 0xf0;
	if (s < 0x80 || s == 0xf0) return; // (system messages: not notes)
	const bool two = s == 0xc0 || s == 0xd0;
	push(f, 0x20000000u | static_cast<uint32_t>(m[0]) << 16 | static_cast<uint32_t>(m[1] & 0x7f) << 8 | (two ? 0u : m[2] & 0x7fu));
}
bool Plugin::matches(const Note& n, int32_t id, int16_t port, int16_t channel, int16_t key) const {
	if (!n.used || !n.held) return false;
	if (id >= 0) return n.id == id;
	return (port < 0 || n.port == port) && (channel < 0 || n.channel == channel) && (key < 0 || n.key == key);
}
void Plugin::note_on(uint64_t f, const clap_event_note& e) {
	if (e.key < 0 || e.key > 127) return;
	const int16_t channel = e.channel >= 0 && e.channel < 16 ? e.channel : 0;
	Note* slot = nullptr;
	for (Note& n : notes) {
		if (n.used && n.held && n.channel == channel && n.key == e.key) n.superseded = true; // (its channel and key are the new note's now)
		if (!slot && !n.used) slot = &n;
	}
	if (!slot) return; // (128 notes not yet ended: dropped)
	*slot = {true, true, false, next_token, e.note_id, e.port_index, channel, e.key};
	next_token = (next_token + 1) & 0x7fffffff;
	push(f, w0_of(0x90, static_cast<uint8_t>(channel), static_cast<uint8_t>(e.key), 0), u16_of(e.velocity) << 16, 0, 0, slot->token);
}
void Plugin::note_off(uint64_t f, const clap_event_note& e) {
	for (Note& n : notes) {
		if (!matches(n, e.note_id, e.port_index, e.channel, e.key)) continue;
		n.held = false;
		if (!n.superseded)
			push(f, w0_of(0x80, static_cast<uint8_t>(n.channel), static_cast<uint8_t>(n.key), 0), u16_of(e.velocity) << 16);
	}
}
void Plugin::expression(uint64_t f, const clap_event_note_expression& e) {
	for (Note& n : notes) {
		if (!matches(n, e.note_id, e.port_index, e.channel, e.key) || n.superseded) continue;
		const uint8_t ch = static_cast<uint8_t>(n.channel), key = static_cast<uint8_t>(n.key);
		switch (e.expression_id) {
		case CLAP_NOTE_EXPRESSION_PRESSURE: push(f, w0_of(0xa0, ch, key, 0), u32_of(e.value)); break;
		case CLAP_NOTE_EXPRESSION_TUNING: { // Pitch 7.25: the note's absolute pitch, semitones
			const double pitch = std::clamp(static_cast<double>(n.key) + e.value, 0.0, 127.99999);
			push(f, w0_of(0x00, ch, key, 3), static_cast<uint32_t>(pitch * 33554432.0));
			break;
		}
		case CLAP_NOTE_EXPRESSION_BRIGHTNESS: push(f, w0_of(0x00, ch, key, 74), u32_of(e.value)); break;
		default: break; // volume, pan, vibrato, expression: not engine inputs
		}
	}
}
// the notes the component handed back since the last block: their NOTE_END, at this frame of the call
void Plugin::ended_notes(uint32_t t) {
	int32_t ids[32];
	for (;;) {
		const int32_t got = c.ended(state, ids, 32);
		for (int32_t k = 0; k < got; k++)
			for (Note& n : notes) {
				if (!n.used || n.token != ids[k]) continue;
				if (out_events) {
					clap_event_note e = {{sizeof(clap_event_note), t, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_END, 0}, n.id, n.port,
					    n.channel, n.key, 0.0};
					out_events->try_push(out_events, &e.header);
				}
				n = Note{};
			}
		if (got < 32) return;
	}
}
// a parameter change (the audio thread, or the main thread while inactive)
void Plugin::param(const clap_event_param_value& e) {
	if (e.param_id == kMonoId) {
		mono = e.value >= 0.5;
		(void)c.mode(state, NSC_MODE_MONO, mono ? 1 : 0);
		return;
	}
	const int i = index_of(e.param_id);
	if (i >= 0) (void)c.set_param(state, i, static_cast<float>(e.value));
}
void Plugin::event(uint64_t start, const clap_event_header* h) {
	if (h->space_id != CLAP_CORE_EVENT_SPACE_ID) return;
	const uint64_t f = start + h->time;
	switch (h->type) {
	case CLAP_EVENT_NOTE_ON: note_on(f, *reinterpret_cast<const clap_event_note*>(h)); break;
	case CLAP_EVENT_NOTE_OFF:
	case CLAP_EVENT_NOTE_CHOKE: note_off(f, *reinterpret_cast<const clap_event_note*>(h)); break;
	case CLAP_EVENT_NOTE_EXPRESSION: expression(f, *reinterpret_cast<const clap_event_note_expression*>(h)); break;
	case CLAP_EVENT_MIDI: midi1(f, reinterpret_cast<const clap_event_midi*>(h)->data); break;
	case CLAP_EVENT_MIDI2: {
		const uint32_t* w = reinterpret_cast<const clap_event_midi2*>(h)->data;
		push(f, w[0], w[1], w[2], w[3]);
		break;
	}
	default: break;
	}
}

} // namespace awva_plugin
