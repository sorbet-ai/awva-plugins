// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The core's catalog and sound (app_core.h): the engines scanned, an engine and its presets chosen,
// its values; the UI thread's side of the commands to the audio thread.
#include "app_core.h"

namespace awva_app {

int Core::rescan() {
	if (engine_) playing_ = *engine_; // (the engine playing keeps playing, its package gone or not)
	catalog_.scan(builtin_dir_, user_dir_);
	folder_ = Catalog::signature(user_dir_);
	engine_ = catalog_.find(engine_key_);
	if (!engine_ && !engine_key_.empty()) engine_ = &playing_;
	if (engine_ && preset_ >= static_cast<int>(engine_->presets.size())) preset_ = -1;
	catalog_gen_++;
	return static_cast<int>(catalog_.engines().size());
}
// the packages folder changed since the last scan: scan it again
void Core::rescan_if_changed() {
	if (Catalog::signature(user_dir_) != folder_) rescan();
}
int Core::select_engine(const std::string& key) {
	drain_rets();
	const Engine* e = catalog_.find(key);
	if (!e || !e->ok) return fail("no such engine, or it was refused: " + key);
	auto* in = new Instance(*e->c);
	if (in->init(rate_) != 0) {
		delete in;
		return fail("this engine doesn't run at " + std::to_string(rate_) + " Hz");
	}
	engine_ = e;
	engine_key_ = key;
	preset_ = -1;
	const int fp = in->c.preset(in->state);
	for (size_t i = 0; i < e->presets.size(); i++)
		if (e->presets[i].factory >= 0 && e->presets[i].factory == fp) preset_ = static_cast<int>(i);
	// a package of preset files only: its first preset (not live yet: set here)
	if (preset_ < 0 && !e->presets.empty() && e->presets[0].factory < 0) {
		const std::string& t = e->presets[0].text;
		(void)in->c.load_preset(in->state, t.data(), static_cast<uint32_t>(t.size()), nullptr);
		preset_ = 0;
	}
	ui_ = in;
	mono_ = false;
	send({Cmd::Swap, e->key.rfind("package:", 0) == 0 && e->c != builtin_of(e) ? 1 : 0, 0, in});
	set_setting("engine", key);
	set_setting("preset", std::to_string(preset_));
	return 0;
}
int Core::select_preset(int i) {
	if (!engine_ || !ui_ || i < 0 || i >= static_cast<int>(engine_->presets.size())) return fail("no such preset");
	const PresetEntry& p = engine_->presets[static_cast<size_t>(i)];
	if (p.factory >= 0) send({Cmd::SetPreset, p.factory, 0, nullptr});
	else send({Cmd::LoadText, 0, 0, new std::string(p.text)});
	preset_ = i;
	set_setting("preset", std::to_string(i));
	return 0;
}
int Core::set_param(int i, float v) {
	if (!ui_ || i < 0 || i >= ui_->d.num_params) return fail("no such parameter");
	send({Cmd::SetParam, i, v, nullptr});
	return 0;
}
void Core::set_mono(bool on) {
	mono_ = on;
	send({Cmd::Mono, on ? 1 : 0, 0, nullptr});
}
void Core::set_master(float db) {
	db = std::clamp(db, kMasterMinDb, kMasterMaxDb);
	master_db_ = db;
	send({Cmd::Master, 0, db, nullptr});
	set_setting("master_db", jnum(db));
}
std::string Core::sound_json() {
	drain_rets();
	if (!engine_ || !ui_) return "{}";
	const nsc_desc& d = ui_->d;
	std::string s = "{\"engine\":{\"key\":" + jstr(engine_->key) + ",\"label\":" + jstr(engine_->label) + ",\"id\":" + jstr(d.id) +
	    ",\"title\":" + jstr(d.name) + ",\"version\":" + jstr(d.version ? d.version : "") + ",\"summary\":" + jstr(d.summary ? d.summary : "") +
	    ",\"builtin\":" + (engine_->key.rfind("builtin:", 0) == 0 ? "true" : "false") + "}";
	s += ",\"presets\":[";
	for (size_t i = 0; i < engine_->presets.size(); i++)
		s += std::string(i ? "," : "") + "{\"name\":" + jstr(engine_->presets[i].name) + ",\"factory\":" + (engine_->presets[i].factory >= 0 ? "true" : "false") + "}";
	s += "],\"preset\":" + std::to_string(preset_) + ",\"heroes\":[";
	bool first = true;
	for (const std::string& h : heroes()) {
		const int k = ui_->param_index(h.c_str());
		if (k < 0) continue;
		s += (first ? "" : ",") + std::to_string(k);
		first = false;
	}
	s += "],\"params\":[";
	for (int i = 0; i < d.num_params; i++) {
		const nsc_param& p = d.params[i];
		s += std::string(i ? "," : "") + "{\"name\":" + jstr(p.name) + ",\"group\":" + jstr(p.group ? p.group : "") + ",\"min\":" + jnum(p.min) +
		    ",\"max\":" + jnum(p.max) + ",\"kind\":" + std::to_string(p.kind) + ",\"def\":" + jnum(p.def) + ",\"value\":" + jnum(param(i)) +
		    ",\"label\":" + jstr(p.label ? p.label : "") + ",\"hover\":" + jstr(p.hover ? p.hover : "") + ",\"icon\":" + jstr(p.icon ? p.icon : "") + "}";
	}
	s += "],\"mono\":" + std::string(mono_ ? "true" : "false") + "}";
	return s;
}
void Core::status(awva_status* o) {
	o->peak_l = peak_l_.exchange(0.0f);
	o->peak_r = peak_r_.exchange(0.0f);
	const float g = min_gain_.exchange(1.0f);
	o->reduction_db = g < 1.0f ? -20.0f * std::log10(std::max(g, 1e-6f)) : 0.0f;
	o->master_db = master_db_;
	o->cpu = cpu_.load();
	o->voices = voices_.load();
	const AudioInfo i = audio_info();
	o->audio_state = i.state;
	o->sample_rate = i.rate;
	o->buffer_frames = i.buffer;
	o->midi_events = midi_count_.load();
	o->generation = generation_.load();
	o->xruns = xruns_.load();
	o->faults = faults_.load();
	o->engine_stopped = stopped_.load();
	o->catalog = catalog_gen_;
}
// the UI thread, with the audio thread stopped
void Core::apply_all() {
	Cmd c;
	while (cmds_.pop(&c)) apply(c);
}
void Core::send(const Cmd& c) {
	for (int k = 0; !cmds_.push(c); k++) {
		if (k > 200) return; // (the audio thread isn't taking them: dropped)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}
void Core::drain_rets() {
	Ret r;
	while (rets_.pop(&r)) {
		if (r.t == Ret::Inst) delete static_cast<Instance*>(r.p);
		else delete static_cast<std::string*>(r.p);
	}
}
// the built-in engine's library a presets-only package plays (not stopped like a package's own)
const nsc_component* Core::builtin_of(const Engine* e) const {
	for (const Engine& b : catalog_.engines())
		if (b.key.rfind("builtin:", 0) == 0 && b.c == e->c) return b.c;
	return nullptr;
}
std::vector<std::string> Core::heroes() const {
	std::vector<std::string> v;
	const nsc_desc& d = ui_->d;
	const PresetEntry* p = engine_ && preset_ >= 0 && preset_ < static_cast<int>(engine_->presets.size()) ? &engine_->presets[static_cast<size_t>(preset_)] : nullptr;
	if (p && p->factory < 0 && !p->heroes.empty()) return p->heroes;
	const char* const* h = d.heroes;
	if (p && p->factory >= 0 && d.presets[p->factory].heroes[0]) h = d.presets[p->factory].heroes;
	for (int i = 0; i < NSC_MAX_HEROES && h[i]; i++) v.push_back(h[i]);
	return v;
}

} // namespace awva_app
