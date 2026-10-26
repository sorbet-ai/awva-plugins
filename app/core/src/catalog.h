// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The engines the app can play: the built-in components (beside the core) and the user's
// packages (.awvapkg files, awva-package/2, app/README.md). Libraries stay loaded for the session
// (an instance may still be playing from one); a package's image is loaded from a copy, so the
// package can be written again while the app runs.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "nsc/nsc.h"

namespace awva_app {

struct PresetEntry {
	std::string name;
	int factory = -1;               // a factory preset's index, or -1 for a file
	std::string text;               // a file's awva-preset/1 text
	std::vector<std::string> heroes; // a file's own heroes (empty: the engine's)
};

struct Engine {
	std::string key;    // "builtin:<file>" or "package:<file name without .awvapkg>"
	std::string label;  // "built in", or the package's label
	std::string source; // the file or folder it came from
	bool ok = false;
	std::string error;
	const nsc_component* c = nullptr;
	std::vector<PresetEntry> presets;
	std::vector<std::string> refused; // preset files not loaded, and why
};

// the image tag of this computer in an .awvapkg ("x86_64-windows", "arm64-macos", ...)
const char* host_tag();

class Catalog {
public:
	// the built-ins and the packages, again
	void scan(const std::string& builtin_dir, const std::string& user_dir);
	const std::vector<Engine>& engines() const { return engines_; }
	const Engine* find(const std::string& key) const;
	std::string json() const;
	// what the packages folder holds now (folders, their files' sizes and times): a new value means
	// a package came, went or changed
	static std::string signature(const std::string& user_dir);
	~Catalog();

private:
	const nsc_component* load(const std::string& path, std::string* error);
	void add_presets(Engine& e, const std::vector<std::pair<std::string, std::string>>& files);
	void add_package(Engine& e, const std::string& path, const std::vector<Engine>& builtins);
	std::vector<Engine> engines_;
	std::vector<void*> libs_; // loaded for the session
	int copies_ = 0;
};

} // namespace awva_app
