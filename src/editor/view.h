// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The editor's view on each OS (editor_win.cpp: a child HWND drawn with Direct2D and DirectWrite;
// editor_mac.mm: an NSView drawn with Core Graphics and Core Text), and an offscreen render of the
// editor to a PNG (the screenshots). The font (DM Sans) and the logo are in the binary
// (editor_assets.cpp, generated).
#pragma once

#include <cstdint>
#include <string>

#include "editor.h"

namespace awva_editor {

struct View;

const char* view_api(); // CLAP_WINDOW_API_WIN32 or CLAP_WINDOW_API_COCOA
View* view_create(Editor& editor, Host& host);
void view_destroy(View* v);
bool view_attach(View* v, void* parent); // an HWND, or an NSView
void view_set_scale(View* v, double scale); // Windows: the host's scale (CLAP set_scale); macOS: ignored
void view_set_size(View* v, uint32_t w, uint32_t h); // CLAP's units: Windows pixels, macOS points
void view_show(View* v, bool show);
void view_repaint(View* v);
bool view_os_dark();

// the editor painted at w x h points (scale: pixels per point) into a PNG
bool render_png(Editor& editor, float w, float h, float scale, const std::string& path);

// the embedded assets
extern const unsigned char kFontData[];
extern const unsigned int kFontSize;
extern const unsigned char kLogoData[];
extern const unsigned int kLogoSize;

} // namespace awva_editor
