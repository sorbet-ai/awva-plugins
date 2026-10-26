// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The plugins' editor, rendered offscreen to PNGs (no window, no host): the screenshots of the
// editor in both themes, and a check that it paints with an engine's descriptor.
//   editor_shot COMPONENT_LIB OUT_DIR
// writes OUT_DIR/editor_<engine>_{light,dark}.png (760 x 480 points at 2x), the "Every parameter"
// view and the preset menu.
#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <cstdio>
#include <string>

#include "editor/view.h"
#include "nsc/load.h"

using namespace awva_editor;

namespace {

struct ShotHost : Host {
	const nsc_component& c;
	nsc_instance in{};
	std::string th;
	explicit ShotHost(const nsc_component& comp, const std::string& theme) : c(comp), th(theme) {
		nsc_instance_new(&c, &in);
		c.init(in.state, in.fast, in.large, 48000, 10);
	}
	~ShotHost() override { nsc_instance_free(&in); }
	const nsc_desc& desc() const override { return *c.desc; }
	int preset() const override { return c.preset(in.state); }
	float value(int i) const override { return c.param(in.state, i); }
	void begin(int) override {}
	void set(int i, float v) override { c.set_param(in.state, i, v); }
	void end(int) override {}
	void select_preset(int i) override { c.set_preset(in.state, i); }
	bool legato() const override { return false; }
	void set_legato(bool) override {}
	int voices() const override { return 3; }
	std::string theme() const override { return th; }
	void set_theme(const std::string& t) override { th = t; }
	bool os_dark() const override { return false; }
	void repaint() override {}
};

} // namespace

int main(int argc, char** argv) {
	if (argc < 3) {
		std::fprintf(stderr, "usage: editor_shot COMPONENT_LIB OUT_DIR\n");
		return 2;
	}
	nsc_library lib;
	if (nsc_load(argv[1], &lib) != 0) {
		std::fprintf(stderr, "editor_shot: can't load %s\n", argv[1]);
		return 2;
	}
	const std::string out = argv[2], id = lib.c->desc->id;
	int bad = 0;
	for (const char* theme : {"light", "dark"}) {
		ShotHost h(*lib.c, theme);
		if (h.desc().num_presets > 2) h.select_preset(2);
		Editor e(h);
		const std::string base = out + "/editor_" + id + "_" + theme;
		bad += !render_png(e, 760, 480, 2.0f, base + ".png");
		Editor all(h); // "Every parameter" open, taller
		all.show_all(true);
		bad += !render_png(all, 760, 760, 2.0f, base + "_params.png");
		Editor menu(h); // the preset menu
		menu.show_menu(true);
		bad += !render_png(menu, 760, 480, 2.0f, base + "_menu.png");
		std::printf("editor_shot: %s\n", (base + ".png").c_str());
	}
	nsc_unload(&lib);
	return bad ? 1 : 0;
}
