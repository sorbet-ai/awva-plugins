// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The generic plugin's presets and state (plugin.h): a preset or a state loaded (now while
// inactive, else by the audio thread before its next block), the state as the component's
// awva-preset/1 with the plugin's own key; the state and preset-load extensions, and the
// preset-discovery factory that lists the factory presets.
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "plugin.h"

namespace awva_plugin {

void Plugin::apply(int preset, int leg, const std::vector<float>& values) {
	if (preset >= 0 && preset < d.num_presets) c.set_preset(state, preset);
	if (leg >= 0) {
		mono = leg != 0;
		(void)c.mode(state, NSC_MODE_MONO, leg);
	}
	for (int i = 0; i < d.num_params && i < static_cast<int>(values.size()); i++)
		if (!std::isnan(values[static_cast<size_t>(i)])) (void)c.set_param(state, i, values[static_cast<size_t>(i)]);
}
// the main thread: now while inactive, else by the audio thread before its next block
void Plugin::load_values(int preset, int leg, const std::vector<float>& values) {
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
void Plugin::apply_pending() { // the audio thread
	int s = 2;
	if (!pending.st.compare_exchange_strong(s, 3)) return;
	apply(pending.preset, pending.mono, pending.values);
	pending.st = 0;
	rescan = true;
	host->request_callback(host);
}
// a value as the host should see it: a loaded state not yet applied first
double Plugin::value(int i) const {
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
bool Plugin::read_preset(const std::string& json, float keep_output, std::vector<float>* values) {
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
bool Plugin::load_preset_text(const std::string& json, float output, int leg) {
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
// the state: the sound as the component writes it (awva-preset/1), and the plugin's own key
std::string Plugin::save() const {
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
bool Plugin::load(const std::string& s) {
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
extern const clap_plugin_state kState = {state_save, state_load};

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
extern const clap_plugin_preset_load kPresetLoad = {preset_from_location};

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
extern const clap_preset_discovery_factory kPresetFactory = {pd_count, pd_desc, pd_create};

} // namespace awva_plugin
