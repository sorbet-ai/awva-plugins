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
// Editor: the awva app's screen drawn natively (src/editor, app/DESIGN.md): the presets, the hero
// knobs, every parameter, mono, the theme. Its changes reach the host as CLAP gestures and values
// (the next process() or params flush puts them out and applies them), as a host's would.
#include <clap/clap.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "awva_plugin/voice_controls.h"
#include "editor/view.h"
#include "clapwrapper/auv2.h"
#include "clapwrapper/vst3.h"
#include "nsc/nsc.h"


namespace {

constexpr int kMaxVoices = 10;  // (awva_render's default)
constexpr clap_id kMonoId = 1; // (an engine parameter's id is FNV-1a of its name: never 1, the tests check)
constexpr const char* kVendor = "Sorbet AI";
constexpr const char* kVersion = "0.1.0";

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

uint32_t u32_of(double v) { // 0 .. 1 -> 0 .. 0xffffffff
	return static_cast<uint32_t>(std::clamp(v, 0.0, 1.0) * 4294967295.0 + 0.5);
}
uint32_t u16_of(double v) { return static_cast<uint32_t>(std::clamp(v, 0.0, 1.0) * 65535.0 + 0.5); }

// a subnormal sample as zero, its sign kept: hosts and validators reject subnormals, and they are
// below anything audible (the engine's own arithmetic is left as it is: nsc.h)
float flush(float x) { return x != 0.0f && std::fabs(x) < FLT_MIN ? std::copysign(0.0f, x) : x; }

void* region(size_t n) {
	n = std::max<size_t>((n + 63) & ~static_cast<size_t>(63), 64);
	void* p = ::operator new(n, std::align_val_t(64));
	std::memset(p, 0, n);
	return p;
}
void region_free(void* p) { ::operator delete(p, std::align_val_t(64)); }

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
	std::unique_ptr<EditorHost> editor_host;

	int factory = -1; // the factory preset the sound started from (preset-load, or a state); -1: none
	int out_index = -1; // the component's "output" parameter (not a preset's: the state keeps it)
	void *sx_state = nullptr, *sx_stream = nullptr, *sx_bulk = nullptr; // a scratch instance (main thread)

	explicit Plugin(const clap_host* h) : host(h), nv(std::min(kMaxVoices, d.max_voices)) {
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
	~Plugin();
	void free_regions() {
		region_free(state);
		region_free(stream);
		region_free(bulk);
		if (sx_state) {
			region_free(sx_state);
			region_free(sx_stream);
			region_free(sx_bulk);
		}
	}

	// init at a rate: the plugin's configuration (MIDI taken every block, nv voices)
	int32_t init_at(void* s, void* st, void* bu, int rate) const {
		const nsc_config cfg = {rate, 0, nv};
		return c.init(s, st, bu, &cfg);
	}

	int index_of(clap_id id) const { return id == kMonoId ? -1 : c.param_index(id); }

	void rescan_values() {
		if (const auto* hp = static_cast<const clap_host_params*>(host->get_extension(host, CLAP_EXT_PARAMS)))
			hp->rescan(host, CLAP_PARAM_RESCAN_VALUES);
	}

	// --- events --------------------------------------------------------------------------------
	void push(uint64_t f, uint32_t w0, uint32_t w1 = 0, uint32_t w2 = 0, uint32_t w3 = 0, int32_t note_id = -1) {
		if (count == kQueue) return; // (4096 events ahead of the audio: dropped)
		queue[(head + count) % kQueue] = {f, {w0, w1, w2, w3}, note_id};
		count++;
	}
	static uint32_t w0_of(uint8_t status, uint8_t ch, uint8_t d1, uint8_t d2) {
		return 0x40000000u | static_cast<uint32_t>(status | ch) << 16 | static_cast<uint32_t>(d1) << 8 | d2;
	}
	void midi1(uint64_t f, const uint8_t* m) {
		const uint8_t s = m[0] & 0xf0;
		if (s < 0x80 || s == 0xf0) return; // (system messages: not notes)
		const bool two = s == 0xc0 || s == 0xd0;
		push(f, 0x20000000u | static_cast<uint32_t>(m[0]) << 16 | static_cast<uint32_t>(m[1] & 0x7f) << 8 | (two ? 0u : m[2] & 0x7fu));
	}

	bool matches(const Note& n, int32_t id, int16_t port, int16_t channel, int16_t key) const {
		if (!n.used || !n.held) return false;
		if (id >= 0) return n.id == id;
		return (port < 0 || n.port == port) && (channel < 0 || n.channel == channel) && (key < 0 || n.key == key);
	}
	void note_on(uint64_t f, const clap_event_note& e) {
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
	void note_off(uint64_t f, const clap_event_note& e) {
		for (Note& n : notes) {
			if (!matches(n, e.note_id, e.port_index, e.channel, e.key)) continue;
			n.held = false;
			if (!n.superseded)
				push(f, w0_of(0x80, static_cast<uint8_t>(n.channel), static_cast<uint8_t>(n.key), 0), u16_of(e.velocity) << 16);
		}
	}
	void expression(uint64_t f, const clap_event_note_expression& e) {
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
	void ended_notes(uint32_t t) {
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
	void param(const clap_event_param_value& e) {
		if (e.param_id == kMonoId) {
			mono = e.value >= 0.5;
			(void)c.mode(state, NSC_MODE_MONO, mono ? 1 : 0);
			return;
		}
		const int i = index_of(e.param_id);
		if (i >= 0) (void)c.set_param(state, i, static_cast<float>(e.value));
	}
	void event(uint64_t start, const clap_event_header* h) {
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

	// --- presets and state -----------------------------------------------------------------------
	void apply(int preset, int leg, const std::vector<float>& values) {
		if (preset >= 0 && preset < d.num_presets) c.set_preset(state, preset);
		if (leg >= 0) {
			mono = leg != 0;
			(void)c.mode(state, NSC_MODE_MONO, leg);
		}
		for (int i = 0; i < d.num_params && i < static_cast<int>(values.size()); i++)
			if (!std::isnan(values[static_cast<size_t>(i)])) (void)c.set_param(state, i, values[static_cast<size_t>(i)]);
	}
	// the main thread: now while inactive, else by the audio thread before its next block
	void load_values(int preset, int leg, const std::vector<float>& values) {
		if (!active) {
			apply(preset, leg, values);
			rescan_values();
			return;
		}
		for (;;) { // take the slot (the audio thread holds it only while applying it)
			int s = pending.st.load();
			if ((s == 0 || s == 2) && pending.st.compare_exchange_weak(s, 1)) break;
		}
		pending.preset = preset;
		pending.mono = leg;
		for (size_t i = 0; i < pending.values.size(); i++) pending.values[i] = i < values.size() ? values[i] : NAN;
		pending.st = 2;
	}
	void apply_pending() { // the audio thread
		int s = 2;
		if (!pending.st.compare_exchange_strong(s, 3)) return;
		apply(pending.preset, pending.mono, pending.values);
		pending.st = 0;
		rescan = true;
		host->request_callback(host);
	}
	// a value as the host should see it: a loaded state not yet applied first
	double value(int i) const {
		const float g = gui_val[static_cast<size_t>(i)];
		if (!std::isnan(g)) {
			if (c.param(state, i) != g) return g;
			gui_val[static_cast<size_t>(i)] = NAN; // (applied)
		}
		if (pending.st.load() == 2) {
			const float v = pending.values[static_cast<size_t>(i)];
			if (!std::isnan(v)) return v;
		}
		return c.param(state, i);
	}

	// a preset file's values (every parameter; output: keep_output), read into the scratch instance
	// so the live one isn't touched: false if it isn't this engine's preset
	bool read_preset(const std::string& json, float keep_output, std::vector<float>* values) {
		if (!sx_state) {
			sx_state = region(d.hot_bytes);
			sx_stream = region(d.stream_bytes);
			sx_bulk = region(d.bulk_bytes);
		}
		if (init_at(sx_state, sx_stream, sx_bulk, sr) != 0) return false;
		if (c.load_preset(sx_state, json.data(), static_cast<uint32_t>(json.size()), nullptr) != 1) return false;
		values->assign(static_cast<size_t>(d.num_params), 0.0f);
		for (int i = 0; i < d.num_params; i++) (*values)[static_cast<size_t>(i)] = c.param(sx_state, i);
		if (out_index >= 0) (*values)[static_cast<size_t>(out_index)] = keep_output;
		return true;
	}
	// a preset file into the plugin (main thread): exactly as the component reads it while inactive,
	// else its values by the audio thread before its next block
	bool load_preset_text(const std::string& json, float output, int leg) {
		std::vector<float> values;
		if (!read_preset(json, output, &values)) return false;
		if (!active) {
			(void)c.load_preset(state, json.data(), static_cast<uint32_t>(json.size()), nullptr);
			if (out_index >= 0) (void)c.set_param(state, out_index, output);
			if (leg >= 0) {
				mono = leg != 0;
				(void)c.mode(state, NSC_MODE_MONO, leg);
			}
			rescan_values();
			return true;
		}
		load_values(-1, leg, values);
		return true;
	}
	float output_now() const { return out_index >= 0 ? static_cast<float>(value(out_index)) : 1.0f; }

	// the state: the sound as the component writes it (awva-preset/1), and the plugin's own key
	std::string save() const {
		std::string out(16384, '\0');
		const char* title = factory >= 0 && factory < d.num_presets ? d.presets[factory].name : d.name;
		const uint32_t n = c.save_preset(state, title, out.data(), static_cast<uint32_t>(out.size()));
		out.resize(n);
		const size_t close = out.rfind('}');
		if (n == 0 || close == std::string::npos) return {};
		char extra[160];
		std::snprintf(extra, sizeof extra, ",\n  \"awva-plugin\": {\"output\": %.9g, \"mono\": %s, \"factory_preset\": %d}\n",
		    static_cast<double>(out_index >= 0 ? c.param(state, out_index) : 1.0f), mono ? "true" : "false", factory);
		size_t last = close; // (after the last value, before the closing brace)
		while (last > 0 && (out[last - 1] == '\n' || out[last - 1] == ' ' || out[last - 1] == '\r')) last--;
		return out.substr(0, last) + extra + out.substr(close);
	}
	bool load(const std::string& s) {
		float output = 1.0f;
		int leg = 0, fac = -1;
		const size_t at = s.rfind("\"awva-plugin\"");
		if (at != std::string::npos) {
			char lg[8] = {};
			if (std::sscanf(s.c_str() + at, "\"awva-plugin\": {\"output\": %f, \"mono\": %7[a-z], \"factory_preset\": %d}", &output, lg, &fac) != 3)
				return false;
			leg = !std::strcmp(lg, "true") ? 1 : 0;
		}
		fac = fac >= 0 && fac < d.num_presets ? fac : -1;
		// a sound made from a factory preset: restored the way it was made, the preset by its index
		// (so the component has it as its preset again: its heroes, preset()) and then the values
		// that differ from it, as edits after it
		std::vector<float> values;
		if (fac >= 0) {
			if (!read_preset(s, output, &values)) return false; // (another engine's, or not a preset)
			c.set_preset(sx_state, fac);
			for (int i = 0; i < d.num_params; i++) {
				const float a = c.param(sx_state, i), b = values[static_cast<size_t>(i)];
				if (i != out_index && std::memcmp(&a, &b, sizeof a) == 0) values[static_cast<size_t>(i)] = NAN; // (the preset's own)
			}
			if (!active) {
				apply(fac, leg, values);
				rescan_values();
			} else {
				load_values(fac, leg, values);
			}
			factory = fac;
			return true;
		}
		if (!load_preset_text(s, output, leg)) return false; // (another engine's, or not a preset)
		factory = fac;
		return true;
	}

	// --- the editor's changes ----------------------------------------------------------------------
	void gui_push(uint16_t type, clap_id id, double v) { // main thread
		const size_t w = gui_w.load(std::memory_order_relaxed);
		if (w - gui_r.load(std::memory_order_acquire) == kGui) return; // (512 ahead of the host: dropped)
		gui_ev[w % kGui] = {type, id, v};
		gui_w.store(w + 1, std::memory_order_release);
		if (const auto* hp = static_cast<const clap_host_params*>(host->get_extension(host, CLAP_EXT_PARAMS))) hp->request_flush(host);
	}
	// the audio thread (or the flush): applied, and told to the host, at frame 0
	void gui_drain(const clap_output_events* out) {
		for (size_t r = gui_r.load(std::memory_order_relaxed); r != gui_w.load(std::memory_order_acquire); r++) {
			const GuiEv e = gui_ev[r % kGui];
			if (e.type == CLAP_EVENT_PARAM_VALUE) {
				clap_event_param_value v = {{sizeof v, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0}, e.id, nullptr, -1, -1, -1, -1, e.value};
				param(v);
				if (out) out->try_push(out, &v.header);
			} else if (out) {
				clap_event_param_gesture g = {{sizeof g, 0, CLAP_CORE_EVENT_SPACE_ID, e.type, 0}, e.id};
				out->try_push(out, &g.header);
			}
			gui_r.store(r + 1, std::memory_order_release);
		}
	}

	// --- audio -----------------------------------------------------------------------------------
	void render_block() {
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
	void render(float* l, float* r, uint32_t from, uint32_t to) {
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
	clap_process_status process(const clap_process* p) {
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

	bool activate(double rate) {
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
	void clear() {
		frame = blocks = 0;
		pos = NSC_BLOCK;
		head = count = 0;
		for (Note& n : notes) n = Note{};
	}
};

Plugin* self(const clap_plugin* p) { return static_cast<Plugin*>(p->plugin_data); }

// --- the editor --------------------------------------------------------------------------------------
// the editor's theme choice ("system", "light", "dark"), the user's for every awva plugin:
// <app data>/awva/plugin-editor.json
std::string theme_file() {
#if defined(_WIN32)
	const char* base = std::getenv("APPDATA");
	return base ? std::string(base) + "\\awva\\plugin-editor.json" : std::string();
#elif defined(__APPLE__)
	const char* home = std::getenv("HOME");
	return home ? std::string(home) + "/Library/Application Support/awva/plugin-editor.json" : std::string();
#else
	const char* config = std::getenv("XDG_CONFIG_HOME");
	const char* home = std::getenv("HOME");
	if (config && *config) return std::string(config) + "/awva/plugin-editor.json";
	return home ? std::string(home) + "/.config/awva/plugin-editor.json" : std::string();
#endif
}
std::string read_theme() {
	const std::string f = theme_file();
	FILE* fp = f.empty() ? nullptr : std::fopen(f.c_str(), "rb");
	if (!fp) return "system";
	char buf[256] = {};
	const size_t n = std::fread(buf, 1, sizeof buf - 1, fp);
	std::fclose(fp);
	const std::string t(buf, n);
	for (const char* k : {"light", "dark"})
		if (t.find(std::string("\"") + k + "\"") != std::string::npos) return k;
	return "system";
}
void write_theme(const std::string& theme) {
	const std::string f = theme_file();
	if (f.empty()) return;
	FILE* fp = std::fopen(f.c_str(), "wb");
	if (!fp) return; // (no awva folder yet: the choice lasts for this session)
	std::fprintf(fp, "{\"theme\": \"%s\"}\n", theme.c_str());
	std::fclose(fp);
}

struct Plugin::EditorHost : awva_editor::Host {
	Plugin& s;
	std::string th = read_theme();
	explicit EditorHost(Plugin& p) : s(p) {}
	const nsc_desc& desc() const override { return s.d; }
	int preset() const override { return s.factory; }
	float value(int i) const override { return static_cast<float>(s.value(i)); }
	void begin(int i) override { s.gui_push(CLAP_EVENT_PARAM_GESTURE_BEGIN, s.d.params[i].id, 0); }
	void set(int i, float v) override {
		const nsc_param& p = s.d.params[i];
		v = std::clamp(v, p.min, p.max);
		if (p.kind != NSC_PARAM_CONTINUOUS) v = std::round(v);
		s.gui_val[static_cast<size_t>(i)] = v;
		s.gui_push(CLAP_EVENT_PARAM_VALUE, p.id, v);
	}
	void end(int i) override { s.gui_push(CLAP_EVENT_PARAM_GESTURE_END, s.d.params[i].id, 0); }
	void select_preset(int i) override {
		s.load_values(i, -1, {});
		s.factory = i;
	}
	bool mono() const override { return s.mono.load(); }
	void set_mono(bool on) override {
		s.gui_push(CLAP_EVENT_PARAM_GESTURE_BEGIN, kMonoId, 0);
		s.gui_push(CLAP_EVENT_PARAM_VALUE, kMonoId, on ? 1.0 : 0.0);
		s.gui_push(CLAP_EVENT_PARAM_GESTURE_END, kMonoId, 0);
		if (!s.active) s.mono = on;
	}
	int voices() const override { return s.active ? s.c.active_voices(s.state) : 0; }
	std::string theme() const override { return th; }
	void set_theme(const std::string& t) override {
		th = t;
		write_theme(t);
	}
	bool os_dark() const override { return awva_editor::view_os_dark(); }
	void repaint() override { awva_editor::view_repaint(s.view); }
};

Plugin::~Plugin() {
	if (view) awva_editor::view_destroy(view);
	view = nullptr;
	editor.reset();
	editor_host.reset();
	free_regions();
}

// CLAP's sizes are pixels on Windows and Linux (the scale given by set_scale) and points on macOS
#if defined(_WIN32) || defined(__linux__)
constexpr bool kGuiPixels = true;
#else
constexpr bool kGuiPixels = false;
#endif
bool gui_supported(const clap_plugin*, const char* api, bool floating) { return !floating && api && !std::strcmp(api, awva_editor::view_api()); }
bool gui_preferred(const clap_plugin*, const char** api, bool* floating) {
	*api = awva_editor::view_api();
	*floating = false;
	return true;
}
bool gui_create(const clap_plugin* p, const char* api, bool floating) {
	Plugin& s = *self(p);
	if (!gui_supported(p, api, floating) || s.view) return false;
	s.editor_host = std::make_unique<Plugin::EditorHost>(s);
	s.editor = std::make_unique<awva_editor::Editor>(*s.editor_host);
	s.view = awva_editor::view_create(*s.editor, *s.editor_host);
#if defined(__linux__)
	// the host runs the view: its X connection's fd and a 30 Hz timer
	if (s.view) {
		const auto* fd = static_cast<const clap_host_posix_fd_support*>(s.host->get_extension(s.host, CLAP_EXT_POSIX_FD_SUPPORT));
		const auto* timer = static_cast<const clap_host_timer_support*>(s.host->get_extension(s.host, CLAP_EXT_TIMER_SUPPORT));
		if (fd && fd->register_fd(s.host, awva_editor::view_fd(s.view), CLAP_POSIX_FD_READ)) s.gui_fd = awva_editor::view_fd(s.view);
		if (!timer || !timer->register_timer(s.host, 33, &s.gui_timer)) s.gui_timer = CLAP_INVALID_ID;
	}
#endif
	return s.view != nullptr;
}
void gui_destroy(const clap_plugin* p) {
	Plugin& s = *self(p);
#if defined(__linux__)
	if (s.gui_fd >= 0)
		if (const auto* fd = static_cast<const clap_host_posix_fd_support*>(s.host->get_extension(s.host, CLAP_EXT_POSIX_FD_SUPPORT)))
			fd->unregister_fd(s.host, s.gui_fd);
	if (s.gui_timer != CLAP_INVALID_ID)
		if (const auto* timer = static_cast<const clap_host_timer_support*>(s.host->get_extension(s.host, CLAP_EXT_TIMER_SUPPORT)))
			timer->unregister_timer(s.host, s.gui_timer);
	s.gui_fd = -1;
	s.gui_timer = CLAP_INVALID_ID;
#endif
	if (s.view) awva_editor::view_destroy(s.view);
	s.view = nullptr;
	s.editor.reset();
	s.editor_host.reset();
}
bool gui_set_scale(const clap_plugin* p, double scale) {
	if (!kGuiPixels) return false;
	Plugin& s = *self(p);
	s.gui_scale = scale > 0 ? scale : 1.0;
	if (s.view) awva_editor::view_set_scale(s.view, s.gui_scale);
	return true;
}
bool gui_get_size(const clap_plugin* p, uint32_t* w, uint32_t* h) {
	const Plugin& s = *self(p);
	const double k = kGuiPixels ? s.gui_scale : 1.0;
	*w = static_cast<uint32_t>(std::lround(s.gui_w_pt * k));
	*h = static_cast<uint32_t>(std::lround(s.gui_h_pt * k));
	return true;
}
bool gui_can_resize(const clap_plugin*) { return true; }
bool gui_resize_hints(const clap_plugin*, clap_gui_resize_hints* h) {
	*h = {true, true, false, 0, 0};
	return true;
}
bool gui_adjust_size(const clap_plugin* p, uint32_t* w, uint32_t* h) {
	const double k = kGuiPixels ? self(p)->gui_scale : 1.0;
	*w = std::max(*w, static_cast<uint32_t>(std::lround(awva_tokens::kEditorMinWidth * k)));
	*h = std::max(*h, static_cast<uint32_t>(std::lround(awva_tokens::kEditorMinHeight * k)));
	return true;
}
bool gui_set_size(const clap_plugin* p, uint32_t w, uint32_t h) {
	Plugin& s = *self(p);
	const double k = kGuiPixels ? s.gui_scale : 1.0;
	s.gui_w_pt = static_cast<float>(w / k);
	s.gui_h_pt = static_cast<float>(h / k);
	if (s.view) awva_editor::view_set_size(s.view, w, h);
	return true;
}
bool gui_set_parent(const clap_plugin* p, const clap_window* window) {
	Plugin& s = *self(p);
	if (!s.view || !window) return false;
	if (kGuiPixels) awva_editor::view_set_scale(s.view, s.gui_scale);
	uint32_t w, h;
	gui_get_size(p, &w, &h);
	awva_editor::view_set_size(s.view, w, h);
#if defined(_WIN32)
	return awva_editor::view_attach(s.view, window->win32);
#elif defined(__APPLE__)
	return awva_editor::view_attach(s.view, window->cocoa);
#else
	return awva_editor::view_attach(s.view, reinterpret_cast<void*>(static_cast<uintptr_t>(window->x11)));
#endif
}
bool gui_set_transient(const clap_plugin*, const clap_window*) { return false; }
void gui_suggest_title(const clap_plugin*, const char*) {}
bool gui_show(const clap_plugin* p) {
	if (self(p)->view) awva_editor::view_show(self(p)->view, true);
	return true;
}
bool gui_hide(const clap_plugin* p) {
	if (self(p)->view) awva_editor::view_show(self(p)->view, false);
	return true;
}
const clap_plugin_gui kGui = {gui_supported, gui_preferred, gui_create, gui_destroy, gui_set_scale, gui_get_size, gui_can_resize,
    gui_resize_hints, gui_adjust_size, gui_set_size, gui_set_parent, gui_set_transient, gui_suggest_title, gui_show, gui_hide};
#if defined(__linux__)
// Linux: the host's event loop runs the editor (the X connection's fd, the 30 Hz timer)
void on_fd(const clap_plugin* p, int, clap_posix_fd_flags_t) {
	if (self(p)->view) awva_editor::view_on_fd(self(p)->view);
}
const clap_plugin_posix_fd_support kFdSupport = {on_fd};
void on_timer(const clap_plugin* p, clap_id id) {
	if (self(p)->view && id == self(p)->gui_timer) awva_editor::view_on_timer(self(p)->view);
}
const clap_plugin_timer_support kTimerSupport = {on_timer};
#endif

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
const clap_plugin_audio_ports kAudioPorts = {audio_count, audio_get};

uint32_t note_count(const clap_plugin*, bool is_input) { return is_input ? 1 : 0; }
bool note_get(const clap_plugin*, uint32_t index, bool is_input, clap_note_port_info* info) {
	if (!is_input || index != 0) return false;
	info->id = 0;
	info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI | CLAP_NOTE_DIALECT_MIDI_MPE | CLAP_NOTE_DIALECT_MIDI2;
	info->preferred_dialect = CLAP_NOTE_DIALECT_MIDI2;
	std::snprintf(info->name, sizeof info->name, "Notes");
	return true;
}
const clap_plugin_note_ports kNotePorts = {note_count, note_get};

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
const clap_plugin_params kParams = {params_count, params_info, params_value, params_to_text, params_from_text, params_flush};

bool state_save(const clap_plugin* p, const clap_ostream* os) {
	const std::string s = self(p)->save();
	if (s.empty()) return false;
	size_t at = 0;
	while (at < s.size()) {
		const int64_t w = os->write(os, s.data() + at, s.size() - at);
		if (w <= 0) return false;
		at += static_cast<size_t>(w);
	}
	return true;
}
bool state_load(const clap_plugin* p, const clap_istream* is) {
	std::string s;
	char buf[4096];
	for (;;) {
		const int64_t r = is->read(is, buf, sizeof buf);
		if (r < 0) return false;
		if (r == 0) break;
		s.append(buf, static_cast<size_t>(r));
		if (s.size() > (1u << 20)) return false; // (not ours)
	}
	return self(p)->load(s);
}
const clap_plugin_state kState = {state_save, state_load};

bool voice_get(const clap_plugin* p, clap_voice_info* info) {
	info->voice_count = static_cast<uint32_t>(self(p)->nv);
	info->voice_capacity = info->voice_count;
	info->flags = CLAP_VOICE_INFO_SUPPORTS_OVERLAPPING_NOTES;
	return true;
}
const clap_plugin_voice_info kVoiceInfo = {voice_get};

uint32_t tail_get(const clap_plugin* p) {
	const Plugin& s = *self(p);
	return static_cast<uint32_t>(std::ceil(static_cast<double>(s.d.tail_seconds) * s.sr));
}
const clap_plugin_tail kTail = {tail_get};

// presets: the factory presets, by index (preset-discovery below lists them)
// a preset file's text (at most 1 MB): empty if it can't be read
std::string read_file(const char* path) {
	std::string t;
	FILE* f = path ? std::fopen(path, "rb") : nullptr;
	if (!f) return t;
	char buf[4096];
	size_t got;
	while ((got = std::fread(buf, 1, sizeof buf, f)) > 0 && t.size() < (1u << 20)) t.append(buf, got);
	std::fclose(f);
	return t;
}
// presets: a factory preset by index (preset-discovery lists them), or a preset file by its path
bool preset_from_location(const clap_plugin* p, uint32_t kind, const char* location, const char* key) {
	Plugin& s = *self(p);
	const auto* hl = static_cast<const clap_host_preset_load*>(s.host->get_extension(s.host, CLAP_EXT_PRESET_LOAD));
	bool ok = false;
	if (kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN) {
		char* end = nullptr;
		const long i = key ? std::strtol(key, &end, 10) : -1;
		if (key && end != key && !*end && i >= 0 && i < s.d.num_presets) {
			s.load_values(static_cast<int>(i), -1, {});
			s.factory = static_cast<int>(i);
			ok = true;
		}
	} else if (kind == CLAP_PRESET_DISCOVERY_LOCATION_FILE) {
		const std::string text = read_file(location);
		ok = !text.empty() && s.load_preset_text(text, s.output_now(), -1); // (the output level and mono kept)
		if (ok) s.factory = -1;
	}
	if (hl) {
		if (ok) hl->loaded(s.host, kind, location, key);
		else hl->on_error(s.host, kind, location, key, -1, "not one of this engine's presets");
	}
	return ok;
}
const clap_plugin_preset_load kPresetLoad = {preset_from_location};

bool set_source(const clap_plugin* p, const awva_voice_source* src) {
	Plugin& s = *self(p);
	if (s.active || (src && !src->block)) return false;
	s.use_source = src != nullptr;
	if (src) s.source = *src;
	return true;
}
const awva_plugin_voice_controls kVoiceControls = {set_source};

// clap-wrapper's VST3: the MIDI controllers of all 16 channels (IMidiMapping), and the note
// expressions the engines take: poly pressure, tuning and brightness
uint32_t vst3_channels(const clap_plugin*, uint32_t) { return 16; }
uint32_t vst3_expressions(const clap_plugin*) {
	return AS_VST3_NOTE_EXPRESSION_PRESSURE | AS_VST3_NOTE_EXPRESSION_TUNING | AS_VST3_NOTE_EXPRESSION_BRIGHTNESS;
}
const clap_plugin_as_vst3 kAsVst3 = {vst3_channels, vst3_expressions};

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

// --- preset discovery: the factory presets, inside the plugin -------------------------------------
const clap_preset_discovery_provider_descriptor& provider_descriptor() {
	static std::string id, name;
	static clap_preset_discovery_provider_descriptor d;
	static const bool done = [] {
		id = std::string(plugin_id()) + ".presets";
		name = std::string(desc().name) + " factory presets";
		d = {CLAP_VERSION_INIT, id.c_str(), name.c_str(), kVendor};
		return true;
	}();
	(void)done;
	return d;
}
struct Provider {
	clap_preset_discovery_provider provider;
	const clap_preset_discovery_indexer* indexer;
};
bool provider_init(const clap_preset_discovery_provider* p) {
	const auto* ix = static_cast<const Provider*>(p->provider_data)->indexer;
	const clap_preset_discovery_location loc = {CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT, "Factory presets",
	    CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr};
	return ix->declare_location(ix, &loc);
}
void provider_destroy(const clap_preset_discovery_provider* p) { delete static_cast<Provider*>(p->provider_data); }
bool provider_metadata(const clap_preset_discovery_provider*, uint32_t kind, const char* location,
    const clap_preset_discovery_metadata_receiver* r) {
	if (kind != CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN || location) return false;
	const nsc_desc& d = desc();
	for (int i = 0; i < d.num_presets; i++) {
		// (the load key: the index, zero-padded, so a host or clap-wrapper that sorts the keys as text
		// lists the presets in their order: "007" before "010")
		char key_s[16];
		std::snprintf(key_s, sizeof key_s, "%03d", i);
		const std::string key = key_s;
		if (!r->begin_preset(r, d.presets[i].name, key.c_str())) break;
		const clap_universal_plugin_id id = {"clap", plugin_id()};
		r->add_plugin_id(r, &id);
		r->set_flags(r, CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT);
		r->add_creator(r, kVendor);
	}
	return true;
}
const void* provider_extension(const clap_preset_discovery_provider*, const char*) { return nullptr; }
uint32_t pd_count(const clap_preset_discovery_factory*) { return desc().num_presets > 0 ? 1 : 0; }
const clap_preset_discovery_provider_descriptor* pd_desc(const clap_preset_discovery_factory* f, uint32_t i) {
	return i < pd_count(f) ? &provider_descriptor() : nullptr;
}
const clap_preset_discovery_provider* pd_create(const clap_preset_discovery_factory*, const clap_preset_discovery_indexer* ix,
    const char* id) {
	if (std::strcmp(id, provider_descriptor().id) != 0) return nullptr;
	auto* p = new Provider{{&provider_descriptor(), nullptr, provider_init, provider_destroy, provider_metadata, provider_extension}, ix};
	p->provider.provider_data = p;
	return &p->provider;
}
const clap_preset_discovery_factory kPresetFactory = {pd_count, pd_desc, pd_create};

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

} // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry clap_entry = {CLAP_VERSION_INIT, entry_init, entry_deinit, entry_factory};
