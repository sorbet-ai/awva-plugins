// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The generic plugin's descriptive extensions (plugin.h): the audio and note ports, the parameters
// (the component's by their stable ids, and mono), the voice info, the tail, clap-wrapper's VST3
// info, and the voice-controls test extension.
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "plugin.h"

namespace awva_plugin {

// --- extensions ----------------------------------------------------------------------------------
uint32_t audio_count(const clap_plugin*, bool is_input) { return is_input ? 0 : 1; }
bool audio_get(const clap_plugin*, uint32_t index, bool is_input, clap_audio_port_info* info) {
	if (is_input || index != 0) return false;
	info->id = 0;
	std::snprintf(info->name, sizeof info->name, "Output");
	info->flags = CLAP_AUDIO_PORT_IS_MAIN;
	info->channel_count = 2;
	info->port_type = CLAP_PORT_STEREO;
	info->in_place_pair = CLAP_INVALID_ID;
	return true;
}
extern const clap_plugin_audio_ports kAudioPorts = {audio_count, audio_get};

uint32_t note_count(const clap_plugin*, bool is_input) { return is_input ? 1 : 0; }
bool note_get(const clap_plugin*, uint32_t index, bool is_input, clap_note_port_info* info) {
	if (!is_input || index != 0) return false;
	info->id = 0;
	info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI | CLAP_NOTE_DIALECT_MIDI_MPE | CLAP_NOTE_DIALECT_MIDI2;
	info->preferred_dialect = CLAP_NOTE_DIALECT_MIDI2;
	std::snprintf(info->name, sizeof info->name, "Notes");
	return true;
}
extern const clap_plugin_note_ports kNotePorts = {note_count, note_get};

uint32_t params_count(const clap_plugin* p) { return static_cast<uint32_t>(self(p)->d.num_params) + 1; }
bool params_info(const clap_plugin* p, uint32_t index, clap_param_info* info) {
	const Plugin& s = *self(p);
	std::memset(info, 0, sizeof *info);
	const int i = static_cast<int>(index);
	if (i == s.d.num_params) { // the last: mono
		info->id = kMonoId;
		info->flags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_STEPPED;
		std::snprintf(info->name, sizeof info->name, "mono");
		std::snprintf(info->module, sizeof info->module, "play");
		info->min_value = 0;
		info->max_value = 1;
		info->default_value = 0;
		return true;
	}
	if (i > s.d.num_params) return false;
	const nsc_param& d = s.d.params[i];
	info->id = d.id;
	info->flags = CLAP_PARAM_IS_AUTOMATABLE | (d.kind != NSC_PARAM_CONTINUOUS ? CLAP_PARAM_IS_STEPPED : 0);
	// the label for people where the engine has one (its text; CLAP, VST3 and AU have no field for
	// the hover text: the editor shows it), the name otherwise; the id stays the host's key
	std::snprintf(info->name, sizeof info->name, "%s", d.label && *d.label ? d.label : d.name);
	std::snprintf(info->module, sizeof info->module, "%s", d.group ? d.group : "");
	info->min_value = d.min;
	info->max_value = d.max;
	info->default_value = d.def;
	return true;
}
bool params_value(const clap_plugin* p, clap_id id, double* v) {
	const Plugin& s = *self(p);
	if (id == kMonoId) {
		*v = s.pending.st.load() == 2 && s.pending.mono >= 0 ? s.pending.mono : (s.mono ? 1.0 : 0.0);
		return true;
	}
	const int i = s.index_of(id);
	if (i < 0) return false;
	*v = s.value(i);
	return true;
}
bool params_to_text(const clap_plugin* p, clap_id id, double v, char* out, uint32_t size) {
	const Plugin& s = *self(p);
	if (id == kMonoId) {
		std::snprintf(out, size, "%s", v >= 0.5 ? "on" : "off");
		return true;
	}
	const int i = s.index_of(id);
	if (i < 0) return false;
	if (s.d.params[i].kind != NSC_PARAM_CONTINUOUS) std::snprintf(out, size, "%d", static_cast<int>(std::lround(v)));
	else std::snprintf(out, size, "%.4g", v);
	return true;
}
bool params_from_text(const clap_plugin* p, clap_id id, const char* text, double* v) {
	if (id == kMonoId) {
		if (!std::strcmp(text, "on") || !std::strcmp(text, "off")) {
			*v = !std::strcmp(text, "on") ? 1.0 : 0.0;
			return true;
		}
	} else if (self(p)->index_of(id) < 0) {
		return false;
	}
	char* end = nullptr;
	*v = std::strtod(text, &end);
	return end != text;
}
void params_flush(const clap_plugin* p, const clap_input_events* in, const clap_output_events* out) {
	Plugin& s = *self(p);
	if (s.active) s.apply_pending(); // (the audio thread)
	s.gui_drain(out);
	const uint32_t n = in->size(in);
	for (uint32_t k = 0; k < n; k++) {
		const clap_event_header* h = in->get(in, k);
		if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_PARAM_VALUE)
			s.param(*reinterpret_cast<const clap_event_param_value*>(h));
	}
}
extern const clap_plugin_params kParams = {params_count, params_info, params_value, params_to_text, params_from_text, params_flush};

bool voice_get(const clap_plugin* p, clap_voice_info* info) {
	info->voice_count = static_cast<uint32_t>(self(p)->nv);
	info->voice_capacity = info->voice_count;
	info->flags = CLAP_VOICE_INFO_SUPPORTS_OVERLAPPING_NOTES;
	return true;
}
extern const clap_plugin_voice_info kVoiceInfo = {voice_get};

uint32_t tail_get(const clap_plugin* p) {
	const Plugin& s = *self(p);
	return static_cast<uint32_t>(std::ceil(static_cast<double>(s.d.tail_seconds) * s.sr));
}
extern const clap_plugin_tail kTail = {tail_get};

bool set_source(const clap_plugin* p, const awva_voice_source* src) {
	Plugin& s = *self(p);
	if (s.active || (src && !src->block)) return false;
	s.use_source = src != nullptr;
	if (src) s.source = *src;
	return true;
}
extern const awva_plugin_voice_controls kVoiceControls = {set_source};

// clap-wrapper's VST3: the MIDI controllers of all 16 channels (IMidiMapping), and the note
// expressions the engines take: poly pressure, tuning and brightness
uint32_t vst3_channels(const clap_plugin*, uint32_t) { return 16; }
uint32_t vst3_expressions(const clap_plugin*) {
	return AS_VST3_NOTE_EXPRESSION_PRESSURE | AS_VST3_NOTE_EXPRESSION_TUNING | AS_VST3_NOTE_EXPRESSION_BRIGHTNESS;
}
extern const clap_plugin_as_vst3 kAsVst3 = {vst3_channels, vst3_expressions};

} // namespace awva_plugin
