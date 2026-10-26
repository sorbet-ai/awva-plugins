// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The editor as a host sees it:
//   editor_host_test PLUGIN.clap
// The CLAP gui extension: the API, the size, resizing to the minimum. On Windows the editor goes
// into a hidden window and a knob is dragged with the mouse (window messages): the next process()
// must give the host a gesture begin, the value and a gesture end for that parameter, and the
// plugin must have the new value. On Linux (an X server, e.g. Xvfb) the editor goes into the host's
// window, made on the host's own X connection, which stays open for the whole run as a host's does.
// On macOS: create and destroy (no window). Opens no audio device.
#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <clap/clap.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "awva_plugin/design_tokens.h"

#if !defined(_WIN32) && !defined(__APPLE__)
#include <X11/Xlib.h>
#endif

namespace {

int failures = 0;
void check(bool ok, const std::string& what) {
	std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
	std::fflush(stdout);
	failures += !ok;
}

const clap_plugin_entry* load_entry(const char* path) {
#ifdef _WIN32
	HMODULE m = LoadLibraryA(path);
	return m ? reinterpret_cast<const clap_plugin_entry*>(GetProcAddress(m, "clap_entry")) : nullptr;
#else
	std::string p = path;
	if (p.size() > 5 && p.substr(p.size() - 5) == ".clap") {
		const size_t slash = p.find_last_of('/');
		const std::string inner = p + "/Contents/MacOS/" + p.substr(slash + 1, p.size() - slash - 6);
		if (FILE* f = std::fopen(inner.c_str(), "rb")) {
			std::fclose(f);
			p = inner;
		}
	}
	void* h = dlopen(p.c_str(), RTLD_NOW | RTLD_LOCAL);
	return h ? static_cast<const clap_plugin_entry*>(dlsym(h, "clap_entry")) : nullptr;
#endif
}

int flushes = 0;
const void* host_ext(const clap_host*, const char* id) {
	static const clap_host_params params = {[](const clap_host*, clap_param_rescan_flags) {}, [](const clap_host*, clap_id, clap_param_clear_flags) {},
	    [](const clap_host*) { flushes++; }};
	return !std::strcmp(id, CLAP_EXT_PARAMS) ? &params : nullptr;
}
const clap_host kHost = {CLAP_VERSION_INIT, nullptr, "editor_host_test", "Sorbet AI", "", "0.1", host_ext, [](const clap_host*) {},
    [](const clap_host*) {}, [](const clap_host*) {}};

struct Out {
	std::vector<uint16_t> types;
	std::vector<clap_id> ids;
	std::vector<double> values;
};
Out out;
const clap_output_events kOut = {nullptr, [](const clap_output_events*, const clap_event_header* h) {
	                                 out.types.push_back(h->type);
	                                 if (h->type == CLAP_EVENT_PARAM_VALUE) {
		                                 const auto* v = reinterpret_cast<const clap_event_param_value*>(h);
		                                 out.ids.push_back(v->param_id);
		                                 out.values.push_back(v->value);
	                                 } else if (h->type == CLAP_EVENT_PARAM_GESTURE_BEGIN || h->type == CLAP_EVENT_PARAM_GESTURE_END) {
		                                 out.ids.push_back(reinterpret_cast<const clap_event_param_gesture*>(h)->param_id);
		                                 out.values.push_back(0);
	                                 }
	                                 return true;
                                 }};
const clap_input_events kIn = {nullptr, [](const clap_input_events*) -> uint32_t { return 0; },
    [](const clap_input_events*, uint32_t) -> const clap_event_header* { return nullptr; }};

void process(const clap_plugin* p) {
	float l[256], r[256];
	float* ch[2] = {l, r};
	clap_audio_buffer ob = {ch, nullptr, 2, 0, 0};
	clap_process pr = {0, 256, nullptr, nullptr, &ob, 0, 1, &kIn, &kOut};
	p->process(p, &pr);
}

} // namespace

int main(int argc, char** argv) {
	if (argc < 2) {
		std::fprintf(stderr, "usage: editor_host_test PLUGIN.clap\n");
		return 2;
	}
	const clap_plugin_entry* entry = load_entry(argv[1]);
	if (!entry || !entry->init(argv[1])) return 2;
	const auto* f = static_cast<const clap_plugin_factory*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
	const clap_plugin* p = f->create_plugin(f, &kHost, f->get_plugin_descriptor(f, 0)->id);
	if (!p || !p->init(p)) return 2;
	const auto* gui = static_cast<const clap_plugin_gui*>(p->get_extension(p, CLAP_EXT_GUI));
	const auto* params = static_cast<const clap_plugin_params*>(p->get_extension(p, CLAP_EXT_PARAMS));
	check(gui != nullptr, "the plugin has an editor (CLAP gui)");
	if (!gui) return 1;
#if defined(_WIN32)
	const char* api = CLAP_WINDOW_API_WIN32;
#elif defined(__APPLE__)
	const char* api = CLAP_WINDOW_API_COCOA;
#else
	const char* api = CLAP_WINDOW_API_X11;
#endif
	check(gui->is_api_supported(p, api, false) && !gui->is_api_supported(p, api, true), std::string("embedded in the host's window (") + api + "), not floating");
#if !defined(_WIN32) && !defined(__APPLE__)
	// The host's X connection, open from before the editor's first and until after its last, as a
	// host's is. The editor has its own connection; were it the X server's only client, closing it
	// would leave the server with none, and Xvfb resets then and refuses connections while it does:
	// the editor created again could find no server.
	Display* host_dpy = XOpenDisplay(nullptr);
	check(host_dpy != nullptr, "the host's X connection");
	if (!host_dpy) return 1;
#endif
	p->activate(p, 48000, 1, 4096);
	p->start_processing(p);
	check(gui->create(p, api, false), "created");
	gui->set_scale(p, 1.0);
	uint32_t w = 0, h = 0;
	gui->get_size(p, &w, &h);
	check(w == static_cast<uint32_t>(awva_tokens::kEditorWidth) && h == static_cast<uint32_t>(awva_tokens::kEditorHeight), "its size is the design's (" + std::to_string(w) + " x " + std::to_string(h) + ")");
	uint32_t aw = 10, ah = 10;
	gui->adjust_size(p, &aw, &ah);
	check(gui->can_resize(p) && aw == static_cast<uint32_t>(awva_tokens::kEditorMinWidth) && ah == static_cast<uint32_t>(awva_tokens::kEditorMinHeight),
	    "resizable, down to the design's minimum");
#if defined(_WIN32)
	// a hidden window as the host's
	HWND parent = CreateWindowExW(0, L"STATIC", L"host", WS_OVERLAPPEDWINDOW, 0, 0, 900, 600, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
	clap_window win = {CLAP_WINDOW_API_WIN32, {}};
	win.win32 = parent;
	check(gui->set_parent(p, &win) && gui->show(p), "attached to the host's window");
	HWND child = FindWindowExW(parent, nullptr, nullptr, nullptr);
	check(child != nullptr, "the editor's window is the host window's child");
	if (child) {
		// the first hero knob: the layout of editor.cpp at 760 points wide (the body's column, the
		// sound's name and summary, then the knobs)
		const float colw = 4 * (awva_tokens::knob + 24) + 3 * awva_tokens::s6 + 120;
		const float bx = std::max(awva_tokens::s6, (awva_tokens::kEditorWidth - colw) / 2);
		const float y0 = awva_tokens::header + awva_tokens::s5 + awva_tokens::typeSound.line + awva_tokens::s1 + awva_tokens::typeBody.line + awva_tokens::s6;
		const int x = static_cast<int>(bx + (awva_tokens::knob + 24) / 2), y = static_cast<int>(y0 + awva_tokens::knob / 2);
		const uint32_t n = params->count(p);
		std::vector<double> before(n);
		for (uint32_t i = 0; i < n; i++) {
			clap_param_info info;
			params->get_info(p, i, &info);
			params->get_value(p, info.id, &before[i]);
		}
		SendMessageW(child, WM_PAINT, 0, 0); // (a host shows it before anyone clicks: the layout is the paint's)
		SendMessageW(child, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
		SendMessageW(child, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x, y - 60));
		SendMessageW(child, WM_LBUTTONUP, 0, MAKELPARAM(x, y - 60));
		check(flushes > 0, "the editor asked the host for a flush");
		out = {};
		process(p);
		const bool shape = out.types.size() >= 3 && out.types.front() == CLAP_EVENT_PARAM_GESTURE_BEGIN && out.types.back() == CLAP_EVENT_PARAM_GESTURE_END;
		bool same = shape;
		for (clap_id id : out.ids) same = same && id == out.ids.front();
		check(shape && same, "a knob drag: the host is given a gesture begin, values and a gesture end, for one parameter (" +
		                         std::to_string(out.types.size()) + " events)");
		bool changed = false;
		for (uint32_t i = 0; i < n; i++) {
			clap_param_info info;
			params->get_info(p, i, &info);
			double v;
			params->get_value(p, info.id, &v);
			if (!out.ids.empty() && info.id == out.ids.front()) changed = v != before[i] && std::fabs(v - out.values[out.values.size() - 2]) < 1e-6;
		}
		check(changed, "the plugin has the knob's new value");
	}
	gui->hide(p);
	gui->destroy(p);
	DestroyWindow(parent);
#elif defined(__APPLE__)
	gui->destroy(p);
	check(true, "destroyed");
#else
	// the host's window, on the server before the editor's connection names it as its parent
	const Window parent = XCreateSimpleWindow(host_dpy, DefaultRootWindow(host_dpy), 0, 0, 900, 600, 0, 0, 0);
	XSync(host_dpy, False);
	clap_window win = {CLAP_WINDOW_API_X11, {}};
	win.x11 = parent;
	check(gui->set_parent(p, &win) && gui->show(p), "attached to the host's window");
	Window root_ret = 0, parent_ret = 0, *children = nullptr;
	unsigned int nchildren = 0;
	XQueryTree(host_dpy, parent, &root_ret, &parent_ret, &children, &nchildren);
	check(nchildren == 1, "the editor's window is the host window's child");
	if (children) XFree(children);
	gui->hide(p);
	gui->destroy(p);
	check(true, "destroyed");
#endif
	// created and destroyed again (a host closes and opens the editor)
	check(gui->create(p, api, false), "created again");
	gui->destroy(p);
	p->stop_processing(p);
	p->deactivate(p);
	p->destroy(p);
	entry->deinit();
#if !defined(_WIN32) && !defined(__APPLE__)
	XDestroyWindow(host_dpy, parent);
	XCloseDisplay(host_dpy);
#endif
	std::printf("%s\n", failures ? "editor_host_test: FAILED" : "editor_host_test: ok");
	return failures ? 1 : 0;
}
