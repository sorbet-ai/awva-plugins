// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The generic awva plugin's parts (the design: plugin.cpp's header): the engine component and its
// CLAP descriptor, the plugin instance (Plugin), and the extension tables each part defines.
//   plugin.cpp           the component and descriptor, an instance's life, the CLAP plugin, the entry
//   plugin_events.cpp    notes, expressions, MIDI and parameter events into the component's UMP queue
//   plugin_state.cpp     presets and state; the state, preset-load and preset-discovery extensions
//   plugin_audio.cpp     the 32-frame blocks and process()
//   plugin_editor.cpp    the editor (src/editor) as the gui extension, and its changes to the host
//   plugin_ports.cpp     the audio and note ports, parameters, voice info, tail, VST3 info, the test
//                        extension
#pragma once

#include <clap/clap.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "awva_plugin/voice_controls.h"
#include "editor/view.h"
#include "clapwrapper/auv2.h"
#include "clapwrapper/vst3.h"
#include "nsc/nsc.h"

namespace awva_plugin {

constexpr int kMaxVoices = 10;  // (awva_render's default)
constexpr clap_id kMonoId = 1; // (an engine parameter's id is FNV-1a of its name: never 1, the tests check)
constexpr const char* kVendor = "Sorbet AI";
constexpr const char* kVersion = "0.1.0";

const nsc_component& comp();
const nsc_desc& desc();
const char* plugin_id(); // com.sorbet-ai.awva.<the engine's id>
std::string au_subtype();
const clap_plugin_descriptor& descriptor();

inline uint32_t u32_of(double v) { // 0 .. 1 -> 0 .. 0xffffffff
	return static_cast<uint32_t>(std::clamp(v, 0.0, 1.0) * 4294967295.0 + 0.5);
}
inline uint32_t u16_of(double v) { return static_cast<uint32_t>(std::clamp(v, 0.0, 1.0) * 65535.0 + 0.5); }

// a subnormal sample as zero, its sign kept: hosts and validators reject subnormals, and they are
// below anything audible (the engine's own arithmetic is left as it is: nsc.h)
inline float flush(float x) { return x != 0.0f && std::fabs(x) < FLT_MIN ? std::copysign(0.0f, x) : x; }

void* region(size_t n); // 64-byte aligned, zeroed
void region_free(void* p);

// a CLAP note, from its Note On to its NOTE_END
struct Note {
	bool used = false;       // until its NOTE_END
	bool held = false;       // no note-off yet
	bool superseded = false; // a newer note took its key: its note-off and expressions don't go out
	int32_t token = -1;      // its id in the component (nsc_event.note_id; ours, as a host's may be -1)
	int32_t id = -1;
	int16_t port = 0, channel = 0, key = 0; // as the host named it (channel: its MIDI channel too)
};

// a preset or a state loaded on the main thread while the plugin is active: the audio thread
// applies it before its next block. st: 0 free, 1 being written, 2 ready, 3 being applied
struct Pending {
	std::atomic<int> st{0};
	int preset = -1;            // -1: none
	int mono = -1;            // -1: unchanged
	std::vector<float> values; // by index; NaN: unchanged
};

struct Plugin {
	clap_plugin plugin{};
	const clap_host* host;
	const nsc_component& c = comp();
	const nsc_desc& d = desc();
	void *state, *stream, *bulk; // (the regions: hot, stream, bulk)
	int sr = 48000;
	int nv;
	bool active = false;
	std::atomic<bool> mono{false};

	// time and output
	uint64_t frame = 0;  // frames processed since activation
	uint64_t blocks = 0; // blocks rendered
	int pos = NSC_BLOCK; // the next frame of buf to play (NSC_BLOCK: render one)
	float buf[2][NSC_BLOCK] = {};

	// events waiting for their block, in time order (a ring)
	struct Ev {
		uint64_t frame;
		uint32_t w[4];
		int32_t note_id;
	};
	static constexpr size_t kQueue = 4096;
	std::vector<Ev> queue = std::vector<Ev>(kQueue);
	size_t head = 0, count = 0;
	nsc_event block_ev[1024];

	static constexpr int kNotes = 128;
	Note notes[kNotes];
	int32_t next_token = 0;
	// the process() call being rendered: NOTE_ENDs go out with its frames
	const clap_output_events* out_events = nullptr;
	uint32_t call_frame = 0; // the frame of the call being rendered

	// the test extension (voice_controls.h)
	awva_voice_source source{};
	bool use_source = false;
	std::vector<nsc_voice> vc;
	nsc_globals g{};

	Pending pending;
	std::atomic<bool> rescan{false};

	// the editor's changes, main thread -> audio thread (or params flush): a single-producer ring
	struct GuiEv {
		uint16_t type; // CLAP_EVENT_PARAM_GESTURE_BEGIN, _VALUE, _GESTURE_END
		clap_id id;
		double value;
	};
	static constexpr size_t kGui = 512;
	GuiEv gui_ev[kGui];
	std::atomic<size_t> gui_w{0}, gui_r{0};
	mutable std::vector<float> gui_val; // the editor's values not yet applied (NaN: none), main thread
	std::unique_ptr<awva_editor::Editor> editor;
	awva_editor::View* view = nullptr;
	int gui_fd = -1;                    // (Linux: the view's fd, registered with the host)
	clap_id gui_timer = CLAP_INVALID_ID; // (Linux: its timer)
	double gui_scale = 1.0;
	float gui_w_pt = static_cast<float>(awva_tokens::kEditorWidth), gui_h_pt = static_cast<float>(awva_tokens::kEditorHeight);
	struct EditorHost;
	struct EditorHostDelete {
		void operator()(EditorHost* h) const;
	};
	std::unique_ptr<EditorHost, EditorHostDelete> editor_host; // (EditorHost: plugin_editor.cpp)

	int factory = -1; // the factory preset the sound started from (preset-load, or a state); -1: none
	int out_index = -1; // the component's "output" parameter (not a preset's: the state keeps it)
	void *sx_state = nullptr, *sx_stream = nullptr, *sx_bulk = nullptr; // a scratch instance (main thread)


	explicit Plugin(const clap_host* h);
	~Plugin();
	void free_regions();
	int32_t init_at(void* s, void* st, void* bu, int rate) const;
	void rescan_values();

	int index_of(clap_id id) const { return id == kMonoId ? -1 : c.param_index(id); }

	// --- events (plugin_events.cpp) ---------------------------------------------------------------
	void push(uint64_t f, uint32_t w0, uint32_t w1 = 0, uint32_t w2 = 0, uint32_t w3 = 0, int32_t note_id = -1);
	static uint32_t w0_of(uint8_t status, uint8_t ch, uint8_t d1, uint8_t d2) {
		return 0x40000000u | static_cast<uint32_t>(status | ch) << 16 | static_cast<uint32_t>(d1) << 8 | d2;
	}
	void midi1(uint64_t f, const uint8_t* m);
	bool matches(const Note& n, int32_t id, int16_t port, int16_t channel, int16_t key) const;
	void note_on(uint64_t f, const clap_event_note& e);
	void note_off(uint64_t f, const clap_event_note& e);
	void expression(uint64_t f, const clap_event_note_expression& e);
	// the notes the component handed back since the last block: their NOTE_END, at this frame of the call
	void ended_notes(uint32_t t);
	// a parameter change (the audio thread, or the main thread while inactive)
	void param(const clap_event_param_value& e);
	void event(uint64_t start, const clap_event_header* h);

	// --- presets and state (plugin_state.cpp) ------------------------------------------------------
	void apply(int preset, int leg, const std::vector<float>& values);
	// the main thread: now while inactive, else by the audio thread before its next block
	void load_values(int preset, int leg, const std::vector<float>& values);
	void apply_pending(); // the audio thread
	// a value as the host should see it: a loaded state not yet applied first
	double value(int i) const;
	// a preset file's values (every parameter; output: keep_output), read into the scratch instance
	// so the live one isn't touched: false if it isn't this engine's preset
	bool read_preset(const std::string& json, float keep_output, std::vector<float>* values);
	// a preset file into the plugin (main thread): exactly as the component reads it while inactive,
	// else its values by the audio thread before its next block
	bool load_preset_text(const std::string& json, float output, int leg);
	float output_now() const { return out_index >= 0 ? static_cast<float>(value(out_index)) : 1.0f; }
	// the state: the sound as the component writes it (awva-preset/1), and the plugin's own key
	std::string save() const;
	bool load(const std::string& s);

	// --- the editor's changes (plugin_editor.cpp) ----------------------------------------------------
	void gui_push(uint16_t type, clap_id id, double v); // main thread
	// the audio thread (or the flush): applied, and told to the host, at frame 0
	void gui_drain(const clap_output_events* out);

	// --- audio (plugin_audio.cpp) ------------------------------------------------------------------
	void render_block();
	void render(float* l, float* r, uint32_t from, uint32_t to);
	clap_process_status process(const clap_process* p);
	bool activate(double rate);
	void clear();
};

inline Plugin* self(const clap_plugin* p) { return static_cast<Plugin*>(p->plugin_data); }

// the extensions, each part's
extern const clap_plugin_gui kGui;
#if defined(__linux__)
extern const clap_plugin_posix_fd_support kFdSupport;
extern const clap_plugin_timer_support kTimerSupport;
#endif
extern const clap_plugin_audio_ports kAudioPorts;
extern const clap_plugin_note_ports kNotePorts;
extern const clap_plugin_params kParams;
extern const clap_plugin_voice_info kVoiceInfo;
extern const clap_plugin_tail kTail;
extern const awva_plugin_voice_controls kVoiceControls;
extern const clap_plugin_as_vst3 kAsVst3;
extern const clap_plugin_state kState;
extern const clap_plugin_preset_load kPresetLoad;
extern const clap_preset_discovery_factory kPresetFactory;

} // namespace awva_plugin
