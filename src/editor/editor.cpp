// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
#include "editor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace awva_editor {

namespace T = awva_tokens;

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kStart = kPi * 0.75f, kSweep = kPi * 1.5f; // the knob's 270 degrees

std::string label_of(const char* name) {
	std::string s = name ? name : "";
	std::replace(s.begin(), s.end(), '_', ' ');
	return s;
}

std::string format_value(float v, const nsc_param& p) {
	char b[32];
	if (p.kind != NSC_PARAM_CONTINUOUS) std::snprintf(b, sizeof b, "%d", static_cast<int>(std::lround(v)));
	else {
		const float span = std::fabs(p.max - p.min);
		std::snprintf(b, sizeof b, span >= 100 ? "%.0f" : (span >= 10 ? "%.1f" : "%.2f"), static_cast<double>(v));
	}
	return b;
}

uint32_t with_alpha(uint32_t argb, float a) { return (static_cast<uint32_t>(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f) << 24) | (argb & 0xffffffu); }

// the drawn icons: 16 pt, 1.6 pt lines, round ends
void chevron(Canvas& c, float cx, float cy, int dir, uint32_t col) { // 0 down, -1 left, 1 right
	const float s = 16;
	if (dir == 0) {
		c.line(cx - s * .22f, cy - s * .1f, cx, cy + s * .12f, col, 1.6f);
		c.line(cx, cy + s * .12f, cx + s * .22f, cy - s * .1f, col, 1.6f);
	} else {
		const float d = static_cast<float>(dir);
		c.line(cx - d * s * .1f, cy - s * .22f, cx + d * s * .12f, cy, col, 1.6f);
		c.line(cx + d * s * .12f, cy, cx - d * s * .1f, cy + s * .22f, col, 1.6f);
	}
}
void theme_icon(Canvas& c, float cx, float cy, uint32_t col) {
	const float r = 16 * .32f;
	c.arc(cx, cy, r, 0, 2 * kPi, col, 1.6f);
	for (float a = -kPi / 2; a < kPi / 2; a += kPi / 24) c.line(cx, cy, cx + std::cos(a) * r, cy + std::sin(a) * r, col, 1.4f);
}

} // namespace

Editor::Editor(Host& host) : h_(host) {}

const T::Colors& Editor::colors() const {
	const std::string t = h_.theme();
	const bool dark = t == "dark" || (t != "light" && h_.os_dark());
	return dark ? T::kDark : T::kLight;
}

std::vector<int> Editor::heroes() const {
	const nsc_desc& d = h_.desc();
	const char* const* names = d.heroes;
	const int p = h_.preset();
	if (p >= 0 && p < d.num_presets && d.presets[p].heroes[0]) names = d.presets[p].heroes;
	std::vector<int> v;
	for (int k = 0; k < NSC_MAX_HEROES && names[k]; k++)
		for (int i = 0; i < d.num_params; i++)
			if (!std::strcmp(d.params[i].name, names[k])) v.push_back(i);
	if (v.empty())
		for (int i = 0; i < d.num_params && i < 8; i++) v.push_back(i);
	return v;
}

float Editor::norm(int i) const {
	const nsc_param& p = h_.desc().params[i];
	return p.max > p.min ? std::clamp((h_.value(i) - p.min) / (p.max - p.min), 0.0f, 1.0f) : 0.0f;
}
void Editor::set_norm(int i, float n) {
	const nsc_param& p = h_.desc().params[i];
	float v = p.min + std::clamp(n, 0.0f, 1.0f) * (p.max - p.min);
	if (p.kind != NSC_PARAM_CONTINUOUS) v = std::round(v);
	h_.set(i, v);
}

// --- painting ----------------------------------------------------------------------------------------

void Editor::paint_knob(Canvas& c, const Rect& r, int i, bool active) {
	const T::Colors& k = colors();
	const nsc_param& p = h_.desc().params[i];
	const float size = T::knob;
	const float cx = r.x + r.w / 2, cy = r.y + size / 2, rad = size / 2 - T::knobStroke / 2 - 1;
	const float v = norm(i), from = p.min < 0 && p.max > 0 ? (0 - p.min) / (p.max - p.min) : 0;
	c.arc(cx, cy, rad, kStart, kSweep, k.bg3, T::knobStroke);
	const float a = std::min(from, v), b = std::max(from, v);
	if (b - a > 0.001f) c.arc(cx, cy, rad, kStart + kSweep * a, kSweep * (b - a), k.sage, T::knobStroke);
	const float ang = kStart + kSweep * v, pr = rad - T::knobStroke - 3;
	c.line(cx + std::cos(ang) * (pr - 7), cy + std::sin(ang) * (pr - 7), cx + std::cos(ang) * pr, cy + std::sin(ang) * pr, active ? k.text1 : k.text2, 2.5f);
	c.text(format_value(h_.value(i), p), {cx - 30, cy - 9, 60, 18}, T::typeValue, active ? k.text1 : k.text2, Align::Center);
	c.text(label_of(p.name), {r.x - 4, r.y + size + T::s1, r.w + 8, 18}, T::typeBody, k.text2, Align::Center);
}

void Editor::paint_slider_row(Canvas& c, const Rect& r, int i) {
	const T::Colors& k = colors();
	const nsc_param& p = h_.desc().params[i];
	c.text(label_of(p.name), {r.x, r.y, 180, r.h}, T::typeBody, k.text2, Align::Left);
	const Rect s = {r.x + 180, r.y, r.w - 180 - 64, r.h};
	const float y = r.y + r.h / 2;
	if (p.kind == NSC_PARAM_TOGGLE) {
		const bool on = h_.value(i) >= 0.5f;
		const Rect sw = {s.x, y - 9, 32, 18};
		c.fill(sw, on ? k.sage : k.bg3, 9);
		c.disc(on ? sw.x + 23 : sw.x + 9, y, 6, on ? k.onSage : k.text3);
		targets_.push_back({Hit::Toggle, sw, i});
	} else {
		const float v = norm(i), from = p.min < 0 && p.max > 0 ? (0 - p.min) / (p.max - p.min) : 0;
		c.line(s.x + 2, y, s.x + s.w - 2, y, k.bg3, T::sliderStroke);
		const float a = std::min(from, v), b = std::max(from, v);
		c.line(s.x + 2 + (s.w - 4) * a, y, s.x + 2 + (s.w - 4) * b, y, k.sage, T::sliderStroke);
		targets_.push_back({Hit::Slider, s, i});
	}
	c.text(format_value(h_.value(i), p), {r.x + r.w - 64, r.y, 64, r.h}, T::typeValue, k.text2, Align::Right);
}

void Editor::paint_menu(Canvas& c) {
	const T::Colors& k = colors();
	const nsc_desc& d = h_.desc();
	const float row = T::row;
	const float hmax = std::max(120.0f, h_px_ - menu_rect_.y - T::s4);
	menu_rect_.h = std::min(hmax, d.num_presets * row + 12);
	const float maxs = std::max(0.0f, d.num_presets * row + 12 - menu_rect_.h);
	menu_scroll_ = std::clamp(menu_scroll_, 0.0f, maxs);
	c.fill({menu_rect_.x - 1, menu_rect_.y + 3, menu_rect_.w + 2, menu_rect_.h + 2}, with_alpha(k.shadow, k.dark ? 0.35f : 0.10f), T::radius);
	c.fill(menu_rect_, k.bg1, T::radius);
	c.stroke(menu_rect_, k.hairline, 1, T::radius);
	targets_.push_back({Hit::Menu, menu_rect_, -1});
	c.push_clip({menu_rect_.x, menu_rect_.y + 4, menu_rect_.w, menu_rect_.h - 8});
	for (int i = 0; i < d.num_presets; i++) {
		const Rect r = {menu_rect_.x + 6, menu_rect_.y + 6 + i * row - menu_scroll_, menu_rect_.w - 12, row};
		if (r.y + r.h < menu_rect_.y || r.y > menu_rect_.y + menu_rect_.h) continue;
		if (r.contains(hover_x_, hover_y_)) c.fill(r, k.bg2, T::radiusSmall);
		if (i == h_.preset()) c.disc(r.x + 8, r.y + r.h / 2, 3, k.sage);
		c.text(d.presets[i].name, {r.x + 18, r.y, r.w - 24, r.h}, T::typeBody, k.text1, Align::Left);
		targets_.push_back({Hit::MenuItem, r, i});
	}
	c.pop_clip();
}

void Editor::paint(Canvas& c, float w, float h) {
	w_ = w;
	h_px_ = h;
	targets_.clear();
	const T::Colors& k = colors();
	const nsc_desc& d = h_.desc();
	c.fill({0, 0, w, h}, k.bg0);

	// the header
	const float hy = T::header / 2;
	c.logo({T::s4, hy - 11, 22 * 360.0f / 69.0f, 22});
	float x = T::s4 + 22 * 360.0f / 69.0f + T::s5;
	const float tw = std::min(220.0f, c.text_width(d.name, T::typeTitle));
	c.text(d.name, {x, 0, tw, T::header}, T::typeTitle, k.text2, Align::Left);
	x += tw + T::s4;
	const bool presets = d.num_presets > 0;
	auto button = [&](Hit kind, const Rect& r) {
		if (r.contains(hover_x_, hover_y_) && presets) c.fill(r, k.bg2, T::radiusSmall);
		targets_.push_back({kind, r, -1});
	};
	const Rect prev = {x, hy - 16, 32, 32};
	button(Hit::Prev, prev);
	chevron(c, prev.x + 16, hy, -1, presets ? k.text2 : k.text3);
	x += 32;
	const int p = h_.preset();
	const std::string pname = p >= 0 && p < d.num_presets ? d.presets[p].name : (presets ? "Custom" : "Default");
	const float pw = std::min(220.0f, c.text_width(pname, T::typeTitle)) + 20 + (presets ? 20 : 0);
	const Rect pick = {x, hy - 17, pw, 34};
	button(Hit::Preset, pick);
	c.text(pname, {pick.x + 10, pick.y, pw - 20 - (presets ? 20 : 0), pick.h}, T::typeTitle, k.text1, Align::Left);
	if (presets) chevron(c, pick.x + pw - 18, hy, 0, k.text3);
	x += pw;
	const Rect next = {x, hy - 16, 32, 32};
	button(Hit::Next, next);
	chevron(c, next.x + 16, hy, 1, presets ? k.text2 : k.text3);
	const Rect theme = {w - T::s4 - 32, hy - 16, 32, 32};
	if (theme.contains(hover_x_, hover_y_)) c.fill(theme, k.bg2, T::radiusSmall);
	theme_icon(c, theme.x + 16, hy, k.text2);
	targets_.push_back({Hit::Theme, theme, -1});
	menu_rect_ = {pick.x, pick.y + pick.h + 6, 280, 0};

	// the body, scrolled
	const Rect body = {0, T::header, w, std::max(0.0f, h - T::header - T::footer)};
	c.push_clip(body);
	const float colw = 4 * (T::knob + 24) + 3 * T::s6 + 120;
	const float bx = std::max(T::s6, (w - colw) / 2);
	float y = body.y + T::s5 - scroll_;
	const std::string title = p >= 0 && p < d.num_presets ? d.presets[p].name : d.name;
	c.text(title, {bx, y, w - bx - T::s5, T::typeSound.line}, T::typeSound, k.text1, Align::Left);
	y += T::typeSound.line + T::s1;
	c.text(d.summary ? d.summary : "", {bx, y, w - bx - T::s5, T::typeBody.line}, T::typeBody, k.text2, Align::Left);
	y += T::typeBody.line + T::s6;
	const std::vector<int> hs = heroes();
	const float cell = T::knob + 24;
	const int per_row = std::max(1, std::min(4, static_cast<int>((w - 2 * bx + T::s6) / (cell + T::s6))));
	for (size_t n = 0; n < hs.size(); n++) {
		const int i = hs[n];
		const float cx = bx + static_cast<float>(n % per_row) * (cell + T::s6);
		const float cy = y + static_cast<float>(n / per_row) * (T::knob + T::s1 + 18 + T::s5);
		const Rect r = {cx, cy, cell, T::knob + T::s1 + 18};
		if (d.params[i].kind == NSC_PARAM_TOGGLE) {
			const bool on = h_.value(i) >= 0.5f;
			const Rect sw = {cx + cell / 2 - 16, cy + T::knob / 2 - 9, 32, 18};
			c.fill(sw, on ? k.sage : k.bg3, 9);
			c.disc(on ? sw.x + 23 : sw.x + 9, sw.y + 9, 6, on ? k.onSage : k.text3);
			c.text(label_of(d.params[i].name), {r.x - 4, r.y + T::knob + T::s1, r.w + 8, 18}, T::typeBody, k.text2, Align::Center);
			targets_.push_back({Hit::Toggle, r, i});
		} else {
			paint_knob(c, r, i, drag_ == i || r.contains(hover_x_, hover_y_));
			targets_.push_back({Hit::Knob, {r.x, r.y, r.w, T::knob}, i});
		}
	}
	const int rows = static_cast<int>((hs.size() + per_row - 1) / per_row);
	y += rows * (T::knob + T::s1 + 18) + std::max(0, rows - 1) * T::s5 + T::s6;
	// every parameter
	const float allw = c.text_width("Every parameter", T::typeBodyStrong);
	const Rect all = {bx, y, allw + 24, 22};
	c.text("Every parameter", {bx, y, allw + 4, 22}, T::typeBodyStrong, k.text2, Align::Left);
	chevron(c, bx + allw + 12, y + 11, all_ ? 0 : 1, k.text3);
	targets_.push_back({Hit::All, all, -1});
	y += 22;
	if (all_) {
		y += T::s3;
		const float rw = std::min(colw, w - bx - T::s6);
		const char* group = nullptr;
		for (int i = 0; i < d.num_params; i++) {
			const char* g = d.params[i].group ? d.params[i].group : "";
			if (!group || std::strcmp(g, group) != 0) {
				group = g;
				y += T::s4;
				c.text(*g ? g : "general", {bx, y, rw, T::typeMicro.line}, T::typeMicro, k.text3, Align::Left);
				y += T::typeMicro.line + T::s1;
			}
			paint_slider_row(c, {bx, y, rw, T::row}, i);
			y += T::row;
		}
		y += T::s4;
		c.text("play", {bx, y, rw, T::typeMicro.line}, T::typeMicro, k.text3, Align::Left);
		y += T::typeMicro.line + T::s1;
		c.text("legato", {bx, y, 180, T::row}, T::typeBody, k.text2, Align::Left);
		const Rect sw = {bx + 180, y + T::row / 2 - 9, 32, 18};
		const bool on = h_.legato();
		c.fill(sw, on ? k.sage : k.bg3, 9);
		c.disc(on ? sw.x + 23 : sw.x + 9, sw.y + 9, 6, on ? k.onSage : k.text3);
		targets_.push_back({Hit::Legato, sw, -1});
		y += T::row;
	}
	y += T::s6;
	content_h_ = y + scroll_ - body.y;
	c.pop_clip();

	// the footer
	const float fy = h - T::footer;
	c.fill({0, fy, w, T::footer}, k.bg0);
	char f[96];
	std::snprintf(f, sizeof f, "%d voice%s playing%s", h_.voices(), h_.voices() == 1 ? "" : "s", h_.legato() ? " · legato" : "");
	c.disc(T::s4 + 3, fy + T::footer / 2, 3, h_.voices() > 0 ? k.blue : k.text3);
	c.text(f, {T::s4 + 12, fy, w / 2, T::footer}, T::typeSmall, k.text3, Align::Left);
	c.text(std::string("awva ") + (d.version ? d.version : ""), {w / 2, fy, w / 2 - T::s4, T::footer}, T::typeSmall, k.text3, Align::Right);

	if (menu_ && d.num_presets > 0) paint_menu(c);

	// what this paint showed
	seen_.resize(static_cast<size_t>(d.num_params));
	for (int i = 0; i < d.num_params; i++) seen_[static_cast<size_t>(i)] = h_.value(i);
	seen_preset_ = h_.preset();
	seen_voices_ = h_.voices();
	seen_legato_ = h_.legato();
	seen_dark_ = k.dark;
}

// --- input ---------------------------------------------------------------------------------------------

Editor::Target Editor::hit(float x, float y) const {
	if (menu_) {
		for (auto it = targets_.rbegin(); it != targets_.rend(); ++it)
			if ((it->kind == Hit::MenuItem || it->kind == Hit::Menu) && it->r.contains(x, y)) return *it;
		return {};
	}
	for (auto it = targets_.rbegin(); it != targets_.rend(); ++it)
		if (it->r.contains(x, y)) return *it;
	return {};
}

bool Editor::mouse_down(float x, float y, bool, bool double_click) {
	const Target t = hit(x, y);
	const nsc_desc& d = h_.desc();
	if (menu_) {
		if (t.kind == Hit::MenuItem) h_.select_preset(t.index);
		if (t.kind != Hit::Menu) menu_ = false;
		return true;
	}
	switch (t.kind) {
	case Hit::Prev:
	case Hit::Next:
		if (d.num_presets > 0) {
			const int p = h_.preset();
			const int n = p < 0 ? 0 : (p + (t.kind == Hit::Next ? 1 : -1) + d.num_presets) % d.num_presets;
			h_.select_preset(n);
		}
		return true;
	case Hit::Preset:
		menu_ = d.num_presets > 0;
		menu_scroll_ = std::max(0.0f, (h_.preset() - 3) * T::row);
		return true;
	case Hit::Theme: {
		const bool dark = colors().dark;
		h_.set_theme(dark ? "light" : "dark");
		return true;
	}
	case Hit::All:
		all_ = !all_;
		return true;
	case Hit::Legato:
		h_.set_legato(!h_.legato());
		return true;
	case Hit::Toggle:
		h_.begin(t.index);
		h_.set(t.index, h_.value(t.index) >= 0.5f ? 0.0f : 1.0f);
		h_.end(t.index);
		return true;
	case Hit::Knob:
	case Hit::Slider:
		h_.begin(t.index);
		if (double_click) {
			h_.set(t.index, d.params[t.index].def);
			h_.end(t.index);
			return true;
		}
		drag_ = t.index;
		drag_kind_ = t.kind;
		drag_rect_ = t.r;
		drag_y_ = y;
		drag_v_ = norm(t.index);
		if (t.kind == Hit::Slider) set_norm(t.index, (x - t.r.x - 2) / std::max(1.0f, t.r.w - 4));
		return true;
	default: return false;
	}
}

bool Editor::mouse_drag(float x, float y, bool shift) {
	hover_x_ = x;
	hover_y_ = y;
	if (drag_ < 0) return false;
	if (drag_kind_ == Hit::Knob) {
		drag_v_ = std::clamp(drag_v_ - (y - drag_y_) / (shift ? 800.0f : 200.0f), 0.0f, 1.0f);
		drag_y_ = y;
		set_norm(drag_, drag_v_);
	} else {
		set_norm(drag_, (x - drag_rect_.x - 2) / std::max(1.0f, drag_rect_.w - 4));
	}
	return true;
}

bool Editor::mouse_up(float, float) {
	if (drag_ < 0) return false;
	h_.end(drag_);
	drag_ = -1;
	return true;
}

bool Editor::mouse_move(float x, float y) {
	const Target before = hit(hover_x_, hover_y_);
	hover_x_ = x;
	hover_y_ = y;
	const Target after = hit(x, y);
	return before.kind != after.kind || before.index != after.index || menu_;
}

bool Editor::wheel(float x, float y, float dy) {
	if (menu_) {
		menu_scroll_ -= dy * 40;
		return true;
	}
	const Target t = hit(x, y);
	if (t.kind == Hit::Knob || t.kind == Hit::Slider) {
		const nsc_param& p = h_.desc().params[t.index];
		const float step = p.kind != NSC_PARAM_CONTINUOUS && p.max > p.min ? 1.0f / (p.max - p.min) : 0.01f;
		h_.begin(t.index);
		set_norm(t.index, norm(t.index) + (dy > 0 ? step : -step));
		h_.end(t.index);
		return true;
	}
	const float maxs = std::max(0.0f, content_h_ - (h_px_ - T::header - T::footer));
	const float s = std::clamp(scroll_ - dy * 40, 0.0f, maxs);
	if (s == scroll_) return false;
	scroll_ = s;
	return true;
}

bool Editor::tick() {
	const nsc_desc& d = h_.desc();
	if (h_.preset() != seen_preset_ || h_.voices() != seen_voices_ || h_.legato() != seen_legato_ || colors().dark != seen_dark_) return true;
	if (seen_.size() != static_cast<size_t>(d.num_params)) return true;
	for (int i = 0; i < d.num_params; i++)
		if (h_.value(i) != seen_[static_cast<size_t>(i)]) return true;
	return false;
}

} // namespace awva_editor
