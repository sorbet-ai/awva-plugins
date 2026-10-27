// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// clap_host_test's info mode (clap_host.h): the plugin as its component's, presets, state.
#include "clap_host.h"

namespace host_test {

int list_presets(std::vector<std::string>* names) {
	const auto* pf = static_cast<const clap_preset_discovery_factory*>(entry->get_factory(CLAP_PRESET_DISCOVERY_FACTORY_ID));
	if (!pf || pf->count(pf) == 0) return 0;
	const clap_preset_discovery_indexer ix = {CLAP_VERSION_INIT, "clap_host_test", "", "", "", nullptr,
	    [](const clap_preset_discovery_indexer*, const clap_preset_discovery_filetype*) { return true; },
	    [](const clap_preset_discovery_indexer*, const clap_preset_discovery_location* l) {
		    return l->kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN && l->location == nullptr;
	    },
	    [](const clap_preset_discovery_indexer*, const clap_preset_discovery_soundpack*) { return true; },
	    [](const clap_preset_discovery_indexer*, const char*) -> const void* { return nullptr; }};
	const clap_preset_discovery_provider* prov = pf->create(pf, &ix, pf->get_descriptor(pf, 0)->id);
	clap_preset_discovery_metadata_receiver rcv = {names, [](const clap_preset_discovery_metadata_receiver*, int32_t, const char*) {},
	    [](const clap_preset_discovery_metadata_receiver* r, const char* name, const char*) {
		    static_cast<std::vector<std::string>*>(r->receiver_data)->push_back(name ? name : "");
		    return true;
	    },
	    [](const clap_preset_discovery_metadata_receiver*, const clap_universal_plugin_id*) {},
	    [](const clap_preset_discovery_metadata_receiver*, const char*) {},
	    [](const clap_preset_discovery_metadata_receiver*, uint32_t) {},
	    [](const clap_preset_discovery_metadata_receiver*, const char*) {},
	    [](const clap_preset_discovery_metadata_receiver*, const char*) {},
	    [](const clap_preset_discovery_metadata_receiver*, clap_timestamp, clap_timestamp) {},
	    [](const clap_preset_discovery_metadata_receiver*, const char*) {},
	    [](const clap_preset_discovery_metadata_receiver*, const char*, const char*) {}};
	if (prov && prov->init(prov)) prov->get_metadata(prov, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, &rcv);
	if (prov) prov->destroy(prov);
	return static_cast<int>(names->size());
}

// a short phrase (three notes with pressure and brightness), its output from an instance set up
// by `setup` (inactive) at 48 kHz
std::vector<float> phrase(const std::function<void(Instance&)>& setup, Buffer* state_out = nullptr, const Buffer* state_in = nullptr) {
	Instance in;
	if (state_in) {
		Buffer b{state_in->data};
		const clap_istream is = {&b, buf_read};
		if (!in.ext<clap_plugin_state>(CLAP_EXT_STATE)->load(in.p, &is)) return {};
	}
	if (setup) setup(in);
	// a second of silence first, as a project has before its first note: a preset and the values
	// set after it settle (an engine glides into a preset's values), then the state is saved
	in.start(48000);
	{
		std::vector<float> l(256), r(256);
		for (int k = 0; k < 48000 / 256; k++) {
			Events none;
			in.process(0, 256, none, l.data(), r.data());
		}
	}
	if (state_out) {
		const clap_ostream os = {state_out, buf_write};
		if (!in.ext<clap_plugin_state>(CLAP_EXT_STATE)->save(in.p, &os)) return {};
	}
	std::vector<Timed> ev;
	const int keys[3] = {57, 60, 64};
	for (int n = 0; n < 3; n++) {
		const uint64_t on = 960 + 7200 * static_cast<uint64_t>(n);
		clap_event_note x = {{sizeof x, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_ON, 0}, 300 + n, 0, 0, static_cast<int16_t>(keys[n]), 0.7};
		push(ev, on, x);
		clap_event_note_expression pr = {{sizeof pr, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_EXPRESSION, 0}, CLAP_NOTE_EXPRESSION_PRESSURE,
		    300 + n, 0, 0, static_cast<int16_t>(keys[n]), 0.6};
		push(ev, on, pr);
		clap_event_note y = {{sizeof y, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_OFF, 0}, 300 + n, 0, 0, static_cast<int16_t>(keys[n]), 0.0};
		push(ev, on + 24000, y);
	}
	std::vector<float> out(48000 * 2 * 2);
	std::vector<float> l(256), r(256);
	size_t k = 0;
	for (uint64_t at = 0; at < 48000 * 2; at += 256) {
		Events e;
		for (; k < ev.size() && ev[k].frame < at + 256; k++) {
			e.store.push_back(ev[k].bytes);
			reinterpret_cast<clap_event_header*>(e.store.back().data())->time = static_cast<uint32_t>(ev[k].frame - at);
		}
		in.process(at, 256, e, l.data(), r.data());
		for (int i = 0; i < 256; i++) {
			out[(at + i) * 2] = l[i];
			out[(at + i) * 2 + 1] = r[i];
		}
	}
	return out;
}

// random values for every parameter (inactive: through flush), mono among them
void randomize(Instance& in, uint32_t seed) {
	const auto* pa = in.ext<clap_plugin_params>(CLAP_EXT_PARAMS);
	std::mt19937 rng(seed);
	for (uint32_t i = 0; i < pa->count(in.p); i++) {
		clap_param_info pi;
		pa->get_info(in.p, i, &pi);
		in.set_param(pi.id, pi.min_value + (pi.max_value - pi.min_value) * (rng() % 1000) / 999.0);
	}
}

// a project's state restores the sound exactly: the same phrase from the saved instance and from a
// new one that loaded its state, bit for bit, and the restored one saves the same text again
void exact_restore(const nsc_desc& nd) {
	struct Case {
		const char* name;
		std::function<void(Instance&)> setup;
	};
	std::vector<Case> cases = {{"defaults", nullptr}, {"random parameters", [](Instance& in) { randomize(in, 11); }}};
	if (nd.num_presets > 1)
		cases.push_back({"the last factory preset", [n = nd.num_presets](Instance& in) {
			                 const auto* p = in.ext<clap_plugin_preset_load>(CLAP_EXT_PRESET_LOAD);
			                 (void)p->from_location(in.p, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, std::to_string(n - 1).c_str());
		                 }});
	if (nd.num_presets > 1)
		cases.push_back({"the last factory preset, then random parameters", [n = nd.num_presets](Instance& in) {
			                 const auto* p = in.ext<clap_plugin_preset_load>(CLAP_EXT_PRESET_LOAD);
			                 (void)p->from_location(in.p, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, std::to_string(n - 1).c_str());
			                 randomize(in, 12);
		                 }});
	for (const Case& c : cases) {
		Buffer saved, again;
		const std::vector<float> a = phrase(c.setup, &saved);
		const std::vector<float> b = phrase(nullptr, &again, &saved);
		double peak = 0;
		for (float x : a) peak = std::fmax(peak, std::fabs(x));
		if (!same_bits(a, b)) {
			size_t first = 0, n = 0;
			double md = 0;
			for (size_t i = 0; i < a.size() && i < b.size(); i++)
				if (std::memcmp(&a[i], &b[i], 4) != 0) {
					if (!n++) first = i;
					md = std::fmax(md, std::fabs(static_cast<double>(a[i]) - b[i]));
				}
			std::printf("     the sound differs: %zu samples, the first at frame %zu, at most %g\n", n, first / 2, md);
		}
		if (saved.data != again.data) std::printf("     the state text differs:\n%s\n---\n%s\n", saved.data.c_str(), again.data.c_str());
		check(!a.empty() && same_bits(a, b) && saved.data == again.data && peak > 0.0,
		    std::string("a project restores exactly (") + c.name + "): the same sound, bit for bit (peak " + std::to_string(peak) +
		        "), and the same state text again (" + std::to_string(saved.data.size()) + " bytes)");
	}
}

// the engine's preset files (awva's presets/<engine>/NN-<name>.preset.json, in name order) through
// preset-load: each gives the values its factory preset gives
void preset_files(const nsc_desc& nd, const char* dir) {
	std::vector<std::string> files;
	std::error_code ec;
	for (const auto& f : std::filesystem::directory_iterator(dir, ec))
		if (f.path().string().size() > 12 && f.path().string().rfind(".preset.json") == f.path().string().size() - 12) files.push_back(f.path().string());
	std::sort(files.begin(), files.end());
	if (nd.num_presets == 0) {
		check(files.empty(), "no preset files, no factory presets");
		return;
	}
	int same = 0;
	for (size_t k = 0; k < files.size(); k++) {
		Instance a, b;
		const auto* pa = a.ext<clap_plugin_preset_load>(CLAP_EXT_PRESET_LOAD);
		const auto* pb = b.ext<clap_plugin_preset_load>(CLAP_EXT_PRESET_LOAD);
		const bool la = pa->from_location(a.p, CLAP_PRESET_DISCOVERY_LOCATION_FILE, files[k].c_str(), nullptr);
		const bool lb = pb->from_location(b.p, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, std::to_string(k).c_str());
		const auto* qa = a.ext<clap_plugin_params>(CLAP_EXT_PARAMS);
		const auto* qb = b.ext<clap_plugin_params>(CLAP_EXT_PARAMS);
		bool eq = la && lb;
		for (uint32_t i = 0; eq && i < qa->count(a.p); i++) {
			clap_param_info pi;
			qa->get_info(a.p, i, &pi);
			double va = 0, vb = 0;
			qa->get_value(a.p, pi.id, &va);
			qb->get_value(b.p, pi.id, &vb);
			eq = std::memcmp(&va, &vb, sizeof va) == 0;
		}
		same += eq;
	}
	check(static_cast<int>(files.size()) == nd.num_presets && same == nd.num_presets,
	    std::to_string(files.size()) + " preset files through preset-load: each the values of its factory preset, bit for bit (" + std::to_string(same) + ")");
}

int info_test(const char* lib_path, const char* preset_dir) {
	nsc_library lib;
	if (nsc_load(lib_path, &lib) != 0) {
		std::fprintf(stderr, "clap_host_test: cannot load the component %s\n", lib_path);
		return 2;
	}
	const nsc_desc& nd = *lib.c->desc;
	const clap_plugin_descriptor* d = factory->get_plugin_descriptor(factory, 0);
	std::printf("plugin %s: \"%s\" by %s, %s; %s\n", d->id, d->name, d->vendor, d->version, d->description);
	check(factory->get_plugin_count(factory) == 1, "one plugin in the factory");
	check(std::string(d->id) == std::string("com.sorbet-ai.awva.") + nd.id && std::string(d->id) == nd.uid,
	    std::string("id com.sorbet-ai.awva.") + nd.id + ", the component's uid");
	check(std::string(d->name) == nd.name && std::string(d->vendor) == "Sorbet AI", "name from the component (\"" + std::string(nd.name) + "\"), vendor Sorbet AI");
	check(std::string(d->description).rfind(nd.summary, 0) == 0, "description: the component's summary");
	const std::string version = nd.version ? nd.version : "";
	check(version.size() >= 7 && version != "unknown" && std::string(d->description).find("(awva " + version + ")") != std::string::npos,
	    "the component's version, the awva commit (" + version + "), in the description");
	Instance in;
	check(in.p != nullptr, "created and initialised");
	if (!in.p) return 1;
	const auto* ap = in.ext<clap_plugin_audio_ports>(CLAP_EXT_AUDIO_PORTS);
	clap_audio_port_info ai;
	check(ap && ap->count(in.p, false) == 1 && ap->get(in.p, 0, false, &ai) && ai.channel_count == 2 && ap->count(in.p, true) == 0,
	    "one stereo output, no inputs");
	const auto* np = in.ext<clap_plugin_note_ports>(CLAP_EXT_NOTE_PORTS);
	clap_note_port_info ni;
	const uint32_t all = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI | CLAP_NOTE_DIALECT_MIDI_MPE | CLAP_NOTE_DIALECT_MIDI2;
	check(np && np->count(in.p, true) == 1 && np->get(in.p, 0, true, &ni) && ni.supported_dialects == all, "a note input: CLAP, MIDI, MIDI-MPE, MIDI2");
	{ // the sample rates: the component's min_rate .. max_rate, no other
		Instance r;
		const bool lo = r.p->activate(r.p, nd.min_rate, 1, 4096);
		if (lo) r.p->deactivate(r.p);
		const bool hi = r.p->activate(r.p, nd.max_rate, 1, 4096);
		if (hi) r.p->deactivate(r.p);
		const bool below = r.p->activate(r.p, nd.min_rate - 1, 1, 4096);
		if (below) r.p->deactivate(r.p);
		const bool above = r.p->activate(r.p, nd.max_rate + 1, 1, 4096);
		if (above) r.p->deactivate(r.p);
		check(lo && hi && !below && !above, "activates at " + std::to_string(nd.min_rate) + " .. " + std::to_string(nd.max_rate) + " Hz, the component's rates, and refuses others");
	}
	const uint32_t want_voices = static_cast<uint32_t>(std::min(10, nd.max_voices));
	check(plugin_voices() == want_voices, "voice info: " + std::to_string(want_voices) + " voices");
	const auto* params = in.ext<clap_plugin_params>(CLAP_EXT_PARAMS);
	const uint32_t count = params ? params->count(in.p) : 0;
	std::vector<clap_id> ids;
	bool match = count == static_cast<uint32_t>(nd.num_params) + 1;
	for (uint32_t i = 0; i < count; i++) {
		clap_param_info pi;
		params->get_info(in.p, i, &pi);
		ids.push_back(pi.id);
		if (i < static_cast<uint32_t>(nd.num_params)) {
			const nsc_param& q = nd.params[i];
			double v = 0; // (the value after init: the declared default)
			params->get_value(in.p, pi.id, &v);
			match = match && pi.default_value == static_cast<double>(q.def) && static_cast<float>(v) == q.def;
			match = match && pi.id == q.id && std::string(pi.name) == (q.label && *q.label ? q.label : q.name) && pi.min_value == q.min && pi.max_value == q.max &&
			    ((pi.flags & CLAP_PARAM_IS_STEPPED) != 0) == (q.kind != NSC_PARAM_CONTINUOUS) && pi.id != kMonoId && pi.id != CLAP_INVALID_ID;
		} else {
			match = match && pi.id == kMonoId && std::string(pi.name) == "mono";
		}
	}
	bool unique = true;
	for (size_t i = 0; i < ids.size(); i++)
		for (size_t j = 0; j < i; j++) unique = unique && ids[i] != ids[j];
	check(match && unique, std::to_string(nd.num_params) + " parameters as the component's (ids, names, ranges, kinds, defaults: def, the value after init), and mono; ids unique");
	std::vector<std::string> presets;
	const int listed = list_presets(&presets);
	bool names = listed == nd.num_presets;
	for (int i = 0; names && i < listed; i++) names = presets[static_cast<size_t>(i)] == nd.presets[i].name;
	check(names, "preset discovery: the " + std::to_string(nd.num_presets) + " factory presets, by name");
	const auto* pl = in.ext<clap_plugin_preset_load>(CLAP_EXT_PRESET_LOAD);
	if (nd.num_presets > 1) {
		rescans = loaded_presets = 0;
		const std::string key = std::to_string(nd.num_presets - 1);
		const bool loaded = pl && pl->from_location(in.p, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, key.c_str());
		check(loaded && rescans == 1 && loaded_presets == 1, "preset-load: preset " + key + " loaded, a rescan asked, the host told");
		in.start(48000);
		callbacks = rescans = 0;
		(void)pl->from_location(in.p, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, "0"); // active: the audio thread loads it
		Events ev;
		float l[64], r[64];
		in.process(0, 64, ev, l, r);
		in.p->on_main_thread(in.p);
		check(callbacks == 1 && rescans == 1, "preset-load while active: applied by the audio thread, then a callback and a rescan");
	}
	check(pl && !pl->from_location(in.p, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, "999"), "preset-load: no preset 999");

	// state: random values sent through process() at 44.1 kHz, saved, loaded into a new instance,
	// read back (the validator's state-reproducibility)
	{
		Instance a;
		a.start(44100);
		const auto* pa = a.ext<clap_plugin_params>(CLAP_EXT_PARAMS);
		std::mt19937 rng(3);
		Events ev;
		for (uint32_t i = 0; i < pa->count(a.p); i++) {
			clap_param_info pi;
			pa->get_info(a.p, i, &pi);
			const double v = pi.min_value + (pi.max_value - pi.min_value) * (rng() % 1000) / 999.0;
			clap_event_param_value e = {{sizeof e, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0}, pi.id, nullptr, -1, -1, -1, -1, v};
			ev.add(e);
		}
		float l[64], r[64];
		a.process(0, 64, ev, l, r);
		std::vector<double> want(ids.size());
		for (size_t i = 0; i < ids.size(); i++) pa->get_value(a.p, ids[i], &want[i]);
		Buffer st;
		const clap_ostream os = {&st, buf_write};
		const bool saved = a.ext<clap_plugin_state>(CLAP_EXT_STATE)->save(a.p, &os);
		for (int active = 0; active < 2; active++) {
			Instance b;
			if (active) b.start(48000);
			st.at = 0;
			const clap_istream is = {&st, buf_read};
			rescans = 0;
			const bool ok = b.ext<clap_plugin_state>(CLAP_EXT_STATE)->load(b.p, &is);
			if (active) { // the audio thread applies it
				Events none;
				b.process(0, 64, none, l, r);
				b.p->on_main_thread(b.p);
			}
			const auto* pb = b.ext<clap_plugin_params>(CLAP_EXT_PARAMS);
			int diff = 0;
			for (size_t i = 0; i < ids.size(); i++) {
				double got = 0;
				pb->get_value(b.p, ids[i], &got);
				if (std::memcmp(&got, &want[i], sizeof got) != 0) {
					if (diff < 3) std::printf("     param %zu: %g saved, %g loaded\n", i, want[i], got);
					diff++;
				}
			}
			check(saved && ok && diff == 0 && rescans == 1, std::string("state round trip into a new instance, ") + (active ? "active" : "inactive") +
			        ": every value the same, bit for bit (" + std::to_string(diff) + " differ), a rescan");
		}
		check(st.data.rfind("{", 0) == 0 && st.data.find("\"format\": \"awva-preset/1\"") != std::string::npos &&
		          st.data.find("\"awva-plugin\": {") != std::string::npos,
		    "the state is an awva-preset/1 file, with the plugin's own key");
		for (const char* text : {"{\"format\": \"awva-preset/1\", \"engine\": {\"id\": \"no-such-engine\"}, \"params\": {}}",
		         "awva-plugin 1\nengine x\n", ""}) {
			Buffer bad{text};
			const clap_istream is = {&bad, buf_read};
			Instance c;
			check(!c.ext<clap_plugin_state>(CLAP_EXT_STATE)->load(c.p, &is), std::string("refused as a state: ") + (*text ? std::string(text).substr(0, std::min<size_t>(40, std::string(text).find('\n'))) : "nothing"));
		}
	}
	exact_restore(nd);
	if (preset_dir) preset_files(nd, preset_dir);
	nsc_unload(&lib);
	return 0;
}

} // namespace host_test
