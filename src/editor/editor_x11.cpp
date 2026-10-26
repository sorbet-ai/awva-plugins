// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The editor on Linux: an X11 child window in the host's, drawn with cairo, its type set from the
// embedded DM Sans through FreeType (the variable font's weight as a cairo font variation). It has
// its own connection to the X server; the host runs it: the plugin registers the connection's fd
// (CLAP posix-fd-support) and a 30 Hz timer (timer-support) and calls view_on_fd / view_on_timer.
// Sizes are pixels and the host's scale (set_scale) multiplies the editor's points, as on Windows.
#if defined(__linux__)
#include "view.h" // (before X11's headers: their macros, None among them, would break the editor's names)

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <cairo-ft.h>
#include <cairo-xlib.h>
#include <cairo.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace awva_editor {

namespace {

void set_color(cairo_t* c, uint32_t argb) {
	cairo_set_source_rgba(c, ((argb >> 16) & 0xff) / 255.0, ((argb >> 8) & 0xff) / 255.0, (argb & 0xff) / 255.0, ((argb >> 24) & 0xff) / 255.0);
}

// the font, once per process: one FreeType face, its weights as cairo font variations
cairo_font_face_t* font_face() {
	static cairo_font_face_t* face = [] () -> cairo_font_face_t* {
		static FT_Library lib = nullptr;
		FT_Face ft = nullptr;
		if (FT_Init_FreeType(&lib) != 0 || FT_New_Memory_Face(lib, kFontData, static_cast<FT_Long>(kFontSize), 0, &ft) != 0) return nullptr;
		return cairo_ft_font_face_create_for_ft_face(ft, 0);
	}();
	return face;
}
void set_font(cairo_t* c, const awva_tokens::Type& t) {
	if (cairo_font_face_t* f = font_face()) cairo_set_font_face(c, f);
	else cairo_select_font_face(c, "sans-serif", CAIRO_FONT_SLANT_NORMAL, t.weight >= 600 ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(c, t.size);
	cairo_font_options_t* o = cairo_font_options_create();
	const std::string var = "wght=" + std::to_string(t.weight);
	cairo_font_options_set_variations(o, var.c_str());
	cairo_font_options_set_antialias(o, CAIRO_ANTIALIAS_GRAY);
	cairo_font_options_set_hint_style(o, CAIRO_HINT_STYLE_SLIGHT);
	cairo_set_font_options(c, o);
	cairo_font_options_destroy(o);
}

// the logo, decoded once
cairo_surface_t* logo_surface() {
	static cairo_surface_t* s = [] {
		struct Reader {
			const unsigned char* p;
			unsigned int left;
		} r = {kLogoData, kLogoSize};
		return cairo_image_surface_create_from_png_stream(
		    [](void* ctx, unsigned char* out, unsigned int n) {
			    auto* rd = static_cast<Reader*>(ctx);
			    if (n > rd->left) return CAIRO_STATUS_READ_ERROR;
			    std::memcpy(out, rd->p, n);
			    rd->p += n;
			    rd->left -= n;
			    return CAIRO_STATUS_SUCCESS;
		    },
		    &r);
	}();
	return cairo_surface_status(s) == CAIRO_STATUS_SUCCESS ? s : nullptr;
}

void rounded(cairo_t* c, double x, double y, double w, double h, double r) {
	r = std::max(0.0, std::min({r, w / 2, h / 2}));
	if (r <= 0) {
		cairo_rectangle(c, x, y, w, h);
		return;
	}
	cairo_new_sub_path(c);
	cairo_arc(c, x + w - r, y + r, r, -M_PI / 2, 0);
	cairo_arc(c, x + w - r, y + h - r, r, 0, M_PI / 2);
	cairo_arc(c, x + r, y + h - r, r, M_PI / 2, M_PI);
	cairo_arc(c, x + r, y + r, r, M_PI, 3 * M_PI / 2);
	cairo_close_path(c);
}

// the UTF-8 code points of s, by their byte offsets (to cut a line for its ellipsis)
std::vector<size_t> code_points(const std::string& s) {
	std::vector<size_t> at;
	for (size_t i = 0; i < s.size(); i++)
		if ((static_cast<unsigned char>(s[i]) & 0xc0) != 0x80) at.push_back(i);
	return at;
}

class CairoCanvas : public Canvas {
public:
	explicit CairoCanvas(cairo_t* c) : c_(c) {
		cairo_set_line_cap(c_, CAIRO_LINE_CAP_ROUND);
		cairo_set_line_join(c_, CAIRO_LINE_JOIN_ROUND);
	}
	void fill(const Rect& r, uint32_t argb, float radius) override {
		set_color(c_, argb);
		rounded(c_, r.x, r.y, r.w, r.h, radius);
		cairo_fill(c_);
	}
	void stroke(const Rect& r, uint32_t argb, float width, float radius) override {
		set_color(c_, argb);
		cairo_set_line_width(c_, width);
		rounded(c_, r.x + width / 2, r.y + width / 2, r.w - width, r.h - width, radius);
		cairo_stroke(c_);
	}
	void line(float x0, float y0, float x1, float y1, uint32_t argb, float width) override {
		set_color(c_, argb);
		cairo_set_line_width(c_, width);
		cairo_move_to(c_, x0, y0);
		cairo_line_to(c_, x1, y1);
		cairo_stroke(c_);
	}
	void arc(float cx, float cy, float r, float from, float sweep, uint32_t argb, float width) override {
		set_color(c_, argb);
		cairo_set_line_width(c_, width);
		cairo_new_sub_path(c_);
		cairo_arc(c_, cx, cy, r, from, from + sweep); // (y down: increasing angles go clockwise)
		cairo_stroke(c_);
	}
	void disc(float cx, float cy, float r, uint32_t argb) override {
		set_color(c_, argb);
		cairo_new_sub_path(c_);
		cairo_arc(c_, cx, cy, r, 0, 2 * M_PI);
		cairo_fill(c_);
	}
	// the glyphs of s at x, baseline y, each followed by the type's letter spacing; their width
	double glyphs(const std::string& s, const awva_tokens::Type& t, double x, double y, std::vector<cairo_glyph_t>* out) {
		set_font(c_, t);
		cairo_scaled_font_t* sf = cairo_get_scaled_font(c_);
		cairo_glyph_t* g = nullptr;
		int n = 0;
		if (cairo_scaled_font_text_to_glyphs(sf, x, y, s.data(), static_cast<int>(s.size()), &g, &n, nullptr, nullptr, nullptr) !=
		        CAIRO_STATUS_SUCCESS ||
		    n == 0) {
			if (g) cairo_glyph_free(g);
			return 0;
		}
		for (int i = 0; i < n; i++) g[i].x += i * t.spacing;
		cairo_text_extents_t last;
		cairo_scaled_font_glyph_extents(sf, &g[n - 1], 1, &last);
		const double w = g[n - 1].x + last.x_advance + t.spacing - x;
		if (out) out->assign(g, g + n);
		cairo_glyph_free(g);
		return w;
	}
	void text(const std::string& s, const Rect& r, const awva_tokens::Type& t, uint32_t argb, Align al) override {
		if (s.empty()) return;
		std::string shown = s;
		double w = glyphs(shown, t, 0, 0, nullptr);
		if (w > r.w) { // ellipsised: the longest start that fits with "..."
			const std::vector<size_t> at = code_points(s);
			for (size_t k = at.size(); k-- > 0;) {
				shown = s.substr(0, at[k]) + "\xE2\x80\xA6";
				w = glyphs(shown, t, 0, 0, nullptr);
				if (w <= r.w) break;
			}
		}
		set_font(c_, t);
		cairo_font_extents_t fe;
		cairo_font_extents(c_, &fe);
		const double x = al == Align::Left ? r.x : (al == Align::Center ? r.x + (r.w - w) / 2 : r.x + r.w - w);
		const double baseline = r.y + r.h / 2 + (fe.ascent - fe.descent) / 2;
		std::vector<cairo_glyph_t> g;
		glyphs(shown, t, x, baseline, &g);
		set_color(c_, argb);
		cairo_show_glyphs(c_, g.data(), static_cast<int>(g.size()));
	}
	float text_width(const std::string& s, const awva_tokens::Type& t) override { return static_cast<float>(glyphs(s, t, 0, 0, nullptr)); }
	void logo(const Rect& r) override {
		cairo_surface_t* img = logo_surface();
		if (!img) return;
		const double iw = cairo_image_surface_get_width(img), ih = cairo_image_surface_get_height(img);
		cairo_save(c_);
		cairo_translate(c_, r.x, r.y);
		cairo_scale(c_, r.w / iw, r.h / ih);
		cairo_set_source_surface(c_, img, 0, 0);
		cairo_pattern_set_filter(cairo_get_source(c_), CAIRO_FILTER_BEST);
		cairo_paint(c_);
		cairo_restore(c_);
	}
	void push_clip(const Rect& r) override {
		cairo_save(c_);
		cairo_rectangle(c_, r.x, r.y, r.w, r.h);
		cairo_clip(c_);
	}
	void pop_clip() override { cairo_restore(c_); }

private:
	cairo_t* c_;
};

// the editor painted at w x h points into a cairo context at scale pixels per point
void paint_into(cairo_t* c, Editor& ed, float w, float h, double scale) {
	cairo_save(c);
	cairo_scale(c, scale, scale);
	{
		CairoCanvas canvas(c);
		ed.paint(canvas, w, h);
	}
	cairo_restore(c);
}

} // namespace

struct View {
	Editor* ed = nullptr;
	Display* dpy = nullptr;
	Window win = 0;
	uint32_t w = awva_tokens::kEditorWidth, h = awva_tokens::kEditorHeight; // pixels
	double scale = 1.0;
	bool dragging = false;
	bool dirty = true;
	std::chrono::steady_clock::time_point last_click{};
	int last_x = -100, last_y = -100;

	void paint() {
		if (!win) return;
		cairo_surface_t* xs = cairo_xlib_surface_create(dpy, win, DefaultVisual(dpy, DefaultScreen(dpy)), static_cast<int>(w), static_cast<int>(h));
		// (drawn into an image, then copied: no flicker)
		cairo_surface_t* img = cairo_image_surface_create(CAIRO_FORMAT_RGB24, static_cast<int>(w), static_cast<int>(h));
		cairo_t* c = cairo_create(img);
		paint_into(c, *ed, static_cast<float>(w / scale), static_cast<float>(h / scale), scale);
		cairo_destroy(c);
		cairo_t* x = cairo_create(xs);
		cairo_set_source_surface(x, img, 0, 0);
		cairo_paint(x);
		cairo_destroy(x);
		cairo_surface_destroy(img);
		cairo_surface_flush(xs);
		cairo_surface_destroy(xs);
		XFlush(dpy);
		dirty = false;
	}
	void event(const XEvent& e) {
		const auto pt = [this](int x, int y) { return std::make_pair(static_cast<float>(x / scale), static_cast<float>(y / scale)); };
		switch (e.type) {
		case Expose:
			if (e.xexpose.count == 0) dirty = true;
			break;
		case ConfigureNotify:
			w = static_cast<uint32_t>(std::max(1, e.xconfigure.width));
			h = static_cast<uint32_t>(std::max(1, e.xconfigure.height));
			dirty = true;
			break;
		case ButtonPress: {
			const auto [x, y] = pt(e.xbutton.x, e.xbutton.y);
			if (e.xbutton.button == Button1) {
				const auto now = std::chrono::steady_clock::now();
				const bool dbl = now - last_click < std::chrono::milliseconds(400) && std::abs(e.xbutton.x - last_x) < 4 &&
				                 std::abs(e.xbutton.y - last_y) < 4;
				last_click = now, last_x = e.xbutton.x, last_y = e.xbutton.y;
				dragging = true;
				dirty |= ed->mouse_down(x, y, (e.xbutton.state & ShiftMask) != 0, dbl);
			} else if (e.xbutton.button == Button4 || e.xbutton.button == Button5) {
				dirty |= ed->wheel(x, y, e.xbutton.button == Button4 ? 1.0f : -1.0f);
			}
			break;
		}
		case ButtonRelease:
			if (e.xbutton.button == Button1) {
				const auto [x, y] = pt(e.xbutton.x, e.xbutton.y);
				dragging = false;
				dirty |= ed->mouse_up(x, y);
			}
			break;
		case MotionNotify: {
			const auto [x, y] = pt(e.xmotion.x, e.xmotion.y);
			dirty |= dragging ? ed->mouse_drag(x, y, (e.xmotion.state & ShiftMask) != 0) : ed->mouse_move(x, y);
			break;
		}
		case LeaveNotify:
			if (!dragging) dirty |= ed->mouse_move(-1, -1);
			break;
		default: break;
		}
	}
	void pump() {
		while (dpy && XPending(dpy) > 0) {
			XEvent e;
			XNextEvent(dpy, &e);
			if (e.xany.window == win) event(e);
		}
		if (dirty) paint();
	}
};

const char* view_api() { return "x11"; }

View* view_create(Editor& editor, Host&) {
	Display* dpy = XOpenDisplay(nullptr);
	if (!dpy) return nullptr; // (no X server: no editor)
	auto* v = new View;
	v->ed = &editor;
	v->dpy = dpy;
	return v;
}

void view_destroy(View* v) {
	if (!v) return;
	if (v->win) XDestroyWindow(v->dpy, v->win);
	XCloseDisplay(v->dpy);
	delete v;
}

bool view_attach(View* v, void* parent) {
	const Window p = static_cast<Window>(reinterpret_cast<uintptr_t>(parent));
	if (!p) return false;
	if (v->win) {
		XReparentWindow(v->dpy, v->win, p, 0, 0);
		XFlush(v->dpy);
		return true;
	}
	v->win = XCreateSimpleWindow(v->dpy, p, 0, 0, v->w, v->h, 0, 0, 0);
	if (!v->win) return false;
	XSelectInput(v->dpy, v->win,
	    ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | LeaveWindowMask | StructureNotifyMask);
	XMapWindow(v->dpy, v->win);
	XFlush(v->dpy);
	v->dirty = true;
	return true;
}

void view_set_scale(View* v, double scale) {
	v->scale = scale > 0 ? scale : 1.0;
	v->dirty = true;
}

void view_set_size(View* v, uint32_t w, uint32_t h) {
	v->w = std::max<uint32_t>(w, 1);
	v->h = std::max<uint32_t>(h, 1);
	if (v->win) {
		XResizeWindow(v->dpy, v->win, v->w, v->h);
		XFlush(v->dpy);
	}
	v->dirty = true;
}

void view_show(View* v, bool show) {
	if (!v->win) return;
	if (show) XMapWindow(v->dpy, v->win);
	else XUnmapWindow(v->dpy, v->win);
	XFlush(v->dpy);
}

void view_repaint(View* v) {
	if (v) v->dirty = true;
}

// GNOME's and KDE's colour scheme preference, as the desktop portal reports it, isn't reachable
// without D-Bus: the system theme is light here (the editor's own theme choice still applies)
bool view_os_dark() { return false; }

int view_fd(View* v) { return v && v->dpy ? ConnectionNumber(v->dpy) : -1; }
void view_on_fd(View* v) {
	if (v) v->pump();
}
void view_on_timer(View* v) {
	if (!v) return;
	if (v->ed->tick()) v->dirty = true;
	v->pump();
}

bool render_png(Editor& editor, float w, float h, float scale, const std::string& path) {
	const int pw = static_cast<int>(std::lround(w * scale)), ph = static_cast<int>(std::lround(h * scale));
	cairo_surface_t* img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
	cairo_t* c = cairo_create(img);
	paint_into(c, editor, w, h, scale);
	cairo_destroy(c);
	const bool ok = cairo_surface_write_to_png(img, path.c_str()) == CAIRO_STATUS_SUCCESS;
	cairo_surface_destroy(img);
	return ok;
}

} // namespace awva_editor
#endif
