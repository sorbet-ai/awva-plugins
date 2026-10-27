// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The editor on Windows: a child window of the host's, drawn with Direct2D, its type set with
// DirectWrite from the embedded DM Sans (an in-memory font collection, so nothing is installed).
// The window class is registered per module, so several awva plugins in one host never share one.
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <d2d1.h>
#include <dwrite_3.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "view.h"

namespace awva_editor {

namespace {

template <class T> void release(T*& p) {
	if (p) p->Release();
	p = nullptr;
}

std::wstring wide(const std::string& s) {
	const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
	std::wstring w(static_cast<size_t>(n), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
	return w;
}

D2D1_COLOR_F col(uint32_t argb) {
	return D2D1::ColorF(((argb >> 16) & 0xff) / 255.0f, ((argb >> 8) & 0xff) / 255.0f, (argb & 0xff) / 255.0f, ((argb >> 24) & 0xff) / 255.0f);
}

// the factories and the font, once per process
struct Shared {
	ID2D1Factory* d2d = nullptr;
	IDWriteFactory5* dw = nullptr;
	IWICImagingFactory* wic = nullptr;
	IDWriteFontCollection1* fonts = nullptr;
	std::wstring family = L"Segoe UI";
	ID2D1StrokeStyle* round = nullptr;

	Shared() {
		CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
		D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d);
		DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory5), reinterpret_cast<IUnknown**>(&dw));
		CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
		if (d2d) {
			const D2D1_STROKE_STYLE_PROPERTIES p = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND);
			d2d->CreateStrokeStyle(p, nullptr, 0, &round);
		}
		// DM Sans from memory
		IDWriteInMemoryFontFileLoader* loader = nullptr;
		IDWriteFontFile* file = nullptr;
		IDWriteFontSetBuilder1* b = nullptr;
		IDWriteFontSet* set = nullptr;
		if (dw && SUCCEEDED(dw->CreateInMemoryFontFileLoader(&loader)) && SUCCEEDED(dw->RegisterFontFileLoader(loader)) &&
		    SUCCEEDED(loader->CreateInMemoryFontFileReference(dw, kFontData, kFontSize, nullptr, &file)) && SUCCEEDED(dw->CreateFontSetBuilder(&b)) &&
		    SUCCEEDED(b->AddFontFile(file)) && SUCCEEDED(b->CreateFontSet(&set)) && SUCCEEDED(dw->CreateFontCollectionFromFontSet(set, &fonts))) {
			// the family's name as the variable font's named instances give it ("DM Sans", or with its
			// optical size, "DM Sans 14pt"): the first family whose name starts with "DM Sans"
			for (UINT32 k = 0; k < fonts->GetFontFamilyCount() && family == L"Segoe UI"; k++) {
				IDWriteFontFamily1* fam = nullptr;
				IDWriteLocalizedStrings* names = nullptr;
				if (SUCCEEDED(fonts->GetFontFamily(k, &fam)) && SUCCEEDED(fam->GetFamilyNames(&names)) && names->GetCount() > 0) {
					UINT32 len = 0;
					names->GetStringLength(0, &len);
					std::wstring n(len, L'\0');
					names->GetString(0, n.data(), len + 1);
					if (std::getenv("AWVA_EDITOR_FONTS")) std::fwprintf(stderr, L"editor font family: %ls\n", n.c_str());
					if (n == L"DM Sans" || n.rfind(L"DM Sans", 0) == 0) family = n;
				}
				release(names);
				release(fam);
			}
		}
		release(set);
		release(b);
		release(file);
		// (the loader stays registered for the process: the collection reads through it)
	}
};
Shared& shared() {
	static Shared s;
	return s;
}

class D2DCanvas : public Canvas {
public:
	D2DCanvas(ID2D1RenderTarget* rt, ID2D1Bitmap* logo) : rt_(rt), logo_(logo) { rt_->CreateSolidColorBrush(col(0xff000000u), &brush_); }
	~D2DCanvas() override {
		release(brush_);
		for (auto& f : formats_) release(f.fmt);
	}
	void fill(const Rect& r, uint32_t argb, float radius) override {
		brush_->SetColor(col(argb));
		const D2D1_RECT_F rr = D2D1::RectF(r.x, r.y, r.x + r.w, r.y + r.h);
		if (radius > 0) rt_->FillRoundedRectangle(D2D1::RoundedRect(rr, radius, radius), brush_);
		else rt_->FillRectangle(rr, brush_);
	}
	void stroke(const Rect& r, uint32_t argb, float width, float radius) override {
		brush_->SetColor(col(argb));
		const D2D1_RECT_F rr = D2D1::RectF(r.x + width / 2, r.y + width / 2, r.x + r.w - width / 2, r.y + r.h - width / 2);
		rt_->DrawRoundedRectangle(D2D1::RoundedRect(rr, radius, radius), brush_, width);
	}
	void line(float x0, float y0, float x1, float y1, uint32_t argb, float width) override {
		brush_->SetColor(col(argb));
		rt_->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), brush_, width, shared().round);
	}
	void arc(float cx, float cy, float r, float from, float sweep, uint32_t argb, float width) override {
		if (sweep >= 6.2831f) {
			brush_->SetColor(col(argb));
			rt_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), brush_, width);
			return;
		}
		ID2D1PathGeometry* g = nullptr;
		ID2D1GeometrySink* s = nullptr;
		if (FAILED(shared().d2d->CreatePathGeometry(&g)) || FAILED(g->Open(&s))) {
			release(g);
			return;
		}
		s->BeginFigure(D2D1::Point2F(cx + r * std::cos(from), cy + r * std::sin(from)), D2D1_FIGURE_BEGIN_HOLLOW);
		const float to = from + sweep;
		s->AddArc(D2D1::ArcSegment(D2D1::Point2F(cx + r * std::cos(to), cy + r * std::sin(to)), D2D1::SizeF(r, r), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE,
		    sweep > 3.14159265f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
		s->EndFigure(D2D1_FIGURE_END_OPEN);
		s->Close();
		brush_->SetColor(col(argb));
		rt_->DrawGeometry(g, brush_, width, shared().round);
		release(s);
		release(g);
	}
	void disc(float cx, float cy, float r, uint32_t argb) override {
		brush_->SetColor(col(argb));
		rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), brush_);
	}
	void text(const std::string& s, const Rect& r, const awva_tokens::Type& t, uint32_t argb, Align a) override {
		IDWriteTextFormat* f = format(t);
		if (!f || s.empty()) return;
		f->SetTextAlignment(a == Align::Left ? DWRITE_TEXT_ALIGNMENT_LEADING : (a == Align::Center ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_TRAILING));
		const std::wstring w = wide(s);
		IDWriteTextLayout* l = nullptr;
		if (FAILED(shared().dw->CreateTextLayout(w.c_str(), static_cast<UINT32>(w.size()), f, std::max(1.0f, r.w), std::max(1.0f, r.h), &l))) return;
		brush_->SetColor(col(argb));
		rt_->DrawTextLayout(D2D1::Point2F(r.x, r.y), l, brush_, D2D1_DRAW_TEXT_OPTIONS_CLIP);
		release(l);
	}
	float text_width(const std::string& s, const awva_tokens::Type& t) override {
		IDWriteTextFormat* f = format(t);
		if (!f) return 0;
		const std::wstring w = wide(s);
		IDWriteTextLayout* l = nullptr;
		if (FAILED(shared().dw->CreateTextLayout(w.c_str(), static_cast<UINT32>(w.size()), f, 10000, 100, &l))) return 0;
		DWRITE_TEXT_METRICS m{};
		l->GetMetrics(&m);
		release(l);
		return m.widthIncludingTrailingWhitespace;
	}
	void logo(const Rect& r) override {
		if (logo_) rt_->DrawBitmap(logo_, D2D1::RectF(r.x, r.y, r.x + r.w, r.y + r.h), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
	}
	void push_clip(const Rect& r) override { rt_->PushAxisAlignedClip(D2D1::RectF(r.x, r.y, r.x + r.w, r.y + r.h), D2D1_ANTIALIAS_MODE_ALIASED); }
	void pop_clip() override { rt_->PopAxisAlignedClip(); }

private:
	IDWriteTextFormat* format(const awva_tokens::Type& t) {
		for (auto& f : formats_)
			if (f.t == &t) return f.fmt;
		IDWriteTextFormat* fmt = nullptr;
		Shared& sh = shared();
		if (FAILED(sh.dw->CreateTextFormat(sh.family.c_str(), sh.family != L"Segoe UI" ? sh.fonts : nullptr, static_cast<DWRITE_FONT_WEIGHT>(t.weight),
		        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, t.size, L"en-us", &fmt)))
			return nullptr;
		fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
		fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
		IDWriteInlineObject* dots = nullptr;
		sh.dw->CreateEllipsisTrimmingSign(fmt, &dots);
		const DWRITE_TRIMMING trim = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
		fmt->SetTrimming(&trim, dots);
		release(dots);
		formats_.push_back({&t, fmt});
		return fmt;
	}
	struct Fmt {
		const awva_tokens::Type* t;
		IDWriteTextFormat* fmt;
	};
	ID2D1RenderTarget* rt_;
	ID2D1Bitmap* logo_;
	ID2D1SolidColorBrush* brush_ = nullptr;
	std::vector<Fmt> formats_;
};

// the logo, decoded from the embedded PNG
IWICFormatConverter* decode_logo() {
	Shared& sh = shared();
	IWICStream* st = nullptr;
	IWICBitmapDecoder* dec = nullptr;
	IWICBitmapFrameDecode* fr = nullptr;
	IWICFormatConverter* conv = nullptr;
	if (sh.wic && SUCCEEDED(sh.wic->CreateStream(&st)) && SUCCEEDED(st->InitializeFromMemory(const_cast<BYTE*>(kLogoData), kLogoSize)) &&
	    SUCCEEDED(sh.wic->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnLoad, &dec)) && SUCCEEDED(dec->GetFrame(0, &fr)) &&
	    SUCCEEDED(sh.wic->CreateFormatConverter(&conv)))
		conv->Initialize(fr, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeMedianCut);
	release(fr);
	release(dec);
	release(st);
	return conv;
}

} // namespace

struct View {
	Editor* ed = nullptr;
	Host* host = nullptr;
	HWND hwnd = nullptr;
	ID2D1HwndRenderTarget* rt = nullptr;
	ID2D1Bitmap* logo = nullptr;
	double scale = 1.0;
	UINT w = 760, h = 480; // pixels
	bool dragging = false;

	float pts(int px) const { return static_cast<float>(px / scale); }
	void drop_target() {
		release(logo);
		release(rt);
	}
	void paint() {
		if (!hwnd) return;
		if (!rt) {
			const D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
			    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), static_cast<float>(96 * scale), static_cast<float>(96 * scale));
			if (FAILED(shared().d2d->CreateHwndRenderTarget(p, D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(w, h)), &rt))) return;
			if (IWICFormatConverter* c = decode_logo()) {
				rt->CreateBitmapFromWicBitmap(c, nullptr, &logo);
				c->Release();
			}
		}
		rt->BeginDraw();
		{
			D2DCanvas canvas(rt, logo);
			ed->paint(canvas, static_cast<float>(w / scale), static_cast<float>(h / scale));
		}
		if (rt->EndDraw() == D2DERR_RECREATE_TARGET) drop_target();
	}
	static LRESULT CALLBACK proc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
		auto* v = reinterpret_cast<View*>(GetWindowLongPtrW(hw, GWLP_USERDATA));
		if (!v) return DefWindowProcW(hw, msg, wp, lp);
		const bool shift = (wp & MK_SHIFT) != 0;
		switch (msg) {
		case WM_PAINT: {
			PAINTSTRUCT ps;
			BeginPaint(hw, &ps);
			v->paint();
			EndPaint(hw, &ps);
			return 0;
		}
		case WM_ERASEBKGND: return 1;
		case WM_SIZE:
			v->w = LOWORD(lp);
			v->h = HIWORD(lp);
			if (v->rt) v->rt->Resize(D2D1::SizeU(v->w, v->h));
			InvalidateRect(hw, nullptr, FALSE);
			return 0;
		case WM_TIMER:
			if (v->ed->tick()) InvalidateRect(hw, nullptr, FALSE);
			return 0;
		case WM_LBUTTONDOWN:
		case WM_LBUTTONDBLCLK:
			SetFocus(hw);
			SetCapture(hw);
			v->dragging = true;
			if (v->ed->mouse_down(v->pts(GET_X_LPARAM(lp)), v->pts(GET_Y_LPARAM(lp)), shift, msg == WM_LBUTTONDBLCLK)) InvalidateRect(hw, nullptr, FALSE);
			return 0;
		case WM_MOUSEMOVE: {
			const float x = v->pts(GET_X_LPARAM(lp)), y = v->pts(GET_Y_LPARAM(lp));
			const bool r = v->dragging ? v->ed->mouse_drag(x, y, shift) : v->ed->mouse_move(x, y);
			if (r) InvalidateRect(hw, nullptr, FALSE);
			TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, hw, 0};
			TrackMouseEvent(&t);
			return 0;
		}
		case WM_MOUSELEAVE:
			if (v->ed->mouse_move(-1, -1)) InvalidateRect(hw, nullptr, FALSE);
			return 0;
		case WM_LBUTTONUP:
			ReleaseCapture();
			v->dragging = false;
			if (v->ed->mouse_up(v->pts(GET_X_LPARAM(lp)), v->pts(GET_Y_LPARAM(lp)))) InvalidateRect(hw, nullptr, FALSE);
			return 0;
		case WM_MOUSEWHEEL: {
			POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
			ScreenToClient(hw, &p);
			if (v->ed->wheel(v->pts(p.x), v->pts(p.y), GET_WHEEL_DELTA_WPARAM(wp) / 120.0f)) InvalidateRect(hw, nullptr, FALSE);
			return 0; // (the editor's alone: not passed on to the host's window)
		}
		case WM_MOUSEHWHEEL: return 0; // (nothing scrolls sideways; not passed on to the host either)
		case WM_SETTINGCHANGE: InvalidateRect(hw, nullptr, FALSE); break;
		default: break;
		}
		return DefWindowProcW(hw, msg, wp, lp);
	}
};

namespace {
// the window class, one per module (each plugin DLL its own)
const wchar_t* window_class() {
	static wchar_t name[64] = {};
	if (!name[0]) {
		HMODULE m = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&window_class), &m);
		swprintf(name, 64, L"awva_editor_%p", static_cast<void*>(m));
		WNDCLASSEXW wc{};
		wc.cbSize = sizeof wc;
		wc.style = CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
		wc.lpfnWndProc = View::proc;
		wc.hInstance = m;
		wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // (IDC_ARROW)
		wc.lpszClassName = name;
		RegisterClassExW(&wc);
	}
	return name;
}
} // namespace

const char* view_api() { return "win32"; }

View* view_create(Editor& editor, Host& host) {
	auto* v = new View;
	v->ed = &editor;
	v->host = &host;
	return v;
}

void view_destroy(View* v) {
	if (!v) return;
	if (v->hwnd) {
		KillTimer(v->hwnd, 1);
		SetWindowLongPtrW(v->hwnd, GWLP_USERDATA, 0);
		DestroyWindow(v->hwnd);
	}
	v->drop_target();
	delete v;
}

bool view_attach(View* v, void* parent) {
	if (v->hwnd) {
		SetParent(v->hwnd, static_cast<HWND>(parent));
		return true;
	}
	HMODULE m = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&view_api), &m);
	v->hwnd = CreateWindowExW(0, window_class(), L"awva", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, static_cast<int>(v->w), static_cast<int>(v->h),
	    static_cast<HWND>(parent), nullptr, m, nullptr);
	if (!v->hwnd) return false;
	SetWindowLongPtrW(v->hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(v));
	SetTimer(v->hwnd, 1, 33, nullptr);
	return true;
}

void view_set_scale(View* v, double scale) {
	v->scale = scale > 0 ? scale : 1.0;
	v->drop_target();
	if (v->hwnd) InvalidateRect(v->hwnd, nullptr, FALSE);
}

void view_set_size(View* v, uint32_t w, uint32_t h) {
	v->w = w;
	v->h = h;
	if (v->hwnd) SetWindowPos(v->hwnd, nullptr, 0, 0, static_cast<int>(w), static_cast<int>(h), SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
}

void view_show(View* v, bool show) {
	if (v->hwnd) ShowWindow(v->hwnd, show ? SW_SHOW : SW_HIDE);
}

void view_repaint(View* v) {
	if (v && v->hwnd) InvalidateRect(v->hwnd, nullptr, FALSE);
}

bool view_os_dark() {
	DWORD light = 1, size = sizeof light;
	if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD,
	        nullptr, &light, &size) != ERROR_SUCCESS)
		return false;
	return light == 0;
}

bool render_png(Editor& editor, float w, float h, float scale, const std::string& path) {
	Shared& sh = shared();
	const UINT pw = static_cast<UINT>(std::lround(w * scale)), ph = static_cast<UINT>(std::lround(h * scale));
	IWICBitmap* bmp = nullptr;
	ID2D1RenderTarget* rt = nullptr;
	ID2D1Bitmap* logo = nullptr;
	if (!sh.wic || FAILED(sh.wic->CreateBitmap(pw, ph, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp))) return false;
	const D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
	    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f * scale, 96.0f * scale);
	bool ok = SUCCEEDED(sh.d2d->CreateWicBitmapRenderTarget(bmp, p, &rt));
	if (ok) {
		if (IWICFormatConverter* c = decode_logo()) {
			rt->CreateBitmapFromWicBitmap(c, nullptr, &logo);
			c->Release();
		}
		rt->BeginDraw();
		{
			D2DCanvas canvas(rt, logo);
			editor.paint(canvas, w, h);
		}
		ok = SUCCEEDED(rt->EndDraw());
	}
	IWICStream* st = nullptr;
	IWICBitmapEncoder* enc = nullptr;
	IWICBitmapFrameEncode* fr = nullptr;
	const std::wstring wp = wide(path);
	ok = ok && SUCCEEDED(sh.wic->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(wp.c_str(), GENERIC_WRITE)) &&
	     SUCCEEDED(sh.wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) && SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) &&
	     SUCCEEDED(enc->CreateNewFrame(&fr, nullptr)) && SUCCEEDED(fr->Initialize(nullptr)) && SUCCEEDED(fr->WriteSource(bmp, nullptr)) &&
	     SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
	release(fr);
	release(enc);
	release(st);
	release(logo);
	release(rt);
	release(bmp);
	return ok;
}

} // namespace awva_editor
#endif
