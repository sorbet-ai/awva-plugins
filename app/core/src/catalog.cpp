// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
#include "catalog.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "instance.h"
#include "json.h"
#include "nsc/load.h"
#include "nsc/package.h"
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

// a package's presets (its kind-3 sections, in tag order), each checked by the component's own
// load_preset(); another engine's preset is listed as refused
void Catalog::add_presets(Engine& e, const std::vector<std::pair<std::string, std::string>>& files) {
	std::unique_ptr<Instance> scratch;
	for (const auto& [tag, text] : files) {
		if (!scratch) {
			scratch = std::make_unique<Instance>(*e.c);
			if (scratch->init(48000) != 0) return;
		}
		nsc_preset_report rep{};
		if (e.c->load_preset(scratch->state, text.data(), static_cast<uint32_t>(text.size()), &rep) != 1) {
			e.refused.push_back(tag + ": " + (rep.error ? rep.error : "refused"));
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
		if (p.name.empty()) p.name = tag;
		p.text = text;
		e.presets.push_back(std::move(p));
	}
}

// the image tag of this computer (awva-package/2)
const char* host_tag() {
#if defined(_WIN32)
	return "x86_64-windows";
#elif defined(__APPLE__) && defined(__aarch64__)
	return "arm64-macos";
#elif defined(__APPLE__)
	return "x86_64-macos";
#else
	return "x86_64-linux";
#endif
}

// one .awvapkg (app/README.md): its container checked (nsc/package.h), its manifest,
// this computer's image copied out and loaded, its presets; e.error says why when refused
void Catalog::add_package(Engine& e, const std::string& path, const std::vector<Engine>& builtins) {
	std::string bytes;
	if (!read_file(path, &bytes, 256u << 20)) {
		e.error = "can't be read";
		return;
	}
	const auto* p = reinterpret_cast<const uint8_t*>(bytes.data());
	nscpkg::Reason why;
	if (!nscpkg::check_package(p, bytes.size(), static_cast<uint32_t>(bytes.size()), why)) {
		e.error = why.text;
		return;
	}
	nscpkg::PackageHeader h;
	std::memcpy(&h, p, sizeof h);
	if (h.abi != NSC_ABI) {
		e.error = "an awva component of ABI " + std::to_string(h.abi) + " (this app takes ABI " + std::to_string(NSC_ABI) + ")";
		return;
	}
	// every section's crc
	std::vector<nscpkg::Section> secs(h.n_sections);
	for (uint32_t i = 0; i < h.n_sections; i++) {
		std::memcpy(&secs[i], p + sizeof h + i * sizeof(nscpkg::Section), sizeof(nscpkg::Section));
		if (nscpkg::crc32(0, p + secs[i].offset, secs[i].size) != secs[i].crc) {
			e.error = std::string("section ") + secs[i].tag + ": crc";
			return;
		}
	}
	// the manifest
	Json m;
	nscpkg::Section ms;
	if (nscpkg::find_section(p, nscpkg::KIND_MANIFEST, nullptr, &ms) < 0 ||
	    !Json::parse(std::string(bytes.data() + ms.offset, ms.size), &m) || m.type != Json::Object) {
		e.error = "no manifest";
		return;
	}
	if (m.str("format") != "awva-package/2") {
		e.error = "manifest format \"" + m.str("format") + "\" (this app reads awva-package/2)";
		return;
	}
	e.label = m.str("label", e.label);
	const std::string id = m.str("id");
	const std::string base = m.str("awva");
	const std::string pin = AWVA_APP_AWVA_COMMIT; // (the pinned awva commit, all 40 digits)
	bool any_image = false;
	if (const Json* im = m.get("images"); im && im->type == Json::Array) any_image = !im->a.empty();
	std::vector<std::pair<std::string, std::string>> presets;
	for (const nscpkg::Section& s : secs)
		if (s.kind == nscpkg::KIND_PRESET) presets.push_back({s.tag, std::string(bytes.data() + s.offset, s.size)});
	std::sort(presets.begin(), presets.end());
	if (!any_image) { // presets for a built-in engine
		for (const Engine& b : builtins)
			if (b.ok && id == b.c->desc->id) e.c = b.c;
		if (!e.c) {
			e.error = "presets for \"" + id + "\", which isn't a built-in engine";
			return;
		}
	} else {
		if (base.size() < 7 || base.size() > pin.size() || pin.compare(0, base.size(), base) != 0) {
			e.error = "built for a different awva (" + (base.empty() ? std::string("no \"awva\" commit") : base) + "; this app needs " + pin.substr(0, 7) + ")";
			return;
		}
		nscpkg::Section is;
		if (nscpkg::find_section(p, nscpkg::KIND_IMAGE, host_tag(), &is) < 0) {
			e.error = std::string("no image for this computer (") + host_tag() + ")";
			return;
		}
		nscpkg::ModuleHeader mh;
		if (is.size < sizeof mh) {
			e.error = "image: short";
			return;
		}
		std::memcpy(&mh, p + is.offset, sizeof mh);
		if (mh.magic != nscpkg::MODULE_MAGIC || nscpkg::crc32(0, &mh, 252) != mh.header_crc || mh.reloc_count != 0 ||
		    sizeof mh + mh.image_size > is.size || !nscpkg::field_is(mh.arch, host_tag()) || mh.abi != NSC_ABI ||
		    nscpkg::crc32(0, p + is.offset + sizeof mh, is.size - sizeof mh) != mh.payload_crc) {
			e.error = std::string("image for ") + host_tag() + ": damaged or another ABI";
			return;
		}
		// copied out (a package can be written again while its engine plays) and loaded
		const std::string copy = join(cache_dir(), e.source + "-" + std::to_string(now_ns()) + "-" + std::to_string(copies_++) + library_suffix());
		if (!write_file(copy, std::string(bytes.data() + is.offset + sizeof mh, mh.image_size))) {
			e.error = "the image can't be copied out";
			return;
		}
		e.c = load(copy, &e.error);
		if (!e.c) return;
		if (id != e.c->desc->id) {
			e.error = "the image is " + std::string(e.c->desc->id) + ", the manifest says " + id;
			e.c = nullptr;
			return;
		}
		for (int i = 0; i < e.c->desc->num_presets; i++) e.presets.push_back({e.c->desc->presets[i].name, i, "", {}});
	}
	e.ok = true;
	add_presets(e, presets);
	if (e.presets.empty() && !any_image) {
		e.ok = false;
		e.error = "a presets-only package without a preset that loads";
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
	// the packages: <name>.awvapkg files (a ".tmp" being written is not one yet)
	for (const std::string& f : list_dir(user_dir, false)) {
		if (f.empty() || f[0] == '.' || !ends_with(f, ".awvapkg")) continue;
		Engine e;
		e.source = f.substr(0, f.size() - 8);
		e.key = "package:" + e.source;
		e.label = e.source;
		add_package(e, join(user_dir, f), out);
		out.push_back(std::move(e));
	}
	engines_ = std::move(out);
}

std::string Catalog::signature(const std::string& user_dir) {
	std::string s;
	s = file_stamp(user_dir); // (the .awvapkg files: names, sizes, times)
	return s;
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
		s += ",\"colour\":" + jstr(d ? colour(d->colour) : "") + ",\"ok\":" + (e.ok ? "true" : "false") + ",\"error\":" + jstr(e.error);
		s += ",\"refused\":[";
		for (size_t i = 0; i < e.refused.size(); i++) s += (i ? "," : "") + jstr(e.refused[i]);
		s += "]}";
	}
	return s + "]";
}

} // namespace awva_app
