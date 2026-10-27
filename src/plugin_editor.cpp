// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The generic plugin's editor (plugin.h): the awva app's screen drawn natively (src/editor) as the
// gui extension, its theme choice in a file of the user's, and its changes to the host (a ring the
// audio thread or the params flush applies and puts out as CLAP gestures and values).
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "plugin.h"

namespace awva_plugin {

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

void Plugin::EditorHostDelete::operator()(EditorHost* h) const { delete h; }

Plugin::~Plugin() {
	if (view) awva_editor::view_destroy(view);
	view = nullptr;
	editor.reset();
	editor_host.reset();
	free_regions();
}

void Plugin::gui_push(uint16_t type, clap_id id, double v) { // main thread
	const size_t w = gui_w.load(std::memory_order_relaxed);
	if (w - gui_r.load(std::memory_order_acquire) == kGui) return; // (512 ahead of the host: dropped)
	gui_ev[w % kGui] = {type, id, v};
	gui_w.store(w + 1, std::memory_order_release);
	if (const auto* hp = static_cast<const clap_host_params*>(host->get_extension(host, CLAP_EXT_PARAMS))) hp->request_flush(host);
}
// the audio thread (or the flush): applied, and told to the host, at frame 0
void Plugin::gui_drain(const clap_output_events* out) {
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
	s.editor_host.reset(new Plugin::EditorHost(s));
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
extern const clap_plugin_gui kGui = {gui_supported, gui_preferred, gui_create, gui_destroy, gui_set_scale, gui_get_size, gui_can_resize,
    gui_resize_hints, gui_adjust_size, gui_set_size, gui_set_parent, gui_set_transient, gui_suggest_title, gui_show, gui_hide};
#if defined(__linux__)
// Linux: the host's event loop runs the editor (the X connection's fd, the 30 Hz timer)
void on_fd(const clap_plugin* p, int, clap_posix_fd_flags_t) {
	if (self(p)->view) awva_editor::view_on_fd(self(p)->view);
}
extern const clap_plugin_posix_fd_support kFdSupport = {on_fd};
void on_timer(const clap_plugin* p, clap_id id) {
	if (self(p)->view && id == self(p)->gui_timer) awva_editor::view_on_timer(self(p)->view);
}
extern const clap_plugin_timer_support kTimerSupport = {on_timer};
#endif

} // namespace awva_plugin
