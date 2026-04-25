// Scintilla source code edit control
/** @file PlatAROS.cxx
 ** Implementation of platform facilities on AROS using MUI/Zune and zunegfx.
 **/
// Copyright 2026 The AROS Development Team
// The License.txt file describes the conditions under which this software may be distributed.

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <climits>

#include <stdexcept>
#include <new>
#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <set>
#include <optional>
#include <algorithm>
#include <memory>

// AROS headers already included by aros_compat.h (force-included).
// zunegfx headers need explicit include.
extern "C" {
#include <libraries/zunegfx.h>
#include <proto/zunegfx.h>
#define DEBUG 1
#include <aros/debug.h>
#undef Indent
#undef D
#define D(x...) x

struct Library *ZuneGfxBase = nullptr;
}

#include "aros_wrappers.h"

#include "ScintillaTypes.h"
#include "ScintillaMessages.h"
#include "ScintillaStructures.h"
#include "Debugging.h"
#include "Geometry.h"
#include "Platform.h"

#include "XPM.h"

using namespace Scintilla;
using namespace Scintilla::Internal;

//----------------------------------------------------------------------
// Shared Window pointer for measuring surfaces
//----------------------------------------------------------------------

// Shared Window pointer for measuring surfaces and popup positioning.
// TODO: For multiple editor instances, these need to be per-instance
// (e.g. via a context struct set before DoPaint/HandleInput).
static struct ::Window *sIntuiWindow = nullptr;

void PlatAROS_SetWindow(struct ::Window *win) {
	sIntuiWindow = win;
}

static Object *sEditorArea = nullptr;

void PlatAROS_SetEditorArea(Object *area) {
	sEditorArea = area;
}

//----------------------------------------------------------------------
// Helpers
//----------------------------------------------------------------------

static ULONG ToZuneColor(ColourRGBA c) {
	return ZUNE_COLOR_ARGB32(c.GetAlpha(), c.GetRed(), c.GetGreen(), c.GetBlue());
}

static struct ZuneRect ToZuneRect(PRectangle rc) {
	struct ZuneRect zr;
	zr.x = static_cast<WORD>(rc.left);
	zr.y = static_cast<WORD>(rc.top);
	zr.width = static_cast<UWORD>(rc.right - rc.left);
	zr.height = static_cast<UWORD>(rc.bottom - rc.top);
	return zr;
}

static struct ZunePoint ToZunePoint(Point pt) {
	struct ZunePoint zp;
	zp.x = static_cast<WORD>(pt.x);
	zp.y = static_cast<WORD>(pt.y);
	return zp;
}

//----------------------------------------------------------------------
// FontAROS
//----------------------------------------------------------------------

class FontAROS : public Font {
public:
	struct TextFont *tf = nullptr;
	struct TextAttr ta = {};
	std::string fontName;

	FontAROS(const FontParameters &fp) {
		fontName = fp.faceName ? fp.faceName : "topaz.font";

		// Ensure .font suffix
		if (fontName.find(".font") == std::string::npos) {
			fontName += ".font";
		}

		ta.ta_Name = const_cast<STRPTR>(fontName.c_str());
		ta.ta_YSize = static_cast<UWORD>(fp.size);
		ta.ta_Style = 0;
		ta.ta_Flags = 0;

		if (fp.italic) {
			ta.ta_Style |= FSF_ITALIC;
		}
		if (fp.weight >= FontWeight::Bold) {
			ta.ta_Style |= FSF_BOLD;
		}

		tf = OpenDiskFont(&ta);
		if (!tf) {
			// Fallback to topaz
			ta.ta_Name = (STRPTR)"topaz.font";
			ta.ta_YSize = 8;
			ta.ta_Style = 0;
			ta.ta_Flags = 0;
			tf = OpenFont(&ta);
		}
	}

	~FontAROS() noexcept override {
		if (tf) {
			CloseFont(tf);
			tf = nullptr;
		}
	}
};

std::shared_ptr<Font> Font::Allocate(const FontParameters &fp) {
	return std::make_shared<FontAROS>(fp);
}

//----------------------------------------------------------------------
// SurfaceAROS
//----------------------------------------------------------------------

class SurfaceAROS : public Surface {
	struct RenderContext *rctx = nullptr;
	struct DrawingBoard *board = nullptr;  // only used for offscreen pixmaps
	bool ownRctx = false;
	bool ownBoard = false;
	bool inited = false;
	int codePage = 0;
	SurfaceMode mode;

	// Clip stack
	std::vector<PRectangle> clipStack;

	static const FontAROS *GetFont(const Font *font_) {
		return dynamic_cast<const FontAROS *>(font_);
	}

	void SetFontOnContext(const Font *font_) {
		const FontAROS *fa = GetFont(font_);
		if (fa && fa->tf && rctx) {
			ZuneSetFont(rctx, fa->tf);
		}
	}

	// Ensure the render context targets the correct surface before drawing
	void ActivateTarget() {
		if (rctx) {
			ZuneSetTarget(rctx, board);  // board=NULL means RastPort
		}
	}

public:
	SurfaceAROS() noexcept = default;

	~SurfaceAROS() noexcept override {
		Release();
	}

	// Init for measuring only (no drawing)
	void Init(WindowID wid) override {
		Release();
		// wid is a MUI Object* (not an Intuition Window*), so use the
		// shared Window pointer set by ScintillaAROS::SetupRenderContext().
		struct ::Window *win = sIntuiWindow;
		if (win) {
			rctx = ZuneCreateRenderContextForWindow(win, win->WScreen->ViewPort.ColorMap, BACKEND_CYBERGFX);
			ownRctx = true;
		}
		inited = true;
	}

	// Init for drawing (given an existing surface)
	void Init(SurfaceID sid, WindowID wid) override {
		Release();
		rctx = static_cast<struct RenderContext *>(sid);
		ownRctx = false;
		// Pick up the current DrawingBoard target from the RenderContext.
		// DoPaint sets this before calling Paint().
		board = rctx ? rctx->target_board : nullptr;
		ownBoard = false;
		inited = true;
	}

	std::unique_ptr<Surface> AllocatePixMap(int width, int height) override {
		auto surf = std::make_unique<SurfaceAROS>();
		if (rctx) {
			struct DrawingBoard *db = ZuneCreateDrawingBoardForRenderContext(
				rctx, static_cast<UWORD>(width), static_cast<UWORD>(height),
				ZUNE_DRAWINGBOARD_ALPHA);
			if (db) {
				surf->rctx = rctx;
				surf->ownRctx = false;
				surf->board = db;
				surf->ownBoard = true;
				surf->inited = true;
			}
		}
		return surf;
	}

	void SetMode(SurfaceMode mode_) override {
		mode = mode_;
		codePage = mode_.codePage;
	}

	void Release() noexcept override {
		if (ownBoard && board && rctx) {
			ZuneDestroyDrawingBoard(rctx, board);
		}
		board = nullptr;
		ownBoard = false;

		if (ownRctx && rctx) {
			ZuneDestroyRenderContext(rctx);
		}
		rctx = nullptr;
		ownRctx = false;
		inited = false;
		clipStack.clear();
	}

	int SupportsFeature(Supports feature) noexcept override {
		switch (feature) {
		case Supports::LineDrawsFinal:
			return 1;
		case Supports::FractionalStrokeWidth:
			return 0;
		case Supports::TranslucentStroke:
			return 1;
		case Supports::PixelModification:
			return 1;
		case Supports::ThreadSafeMeasureWidths:
			return 0;
		default:
			return 0;
		}
	}

	bool Initialised() override {
		return inited;
	}

	int LogPixelsY() override {
		return 72; // AROS standard
	}

	int PixelDivisions() override {
		return 1;
	}

	int DeviceHeightFont(int points) override {
		return points;
	}

	//------------------------------------------------------------------
	// Drawing primitives
	//------------------------------------------------------------------

	void LineDraw(Point start, Point end, Stroke stroke) override {
		if (!rctx) return;
		ActivateTarget();
		struct ZunePoint zStart = ToZunePoint(start);
		struct ZunePoint zEnd = ToZunePoint(end);
		ZuneDrawLine(rctx, &zStart, &zEnd, ToZuneColor(stroke.colour));
	}

	void PolyLine(const Point *pts, size_t npts, Stroke stroke) override {
		if (!rctx || npts < 2) return;
		ActivateTarget();
		std::vector<struct ZunePoint> zpts(npts);
		for (size_t i = 0; i < npts; i++) {
			zpts[i] = ToZunePoint(pts[i]);
		}
		ZuneDrawPolyline(rctx, zpts.data(), static_cast<UWORD>(npts), ToZuneColor(stroke.colour));
	}

	void Polygon(const Point *pts, size_t npts, FillStroke fillStroke) override {
		if (!rctx || npts < 3) return;
		ActivateTarget();
		std::vector<struct ZunePoint> zpts(npts);
		for (size_t i = 0; i < npts; i++) {
			zpts[i] = ToZunePoint(pts[i]);
		}
		struct ZuneBrush brush = ZUNE_BRUSH_LITERAL_SOLID(ToZuneColor(fillStroke.fill.colour));
		ZuneFillPolygon(rctx, zpts.data(), static_cast<UWORD>(npts), &brush);
		ZuneDrawPolyline(rctx, zpts.data(), static_cast<UWORD>(npts), ToZuneColor(fillStroke.stroke.colour));
		// Close the polygon outline
		struct ZunePoint closing[2] = { zpts[npts - 1], zpts[0] };
		ZuneDrawLine(rctx, &closing[0], &closing[1], ToZuneColor(fillStroke.stroke.colour));
	}

	void RectangleDraw(PRectangle rc, FillStroke fillStroke) override {
		if (!rctx) return;
		ActivateTarget();
		struct ZuneRect zr = ToZuneRect(rc);
		ULONG fillColor = ToZuneColor(fillStroke.fill.colour);
		ULONG borderColor = ToZuneColor(fillStroke.stroke.colour);
		UBYTE borderWidth = static_cast<UBYTE>(std::max(1.0, fillStroke.stroke.width));
		struct ZuneBrush brush = ZUNE_BRUSH_LITERAL_SOLID(fillColor);
		ZuneDrawRectangleRoundedStyled(rctx, &zr, 0, borderWidth,
			&brush, borderColor);
	}

	void RectangleFrame(PRectangle rc, Stroke stroke) override {
		if (!rctx) return;
		ActivateTarget();
		struct ZuneRect zr = ToZuneRect(rc);
		UBYTE lineWidth = static_cast<UBYTE>(std::max(1.0, stroke.width));
		ZuneDrawRectangleOutlineStyled(rctx, &zr, lineWidth, ToZuneColor(stroke.colour));
	}

	void FillRectangle(PRectangle rc, Fill fill) override {
		if (!rctx) return;
		ActivateTarget();
		struct ZuneRect zr = ToZuneRect(rc);
		struct ZuneBrush brush = ZUNE_BRUSH_LITERAL_SOLID(ToZuneColor(fill.colour));
		ZuneFillRectangle(rctx, &zr, &brush);
	}

	void FillRectangleAligned(PRectangle rc, Fill fill) override {
		FillRectangle(PixelAlign(rc, 1), fill);
	}

	void FillRectangle(PRectangle rc, Surface &surfacePattern) override {
		// Pattern fill: just fill with a solid color as fallback
		FillRectangle(rc, ColourRGBA(0, 0, 0));
	}

	void RoundedRectangle(PRectangle rc, FillStroke fillStroke) override {
		if (!rctx) return;
		ActivateTarget();
		struct ZuneRect zr = ToZuneRect(rc);
		UBYTE radius = 4;
		UBYTE borderWidth = static_cast<UBYTE>(std::max(1.0, fillStroke.stroke.width));
		struct ZuneBrush brush = ZUNE_BRUSH_LITERAL_SOLID(ToZuneColor(fillStroke.fill.colour));
		ZuneDrawRectangleRoundedStyled(rctx, &zr, radius, borderWidth,
			&brush, ToZuneColor(fillStroke.stroke.colour));
	}

	void AlphaRectangle(PRectangle rc, XYPOSITION cornerSize, FillStroke fillStroke) override {
		if (!rctx) return;
		ActivateTarget();
		struct ZuneRect zr = ToZuneRect(rc);
		UBYTE radius = static_cast<UBYTE>(cornerSize);
		UBYTE borderWidth = static_cast<UBYTE>(std::max(1.0, fillStroke.stroke.width));
		struct ZuneBrush brush = ZUNE_BRUSH_LITERAL_SOLID(ToZuneColor(fillStroke.fill.colour));
		if (radius > 0) {
			ZuneFillRectangleRoundedStyledAA(rctx, &zr, radius, borderWidth,
				&brush, ToZuneColor(fillStroke.stroke.colour));
		} else {
			ZuneDrawRectangleRoundedStyled(rctx, &zr, 0, borderWidth,
				&brush, ToZuneColor(fillStroke.stroke.colour));
		}
	}

	void GradientRectangle(PRectangle rc, const std::vector<ColourStop> &stops, GradientOptions options) override {
		if (!rctx || stops.empty()) return;
		ActivateTarget();

		struct ZuneRect zr = ToZuneRect(rc);

		// Build gradient stops
		std::vector<struct ZuneGradientStop> zuneStops(stops.size());
		for (size_t i = 0; i < stops.size(); i++) {
			zuneStops[i].position = static_cast<float>(stops[i].position);
			zuneStops[i].color = ToZuneColor(stops[i].colour);
		}

		struct ZuneBrush brush = {};
		brush.type = ZUNE_BRUSH_TYPE_LINEAR_GRADIENT;
		brush.flags = 0;
		std::memset(&brush.internal, 0, sizeof(brush.internal));

		if (options == GradientOptions::leftToRight) {
			brush.data.linear.start = { zr.x, zr.y };
			brush.data.linear.end = { static_cast<WORD>(zr.x + zr.width), zr.y };
		} else {
			brush.data.linear.start = { zr.x, zr.y };
			brush.data.linear.end = { zr.x, static_cast<WORD>(zr.y + zr.height) };
		}
		brush.data.linear.stops = zuneStops.data();
		brush.data.linear.stop_count = static_cast<UWORD>(zuneStops.size());

		ZuneFillRectangle(rctx, &zr, &brush);
	}

	void DrawRGBAImage(PRectangle rc, int width, int height, const unsigned char *pixelsImage) override {
		if (!rctx || !pixelsImage) return;
		ActivateTarget();
		struct ZuneTexture *tex = ZuneCreateTextureFromData(
			rctx, const_cast<unsigned char *>(pixelsImage),
			static_cast<UWORD>(width), static_cast<UWORD>(height),
			32, ZUNE_TEXTURE_FORMAT_ARGB32,
			static_cast<ULONG>(width * 4), ZUNE_TEXTURE_ALPHA);
		if (tex) {
			struct ZuneRect dest = ToZuneRect(rc);
			ZuneDrawTextureScaled(rctx, tex, &dest);
			ZuneDestroyTexture(rctx, tex);
		}
	}

	void Ellipse(PRectangle rc, FillStroke fillStroke) override {
		if (!rctx) return;
		ActivateTarget();
		XYPOSITION cx = (rc.left + rc.right) / 2.0;
		XYPOSITION cy = (rc.top + rc.bottom) / 2.0;
		UWORD radius = static_cast<UWORD>(std::min(rc.Width(), rc.Height()) / 2.0);
		struct ZunePoint center = { static_cast<WORD>(cx), static_cast<WORD>(cy) };
		UBYTE borderWidth = static_cast<UBYTE>(std::max(1.0, fillStroke.stroke.width));
		struct ZuneBrush brush = ZUNE_BRUSH_LITERAL_SOLID(ToZuneColor(fillStroke.fill.colour));
		ZuneFillCircleStyledAA(rctx, &center, radius, borderWidth,
			&brush, ToZuneColor(fillStroke.stroke.colour));
	}

	void Stadium(PRectangle rc, FillStroke fillStroke, Ends ends) override {
		if (!rctx) return;
		ActivateTarget();
		struct ZuneRect zr = ToZuneRect(rc);
		UBYTE radius = static_cast<UBYTE>(std::min(rc.Width(), rc.Height()) / 2.0);
		UBYTE borderWidth = static_cast<UBYTE>(std::max(1.0, fillStroke.stroke.width));
		struct ZuneBrush brush = ZUNE_BRUSH_LITERAL_SOLID(ToZuneColor(fillStroke.fill.colour));
		ZuneFillRectangleRoundedStyledAA(rctx, &zr, radius, borderWidth,
			&brush, ToZuneColor(fillStroke.stroke.colour));
	}

	void Copy(PRectangle rc, Point from, Surface &surfaceSource) override {
		if (!rctx) return;
		SurfaceAROS *src = dynamic_cast<SurfaceAROS *>(&surfaceSource);
		if (src && src->rctx) {
			ZuneSetTarget(src->rctx, src->board);
			ZuneSetTarget(rctx, board);
			ZuneBlit(src->rctx, rctx,
				static_cast<WORD>(from.x), static_cast<WORD>(from.y),
				static_cast<WORD>(rc.left), static_cast<WORD>(rc.top),
				static_cast<UWORD>(rc.Width()), static_cast<UWORD>(rc.Height()));
		}
	}

	//------------------------------------------------------------------
	// Text
	//------------------------------------------------------------------

	void DrawTextNoClip(PRectangle rc, const Font *font_, XYPOSITION ybase,
			std::string_view text, ColourRGBA fore, ColourRGBA back) override {
		if (!rctx || text.empty()) return;
		ActivateTarget();
		SetFontOnContext(font_);
		XYPOSITION y = ybase - Ascent(font_);
		struct ZunePoint pos = { static_cast<WORD>(rc.left), static_cast<WORD>(y) };
		ZuneDrawTextBackground(rctx, &pos, text.data(), static_cast<UWORD>(text.length()),
			ToZuneColor(fore), ToZuneColor(back));
	}

	void DrawTextClipped(PRectangle rc, const Font *font_, XYPOSITION ybase,
			std::string_view text, ColourRGBA fore, ColourRGBA back) override {
		SetClip(rc);
		DrawTextNoClip(rc, font_, ybase, text, fore, back);
		PopClip();
	}

	void DrawTextTransparent(PRectangle rc, const Font *font_, XYPOSITION ybase,
			std::string_view text, ColourRGBA fore) override {
		if (!rctx || text.empty()) return;
		ActivateTarget();
		SetFontOnContext(font_);
		XYPOSITION y = ybase - Ascent(font_);
		struct ZunePoint pos = { static_cast<WORD>(rc.left), static_cast<WORD>(y) };
		ZuneDrawText(rctx, &pos, text.data(), static_cast<UWORD>(text.length()),
			ToZuneColor(fore));
	}

	void MeasureWidths(const Font *font_, std::string_view text, XYPOSITION *positions) override {
		if (!rctx || text.empty()) return;
		SetFontOnContext(font_);
		for (size_t i = 1; i <= text.length(); i++) {
			positions[i - 1] = static_cast<XYPOSITION>(
				ZuneTextLength(rctx, text.data(), static_cast<UWORD>(i)));
		}
	}

	XYPOSITION WidthText(const Font *font_, std::string_view text) override {
		if (!rctx || text.empty()) return 0;
		SetFontOnContext(font_);
		return static_cast<XYPOSITION>(
			ZuneTextLength(rctx, text.data(), static_cast<UWORD>(text.length())));
	}

	// UTF-8 versions - delegate to same implementation since AROS doesn't distinguish
	void DrawTextNoClipUTF8(PRectangle rc, const Font *font_, XYPOSITION ybase,
			std::string_view text, ColourRGBA fore, ColourRGBA back) override {
		DrawTextNoClip(rc, font_, ybase, text, fore, back);
	}

	void DrawTextClippedUTF8(PRectangle rc, const Font *font_, XYPOSITION ybase,
			std::string_view text, ColourRGBA fore, ColourRGBA back) override {
		DrawTextClipped(rc, font_, ybase, text, fore, back);
	}

	void DrawTextTransparentUTF8(PRectangle rc, const Font *font_, XYPOSITION ybase,
			std::string_view text, ColourRGBA fore) override {
		DrawTextTransparent(rc, font_, ybase, text, fore);
	}

	void MeasureWidthsUTF8(const Font *font_, std::string_view text, XYPOSITION *positions) override {
		MeasureWidths(font_, text, positions);
	}

	XYPOSITION WidthTextUTF8(const Font *font_, std::string_view text) override {
		return WidthText(font_, text);
	}

	//------------------------------------------------------------------
	// Font metrics
	//------------------------------------------------------------------

	XYPOSITION Ascent(const Font *font_) override {
		const FontAROS *fa = GetFont(font_);
		if (fa && fa->tf)
			return static_cast<XYPOSITION>(fa->tf->tf_Baseline);
		return 10;
	}

	XYPOSITION Descent(const Font *font_) override {
		const FontAROS *fa = GetFont(font_);
		if (fa && fa->tf)
			return static_cast<XYPOSITION>(fa->tf->tf_YSize - fa->tf->tf_Baseline);
		return 2;
	}

	XYPOSITION InternalLeading(const Font *font_) override {
		return 0;
	}

	XYPOSITION Height(const Font *font_) override {
		const FontAROS *fa = GetFont(font_);
		if (fa && fa->tf)
			return static_cast<XYPOSITION>(fa->tf->tf_YSize);
		return 12;
	}

	XYPOSITION AverageCharWidth(const Font *font_) override {
		const FontAROS *fa = GetFont(font_);
		if (fa && fa->tf)
			return static_cast<XYPOSITION>(fa->tf->tf_XSize);
		return 8;
	}

	//------------------------------------------------------------------
	// Layout (bidirectional text - stub)
	//------------------------------------------------------------------

	std::unique_ptr<IScreenLineLayout> Layout(const IScreenLine *screenLine) override {
		return {};
	}

	//------------------------------------------------------------------
	// Clipping
	//------------------------------------------------------------------

	void SetClip(PRectangle rc) override {
		clipStack.push_back(rc);
		if (!rctx) return;
		ActivateTarget();

		struct Region *region = NewRegion();
		if (region) {
			struct ::Rectangle rect;
			rect.MinX = static_cast<WORD>(rc.left);
			rect.MinY = static_cast<WORD>(rc.top);
			rect.MaxX = static_cast<WORD>(rc.right - 1);
			rect.MaxY = static_cast<WORD>(rc.bottom - 1);
			OrRectRegion(region, &rect);
			ZuneSetClipRegion(rctx, region);
			DisposeRegion(region);
		}
	}

	void PopClip() override {
		if (!clipStack.empty()) {
			clipStack.pop_back();
		}
		if (!rctx) return;

		if (clipStack.empty()) {
			ZuneClearClipRegion(rctx);
		} else {
			// Restore previous clip
			PRectangle rc = clipStack.back();
			struct Region *region = NewRegion();
			if (region) {
				struct ::Rectangle rect;
				rect.MinX = static_cast<WORD>(rc.left);
				rect.MinY = static_cast<WORD>(rc.top);
				rect.MaxX = static_cast<WORD>(rc.right - 1);
				rect.MaxY = static_cast<WORD>(rc.bottom - 1);
				OrRectRegion(region, &rect);
				ZuneSetClipRegion(rctx, region);
				DisposeRegion(region);
			}
		}
	}

	void FlushCachedState() override {
	}

	void FlushDrawing() override {
		if (rctx) {
			ZuneFlushBatch(rctx);
		}
	}

	// Accessor for ScintillaAROS
	struct RenderContext *GetRenderContext() const { return rctx; }
};

std::unique_ptr<Surface> Surface::Allocate(Technology technology) {
	return std::make_unique<SurfaceAROS>();
}

//----------------------------------------------------------------------
// Popup window helper
//----------------------------------------------------------------------

// Track popup Intuition windows so we can distinguish them from MUI objects
static std::set<void *> sPopupWindows;

static bool IsPopupWindow(void *wid) {
	return wid && sPopupWindows.count(wid) > 0;
}

// Forward declare for use in Window methods
class ListBoxAROS;
static ListBoxAROS *sActiveListBox = nullptr;
static Scintilla::Internal::Window *sActiveListBoxWindow = nullptr;
static void ListBoxHidePopupCall(ListBoxAROS *lb);

struct ::Window *CreatePopupWindow(struct Screen *scr, LONG x, LONG y, LONG w, LONG h) {
	struct ::Window *win = OpenWindowTags(nullptr,
		WA_Left, x,
		WA_Top, y,
		WA_InnerWidth, w,
		WA_InnerHeight, h,
		WA_Borderless, TRUE,
		WA_Activate, FALSE,
		WA_RMBTrap, TRUE,
		WA_CustomScreen, (IPTR)scr,
		WA_BackFill, (IPTR)LAYERS_NOBACKFILL,
		TAG_DONE);
	if (win) {
		sPopupWindows.insert(win);
	}
	return win;
}

// Calltip callbacks - set by ScintillaAROS::CreateCallTipWindow
static void (*sCallTipShowFunc)(void *) = nullptr;
static void (*sCallTipHideFunc)(void *) = nullptr;
static void (*sCallTipPaintFunc)(void *) = nullptr;
static void *sCallTipData = nullptr;
static Scintilla::Internal::Window *sCallTipWindow = nullptr;

void DestroyPopupWindow(struct ::Window *win) {
	if (win) {
		sPopupWindows.erase(win);
		CloseWindow(win);
	}
}

//----------------------------------------------------------------------
// Window
//----------------------------------------------------------------------

Scintilla::Internal::Window::~Window() noexcept {
}

void Scintilla::Internal::Window::Destroy() noexcept {
	if (sActiveListBox && this == sActiveListBoxWindow) {
		// ListBox popup: don't close the window yet, just let HidePopup handle it
		ListBoxHidePopupCall(sActiveListBox);
	} else if (IsPopupWindow(wid)) {
		// Other popups (calltip): close immediately
		DestroyPopupWindow(static_cast<struct ::Window *>(wid));
	}
	wid = nullptr;
}

PRectangle Scintilla::Internal::Window::GetPosition() const {
	if (!wid) return PRectangle();
	// wid is an Object* (MUI object) — get dimensions from MUI attrs
	Object *obj = static_cast<Object *>(wid);
	LONG left = 0, top = 0, width = 0, height = 0;
	MUIGet(obj, MUIA_LeftEdge, &left);
	MUIGet(obj, MUIA_TopEdge, &top);
	MUIGet(obj, MUIA_Width, &width);
	MUIGet(obj, MUIA_Height, &height);
	return PRectangle(left, top, left + width, top + height);
}

void Scintilla::Internal::Window::SetPosition(PRectangle rc) {
	if (!IsPopupWindow(wid)) return;
	struct ::Window *win = static_cast<struct ::Window *>(wid);
	ChangeWindowBox(win,
		static_cast<LONG>(rc.left), static_cast<LONG>(rc.top),
		static_cast<LONG>(rc.Width()), static_cast<LONG>(rc.Height()));
}

// Stored desired position for popups not yet created
PRectangle sPendingPopupPos;

// Trampoline functions - implemented after ListBoxAROS definition
static void ListBoxShowPopup(ListBoxAROS *lb);
static void ListBoxHidePopup(ListBoxAROS *lb);

void Scintilla::Internal::Window::SetPositionRelative(PRectangle rc, const Scintilla::Internal::Window *relativeTo) {
	// Calculate absolute screen position: editor area position within MUI window + window position on screen
	LONG parentX = 0, parentY = 0;
	if (sEditorArea) {
		MUIGet(sEditorArea, MUIA_LeftEdge, &parentX);
		MUIGet(sEditorArea, MUIA_TopEdge, &parentY);
	}
	if (sIntuiWindow) {
		parentX += sIntuiWindow->LeftEdge;
		parentY += sIntuiWindow->TopEdge;
	}

	LONG x = parentX + static_cast<LONG>(rc.left);
	LONG y = parentY + static_cast<LONG>(rc.top);
	LONG w = static_cast<LONG>(rc.Width());
	LONG h = static_cast<LONG>(rc.Height());

	// Clamp to screen
	if (sIntuiWindow) {
		struct Screen *scr = sIntuiWindow->WScreen;
		if (x + w > scr->Width) x = scr->Width - w;
		if (y + h > scr->Height) y = scr->Height - h;
		if (x < 0) x = 0;
		if (y < 0) y = 0;
	}

	sPendingPopupPos = PRectangle(x, y, x + w, y + h);

	if (IsPopupWindow(wid)) {
		struct ::Window *win = static_cast<struct ::Window *>(wid);
		ChangeWindowBox(win, x, y, w, h);
	}
}

PRectangle Scintilla::Internal::Window::GetClientPosition() const {
	// Use the editor area dimensions, not the whole group
	Object *obj = sEditorArea ? sEditorArea : static_cast<Object *>(wid);
	if (!obj) return PRectangle();
	LONG width = 0, height = 0;
	MUIGet(obj, MUIA_Width, &width);
	MUIGet(obj, MUIA_Height, &height);
	return PRectangle(0, 0, width, height);
}

void Scintilla::Internal::Window::Show(bool show) {
	// ListBox popup: create/destroy on demand
	if (sActiveListBox && this == sActiveListBoxWindow) {
		if (show) {
			ListBoxShowPopup(sActiveListBox);
		} else {
			ListBoxHidePopup(sActiveListBox);
		}
		return;
	}

	// Calltip popup: create on show, destroy on hide
	if (sCallTipWindow == this) {
		if (show && sCallTipShowFunc) {
			sCallTipShowFunc(sCallTipData);
		} else if (!show && sCallTipHideFunc) {
			sCallTipHideFunc(sCallTipData);
		}
		return;
	}

	if (!wid) return;
	if (IsPopupWindow(wid)) {
		if (show) {
			WindowToFront(static_cast<struct ::Window *>(wid));

		} else {
			DestroyPopupWindow(static_cast<struct ::Window *>(wid));
			wid = nullptr;
		}
	} else {
		Object *obj = static_cast<Object *>(wid);
		MUISet(obj, MUIA_ShowMe, show ? TRUE : FALSE);
	}
}

void PlatAROS_SetCallTipWindow(Scintilla::Internal::Window *w) {
	sCallTipWindow = w;
}

void PlatAROS_SetCallTipCallbacks(void (*showFunc)(void *), void (*hideFunc)(void *),
		void (*paintFunc)(void *), void *data) {
	sCallTipShowFunc = showFunc;
	sCallTipHideFunc = hideFunc;
	sCallTipPaintFunc = paintFunc;
	sCallTipData = data;
}

// Dirty rectangle tracking for partial updates
static PRectangle sDirtyRect;
static bool sDirtyAll = false;

void PlatAROS_GetDirtyRect(PRectangle *rc, bool *all) {
	*rc = sDirtyRect;
	*all = sDirtyAll;
}

void PlatAROS_ClearDirtyRect() {
	sDirtyRect = PRectangle();
	sDirtyAll = false;
}

void Scintilla::Internal::Window::InvalidateAll() {
	if (sCallTipWindow == this && sCallTipPaintFunc) {
		sCallTipPaintFunc(sCallTipData);
		return;
	}
	sDirtyAll = true;
}

void Scintilla::Internal::Window::InvalidateRectangle(PRectangle rc) {
	if (sDirtyAll) return;
	if (sDirtyRect.Empty()) {
		sDirtyRect = rc;
	} else {
		// Union with existing dirty rect
		if (rc.left < sDirtyRect.left) sDirtyRect.left = rc.left;
		if (rc.top < sDirtyRect.top) sDirtyRect.top = rc.top;
		if (rc.right > sDirtyRect.right) sDirtyRect.right = rc.right;
		if (rc.bottom > sDirtyRect.bottom) sDirtyRect.bottom = rc.bottom;
	}
}

// I-beam pointer for text cursor
static Object *sIBeamPointer = nullptr;

static Object *GetIBeamPointer() {
	if (sIBeamPointer) return sIBeamPointer;

	// Create a 16x16 2-color bitmap for the I-beam shape
	struct BitMap *bm = AllocBitMap(16, 16, 2, BMF_CLEAR, nullptr);
	if (!bm) return nullptr;

	// Draw I-beam shape into plane 1 (color index 1 = foreground)
	// Shape: top/bottom serifs + vertical bar
	UBYTE *plane = reinterpret_cast<UBYTE *>(bm->Planes[1]);
	UWORD stride = bm->BytesPerRow;
	//  ..xx.xx.  = 0x36
	//  ....x...  = 0x08
	auto setRow = [&](int y, UBYTE val) { plane[y * stride] = val; };
	setRow(2,  0x36);  // serifs top
	setRow(3,  0x08);
	setRow(4,  0x08);
	setRow(5,  0x08);
	setRow(6,  0x08);
	setRow(7,  0x08);
	setRow(8,  0x08);
	setRow(9,  0x08);
	setRow(10, 0x08);
	setRow(11, 0x08);
	setRow(12, 0x08);
	setRow(13, 0x36);  // serifs bottom

	sIBeamPointer = reinterpret_cast<Object *>(
		NewObject(nullptr, (UBYTE *)"pointerclass",
			POINTERA_BitMap, (IPTR)bm,
			POINTERA_XOffset, -7,
			POINTERA_YOffset, -8,
			POINTERA_WordWidth, 1,
			POINTERA_XResolution, POINTERXRESN_SCREENRES,
			POINTERA_YResolution, POINTERYRESN_SCREENRES,
			TAG_DONE));

	if (!sIBeamPointer) {
		FreeBitMap(bm);
	}
	return sIBeamPointer;
}

void Scintilla::Internal::Window::SetCursor(Cursor curs) {
	cursorLast = curs;
	if (!sIntuiWindow) return;

	struct TagItem tags[3] = {};
	switch (curs) {
	case Cursor::text: {
		Object *ibeam = GetIBeamPointer();
		if (ibeam) {
			tags[0] = { WA_Pointer, (IPTR)ibeam };
		} else {
			tags[0] = { WA_Pointer, (IPTR)nullptr };
		}
		tags[1] = { TAG_DONE, 0 };
		break;
	}
	case Cursor::wait:
		tags[0] = { WA_BusyPointer, TRUE };
		tags[1] = { TAG_DONE, 0 };
		break;
	default:
		tags[0] = { WA_Pointer, (IPTR)nullptr };
		tags[1] = { TAG_DONE, 0 };
		break;
	}
	IntuiSetWindowPointer(sIntuiWindow, tags);
}

PRectangle Scintilla::Internal::Window::GetMonitorRect(Point pt) {
	// Return screen dimensions
	struct Screen *scr = IntuitionBase->ActiveScreen;
	if (scr) {
		return PRectangle(0, 0, scr->Width, scr->Height);
	}
	return PRectangle(0, 0, 1024, 768);
}

//----------------------------------------------------------------------
// ListBox
//----------------------------------------------------------------------

class ListBoxAROS : public ListBox {
	int visibleRows = 5;
	int averageCharWidth = 8;
	int lineHeight = 12;
	std::vector<std::string> entries;
	std::vector<int> entryTypes;
	int selection = -1;
	int scrollOffset = 0;  // first visible item index
	IListBoxDelegate *delegate = nullptr;

	struct ::Window *popupWin = nullptr;
	struct RenderContext *popupRctx = nullptr;
	struct Screen *parentScreen = nullptr;
	struct TextFont *listFont = nullptr;

	// Registered images for autocomplete icons
	RGBAImageSet images;

	// Colors from SetOptions
	ULONG colFore = ZUNE_BLACK;
	ULONG colBack = ZUNE_WHITE;
	ULONG colForeSelected = ZUNE_WHITE;
	ULONG colBackSelected = ZUNE_COLOR_ARGB32(255, 50, 100, 200);
	float imageScale = 1.0f;

	void PaintList() {
		if (!popupWin || !popupRctx) return;

		WORD w = popupWin->Width;
		WORD h = popupWin->Height;

		ZuneSetTarget(popupRctx, nullptr);
		if (listFont) {
			ZuneSetFont(popupRctx, listFont);
		} else {
			// Fallback: use the popup window's RastPort font
			ZuneSetFont(popupRctx, popupWin->RPort->Font);
		}

		WORD borderWidth = 2;

		struct ZuneRect rcAll = { borderWidth, borderWidth, static_cast<UWORD>(w-borderWidth*2), static_cast<UWORD>(h-borderWidth*2) };
		struct ZuneBrush bgBrush = ZUNE_BRUSH_LITERAL_SOLID(colBack);
		ZuneFillRectangleRoundedStyledAA(popupRctx, &rcAll, 3, borderWidth,
			&bgBrush, ZUNE_COLOR_ARGB32(255, 60, 60, 60));

		// Calculate icon column width
		int iconW = 0;
		if (images.GetHeight() > 0) {
			iconW = static_cast<int>(lineHeight * imageScale) + 2;
		}

		WORD pad = borderWidth + 2;
		int y = pad;
		for (int i = scrollOffset; i < static_cast<int>(entries.size()) && y + lineHeight <= h - pad; i++) {
			bool isSel = (i == selection);
			ULONG fg = isSel ? colForeSelected : colFore;
			ULONG bg = isSel ? colBackSelected : colBack;

			// Draw icon if available for this entry type
			int textX = pad + iconW;
			if (iconW > 0 && entryTypes[i] >= 0) {
				RGBAImage *img = images.Get(entryTypes[i]);
				if (img) {
					int drawH = static_cast<int>(lineHeight * imageScale);
					int drawW = img->GetWidth() * drawH / img->GetHeight();
					// RGBAImage stores RGBA, zunegfx wants ARGB - convert on the fly
					int pixCount = img->GetWidth() * img->GetHeight();
					std::vector<unsigned char> argb(pixCount * 4);
					const unsigned char *rgba = img->Pixels();
					for (int p = 0; p < pixCount; p++) {
						argb[p * 4 + 0] = rgba[p * 4 + 3]; // A
						argb[p * 4 + 1] = rgba[p * 4 + 0]; // R
						argb[p * 4 + 2] = rgba[p * 4 + 1]; // G
						argb[p * 4 + 3] = rgba[p * 4 + 2]; // B
					}
					struct ZuneTexture *tex = ZuneCreateTextureFromData(
						popupRctx, argb.data(),
						static_cast<UWORD>(img->GetWidth()),
						static_cast<UWORD>(img->GetHeight()),
						32, ZUNE_TEXTURE_FORMAT_ARGB32,
						static_cast<ULONG>(img->GetWidth() * 4),
						ZUNE_TEXTURE_ALPHA);
					if (tex) {
						struct ZuneRect dest = {
							static_cast<WORD>(pad), static_cast<WORD>(y),
							static_cast<UWORD>(drawW), static_cast<UWORD>(drawH) };
						ZuneDrawTextureScaled(popupRctx, tex, &dest);
						ZuneDestroyTexture(popupRctx, tex);
					}
				}
			}

			struct ZunePoint pos = { static_cast<WORD>(textX), static_cast<WORD>(y) };
			ZuneDrawTextBackground(popupRctx, &pos, entries[i].c_str(),
				static_cast<UWORD>(entries[i].length()), fg, bg);
			y += lineHeight;
		}
	}

	void DestroyPopup() {
		if (popupRctx) {
			ZuneDestroyRenderContext(popupRctx);
			popupRctx = nullptr;
		}
		if (popupWin) {
			DestroyPopupWindow(popupWin);
			popupWin = nullptr;
		}
		// Don't clear wid here - let Window::Destroy() do that,
		// so AutoComplete::Cancel() sees Created()==true and sets active=false
	}

public:
	ListBoxAROS() noexcept = default;
	~ListBoxAROS() noexcept override {
		DestroyPopup();
	}

	void ShowPopup() {
		if (popupWin) {
			PaintList();
			return;
		}
		if (!parentScreen) return;

		LONG x = static_cast<LONG>(sPendingPopupPos.left);
		LONG y = static_cast<LONG>(sPendingPopupPos.top);
		LONG w = static_cast<LONG>(sPendingPopupPos.Width());
		LONG h = static_cast<LONG>(sPendingPopupPos.Height());
		if (w < 10) w = 100;
		if (h < 10) h = 50;

		popupWin = CreatePopupWindow(parentScreen, x, y, w, h);
		if (popupWin) {
			popupRctx = ZuneCreateRenderContextForWindow(popupWin,
				parentScreen->ViewPort.ColorMap, BACKEND_CYBERGFX);
			wid = popupWin;
			PaintList();
		}
	}

	void HidePopup() {
		if (sActiveListBox == this) {
			sActiveListBox = nullptr;
			sActiveListBoxWindow = nullptr;
		}
		DestroyPopup();
	}

	void SetFont(const Font *font) override {
		const FontAROS *fa = dynamic_cast<const FontAROS *>(font);
		if (fa && fa->tf) {
			lineHeight = fa->tf->tf_YSize;
			averageCharWidth = fa->tf->tf_XSize;
			listFont = fa->tf;
		}
	}

	void Create(Window &parent, int ctrlID, Point location, int lineHeight_,
			bool unicodeMode_, Technology technology_) override {
		lineHeight = lineHeight_;
		DestroyPopup();
		parentScreen = sIntuiWindow ? sIntuiWindow->WScreen : nullptr;
		sActiveListBox = this;
		sActiveListBoxWindow = this;  // Window* from implicit upcast
	}

	void SetAverageCharWidth(int width) override {
		averageCharWidth = width;
	}

	void SetVisibleRows(int rows) override {
		visibleRows = rows;
	}

	int GetVisibleRows() const override {
		return visibleRows;
	}

	PRectangle GetDesiredRect() override {
		int maxWidth = 100;
		for (const auto &e : entries) {
			int w = static_cast<int>(e.length()) * averageCharWidth;
			if (w > maxWidth) maxWidth = w;
		}
		int rows = std::min(visibleRows, static_cast<int>(entries.size()));
		if (rows < 1) rows = 1;
		int pad = 4;  // borderWidth + 2
		return PRectangle(0, 0, maxWidth + 20 + pad * 2,
			(rows + 2) * lineHeight + pad * 2);
	}

	int CaretFromEdge() override {
		return 4;
	}

	void Clear() noexcept override {
		entries.clear();
		entryTypes.clear();
		selection = -1;
		scrollOffset = 0;
	}

	void Append(char *s, int type) override {
		entries.emplace_back(s);
		entryTypes.push_back(type);
	}

	int Length() override {
		return static_cast<int>(entries.size());
	}

	void Select(int n) override {
		selection = n;
		if (popupWin && lineHeight > 0) {
			int pad = 4;  // borderWidth + 2
			int visibleItems = (popupWin->Height - pad * 2) / lineHeight;
			if (visibleItems < 1) visibleItems = 1;
			if (selection < scrollOffset) {
				scrollOffset = selection;
			} else if (selection >= scrollOffset + visibleItems) {
				scrollOffset = selection - visibleItems + 1;
			}
			PaintList();
		}
	}

	int GetSelection() override {
		return selection;
	}


	int Find(const char *prefix) override {
		std::string_view pfx(prefix);
		for (size_t i = 0; i < entries.size(); i++) {
			if (entries[i].compare(0, pfx.length(), pfx) == 0) {
				return static_cast<int>(i);
			}
		}
		return -1;
	}

	std::string GetValue(int n) override {
		if (n >= 0 && n < static_cast<int>(entries.size())) {
			return entries[n];
		}
		return {};
	}

	void RegisterImage(int type, const char *xpm_data) override {
		if (!xpm_data) return;
		XPM xpm(xpm_data);
		images.AddImage(type, std::make_unique<RGBAImage>(xpm));
	}

	void RegisterRGBAImage(int type, int width, int height, const unsigned char *pixelsImage) override {
		if (!pixelsImage || width <= 0 || height <= 0) return;
		images.AddImage(type, std::make_unique<RGBAImage>(width, height, 1.0f, pixelsImage));
	}

	void ClearRegisteredImages() override {
		images.Clear();
	}

	void SetDelegate(IListBoxDelegate *lbDelegate) override {
		delegate = lbDelegate;
	}

	void SetList(const char *list, char separator, char typesep) override {
		Clear();
		if (!list) return;
		const char *p = list;
		while (*p) {
			const char *end = p;
			while (*end && *end != separator) end++;
			std::string item(p, end);
			int type = -1;
			size_t tspos = item.find(typesep);
			if (tspos != std::string::npos) {
				type = std::atoi(item.c_str() + tspos + 1);
				item.resize(tspos);
			}
			Append(const_cast<char *>(item.c_str()), type);
			if (*end) end++;
			p = end;
		}
	}

	void SetOptions(ListOptions options_) override {
		if (options_.fore)
			colFore = ToZuneColor(*options_.fore);
		if (options_.back)
			colBack = ToZuneColor(*options_.back);
		if (options_.foreSelected)
			colForeSelected = ToZuneColor(*options_.foreSelected);
		if (options_.backSelected)
			colBackSelected = ToZuneColor(*options_.backSelected);
		imageScale = options_.imageScale;
	}
};

static void ListBoxShowPopup(ListBoxAROS *lb) { lb->ShowPopup(); }
static void ListBoxHidePopup(ListBoxAROS *lb) { lb->HidePopup(); }
static void ListBoxHidePopupCall(ListBoxAROS *lb) { lb->HidePopup(); }

ListBox::ListBox() noexcept = default;
ListBox::~ListBox() noexcept = default;

std::unique_ptr<ListBox> ListBox::Allocate() {
	return std::make_unique<ListBoxAROS>();
}

//----------------------------------------------------------------------
// Menu
//----------------------------------------------------------------------

// Context menu data
struct MenuItemData {
	std::string label;
	int cmd;
	bool enabled;
};
static std::vector<MenuItemData> sMenuItems;
struct ::Window *sMenuWin = nullptr;
struct RenderContext *sMenuRctx = nullptr;
static int sMenuSelection = -1;
static int sMenuLineHeight = 12;

static void DestroyMenuPopup() {
	if (sMenuRctx) {
		ZuneDestroyRenderContext(sMenuRctx);
		sMenuRctx = nullptr;
	}
	if (sMenuWin) {
		DestroyPopupWindow(sMenuWin);
		sMenuWin = nullptr;
	}
	sMenuItems.clear();
	sMenuSelection = -1;
}

static void PaintMenuPopup() {
	if (!sMenuWin || !sMenuRctx) return;
	WORD w = sMenuWin->Width;
	WORD h = sMenuWin->Height;

	ZuneSetTarget(sMenuRctx, nullptr);
	if (sMenuWin->RPort->Font)
		ZuneSetFont(sMenuRctx, sMenuWin->RPort->Font);

	struct ZuneRect rcAll = { 0, 0, static_cast<UWORD>(w), static_cast<UWORD>(h) };
	struct ZuneBrush bgBrush = ZUNE_BRUSH_LITERAL_SOLID(ZUNE_COLOR_ARGB32(255, 240, 240, 240));
	ZuneFillRectangleRoundedStyledAA(sMenuRctx, &rcAll, 4, 1,
		&bgBrush, ZUNE_COLOR_ARGB32(255, 160, 160, 160));

	WORD pad = 4;
	int y = pad;
	for (int i = 0; i < static_cast<int>(sMenuItems.size()); i++) {
		const auto &item = sMenuItems[i];
		if (item.label.empty()) {
			// Separator
			struct ZunePoint p1 = { pad, static_cast<WORD>(y + 3) };
			struct ZunePoint p2 = { static_cast<WORD>(w - pad), static_cast<WORD>(y + 3) };
			ZuneDrawLine(sMenuRctx, &p1, &p2, ZUNE_COLOR_ARGB32(255, 180, 180, 180));
			y += 7;
		} else {
			struct ZunePoint pos = { static_cast<WORD>(pad + 2), static_cast<WORD>(y) };
			ULONG fg = item.enabled ? ZUNE_BLACK : ZUNE_COLOR_ARGB32(255, 160, 160, 160);
			ULONG bg = (i == sMenuSelection) ?
				ZUNE_COLOR_ARGB32(255, 50, 100, 200) :
				ZUNE_COLOR_ARGB32(255, 240, 240, 240);
			if (i == sMenuSelection) fg = ZUNE_WHITE;
			ZuneDrawTextBackground(sMenuRctx, &pos, item.label.c_str(),
				static_cast<UWORD>(item.label.length()), fg, bg);
			y += sMenuLineHeight;
		}
	}
}

// ScintillaBase::Command callback - set by ScintillaAROS
static void (*sMenuCommandFunc)(void *, int) = nullptr;
static void *sMenuCommandData = nullptr;

void PlatAROS_SetMenuCommand(void (*func)(void *, int), void *data) {
	sMenuCommandFunc = func;
	sMenuCommandData = data;
}

void PlatAROS_HandleMenuClick(int screenX, int screenY,
		void (*commandFunc)(void *, int), void *commandData) {
	if (!sMenuWin) return;

	// Check if click is inside menu window
	int mx = screenX - sMenuWin->LeftEdge;
	int my = screenY - sMenuWin->TopEdge;

	if (mx < 0 || my < 0 || mx >= sMenuWin->Width || my >= sMenuWin->Height) {
		// Clicked outside - close menu
		DestroyMenuPopup();
		return;
	}

	// Find which item was clicked
	WORD pad = 4;
	int y = pad;
	for (int i = 0; i < static_cast<int>(sMenuItems.size()); i++) {
		const auto &item = sMenuItems[i];
		int itemH = item.label.empty() ? 7 : sMenuLineHeight;
		if (my >= y && my < y + itemH && !item.label.empty() && item.enabled) {
			int cmd = item.cmd;
			DestroyMenuPopup();
			if (commandFunc) commandFunc(commandData, cmd);
			return;
		}
		y += itemH;
	}
	DestroyMenuPopup();
}

void PlatAROS_AddMenuItem(const char *label, int cmd, bool enabled) {
	sMenuItems.push_back({ label ? label : "", cmd, enabled });
}

Scintilla::Internal::Menu::Menu() noexcept : mid(nullptr) {
}

void Scintilla::Internal::Menu::CreatePopUp() {
	DestroyMenuPopup();
	mid = reinterpret_cast<MenuID>(1);  // non-null to indicate active
}

void Scintilla::Internal::Menu::Destroy() noexcept {
	DestroyMenuPopup();
	mid = nullptr;
}

void Scintilla::Internal::Menu::Show(Point pt, const Scintilla::Internal::Window &w) {
	if (sMenuItems.empty() || !sIntuiWindow) return;

	// Calculate menu size
	int maxWidth = 80;
	int totalHeight = 8;  // padding
	sMenuLineHeight = sIntuiWindow->RPort->Font ? sIntuiWindow->RPort->Font->tf_YSize : 12;
	for (const auto &item : sMenuItems) {
		if (item.label.empty()) {
			totalHeight += 7;  // separator
		} else {
			int itemW = static_cast<int>(item.label.length()) * 8 + 16;
			if (itemW > maxWidth) maxWidth = itemW;
			totalHeight += sMenuLineHeight;
		}
	}

	// Position: editor area in MUI window + window position on screen
	LONG parentX = 0, parentY = 0;
	if (sEditorArea) {
		MUIGet(sEditorArea, MUIA_LeftEdge, &parentX);
		MUIGet(sEditorArea, MUIA_TopEdge, &parentY);
	}
	if (sIntuiWindow) {
		parentX += sIntuiWindow->LeftEdge;
		parentY += sIntuiWindow->TopEdge;
	}
	LONG x = parentX + static_cast<LONG>(pt.x);
	LONG y = parentY + static_cast<LONG>(pt.y);

	// Clamp to screen
	struct Screen *scr = sIntuiWindow->WScreen;
	if (x + maxWidth > scr->Width) x = scr->Width - maxWidth;
	if (y + totalHeight > scr->Height) y = scr->Height - totalHeight;

	sMenuWin = CreatePopupWindow(scr, x, y, maxWidth, totalHeight);
	if (!sMenuWin) return;

	sMenuRctx = ZuneCreateRenderContextForWindow(sMenuWin,
		scr->ViewPort.ColorMap, BACKEND_CYBERGFX);
	if (!sMenuRctx) {
		DestroyPopupWindow(sMenuWin);
		sMenuWin = nullptr;
		return;
	}

	PaintMenuPopup();
}

//----------------------------------------------------------------------
// Platform namespace
//----------------------------------------------------------------------

namespace Scintilla::Internal::Platform {

ColourRGBA Chrome() {
	return ColourRGBA(0xc0, 0xc0, 0xc0);
}

ColourRGBA ChromeHighlight() {
	return ColourRGBA(0xff, 0xff, 0xff);
}

const char *DefaultFont() {
	return "topaz.font";
}

int DefaultFontSize() {
	return 8;
}

unsigned int DoubleClickTime() {
	return 500;
}

#ifdef DEBUG
void DebugPrintf(const char *format, ...) noexcept {
	char buffer[2000];
	va_list pArguments;
	va_start(pArguments, format);
	vsnprintf(buffer, sizeof(buffer), format, pArguments);
	va_end(pArguments);
	fprintf(stderr, "%s", buffer);
}
#else
void DebugPrintf(const char *, ...) noexcept {
}
#endif

void Assert(const char *c, const char *file, int line) noexcept {
	fprintf(stderr, "SCI ASSERT: [%s] at %s:%d\n", c, file, line);
	// Don't abort — log and continue so we can see what happens next
}

}
