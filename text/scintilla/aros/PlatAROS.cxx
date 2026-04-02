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
		// For measuring we need a RenderContext. If wid is an Intuition Window*, use it.
		struct ::Window *win = static_cast<struct ::Window *>(wid);
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
		// Offscreen pixmaps need a DrawingBoard so Copy() can blit to the window.
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
		D(bug("SCI: FillRect (%d,%d %dx%d) color=%08x board=%p target=%p\n",
			zr.x, zr.y, zr.width, zr.height,
			(unsigned)ToZuneColor(fill.colour),
			(void*)board, (void*)rctx->target_board));
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
			// Set source target (DrawingBoard or RastPort)
			ZuneSetTarget(src->rctx, src->board);
			// Set our target (DrawingBoard or RastPort)
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
		D(bug("SCI: DrawText '%.*s' at (%d,%d) board=%p\n",
			(int)std::min(text.length(), (size_t)20), text.data(),
			(int)rc.left, (int)(ybase - Ascent(font_)),
			(void*)board));
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
// Window
//----------------------------------------------------------------------

Scintilla::Internal::Window::~Window() noexcept {
}

void Scintilla::Internal::Window::Destroy() noexcept {
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
	// MUI handles layout — not directly positionable
}

void Scintilla::Internal::Window::SetPositionRelative(PRectangle rc, const Scintilla::Internal::Window *relativeTo) {
	// MUI handles layout
}

PRectangle Scintilla::Internal::Window::GetClientPosition() const {
	if (!wid) return PRectangle();
	Object *obj = static_cast<Object *>(wid);
	LONG width = 0, height = 0;
	MUIGet(obj, MUIA_Width, &width);
	MUIGet(obj, MUIA_Height, &height);
	return PRectangle(0, 0, width, height);
}

void Scintilla::Internal::Window::Show(bool show) {
	if (wid) {
		Object *obj = static_cast<Object *>(wid);
		MUISet(obj, MUIA_ShowMe, show ? TRUE : FALSE);
	}
}

void Scintilla::Internal::Window::InvalidateAll() {
	if (wid) {
		Object *obj = static_cast<Object *>(wid);
		MUI_Redraw(obj, MADF_DRAWOBJECT);
	}
}

void Scintilla::Internal::Window::InvalidateRectangle(PRectangle rc) {
	InvalidateAll();
}

void Scintilla::Internal::Window::SetCursor(Cursor curs) {
	// TODO: Set Intuition pointer based on cursor type
	cursorLast = curs;
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
	IListBoxDelegate *delegate = nullptr;

public:
	ListBoxAROS() noexcept = default;
	~ListBoxAROS() noexcept override = default;

	void SetFont(const Font *font) override {
		const FontAROS *fa = dynamic_cast<const FontAROS *>(font);
		if (fa && fa->tf) {
			lineHeight = fa->tf->tf_YSize;
			averageCharWidth = fa->tf->tf_XSize;
		}
	}

	void Create(Window &parent, int ctrlID, Point location, int lineHeight_,
			bool unicodeMode_, Technology technology_) override {
		lineHeight = lineHeight_;
		// TODO: Create MUI List popup window
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
		return PRectangle(0, 0, maxWidth + 20, rows * lineHeight + 4);
	}

	int CaretFromEdge() override {
		return 4;
	}

	void Clear() noexcept override {
		entries.clear();
		entryTypes.clear();
		selection = -1;
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
		// TODO
	}

	void RegisterRGBAImage(int type, int width, int height, const unsigned char *pixelsImage) override {
		// TODO
	}

	void ClearRegisteredImages() override {
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
		// TODO: Apply colors
	}
};

ListBox::ListBox() noexcept = default;
ListBox::~ListBox() noexcept = default;

std::unique_ptr<ListBox> ListBox::Allocate() {
	return std::make_unique<ListBoxAROS>();
}

//----------------------------------------------------------------------
// Menu
//----------------------------------------------------------------------

Scintilla::Internal::Menu::Menu() noexcept : mid(nullptr) {
}

void Scintilla::Internal::Menu::CreatePopUp() {
	// TODO: Create Intuition/MUI popup menu
	Destroy();
}

void Scintilla::Internal::Menu::Destroy() noexcept {
	mid = nullptr;
}

void Scintilla::Internal::Menu::Show(Point pt, const Scintilla::Internal::Window &w) {
	// TODO: Show popup menu at coordinates
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
