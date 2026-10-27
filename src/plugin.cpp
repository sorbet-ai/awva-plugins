// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The generic awva plugin: one engine component (nsc.h, the C ABI) as a CLAP instrument. Nothing
// here knows the engine or the SDK: the component is nsc_entry() (each plugin links its engine's
// component in, built by awva_add_component), and everything the plugin shows comes from its
// descriptor: the id, the title and summary, the parameters, the presets, the voices, the tail,
// the sample rates. clap-wrapper makes the same code a VST3 and an AUv2.
//
// Notes, every dialect as UMP into the component (its decoder, up-converter, voice allocator):
//   CLAP      every note as MIDI 2.0 on its own channel (CLAP's channel 0 .. 15 is MIDI channel
//             1 .. 16, so a VST3 host's MPE, its notes and the MIDI controllers of their channels,
//             plays as MPE), per note: Note On/Off with 16-bit velocity and the note's id
//             (nsc_event.note_id); pressure -> Poly Pressure, tuning -> Registered Per-Note
//             Controller 3 (Pitch 7.25, the absolute pitch), brightness -> Registered Per-Note
//             Controller 74 (the note's own timbre, bipolar: 0.5 is its centre). Volume, pan, vibrato and expression aren't
//             engine inputs. A NOTE_END comes when the component's ended() hands the note back
//             (its tail passed, the engine went idle, or a newer note took its channel and key).
//   MIDI      MIDI 1.0 bytes as MIDI 1.0 in UMP (type 2); MIDI_MPE the same
//   MIDI2     UMP as it comes (types 4 and 2 play, the component ignores the others). The
//             preferred dialect: clap-wrapper's AU then asks its host for MIDI 2.0, and gives
//             MIDI 1.0 to the plugin as MIDI (not as CLAP notes, whose bends it fixes at +-2)
// VST3 (clap-wrapper): notes and their expressions as CLAP notes (poly pressure -> pressure, and
// the tuning and brightness note expressions, which the clap.plugin-info-as-vst3 extension
// declares), and the MIDI controllers of the 16 channels (IMidiMapping: pitch bend, channel
// pressure, CCs) as MIDI 1.0.
// Time: the component renders 32-frame blocks and takes its events in 2 ms steps on an integer
// frame grid (nsc.h). A block is rendered when its first frame is due, with every event queued
// for its frames; an event that arrives after its block was rendered (a host block ending inside
// a 32-frame block) goes to the next block with its negative frame (a late event, nsc.h), so the
// component takes it at its own step: any host block size gives the same samples, at any rate.
// A parameter applies from the first block at or after its time.
// Parameters: the component's, by its stable ids, plus "mono" (the component's mono mode, NSC_MODE_MONO: one voice, played by every held key).
// Presets: the factory presets through preset-discovery and preset-load (clap-wrapper makes them
// the VST3's preset list and the AU's factory presets), and preset files (awva-preset/1, the
// component's load_preset) through preset-load. State: the sound as an awva-preset/1 file (the
// component's save_preset), with the plugin's own beside it in one more key ("awva-plugin": the
// output level, mono, the factory preset it started from), which a preset reader skips.
// Output: stereo (a mono engine on both channels), at unity (awva's output is nominal +-1), subnormals flushed.
#include <clap/clap.h>

#include <cstdio>
#include <cstring>
#include <new>

#include "plugin.h"

namespace awva_plugin {

const nsc_component& comp() {
	static const nsc_component* c = nsc_entry();
	return *c;
}
const nsc_desc& desc() { return *comp().desc; }

// the plugin id: com.sorbet-ai.awva.<the engine's id>
const char* plugin_id() {
	static const std::string id = std::string("com.sorbet-ai.awva.") + desc().id;
	return id.c_str();
}

// the AU subtype: the id's first four letters, the first capitalised, "_" filling ("va" -> Va__);
// CMakeLists.txt makes the same for auval and checks no two engines share one
std::string au_subtype() {
	std::string s = std::string(desc().id).substr(0, 4);
	while (s.size() < 4) s += '_';
	if (s[0] >= 'a' && s[0] <= 'z') s[0] = static_cast<char>(s[0] - 'a' + 'A');
	return s;
}

// the CLAP descriptor, from the component's
const clap_plugin_descriptor& descriptor() {
	static std::string description;
	static const char* const features[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER, CLAP_PLUGIN_FEATURE_STEREO, nullptr};
	static clap_plugin_descriptor d;
	static const bool done = [] {
		const nsc_desc& n = desc();
		description = std::string(n.summary ? n.summary : "") + " (awva " + (n.version ? n.version : "") + ")";
		d = {CLAP_VERSION_INIT, plugin_id(), n.name, kVendor, "", "", "", kVersion, description.c_str(), features};
		return true;
	}();
	(void)done;
	return d;
}

void* region(size_t n) {
	n = std::max<size_t>((n + 63) & ~static_cast<size_t>(63), 64);
	void* p = ::operator new(n, std::align_val_t(64));
	std::memset(p, 0, n);
	return p;
}
void region_free(void* p) { ::operator delete(p, std::align_val_t(64)); }

Plugin::Plugin(const clap_host* h) : host(h), nv(std::min(kMaxVoices, d.max_voices)) {
	for (int i = 0; i < d.num_params; i++)
		if (!std::strcmp(d.params[i].name, "output")) out_index = i;
	state = region(d.hot_bytes);
	stream = region(d.stream_bytes);
	bulk = region(d.bulk_bytes);
	(void)init_at(state, stream, bulk, sr); // (48 kHz: in every engine's range)
	factory = c.preset(state); // (the preset init() leaves: the engine's default, -1 for none)
	pending.values.assign(static_cast<size_t>(d.num_params), NAN);
	gui_val.assign(static_cast<size_t>(d.num_params), NAN);
	vc.assign(static_cast<size_t>(nv), nsc_voice{});
}
void Plugin::free_regions() {
	region_free(state);
	region_free(stream);
	region_free(bulk);
	if (sx_state) {
		region_free(sx_state);
		region_free(sx_stream);
		region_free(sx_bulk);
	}
}
int32_t Plugin::init_at(void* s, void* st, void* bu, int rate) const {
	const nsc_config cfg = {rate, 0, nv};
	return c.init(s, st, bu, &cfg);
}
void Plugin::rescan_values() {
	if (const auto* hp = static_cast<const clap_host_params*>(host->get_extension(host, CLAP_EXT_PARAMS)))
		hp->rescan(host, CLAP_PARAM_RESCAN_VALUES);
}
bool Plugin::activate(double rate) {
	const int new_sr = static_cast<int>(std::lround(rate));
	if (new_sr < d.min_rate || new_sr > d.max_rate) return false; // (the engine's rates: init() refuses others)
	if (new_sr != sr) { // init at the new rate (nsc.h): the preset, the values and mono again
		const int preset = c.preset(state);
		std::vector<float> values(static_cast<size_t>(d.num_params));
		for (int i = 0; i < d.num_params; i++) values[static_cast<size_t>(i)] = c.param(state, i);
		if (init_at(state, stream, bulk, new_sr) != 0) return false;
		sr = new_sr;
		apply(preset, mono ? 1 : 0, values);
	} else {
		c.reset(state);
	}
	clear();
	active = true;
	return true;
}
void Plugin::clear() {
	frame = blocks = 0;
	pos = NSC_BLOCK;
	head = count = 0;
	for (Note& n : notes) n = Note{};
}

// --- the plugin ----------------------------------------------------------------------------------
bool p_init(const clap_plugin*) { return true; }
void p_destroy(const clap_plugin* p) { delete self(p); }
bool p_activate(const clap_plugin* p, double sample_rate, uint32_t, uint32_t) { return self(p)->activate(sample_rate); }
void p_deactivate(const clap_plugin* p) {
	Plugin& s = *self(p);
	s.active = false;
	if (s.pending.st.load() == 2) { // a load the audio thread didn't get to
		s.pending.st = 3;
		s.apply(s.pending.preset, s.pending.mono, s.pending.values);
		s.pending.st = 0;
		s.rescan_values();
	}
}
bool p_start(const clap_plugin*) { return true; }
void p_stop(const clap_plugin*) {}
void p_reset(const clap_plugin* p) {
	Plugin& s = *self(p);
	s.c.reset(s.state);
	s.clear();
}
clap_process_status p_process(const clap_plugin* p, const clap_process* pr) { return self(p)->process(pr); }
const void* p_extension(const clap_plugin*, const char* id) {
	if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &kAudioPorts;
	if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS)) return &kNotePorts;
	if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &kParams;
	if (!std::strcmp(id, CLAP_EXT_STATE)) return &kState;
	if (!std::strcmp(id, CLAP_EXT_VOICE_INFO)) return &kVoiceInfo;
	if (!std::strcmp(id, CLAP_EXT_TAIL)) return &kTail;
	if (!std::strcmp(id, CLAP_EXT_GUI)) return &kGui;
#if defined(__linux__)
	if (!std::strcmp(id, CLAP_EXT_POSIX_FD_SUPPORT)) return &kFdSupport;
	if (!std::strcmp(id, CLAP_EXT_TIMER_SUPPORT)) return &kTimerSupport;
#endif
	if (!std::strcmp(id, CLAP_EXT_PRESET_LOAD) || !std::strcmp(id, CLAP_EXT_PRESET_LOAD_COMPAT)) return &kPresetLoad;
	if (!std::strcmp(id, AWVA_EXT_VOICE_CONTROLS)) return &kVoiceControls;
	if (!std::strcmp(id, CLAP_PLUGIN_AS_VST3)) return &kAsVst3;
	return nullptr;
}
void p_main_thread(const clap_plugin* p) {
	// the audio thread applied a loaded preset or state: the host reads the values again
	if (self(p)->rescan.exchange(false)) self(p)->rescan_values();
}

const clap_plugin* create(const clap_plugin_factory*, const clap_host* host, const char* id) {
	if (!clap_version_is_compatible(host->clap_version) || std::strcmp(id, descriptor().id) != 0) return nullptr;
	Plugin* s = new Plugin(host);
	s->plugin = {&descriptor(), s, p_init, p_destroy, p_activate, p_deactivate, p_start, p_stop, p_reset, p_process, p_extension,
	    p_main_thread};
	return &s->plugin;
}
uint32_t factory_count(const clap_plugin_factory*) { return 1; }
const clap_plugin_descriptor* factory_desc(const clap_plugin_factory*, uint32_t i) { return i == 0 ? &descriptor() : nullptr; }
const clap_plugin_factory kFactory = {factory_count, factory_desc, create};

// clap-wrapper's AU description (clapwrapper/auv2.h): the AU's codes; its name and version are
// the CLAP descriptor's
bool auv2_info(const clap_plugin_factory_as_auv2*, uint32_t index, clap_plugin_info_as_auv2_t* info) {
	if (index != 0) return false;
	std::snprintf(info->au_type, sizeof info->au_type, "aumu");
	std::snprintf(info->au_subt, sizeof info->au_subt, "%s", au_subtype().c_str());
	return true;
}
const clap_plugin_factory_as_auv2 kAuv2Factory = {"Awva", kVendor, auv2_info};

bool entry_init(const char*) { return comp().desc && comp().desc->magic == NSC_MAGIC && comp().desc->abi == NSC_ABI; }
void entry_deinit() {}
const void* entry_factory(const char* id) {
	if (!std::strcmp(id, CLAP_PLUGIN_FACTORY_ID)) return &kFactory;
	if (!std::strcmp(id, CLAP_PRESET_DISCOVERY_FACTORY_ID) || !std::strcmp(id, CLAP_PRESET_DISCOVERY_FACTORY_ID_COMPAT)) return &kPresetFactory;
	if (!std::strcmp(id, CLAP_PLUGIN_FACTORY_INFO_AUV2)) return &kAuv2Factory;
	return nullptr;
}

} // namespace awva_plugin

using namespace awva_plugin;

extern "C" CLAP_EXPORT const clap_plugin_entry clap_entry = {CLAP_VERSION_INIT, entry_init, entry_deinit, entry_factory};
