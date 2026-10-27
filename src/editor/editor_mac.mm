// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The editor on macOS: an NSView in the host's, drawn with Core Graphics, its type set with Core
// Text from the embedded DM Sans (registered for this process only). The view's class is made at
// run time under a name unique to this binary, so several awva plugins in one host never share an
// Objective-C class (the collision a framework-based editor can't avoid).
#if defined(__APPLE__)
#import <Cocoa/Cocoa.h>
#import <CoreText/CoreText.h>
#import <ImageIO/ImageIO.h>
#include <dlfcn.h>
#include <objc/message.h>
#include <objc/runtime.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "view.h"

namespace awva_editor {

namespace {

CGColorRef cg(uint32_t argb) {
	return CGColorCreateSRGB(((argb >> 16) & 0xff) / 255.0, ((argb >> 8) & 0xff) / 255.0, (argb & 0xff) / 255.0, ((argb >> 24) & 0xff) / 255.0);
}

// the font, once per process
CTFontDescriptorRef font_descriptor() {
	static CTFontDescriptorRef d = [] {
		CFDataRef data = CFDataCreate(nullptr, kFontData, kFontSize);
		CTFontDescriptorRef r = CTFontManagerCreateFontDescriptorFromData(data);
		CFRelease(data);
		return r;
	}();
	return d;
}

CTFontRef font_for(const awva_tokens::Type& t) {
	static std::vector<std::pair<const awva_tokens::Type*, CTFontRef>> cache;
	for (auto& c : cache)
		if (c.first == &t) return c.second;
	CTFontRef f = nullptr;
	if (CTFontDescriptorRef base = font_descriptor()) {
		// the variable font's weight axis ('wght')
		const int tag = 'wght';
		const double w = t.weight;
		CFNumberRef k = CFNumberCreate(nullptr, kCFNumberIntType, &tag);
		CFNumberRef v = CFNumberCreate(nullptr, kCFNumberDoubleType, &w);
		CFDictionaryRef var = CFDictionaryCreate(nullptr, (const void**)&k, (const void**)&v, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
		CFStringRef key = kCTFontVariationAttribute;
		CFDictionaryRef attrs = CFDictionaryCreate(nullptr, (const void**)&key, (const void**)&var, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
		CTFontDescriptorRef d = CTFontDescriptorCreateCopyWithAttributes(base, attrs);
		f = CTFontCreateWithFontDescriptor(d, t.size, nullptr);
		CFRelease(d);
		CFRelease(attrs);
		CFRelease(var);
		CFRelease(v);
		CFRelease(k);
	}
	if (!f) f = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, t.size, nullptr);
	cache.push_back({&t, f});
	return f;
}

CGImageRef logo_image() {
	static CGImageRef img = [] {
		CGDataProviderRef p = CGDataProviderCreateWithData(nullptr, kLogoData, kLogoSize, nullptr);
		CGImageRef i = CGImageCreateWithPNGDataProvider(p, nullptr, true, kCGRenderingIntentDefault);
		CGDataProviderRelease(p);
		return i;
	}();
	return img;
}

class CGCanvas : public Canvas {
public:
	explicit CGCanvas(CGContextRef ctx) : c_(ctx) {
		CGContextSetLineCap(c_, kCGLineCapRound);
		CGContextSetLineJoin(c_, kCGLineJoinRound);
	}
	void fill(const Rect& r, uint32_t argb, float radius) override {
		CGColorRef col = cg(argb);
		CGContextSetFillColorWithColor(c_, col);
		CGPathRef p = CGPathCreateWithRoundedRect(CGRectMake(r.x, r.y, r.w, r.h), std::min(radius, r.w / 2), std::min(radius, r.h / 2), nullptr);
		CGContextAddPath(c_, p);
		CGContextFillPath(c_);
		CGPathRelease(p);
		CGColorRelease(col);
	}
	void stroke(const Rect& r, uint32_t argb, float width, float radius) override {
		CGColorRef col = cg(argb);
		CGContextSetStrokeColorWithColor(c_, col);
		CGContextSetLineWidth(c_, width);
		const CGRect rr = CGRectInset(CGRectMake(r.x, r.y, r.w, r.h), width / 2, width / 2);
		CGPathRef p = CGPathCreateWithRoundedRect(rr, std::min(radius, (float)rr.size.width / 2), std::min(radius, (float)rr.size.height / 2), nullptr);
		CGContextAddPath(c_, p);
		CGContextStrokePath(c_);
		CGPathRelease(p);
		CGColorRelease(col);
	}
	void line(float x0, float y0, float x1, float y1, uint32_t argb, float width) override {
		CGColorRef col = cg(argb);
		CGContextSetStrokeColorWithColor(c_, col);
		CGContextSetLineWidth(c_, width);
		CGContextMoveToPoint(c_, x0, y0);
		CGContextAddLineToPoint(c_, x1, y1);
		CGContextStrokePath(c_);
		CGColorRelease(col);
	}
	void arc(float cx, float cy, float r, float from, float sweep, uint32_t argb, float width) override {
		CGColorRef col = cg(argb);
		CGContextSetStrokeColorWithColor(c_, col);
		CGContextSetLineWidth(c_, width);
		// (a flipped view: clockwise on screen is increasing angle, CG's "clockwise" 0)
		CGContextAddArc(c_, cx, cy, r, from, from + sweep, 0);
		CGContextStrokePath(c_);
		CGColorRelease(col);
	}
	void disc(float cx, float cy, float r, uint32_t argb) override {
		CGColorRef col = cg(argb);
		CGContextSetFillColorWithColor(c_, col);
		CGContextFillEllipseInRect(c_, CGRectMake(cx - r, cy - r, 2 * r, 2 * r));
		CGColorRelease(col);
	}
	void polyline(const float* xy, int n, uint32_t argb, float width) override {
		if (n < 2) return;
		CGColorRef col = cg(argb);
		CGContextSetStrokeColorWithColor(c_, col);
		CGContextSetLineWidth(c_, width);
		CGContextMoveToPoint(c_, xy[0], xy[1]);
		for (int i = 1; i < n; i++) CGContextAddLineToPoint(c_, xy[2 * i], xy[2 * i + 1]);
		CGContextStrokePath(c_);
		CGColorRelease(col);
	}
	CFAttributedStringRef attributed(const std::string& s, const awva_tokens::Type& t, uint32_t argb) {
		CFStringRef str = CFStringCreateWithBytes(nullptr, (const UInt8*)s.data(), (CFIndex)s.size(), kCFStringEncodingUTF8, false);
		if (!str) return nullptr;
		CGColorRef col = cg(argb);
		const float kern = t.spacing;
		CFNumberRef k = CFNumberCreate(nullptr, kCFNumberFloatType, &kern);
		const void* keys[] = {kCTFontAttributeName, kCTForegroundColorAttributeName, kCTKernAttributeName};
		const void* vals[] = {font_for(t), col, k};
		CFDictionaryRef attrs = CFDictionaryCreate(nullptr, keys, vals, 3, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
		CFAttributedStringRef a = CFAttributedStringCreate(nullptr, str, attrs);
		CFRelease(attrs);
		CFRelease(k);
		CGColorRelease(col);
		CFRelease(str);
		return a;
	}
	void text(const std::string& s, const Rect& r, const awva_tokens::Type& t, uint32_t argb, Align al) override {
		if (s.empty()) return;
		CFAttributedStringRef a = attributed(s, t, argb);
		if (!a) return;
		CTLineRef line = CTLineCreateWithAttributedString(a);
		double w = CTLineGetTypographicBounds(line, nullptr, nullptr, nullptr);
		if (w > r.w) { // ellipsised
			CFAttributedStringRef dots = attributed("\xE2\x80\xA6", t, argb);
			CTLineRef tok = CTLineCreateWithAttributedString(dots);
			CTLineRef cut = CTLineCreateTruncatedLine(line, r.w, kCTLineTruncationEnd, tok);
			CFRelease(tok);
			CFRelease(dots);
			if (cut) {
				CFRelease(line);
				line = cut;
				w = CTLineGetTypographicBounds(line, nullptr, nullptr, nullptr);
			}
		}
		CTFontRef f = font_for(t);
		const double asc = CTFontGetAscent(f), desc = CTFontGetDescent(f);
		const double x = al == Align::Left ? r.x : (al == Align::Center ? r.x + (r.w - w) / 2 : r.x + r.w - w);
		const double baseline = r.y + r.h / 2 + (asc - desc) / 2;
		CGContextSaveGState(c_);
		CGContextSetTextMatrix(c_, CGAffineTransformMakeScale(1, -1)); // (the flipped view)
		CGContextSetTextPosition(c_, x, baseline);
		CTLineDraw(line, c_);
		CGContextRestoreGState(c_);
		CFRelease(line);
		CFRelease(a);
	}
	float text_width(const std::string& s, const awva_tokens::Type& t) override {
		CFAttributedStringRef a = attributed(s, t, 0xff000000u);
		if (!a) return 0;
		CTLineRef line = CTLineCreateWithAttributedString(a);
		const double w = CTLineGetTypographicBounds(line, nullptr, nullptr, nullptr);
		CFRelease(line);
		CFRelease(a);
		return (float)w;
	}
	void logo(const Rect& r) override {
		CGImageRef img = logo_image();
		if (!img) return;
		CGContextSaveGState(c_);
		CGContextTranslateCTM(c_, r.x, r.y + r.h); // (images draw upward: flip back for them)
		CGContextScaleCTM(c_, 1, -1);
		CGContextSetInterpolationQuality(c_, kCGInterpolationHigh);
		CGContextDrawImage(c_, CGRectMake(0, 0, r.w, r.h), img);
		CGContextRestoreGState(c_);
	}
	void push_clip(const Rect& r) override {
		CGContextSaveGState(c_);
		CGContextClipToRect(c_, CGRectMake(r.x, r.y, r.w, r.h));
	}
	void pop_clip() override { CGContextRestoreGState(c_); }

private:
	CGContextRef c_;
};

// the view's state, an associated object of the view (compiled with ARC)
struct State {
	Editor* ed = nullptr;
	NSTimer* timer = nil;
	bool dragging = false;
};
char kStateKey;

State* state_of(id self) {
	NSValue* v = objc_getAssociatedObject(self, &kStateKey);
	return v ? (State*)[v pointerValue] : nullptr;
}

NSPoint local(id self, NSEvent* e) { return [(NSView*)self convertPoint:[e locationInWindow] fromView:nil]; }

BOOL is_flipped(id, SEL) { return YES; }
BOOL accepts_first_mouse(id, SEL, NSEvent*) { return YES; }
void draw_rect(id self, SEL, NSRect) {
	State* s = state_of(self);
	if (!s || !s->ed) return;
	CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
	CGCanvas c(ctx);
	const NSRect b = [(NSView*)self bounds];
	s->ed->paint(c, (float)b.size.width, (float)b.size.height);
}
void mouse_down(id self, SEL, NSEvent* e) {
	State* s = state_of(self);
	if (!s) return;
	const NSPoint p = local(self, e);
	s->dragging = true;
	if (s->ed->mouse_down((float)p.x, (float)p.y, ([e modifierFlags] & NSEventModifierFlagShift) != 0, [e clickCount] == 2))
		[(NSView*)self setNeedsDisplay:YES];
}
void mouse_dragged(id self, SEL, NSEvent* e) {
	State* s = state_of(self);
	if (!s) return;
	const NSPoint p = local(self, e);
	if (s->ed->mouse_drag((float)p.x, (float)p.y, ([e modifierFlags] & NSEventModifierFlagShift) != 0)) [(NSView*)self setNeedsDisplay:YES];
}
void mouse_up(id self, SEL, NSEvent* e) {
	State* s = state_of(self);
	if (!s) return;
	const NSPoint p = local(self, e);
	s->dragging = false;
	if (s->ed->mouse_up((float)p.x, (float)p.y)) [(NSView*)self setNeedsDisplay:YES];
}
void mouse_moved(id self, SEL, NSEvent* e) {
	State* s = state_of(self);
	if (!s) return;
	const NSPoint p = local(self, e);
	if (s->ed->mouse_move((float)p.x, (float)p.y)) [(NSView*)self setNeedsDisplay:YES];
}
void scroll_wheel(id self, SEL, NSEvent* e) {
	State* s = state_of(self);
	if (!s) return;
	const NSPoint p = local(self, e);
	const float dy = [e hasPreciseScrollingDeltas] ? (float)[e scrollingDeltaY] / 40.0f : (float)[e scrollingDeltaY];
	if (dy != 0 && s->ed->wheel((float)p.x, (float)p.y, dy)) [(NSView*)self setNeedsDisplay:YES];
}
void update_tracking_areas(id self, SEL sel) {
	struct objc_super sup = {self, class_getSuperclass(object_getClass(self))};
	((void (*)(struct objc_super*, SEL))objc_msgSendSuper)(&sup, sel);
	NSView* v = (NSView*)self;
	for (NSTrackingArea* a in [[v trackingAreas] copy]) [v removeTrackingArea:a];
	[v addTrackingArea:[[NSTrackingArea alloc] initWithRect:[v bounds]
	                                                options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited | NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
	                                                  owner:v
	                                               userInfo:nil]];
}
void appearance_changed(id self, SEL) { [(NSView*)self setNeedsDisplay:YES]; }

// the view class: one per binary, named after it
Class view_class() {
	static Class cls = [] {
		Dl_info info;
		dladdr((const void*)&view_class, &info);
		char name[96];
		snprintf(name, sizeof name, "AwvaEditorView_%p", info.dli_fbase);
		Class c = objc_allocateClassPair([NSView class], name, 0);
		class_addMethod(c, @selector(isFlipped), (IMP)is_flipped, "c@:");
		class_addMethod(c, @selector(acceptsFirstMouse:), (IMP)accepts_first_mouse, "c@:@");
		class_addMethod(c, @selector(drawRect:), (IMP)draw_rect, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
		class_addMethod(c, @selector(mouseDown:), (IMP)mouse_down, "v@:@");
		class_addMethod(c, @selector(mouseDragged:), (IMP)mouse_dragged, "v@:@");
		class_addMethod(c, @selector(mouseUp:), (IMP)mouse_up, "v@:@");
		class_addMethod(c, @selector(mouseMoved:), (IMP)mouse_moved, "v@:@");
		class_addMethod(c, @selector(scrollWheel:), (IMP)scroll_wheel, "v@:@");
		class_addMethod(c, @selector(updateTrackingAreas), (IMP)update_tracking_areas, "v@:");
		class_addMethod(c, @selector(viewDidChangeEffectiveAppearance), (IMP)appearance_changed, "v@:");
		objc_registerClassPair(c);
		return c;
	}();
	return cls;
}

} // namespace

struct View {
	NSView* view = nil;
	State* st = nullptr;
};

const char* view_api() { return "cocoa"; }

View* view_create(Editor& editor, Host&) {
	auto* v = new View;
	v->st = new State;
	v->st->ed = &editor;
	NSView* nv = [[view_class() alloc] initWithFrame:NSMakeRect(0, 0, awva_tokens::kEditorWidth, awva_tokens::kEditorHeight)];
	objc_setAssociatedObject(nv, &kStateKey, [NSValue valueWithPointer:v->st], OBJC_ASSOCIATION_RETAIN_NONATOMIC);
	v->view = nv;
	State* st = v->st;
	NSView* weak = nv;
	st->timer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 30
	                                            repeats:YES
	                                              block:^(NSTimer*) {
		                                              if (st->ed && st->ed->tick()) [weak setNeedsDisplay:YES];
	                                              }];
	return v;
}

void view_destroy(View* v) {
	if (!v) return;
	[v->st->timer invalidate];
	v->st->timer = nil;
	v->st->ed = nullptr;
	objc_setAssociatedObject(v->view, &kStateKey, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
	[v->view removeFromSuperview];
	v->view = nil;
	delete v->st;
	delete v;
}

bool view_attach(View* v, void* parent) {
	NSView* p = (__bridge NSView*)parent;
	if (!p) return false;
	[p addSubview:v->view];
	return true;
}

void view_set_scale(View*, double) {}

void view_set_size(View* v, uint32_t w, uint32_t h) { [v->view setFrameSize:NSMakeSize(w, h)]; }

void view_show(View* v, bool show) { [v->view setHidden:!show]; }

void view_repaint(View* v) {
	if (v && v->view) [v->view setNeedsDisplay:YES];
}

bool view_os_dark() {
	NSAppearance* a = [NSApp effectiveAppearance];
	if (!a) a = [NSAppearance currentDrawingAppearance];
	NSAppearanceName n = [a bestMatchFromAppearancesWithNames:@[ NSAppearanceNameAqua, NSAppearanceNameDarkAqua ]];
	return [n isEqualToString:NSAppearanceNameDarkAqua];
}

bool render_png(Editor& editor, float w, float h, float scale, const std::string& path) {
	const size_t pw = (size_t)std::lround(w * scale), ph = (size_t)std::lround(h * scale);
	CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
	CGContextRef ctx = CGBitmapContextCreate(nullptr, pw, ph, 8, 0, cs, (CGBitmapInfo)kCGImageAlphaPremultipliedLast);
	if (!ctx) {
		CGColorSpaceRelease(cs);
		return false;
	}
	// (a flipped, scaled space, as the view's)
	CGContextTranslateCTM(ctx, 0, ph);
	CGContextScaleCTM(ctx, scale, -scale);
	{
		CGCanvas c(ctx);
		editor.paint(c, w, h);
	}
	CGImageRef img = CGBitmapContextCreateImage(ctx);
	CFStringRef ps = CFStringCreateWithCString(nullptr, path.c_str(), kCFStringEncodingUTF8);
	CFURLRef url = CFURLCreateWithFileSystemPath(nullptr, ps, kCFURLPOSIXPathStyle, false);
	CGImageDestinationRef dst = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, nullptr);
	bool ok = false;
	if (dst && img) {
		CGImageDestinationAddImage(dst, img, nullptr);
		ok = CGImageDestinationFinalize(dst);
	}
	if (dst) CFRelease(dst);
	CFRelease(url);
	CFRelease(ps);
	if (img) CGImageRelease(img);
	CGContextRelease(ctx);
	CGColorSpaceRelease(cs);
	return ok;
}

} // namespace awva_editor
#endif
