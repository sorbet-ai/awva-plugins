// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
#include "catalog.h"

#include <cstdio>

#include "instance.h"
#include "json.h"
#include "nsc/load.h"
#include "platform.h"

namespace awva_app {

namespace {

bool ends_with(const std::string& s, const std::string& t) { return s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0; }

std::string stem(const std::string& file) {
	const size_t dot = file.rfind('.');
	return dot == std::string::npos ? file : file.substr(0, dot);
}

std::string colour(uint32_t rgb) {
	if (!rgb) return "";
	char b[16];
	std::snprintf(b, sizeof b, "#%06X", rgb & 0xffffffu);
	return b;
}

} // namespace

Catalog::~Catalog() {
	for (void* h : libs_) {
		nsc_library lib{h, nullptr};
		nsc_unload(&lib);
	}
}

const nsc_component* Catalog::load(const std::string& path, std::string* error) {
	nsc_library lib;
	const int r = nsc_load(path.c_str(), &lib);
	if (r != 0) {
		*error = r == -1 ? "the library doesn't load (missing, or built for another system)"
		       : r == -2 ? "not an awva component (no nsc_entry)"
		                 : "an awva component of another ABI (this app takes ABI " + std::to_string(NSC_ABI) + ")";
		return nullptr;
	}
	libs_.push_back(lib.handle);
	return lib.c;
}

// a package's presets/*.preset.json, each checked by the component's own load_preset()
void Catalog::add_presets(Engine& e, const std::string& dir) {
	const std::string pdir = join(dir, "presets");
	std::unique_ptr<Instance> scratch;
	for (const std::string& f : list_dir(pdir, false)) {
		if (!ends_with(f, ".preset.json")) continue;
		std::string text;
		if (!read_file(join(pdir, f), &text, 1u << 20)) {
			e.refused.push_back(f + ": can't be read");
			continue;
		}
		if (!scratch) {
			scratch = std::make_unique<Instance>(*e.c);
			if (scratch->init(48000) != 0) return;
		}
		nsc_preset_report rep{};
		if (e.c->load_preset(scratch->state, text.data(), static_cast<uint32_t>(text.size()), &rep) != 1) {
			e.refused.push_back(f + ": " + (rep.error ? rep.error : "refused"));
			continue;
		}
		PresetEntry p;
		Json j;
		if (Json::parse(text, &j)) {
			p.name = j.str("title");
			if (const Json* h = j.get("heroes"); h && h->type == Json::Array)
				for (const Json& v : h->a)
					if (v.type == Json::String) p.heroes.push_back(v.s);
		}
		if (p.name.empty()) p.name = f.substr(0, f.size() - std::string(".preset.json").size());
		p.text = std::move(text);
		e.presets.push_back(std::move(p));
	}
}

void Catalog::scan(const std::string& builtin_dir, const std::string& user_dir) {
	std::vector<Engine> out;
	// the built-ins: every component library beside the core, under its descriptor's title
	for (const std::string& f : list_dir(builtin_dir, false)) {
		if (!ends_with(f, library_suffix())) continue;
		// (one loaded copy for the session: a rescan reuses it)
		const Engine* had = find("builtin:" + stem(f));
		Engine e;
		e.key = "builtin:" + stem(f);
		e.label = "built in";
		e.source = f;
		e.c = had ? had->c : load(join(builtin_dir, f), &e.error);
		e.ok = e.c != nullptr;
		if (e.ok)
			for (int i = 0; i < e.c->desc->num_presets; i++) e.presets.push_back({e.c->desc->presets[i].name, i, "", {}});
		out.push_back(std::move(e));
	}
	// the packages
	for (const std::string& folder : list_dir(user_dir, true)) {
		const std::string dir = join(user_dir, folder);
		Engine e;
		e.key = "package:" + folder;
		e.label = folder;
		e.source = folder;
		std::string text;
		Json m;
		if (!read_file(join(dir, "awva-package.json"), &text, 1u << 16)) {
			e.error = "no awva-package.json";
		} else if (!Json::parse(text, &m) || m.type != Json::Object) {
			e.error = "awva-package.json isn't JSON";
		} else if (m.str("format") != "awva-package/1") {
			e.error = "awva-package.json: \"format\" isn't \"awva-package/1\"";
		} else {
			e.label = m.str("label", folder);
			const std::string lib = m.str("library"), engine = m.str("engine");
			if (!lib.empty() == !engine.empty()) {
				e.error = "awva-package.json names neither or both of \"library\" and \"engine\"";
			} else if (!lib.empty()) {
				const std::string base = m.str("awva");
				const std::string pin = AWVA_APP_AWVA_COMMIT; // (the pinned awva commit, all 40 digits)
				if (base.size() < 7 || base.size() > pin.size() || pin.compare(0, base.size(), base) != 0) {
					// (da3ad97 changed nsc_voice / nsc_globals under the same NSC_ABI: never load another base's library)
					e.error = "built for a different awva (" + (base.empty() ? std::string("no \"awva\" commit") : base) + "; this app needs " + pin.substr(0, 7) + ")";
				} else if (!ends_with(lib, library_suffix())) {
					e.error = "\"library\" " + lib + " isn't a " + library_suffix() + " (a package is per system)";
				} else {
					// a copy, so the package can be rebuilt while it plays
					const std::string copy = join(cache_dir(), folder + "-" + std::to_string(now_ns()) + "-" + std::to_string(copies_++) + library_suffix());
					if (!copy_file(join(dir, lib), copy)) e.error = "\"library\" " + lib + " isn't in the package";
					else e.c = load(copy, &e.error);
					if (e.c)
						for (int i = 0; i < e.c->desc->num_presets; i++) e.presets.push_back({e.c->desc->presets[i].name, i, "", {}});
				}
			} else {
				for (const Engine& b : out)
					if (b.ok && engine == b.c->desc->id) e.c = b.c;
				if (!e.c) e.error = "\"engine\" " + engine + " isn't a built-in engine";
			}
			if (e.c) {
				e.ok = true;
				add_presets(e, dir);
				if (e.presets.empty() && !engine.empty()) {
					e.ok = false;
					e.error = "a presets-only package without a preset that loads";
				}
			}
		}
		out.push_back(std::move(e));
	}
	engines_ = std::move(out);
}

const Engine* Catalog::find(const std::string& key) const {
	for (const Engine& e : engines_)
		if (e.key == key) return &e;
	return nullptr;
}

std::string Catalog::json() const {
	std::string s = "[";
	for (const Engine& e : engines_) {
		if (s.size() > 1) s += ",";
		const nsc_desc* d = e.c ? e.c->desc : nullptr;
		s += "{\"key\":" + jstr(e.key) + ",\"label\":" + jstr(e.label) + ",\"source\":" + jstr(e.source);
		s += ",\"builtin\":" + std::string(e.key.rfind("builtin:", 0) == 0 ? "true" : "false");
		s += ",\"id\":" + jstr(d ? d->id : "") + ",\"title\":" + jstr(d ? d->name : e.source);
		s += ",\"version\":" + jstr(d && d->version ? d->version : "") + ",\"summary\":" + jstr(d && d->summary ? d->summary : "");
		s += ",\"colour\":" + jstr(d ? colour(d->led) : "") + ",\"ok\":" + (e.ok ? "true" : "false") + ",\"error\":" + jstr(e.error);
		s += ",\"refused\":[";
		for (size_t i = 0; i < e.refused.size(); i++) s += (i ? "," : "") + jstr(e.refused[i]);
		s += "]}";
	}
	return s + "]";
}

} // namespace awva_app
