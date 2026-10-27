// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The plugins' editor (app/DESIGN.md, "The plugin editor"): the standalone app's screen, drawn
// natively to the same design and tokens (design_tokens.h, generated from app/design/tokens.json).
// This part is the same on every OS: the layout, the painting (through Canvas) and the input. The
// OS's view (editor_win.cpp, editor_mac.mm) gives it a canvas, the mouse and a timer.
//
//   header   the logo, the engine's title, the preset picker (previous, the name, next), the theme
//   body     the sound's name, the engine's summary, the hero knobs four to a row, "Every parameter"
//            (grouped slider rows, then mono); the body scrolls. Each parameter shows its icon by its
//            name (awva's assets/icons: param_icons.h), and its label and hover text as a tooltip
//            when the pointer rests on it
//   footer   the voices playing, mono
//   menu     the presets, over the body
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "awva_plugin/design_tokens.h"
#include "nsc/nsc.h"

namespace awva_editor {

struct Rect {
	float x = 0, y = 0, w = 0, h = 0;
	bool contains(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

enum class Align { Left, Center, Right };

// what the OS's view draws with (sizes in points, 0xAARRGGBB)
struct Canvas {
	virtual ~Canvas() = default;
	virtual void fill(const Rect& r, uint32_t argb, float radius = 0) = 0;
	virtual void stroke(const Rect& r, uint32_t argb, float width, float radius = 0) = 0;
	virtual void line(float x0, float y0, float x1, float y1, uint32_t argb, float width) = 0;  // round ends
	virtual void arc(float cx, float cy, float r, float from, float sweep, uint32_t argb, float width) = 0; // radians, clockwise from +x, round ends
	virtual void disc(float cx, float cy, float r, uint32_t argb) = 0;
	virtual void polyline(const float* xy, int n, uint32_t argb, float width) = 0; // n points (x, y), round ends and joins
	virtual void text(const std::string& utf8, const Rect& r, const awva_tokens::Type& t, uint32_t argb, Align a) = 0; // one line, vertically centred, ellipsised
	virtual float text_width(const std::string& utf8, const awva_tokens::Type& t) = 0;
	virtual void logo(const Rect& r) = 0;
	virtual void push_clip(const Rect& r) = 0;
	virtual void pop_clip() = 0;
};

// what the editor needs from the plugin (the UI thread)
struct Host {
	virtual ~Host() = default;
	virtual const nsc_desc& desc() const = 0;
	virtual int preset() const = 0;          // the factory preset the sound started from, -1 none
	virtual float value(int index) const = 0; // a parameter's value now
	virtual void begin(int index) = 0;        // a gesture on a parameter starts (the host records it)
	virtual void set(int index, float v) = 0;
	virtual void end(int index) = 0;
	virtual void select_preset(int index) = 0;
	virtual bool mono() const = 0;
	virtual void set_mono(bool on) = 0;
	virtual int voices() const = 0;
	virtual std::string theme() const = 0; // "system", "light", "dark"
	virtual void set_theme(const std::string& t) = 0;
	virtual bool os_dark() const = 0;      // the OS's appearance
	virtual void repaint() = 0;            // ask the view for a paint
};

class Editor {
public:
	explicit Editor(Host& host);

	void paint(Canvas& c, float w, float h);
	// input, in points; returns true if a paint is needed
	bool mouse_down(float x, float y, bool shift, bool double_click);
	bool mouse_drag(float x, float y, bool shift);
	bool mouse_up(float x, float y);
	bool mouse_move(float x, float y);
	bool wheel(float x, float y, float dy); // dy > 0: up / away
	// 30 times a second: true when something it shows changed (a value, the preset, the voices, a
	// tooltip due)
	bool tick();

	const awva_tokens::Colors& colors() const;
	// the views a click opens (also for the screenshots)
	void show_all(bool on) { all_ = on; }
	void show_menu(bool on) { menu_ = on; }
	void show_tip(int index) { tip_ = index; } // a parameter's tooltip (-1 none; for the screenshots)

private:
	enum class Hit { None, Prev, Next, Preset, Theme, Knob, Toggle, All, Slider, Mono, MenuItem, Menu, Param }; // Param: a parameter's name (its tooltip)
	struct Target {
		Hit kind = Hit::None;
		Rect r;
		int index = -1; // a parameter, or a preset
	};
	Target hit(float x, float y) const;
	std::vector<int> heroes() const;
	void layout(float w, float h);
	void paint_knob(Canvas& c, const Rect& r, int i, bool active);
	void paint_label(Canvas& c, const Rect& r, int i);
	void paint_slider_row(Canvas& c, const Rect& r, int i);
	void paint_menu(Canvas& c);
	void paint_icon(Canvas& c, const char* icon, float x, float y, float size, uint32_t fg, uint32_t bg);
	void paint_tip(Canvas& c);
	void set_norm(int i, float n);
	float norm(int i) const;

	Host& h_;
	float w_ = 760, h_px_ = 480;
	float scroll_ = 0, content_h_ = 0;
	bool all_ = false;
	bool menu_ = false;
	float menu_scroll_ = 0;
	std::vector<Target> targets_; // laid out by the last paint
	Rect menu_rect_;
	int drag_ = -1;         // the parameter being dragged
	Hit drag_kind_ = Hit::None;
	float drag_y_ = 0, drag_v_ = 0;
	Rect drag_rect_;
	float hover_x_ = -1, hover_y_ = -1;
	int rest_ticks_ = 0;   // ticks the pointer has rested on the parameter under it
	int tip_ = -1;         // the parameter whose tooltip shows (-1 none)
	Rect tip_anchor_;      // what it is under
	// what was last painted, to know when to paint again
	std::vector<float> seen_;
	int seen_preset_ = -2, seen_voices_ = -1;
	bool seen_mono_ = false, seen_dark_ = false;
};

} // namespace awva_editor
