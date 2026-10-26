// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The editor's view on each OS (editor_win.cpp: a child HWND drawn with Direct2D and DirectWrite;
// editor_mac.mm: an NSView drawn with Core Graphics and Core Text; editor_x11.cpp: an X11 child
// window drawn with cairo and FreeType), and an offscreen render of the editor to a PNG (the
// screenshots). The font (DM Sans) and the logo are in the binary
// (editor_assets.cpp, generated).
#pragma once

#include <cstdint>
#include <string>

#include "editor.h"

namespace awva_editor {

struct View;

const char* view_api(); // CLAP_WINDOW_API_WIN32, CLAP_WINDOW_API_COCOA or CLAP_WINDOW_API_X11
View* view_create(Editor& editor, Host& host);
void view_destroy(View* v);
bool view_attach(View* v, void* parent); // an HWND, an NSView, or an X11 Window (as a pointer)
void view_set_scale(View* v, double scale); // Windows, Linux: the host's scale (CLAP set_scale); macOS: ignored
void view_set_size(View* v, uint32_t w, uint32_t h); // CLAP's units: Windows and Linux pixels, macOS points
void view_show(View* v, bool show);
void view_repaint(View* v);
bool view_os_dark();
#if defined(__linux__)
// the host runs the view (CLAP posix-fd-support and timer-support): its X connection's fd, and the
// calls for it and for a 30 Hz timer
int view_fd(View* v);
void view_on_fd(View* v);
void view_on_timer(View* v);
#endif

// the editor painted at w x h points (scale: pixels per point) into a PNG
bool render_png(Editor& editor, float w, float h, float scale, const std::string& path);

// the embedded assets
extern const unsigned char kFontData[];
extern const unsigned int kFontSize;
extern const unsigned char kLogoData[];
extern const unsigned int kLogoSize;

} // namespace awva_editor
