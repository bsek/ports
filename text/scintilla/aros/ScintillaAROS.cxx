// Scintilla source code edit control
/** @file ScintillaAROS.cxx
 ** AROS MUI/Zune widget implementation of Scintilla.
 **/
// Copyright 2026 The AROS Development Team
// The License.txt file describes the conditions under which this software may be distributed.

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <climits>
#include <cassert>

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
#include <chrono>
#include <mutex>

// AROS headers already included by aros_compat.h (force-included).
extern "C" {
#include <devices/rawkeycodes.h>
#include <libraries/zunegfx.h>
#include <proto/zunegfx.h>

#define DEBUG 1
#include <aros/debug.h>
#undef Indent
#undef D
#define D(x...) x
}

#include "aros_wrappers.h"

// INST_DATA returns void* — need cast in C++
#define INST_DATA_CPP(cl, obj) static_cast<struct ScintillaMUIData *>(INST_DATA(cl, obj))

#include "ScintillaTypes.h"
#include "ScintillaMessages.h"
#include "ScintillaStructures.h"
#include "Scintilla.h"
#include "Debugging.h"
#include "Geometry.h"
#include "Platform.h"
#include "ILoader.h"
#include "ILexer.h"

#include "CharacterCategoryMap.h"
#include "Position.h"
#include "UniqueString.h"
#include "SplitVector.h"
#include "Partitioning.h"
#include "RunStyles.h"
#include "ContractionState.h"
#include "CellBuffer.h"
#include "CallTip.h"
#include "KeyMap.h"
#include "Indicator.h"
#include "LineMarker.h"
#include "Style.h"
#include "ViewStyle.h"
#include "CharClassify.h"
#include "Decoration.h"
#include "CaseFolder.h"
#include "Document.h"
#include "CaseConvert.h"
#include "UniConversion.h"
#include "Selection.h"
#include "PositionCache.h"
#include "EditModel.h"
#include "MarginView.h"
#include "EditView.h"
#include "Editor.h"
#include "AutoComplete.h"
#include "ScintillaBase.h"

#include "ScintillaAROS.h"

using namespace Scintilla;
using namespace Scintilla::Internal;

//----------------------------------------------------------------------
// RAWKEY → Scintilla key mapping
//----------------------------------------------------------------------

static Keys SciMapRawKey(UWORD code) {
	switch (code) {
	case RAWKEY_UP:       return Keys::Up;
	case RAWKEY_DOWN:     return Keys::Down;
	case RAWKEY_LEFT:     return Keys::Left;
	case RAWKEY_RIGHT:    return Keys::Right;
	case RAWKEY_HOME:     return Keys::Home;
	case RAWKEY_END:      return Keys::End;
	case RAWKEY_PAGEUP:   return Keys::Prior;
	case RAWKEY_PAGEDOWN: return Keys::Next;
	case RAWKEY_DELETE:   return Keys::Delete;
	case RAWKEY_INSERT:   return Keys::Insert;
	case RAWKEY_BACKSPACE: return Keys::Back;
	case RAWKEY_TAB:      return Keys::Tab;
	case RAWKEY_RETURN:   return Keys::Return;
	case RAWKEY_ESCAPE:   return Keys::Escape;
	default:              return static_cast<Keys>(0);
	}
}

static KeyMod MapQualifiers(UWORD qualifier) {
	KeyMod mod = KeyMod::Norm;
	if (qualifier & IEQUALIFIER_LSHIFT || qualifier & IEQUALIFIER_RSHIFT)
		mod = mod | KeyMod::Shift;
	if (qualifier & IEQUALIFIER_CONTROL)
		mod = mod | KeyMod::Ctrl;
	if (qualifier & IEQUALIFIER_LALT || qualifier & IEQUALIFIER_RALT)
		mod = mod | KeyMod::Alt;
	if (qualifier & IEQUALIFIER_LCOMMAND || qualifier & IEQUALIFIER_RCOMMAND)
		mod = mod | KeyMod::Super;
	return mod;
}

//----------------------------------------------------------------------
// ScintillaAROS class
//----------------------------------------------------------------------

class ScintillaAROS : public ScintillaBase {
	struct RenderContext *rctx = nullptr;
	struct DrawingBoard *board = nullptr;
	WORD boardW = 0, boardH = 0;
	Object *muiObj = nullptr;
	struct ::Window *intuiWin = nullptr;

	bool capturedMouse = false;
	bool hasIdler = false;

	// Timer state
	bool tickTimers[5] = {};
	int tickMillis[5] = {};    // requested interval per timer
	int tickAccum[5] = {};     // accumulated ms since last fire

	// Clipboard
	std::string clipboardContent;

	// Scrollbar objects (set from MUI data after creation)
	Object *scrollV = nullptr;
	Object *scrollH = nullptr;
	bool updatingScrollBars = false;  // prevent recursion

	// Notification callback
	ScintillaNotifyFunc notifyFunc = nullptr;
	void *notifyUserData = nullptr;

public:
	ScintillaAROS() {
	}

	~ScintillaAROS() override {
		Finalise();
	}

	void SetMUIObject(Object *obj) {
		muiObj = obj;
	}

	struct RenderContext *GetRenderContext() const { return rctx; }
	struct DrawingBoard *GetBoard() const { return board; }

	Object *GetMUIObject() const {
		return muiObj;
	}

	void SetScrollBars(Object *v, Object *h) {
		scrollV = v;
		scrollH = h;
	}

	void SetFocus(bool focus) {
		SetFocusState(focus);
	}

	void SetNotifyCallback(ScintillaNotifyFunc func, void *userData) {
		notifyFunc = func;
		notifyUserData = userData;
	}

	void ExecuteCommand(int cmd) {
		Command(cmd);
	}

	void HandleVScroll() {
		if (updatingScrollBars || !scrollV) return;
		LONG val = 0;
		MUIGet(scrollV, MUIA_Prop_First, &val);
		ScrollTo(static_cast<Sci::Line>(val), false);
	}

	void HandleHScroll() {
		if (updatingScrollBars || !scrollH) return;
		LONG val = 0;
		MUIGet(scrollH, MUIA_Prop_First, &val);
		HorizontalScrollTo(static_cast<int>(val));
	}

	//------------------------------------------------------------------
	// Required virtual methods from Editor
	//------------------------------------------------------------------

	void Initialise() override {
		wMain = muiObj;
		// Disable Scintilla's internal double-buffering.
		// We already have a DrawingBoard as backbuffer, and pixmap Copy()
		// doesn't work with a shared RenderContext.
		view.bufferedDraw = false;
	}

	void Finalise() override {
		// Close calltip popup if open
		DestroyCallTipWindow();
		ScintillaBase::Finalise();
		ReleaseRenderContext();
	}

	void DestroyCallTipWindow() {
		HideCallTip();
		extern void PlatAROS_SetCallTipCallbacks(void (*)(void *), void (*)(void *), void (*)(void *), void *);
		extern void PlatAROS_SetCallTipWindow(Scintilla::Internal::Window *);
		PlatAROS_SetCallTipCallbacks(nullptr, nullptr, nullptr, nullptr);
		PlatAROS_SetCallTipWindow(nullptr);
	}

	void SetVerticalScrollPos() override {
		if (!scrollV || updatingScrollBars) return;
		updatingScrollBars = true;
		MUISet(scrollV, MUIA_Prop_First, static_cast<LONG>(topLine));
		updatingScrollBars = false;
	}

	void SetHorizontalScrollPos() override {
		if (!scrollH || updatingScrollBars) return;
		updatingScrollBars = true;
		MUISet(scrollH, MUIA_Prop_First, static_cast<LONG>(xOffset));
		updatingScrollBars = false;
	}

	bool ModifyScrollBars(Sci::Line nMax, Sci::Line nPage) override {
		bool modified = false;
		updatingScrollBars = true;

		if (scrollV) {
			Sci::Line maxPos = nMax + 1;
			if (maxPos < 1) maxPos = 1;
			SetAttrs(scrollV,
				MUIA_Prop_Entries, static_cast<LONG>(maxPos),
				MUIA_Prop_Visible, static_cast<LONG>(nPage),
				MUIA_Prop_First, static_cast<LONG>(topLine),
				TAG_DONE);
			modified = true;
		}

		if (scrollH) {
			PRectangle rcText = GetTextRectangle();
			int pageWidth = static_cast<int>(rcText.Width());
			int horizEnd = scrollWidth;
			if (horizEnd < 1) horizEnd = 1;
			SetAttrs(scrollH,
				MUIA_Prop_Entries, static_cast<LONG>(horizEnd),
				MUIA_Prop_Visible, static_cast<LONG>(pageWidth),
				MUIA_Prop_First, static_cast<LONG>(xOffset),
				TAG_DONE);
			modified = true;
		}

		updatingScrollBars = false;
		return modified;
	}

	void Copy() override {
		if (!sel.Empty()) {
			SelectionText selectedText;
			CopySelectionRange(&selectedText);
			CopyToClipboard(selectedText);
		}
	}

	void Paste() override {
		// Read from AROS clipboard
		std::string text = ReadClipboard();
		if (!text.empty()) {
			InsertPasteShape(text.c_str(), static_cast<Sci::Position>(text.length()),
				PasteShape::stream);
		}
	}

	void ClaimSelection() override {
		// AROS doesn't have X11-style primary selection
	}

	void NotifyChange() override {
		// Handled by NotifyParent which is called for all notifications
	}

	void NotifyParent(NotificationData scn) override {
		if (notifyFunc) {
			notifyFunc(notifyUserData,
				static_cast<intptr_t>(scn.nmhdr.code),
				scn.position,
				scn.ch);
		}
		if (muiObj) {
			MUISet(muiObj, MUIA_Scintilla_Notification,
				static_cast<IPTR>(scn.nmhdr.code));
		}
	}

	void CopyToClipboard(const SelectionText &selectedText) override {
		WriteClipboard(selectedText.Data(), selectedText.Length());
	}

	void SetMouseCapture(bool on) override {
		capturedMouse = on;
	}

	bool HaveMouseCapture() override {
		return capturedMouse;
	}

	sptr_t DefWndProc(Scintilla::Message iMessage, uptr_t wParam, sptr_t lParam) override {
		return 0;
	}

	void StartDrag() override {
		// Internal drag-and-drop: Scintilla handles the rest in
		// ButtonMoveWithModifiers (SetDragPosition) and
		// ButtonUpWithModifiers (move/copy text).
		inDragDrop = DragDrop::dragging;
		dropWentOutside = false;
	}

	struct ::Window *calltipWin = nullptr;
	struct RenderContext *calltipRctx = nullptr;

	void PaintCallTip() {
		if (!calltipWin || !calltipRctx) return;

		ZuneSetTarget(calltipRctx, nullptr);

		std::unique_ptr<Surface> surf = Surface::Allocate(Technology::Default);
		surf->Init(static_cast<SurfaceID>(calltipRctx), nullptr);
		surf->SetMode(SurfaceMode(ct.codePage, false));
		ct.PaintCT(surf.get());
		surf->Release();
	}

	void ShowCallTip() {
		if (calltipWin) return;
		if (!intuiWin) return;

		extern PRectangle sPendingPopupPos;
		LONG x = static_cast<LONG>(sPendingPopupPos.left);
		LONG y = static_cast<LONG>(sPendingPopupPos.top);
		LONG w = static_cast<LONG>(sPendingPopupPos.Width());
		LONG h = static_cast<LONG>(sPendingPopupPos.Height());
		if (w < 10) w = 100;
		if (h < 10) h = 20;

		extern struct ::Window *CreatePopupWindow(struct Screen *, LONG, LONG, LONG, LONG);
		calltipWin = CreatePopupWindow(intuiWin->WScreen, x, y, w, h);
		if (calltipWin) {
			calltipRctx = ZuneCreateRenderContextForWindow(calltipWin,
				intuiWin->WScreen->ViewPort.ColorMap, BACKEND_CYBERGFX);
			ct.wCallTip = calltipWin;
			ct.wDraw = calltipWin;
			PaintCallTip();
		}
	}

	void HideCallTip() {
		ct.wCallTip = nullptr;
		ct.wDraw = nullptr;
		if (calltipRctx) {
			ZuneDestroyRenderContext(calltipRctx);
			calltipRctx = nullptr;
		}
		if (calltipWin) {
			extern void DestroyPopupWindow(struct ::Window *);
			DestroyPopupWindow(calltipWin);
			calltipWin = nullptr;
		}
	}

	static void CallTipShowCallback(void *data) {
		static_cast<ScintillaAROS *>(data)->ShowCallTip();
	}

	static void CallTipHideCallback(void *data) {
		static_cast<ScintillaAROS *>(data)->HideCallTip();
	}

	static void CallTipPaintCallback(void *data) {
		static_cast<ScintillaAROS *>(data)->PaintCallTip();
	}

	void CreateCallTipWindow(PRectangle rc) override {
		extern void PlatAROS_SetCallTipCallbacks(void (*)(void *), void (*)(void *), void (*)(void *), void *);
		extern void PlatAROS_SetCallTipWindow(Scintilla::Internal::Window *);
		PlatAROS_SetCallTipCallbacks(CallTipShowCallback, CallTipHideCallback, CallTipPaintCallback, this);
		PlatAROS_SetCallTipWindow(&ct.wCallTip);
	}

	void AddToPopUp(const char *label, int cmd = 0, bool enabled = true) override {
		extern void PlatAROS_AddMenuItem(const char *, int, bool);
		PlatAROS_AddMenuItem(label, cmd, enabled);
	}

	bool SetIdle(bool on) override {
		hasIdler = on;
		return true;
	}

	bool FineTickerRunning(TickReason reason) override {
		return tickTimers[static_cast<int>(reason)];
	}

	void FineTickerStart(TickReason reason, int millis, int tolerance) override {
		int idx = static_cast<int>(reason);
		tickTimers[idx] = true;
		tickMillis[idx] = millis;
		tickAccum[idx] = 0;
	}

	void FineTickerCancel(TickReason reason) override {
		int idx = static_cast<int>(reason);
		tickTimers[idx] = false;
		tickAccum[idx] = 0;
	}

	std::string UTF8FromEncoded(std::string_view encoded) const override {
		// For now, assume input is already UTF-8 or ASCII
		return std::string(encoded);
	}

	std::string EncodedFromUTF8(std::string_view utf8) const override {
		return std::string(utf8);
	}

	//------------------------------------------------------------------
	// Rendering
	//------------------------------------------------------------------

	void SetupRenderContext(struct ::Window *win, Object *editorArea) {
		if (rctx) return;
		intuiWin = win;
		rctx = ZuneCreateRenderContextForWindow(win, win->WScreen->ViewPort.ColorMap, BACKEND_CYBERGFX);
		extern void PlatAROS_SetWindow(struct ::Window *);
		PlatAROS_SetWindow(win);
		extern void PlatAROS_SetEditorArea(Object *);
		PlatAROS_SetEditorArea(editorArea);
	}

	void ReleaseRenderContext() {
		if (board && rctx) {
			ZuneDestroyDrawingBoard(rctx, board);
			board = nullptr;
		}
		boardW = boardH = 0;
		if (rctx) {
			ZuneDestroyRenderContext(rctx);
			rctx = nullptr;
		}
		intuiWin = nullptr;
		extern void PlatAROS_SetWindow(struct ::Window *);
		PlatAROS_SetWindow(nullptr);
		extern void PlatAROS_SetEditorArea(Object *);
		PlatAROS_SetEditorArea(nullptr);
	}

	void EnsureBoard(WORD width, WORD height) {
		if (board && boardW == width && boardH == height)
			return;
		if (board) {
			ZuneDestroyDrawingBoard(rctx, board);
			board = nullptr;
		}
		board = ZuneCreateDrawingBoardForRenderContext(rctx,
			static_cast<UWORD>(width), static_cast<UWORD>(height), 0);
		boardW = width;
		boardH = height;
		if (board) {
			ZuneSetTarget(rctx, board);
		}
	}

	void DoPaint(WORD left, WORD top, WORD width, WORD height, bool
 fullRedraw) {
		if (!rctx) return;

		EnsureBoard(width, height);
		if (!board) return;

		// Determine paint area
		extern void PlatAROS_GetDirtyRect(PRectangle *, bool *);
		extern void PlatAROS_ClearDirtyRect();
		PRectangle dirtyRect;
		bool dirtyAll = false;
		PlatAROS_GetDirtyRect(&dirtyRect, &dirtyAll);
		PlatAROS_ClearDirtyRect();

		PRectangle rcPaint;
		if (fullRedraw || dirtyAll || dirtyRect.Empty()) {
			rcPaint = PRectangle(0, 0, width, height);
		} else {
			// Clamp dirty rect to widget area
			rcPaint = PRectangle(
				std::max(dirtyRect.left, 0.0),
				std::max(dirtyRect.top, 0.0),
				std::min(dirtyRect.right, static_cast<XYPOSITION>(width)),
				std::min(dirtyRect.bottom, static_cast<XYPOSITION>(height)));
		}

		ZuneSetTarget(rctx, board);

		std::unique_ptr<Surface> surfaceWindow = Surface::Allocate(Technology::Default);
		surfaceWindow->Init(static_cast<SurfaceID>(rctx), nullptr);

		Paint(surfaceWindow.get(), rcPaint);
		surfaceWindow->Release();

		// Present only the painted area
		WORD px = static_cast<WORD>(rcPaint.left);
		WORD py = static_cast<WORD>(rcPaint.top);
		UWORD pw = static_cast<UWORD>(rcPaint.Width());
		UWORD ph = static_cast<UWORD>(rcPaint.Height());

		ZuneSetTarget(rctx, board);
		ZunePresent(rctx, px, py,
			static_cast<WORD>(left + px), static_cast<WORD>(top + py),
			pw, ph);
	}

	//------------------------------------------------------------------
	// Input handling
	//------------------------------------------------------------------

	void HandleKeyDown(UWORD rawkey, UWORD qualifier, const char *chars, int charLen) {
		Keys sciKey = SciMapRawKey(rawkey);
		KeyMod mod = MapQualifiers(qualifier);

		if (sciKey != static_cast<Keys>(0)) {
			bool consumed = false;
			KeyDownWithModifiers(sciKey, mod, &consumed);
		} else if (charLen > 0 && static_cast<unsigned char>(chars[0]) >= 0x20) {
			std::string_view sv(chars, charLen);
			InsertCharacter(sv, CharacterSource::DirectInput);
		}
	}

	void HandleMouseDown(WORD x, WORD y, unsigned int time, UWORD qualifier) {
		if (!hasFocus) {
			SetFocusState(true);
		}
		KeyMod mod = MapQualifiers(qualifier);
		Point pt(static_cast<XYPOSITION>(x), static_cast<XYPOSITION>(y));
		ButtonDownWithModifiers(pt, time, mod);
	}

	void HandleMouseUp(WORD x, WORD y, unsigned int time, UWORD qualifier) {
		KeyMod mod = MapQualifiers(qualifier);
		Point pt(static_cast<XYPOSITION>(x), static_cast<XYPOSITION>(y));
		ButtonUpWithModifiers(pt, time, mod);
	}

	void HandleMouseMove(WORD x, WORD y, unsigned int time, UWORD qualifier) {
		KeyMod mod = MapQualifiers(qualifier);
		Point pt(static_cast<XYPOSITION>(x), static_cast<XYPOSITION>(y));
		ButtonMoveWithModifiers(pt, time, mod);
	}

	void HandleRightClick(WORD x, WORD y, unsigned int time, UWORD qualifier) {
		KeyMod mod = MapQualifiers(qualifier);
		Point pt(static_cast<XYPOSITION>(x), static_cast<XYPOSITION>(y));
		RightButtonDownWithModifiers(pt, time, mod);
	}

	// Returns true if any timer fired (needs redraw)
	bool HandleTick() {
		// INTUITICKS is ~100ms per tick
		const int tickIntervalMs = 100;
		bool fired = false;

		for (int i = 0; i < 5; i++) {
			if (tickTimers[i]) {
				tickAccum[i] += tickIntervalMs;
				if (tickAccum[i] >= tickMillis[i]) {
					tickAccum[i] = 0;
					TickFor(static_cast<TickReason>(i));
					fired = true;
				}
			}
		}
		if (hasIdler) {
			Idle();
			fired = true;
		}
		return fired;
	}

	//------------------------------------------------------------------
	// Public API (SciFnDirect)
	//------------------------------------------------------------------

	sptr_t SendMessage(unsigned int iMessage, uptr_t wParam, sptr_t lParam) {
		return WndProc(static_cast<Scintilla::Message>(iMessage), wParam, lParam);
	}

	SciFnDirect GetDirectFunction() {
		return [](sptr_t ptr, unsigned int msg, uptr_t wParam, sptr_t lParam) -> sptr_t {
			ScintillaAROS *sci = reinterpret_cast<ScintillaAROS *>(ptr);
			return sci->SendMessage(msg, wParam, lParam);
		};
	}

	sptr_t GetDirectPointer() {
		return reinterpret_cast<sptr_t>(this);
	}

private:
	//------------------------------------------------------------------
	// Clipboard helpers
	//------------------------------------------------------------------

	void WriteClipboard(const char *text, size_t len) {
		struct IFFHandle *iff = AllocIFF();
		if (!iff) return;

		struct ClipboardHandle *ch;
		if ((ch = OpenClipboard(0))) {
			iff->iff_Stream = (IPTR)ch;
			InitIFFasClip(iff);

			if (OpenIFF(iff, IFFF_WRITE) == 0) {
				if (PushChunk(iff, MAKE_ID('F','T','X','T'), MAKE_ID('F','O','R','M'), IFFSIZE_UNKNOWN) == 0) {
					if (PushChunk(iff, 0, MAKE_ID('C','H','R','S'), IFFSIZE_UNKNOWN) == 0) {
						WriteChunkBytes(iff, const_cast<char *>(text), static_cast<LONG>(len));
						PopChunk(iff);
					}
					PopChunk(iff);
				}
				CloseIFF(iff);
			}
			CloseClipboard(ch);
		}
		FreeIFF(iff);
	}

	std::string ReadClipboard() {
		std::string result;
		struct IFFHandle *iff = AllocIFF();
		if (!iff) return result;

		struct ClipboardHandle *ch;
		if ((ch = OpenClipboard(0))) {
			iff->iff_Stream = (IPTR)ch;
			InitIFFasClip(iff);

			if (OpenIFF(iff, IFFF_READ) == 0) {
				StopChunk(iff, MAKE_ID('F','T','X','T'), MAKE_ID('C','H','R','S'));
				if (ParseIFF(iff, IFFPARSE_SCAN) == 0) {
					struct ContextNode *cn = CurrentChunk(iff);
					if (cn) {
						LONG size = cn->cn_Size;
						result.resize(size);
						ReadChunkBytes(iff, result.data(), size);
					}
				}
				CloseIFF(iff);
			}
			CloseClipboard(ch);
		}
		FreeIFF(iff);
		return result;
	}
};

//----------------------------------------------------------------------
// MUI Custom Class
//----------------------------------------------------------------------

struct ScintillaMUIData {
	ScintillaAROS *sci;
	Object *editorArea;   // The inner Area that we paint on (_mleft etc.)
	Object *scrollV;      // Vertical scrollbar
	Object *scrollH;      // Horizontal scrollbar
	IPTR lastNotification; // Last SCN_ code for MUIM_Notify
};

// MUIA_Scintilla_DirectPtr defined in ScintillaAROS.h
#define MUIM_Scintilla_SendMessage  (MUIA_UserData + 2)
#define MUIM_Scintilla_VScroll      (MUIA_UserData + 3)
#define MUIM_Scintilla_HScroll      (MUIA_UserData + 4)

static IPTR mNew(struct IClass *cl, Object *obj, struct opSet *msg) {
	// Create scrollbar objects before passing to superclass (Group)
	Object *scrollV = reinterpret_cast<Object *>(MUI_NewObject(MUIC_Scrollbar,
		MUIA_Scrollbar_Type, MUIV_Scrollbar_Type_Default,
		MUIA_Group_Horiz, FALSE,
		TAG_DONE));

	Object *scrollH = reinterpret_cast<Object *>(MUI_NewObject(MUIC_Scrollbar,
		MUIA_Scrollbar_Type, MUIV_Scrollbar_Type_Default,
		MUIA_Group_Horiz, TRUE,
		TAG_DONE));

	// Build a group: [editor | vscroll] / [hscroll]
	struct TagItem extraTags[] = {
		{ MUIA_Group_Columns, 2 },
		{ MUIA_Group_Spacing, 0 },
		{ TAG_MORE, reinterpret_cast<IPTR>(msg->ops_AttrList) },
	};
	struct TagItem *origTags = msg->ops_AttrList;
	msg->ops_AttrList = extraTags;

	obj = reinterpret_cast<Object *>(DoSuperMethodA(cl, obj, reinterpret_cast<Msg>(msg)));
	msg->ops_AttrList = origTags;
	if (!obj) return 0;

	// Create the inner editor area (a plain Area for painting)
	Object *editorArea = reinterpret_cast<Object *>(MUI_NewObject(MUIC_Rectangle,
		MUIA_Frame, MUIV_Frame_None,
		TAG_DONE));

	// Add children to the group: editor(col1) vscroll(col2) hscroll(col1+2 span? no, just col1)
	// Actually with 2 columns: row1=[editor, vscroll], row2=[hscroll, empty]
	DoMethod(obj, OM_ADDMEMBER, reinterpret_cast<IPTR>(editorArea));
	DoMethod(obj, OM_ADDMEMBER, reinterpret_cast<IPTR>(scrollV));
	DoMethod(obj, OM_ADDMEMBER, reinterpret_cast<IPTR>(scrollH));
	// Add empty spacer in bottom-right corner
	Object *spacer = reinterpret_cast<Object *>(MUI_NewObject(MUIC_Rectangle,
		MUIA_FixWidth, 0,
		MUIA_FixHeight, 0,
		MUIA_Weight, 0,
		TAG_DONE));
	DoMethod(obj, OM_ADDMEMBER, reinterpret_cast<IPTR>(spacer));

	struct ScintillaMUIData *data = INST_DATA_CPP(cl, obj);
	data->sci = new ScintillaAROS();
	data->sci->SetMUIObject(obj);
	data->sci->SetScrollBars(scrollV, scrollH);
	data->editorArea = editorArea;
	data->scrollV = scrollV;
	data->scrollH = scrollH;

	return reinterpret_cast<IPTR>(obj);
}

static IPTR mDispose(struct IClass *cl, Object *obj, Msg msg) {
	struct ScintillaMUIData *data = INST_DATA_CPP(cl, obj);
	delete data->sci;
	data->sci = nullptr;
	return DoSuperMethodA(cl, obj, msg);
}

static IPTR mSetup(struct IClass *cl, Object *obj, struct MUIP_Setup *msg) {
	if (!DoSuperMethodA(cl, obj, reinterpret_cast<Msg>(msg)))
		return FALSE;

	struct ScintillaMUIData *data = INST_DATA_CPP(cl, obj);
	data->sci->Initialise();

	MUI_RequestIDCMP(obj, IDCMP_RAWKEY | IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE | IDCMP_INTUITICKS);

	// Notify on scrollbar changes
	if (data->scrollV) {
		DoMethod(data->scrollV, MUIM_Notify, MUIA_Prop_First, MUIV_EveryTime,
			(IPTR)obj, 1, MUIM_Scintilla_VScroll);
	}
	if (data->scrollH) {
		DoMethod(data->scrollH, MUIM_Notify, MUIA_Prop_First, MUIV_EveryTime,
			(IPTR)obj, 1, MUIM_Scintilla_HScroll);
	}

	return TRUE;
}

static IPTR mShow(struct IClass *cl, Object *obj, Msg msg) {
	IPTR ret = DoSuperMethodA(cl, obj, msg);
	if (!ret) return ret;

	struct ScintillaMUIData *data = INST_DATA_CPP(cl, obj);

	struct ::Window *win = _window(obj);
	if (win) {
		data->sci->SetupRenderContext(win, data->editorArea);
	}

	data->sci->SetFocus(true);
	return ret;
}

static IPTR mHide(struct IClass *cl, Object *obj, Msg msg) {
	struct ScintillaMUIData *data = INST_DATA_CPP(cl, obj);
	data->sci->ReleaseRenderContext();

	return DoSuperMethodA(cl, obj, msg);
}

static IPTR mCleanup(struct IClass *cl, Object *obj, struct MUIP_Cleanup *msg) {
	struct ScintillaMUIData *data = INST_DATA_CPP(cl, obj);

	MUI_RejectIDCMP(obj, IDCMP_RAWKEY | IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE | IDCMP_INTUITICKS);

	return DoSuperMethodA(cl, obj, reinterpret_cast<Msg>(msg));
}

static IPTR mAskMinMax(struct IClass *cl, Object *obj, struct MUIP_AskMinMax *msg) {
	DoSuperMethodA(cl, obj, reinterpret_cast<Msg>(msg));

	msg->MinMaxInfo->MinWidth += 100;
	msg->MinMaxInfo->MinHeight += 50;
	msg->MinMaxInfo->DefWidth += 400;
	msg->MinMaxInfo->DefHeight += 300;
	msg->MinMaxInfo->MaxWidth = MUI_MAXMAX;
	msg->MinMaxInfo->MaxHeight = MUI_MAXMAX;

	return 0;
}

static IPTR mDraw(struct IClass *cl, Object *obj, struct MUIP_Draw *msg) {
	DoSuperMethodA(cl, obj, reinterpret_cast<Msg>(msg));

	if (!(msg->flags & MADF_DRAWOBJECT) && !(msg->flags & MADF_DRAWUPDATE))
		return 0;

	struct ScintillaMUIData *data = INST_DATA_CPP(cl, obj);
	Object *area = data->editorArea ? data->editorArea : obj;

	WORD left = _mleft(area);
	WORD top = _mtop(area);
	WORD width = _mwidth(area);
	WORD height = _mheight(area);

	bool fullRedraw = (msg->flags & MADF_DRAWOBJECT) != 0;
	data->sci->DoPaint(left, top, width, height, fullRedraw);
	return 0;
}

static IPTR mHandleInput(struct IClass *cl, Object *obj, struct MUIP_HandleInput *msg) {
	struct ScintillaMUIData *data = INST_DATA_CPP(cl, obj);

	if (!msg->imsg)
		return 0;

	struct IntuiMessage *imsg = msg->imsg;
	Object *area = data->editorArea ? data->editorArea : obj;
	WORD mx = imsg->MouseX - _mleft(area);
	WORD my = imsg->MouseY - _mtop(area);
	unsigned int time = imsg->Seconds * 1000 + imsg->Micros / 1000;

	bool needRedraw = false;

	switch (imsg->Class) {
	case IDCMP_RAWKEY: {
		if (imsg->Code & IECODE_UP_PREFIX)
			break;

		// Mouse wheel scrolling
		if (imsg->Code == RAWKEY_NM_WHEEL_UP) {
			data->sci->SendMessage(SCI_LINESCROLL, 0, -3);
			needRedraw = true;
			break;
		}
		if (imsg->Code == RAWKEY_NM_WHEEL_DOWN) {
			data->sci->SendMessage(SCI_LINESCROLL, 0, 3);
			needRedraw = true;
			break;
		}

		struct InputEvent ie = {};
		ie.ie_Class = IECLASS_RAWKEY;
		ie.ie_Code = imsg->Code;
		ie.ie_Qualifier = imsg->Qualifier;
		ie.ie_EventAddress = imsg->IAddress;

		char buf[8] = {};
		int charLen = KeymapMapRawKey(&ie, buf, sizeof(buf) - 1, nullptr);
		if (charLen < 0) charLen = 0;

		// Ctrl+Space: trigger autocomplete
		if (imsg->Code == RAWKEY_SPACE && (imsg->Qualifier & IEQUALIFIER_CONTROL)
				&& !(imsg->Qualifier & (IEQUALIFIER_LSHIFT | IEQUALIFIER_RSHIFT))) {
			static const char *defaultWords =
				"auto?0 break?0 case?0 char?0 const?0 continue?0 default?0 do?0 "
				"double?0 else?0 extern?0 float?0 for?0 goto?0 if?0 include?0 "
				"int?0 long?0 main?1 printf?1 register?0 return?0 short?0 "
				"signed?0 sizeof?0 static?0 struct?0 switch?0 typedef?0 union?0 "
				"unsigned?0 void?0 volatile?0 while?0";
			data->sci->SendMessage(SCI_AUTOCSHOW, 0, reinterpret_cast<sptr_t>(defaultWords));
			needRedraw = true;
			break;
		}

		// Ctrl+Shift+Space: trigger calltip
		if (imsg->Code == RAWKEY_SPACE && (imsg->Qualifier & IEQUALIFIER_CONTROL)
				&& (imsg->Qualifier & (IEQUALIFIER_LSHIFT | IEQUALIFIER_RSHIFT))) {
			data->sci->SendMessage(SCI_CALLTIPSHOW, 0,
				reinterpret_cast<sptr_t>("printf(const char *format, ...)\nFormats and prints text to stdout"));
			needRedraw = true;
			break;
		}

		data->sci->HandleKeyDown(imsg->Code & ~IECODE_UP_PREFIX,
			imsg->Qualifier, buf, charLen);
		needRedraw = true;
		break;
	}

	case IDCMP_MOUSEBUTTONS: {
		// Check if context menu is open and handle click on it
		extern struct ::Window *sMenuWin;
		extern void PlatAROS_HandleMenuClick(int x, int y, void (*)(void *, int), void *);
		if (sMenuWin && imsg->Code == IECODE_LBUTTON) {
			PlatAROS_HandleMenuClick(imsg->MouseX, imsg->MouseY,
				[](void *sci, int cmd) { static_cast<ScintillaAROS *>(sci)->ExecuteCommand(cmd); },
				data->sci);
			needRedraw = true;
			break;
		}

		if (imsg->Code == IECODE_LBUTTON) {
			data->sci->HandleMouseDown(mx, my, time, imsg->Qualifier);
			needRedraw = true;
		} else if (imsg->Code == (IECODE_LBUTTON | IECODE_UP_PREFIX)) {
			data->sci->HandleMouseUp(mx, my, time, imsg->Qualifier);
			needRedraw = true;
		} else if (imsg->Code == IECODE_RBUTTON) {
			data->sci->HandleRightClick(mx, my, time, imsg->Qualifier);
			needRedraw = true;
		}
		break;
	}

	case IDCMP_MOUSEMOVE:
		data->sci->HandleMouseMove(mx, my, time, imsg->Qualifier);
		needRedraw = true;
		break;

	case IDCMP_INTUITICKS:
		if (data->sci->HandleTick()) {
			needRedraw = true;
		}
		break;
	}

	if (needRedraw) {
		MUI_Redraw(obj, MADF_DRAWUPDATE);
	}

	// Consume keyboard and mouse events so MUI doesn't process them
	// (e.g. Escape closing the window)
	if (imsg->Class == IDCMP_RAWKEY || imsg->Class == IDCMP_MOUSEBUTTONS) {
		return MUI_EventHandlerRC_Eat;
	}

	return 0;
}

static IPTR mGoActive(struct IClass *cl, Object *obj, Msg msg) {
	struct ScintillaMUIData *data = INST_DATA_CPP(cl, obj);
	data->sci->SetFocus(true);
	MUI_Redraw(obj, MADF_DRAWUPDATE);
	return DoSuperMethodA(cl, obj, msg);
}

static IPTR mGoInactive(struct IClass *cl, Object *obj, Msg msg) {
	struct ScintillaMUIData *data = INST_DATA_CPP(cl, obj);
	data->sci->SetFocus(false);
	MUI_Redraw(obj, MADF_DRAWUPDATE);
	return DoSuperMethodA(cl, obj, msg);
}

static IPTR mVScroll(struct IClass *cl, Object *obj, Msg msg) {
	struct ScintillaMUIData *data = INST_DATA_CPP(cl, obj);
	data->sci->HandleVScroll();
	MUI_Redraw(obj, MADF_DRAWUPDATE);
	return 0;
}

static IPTR mHScroll(struct IClass *cl, Object *obj, Msg msg) {
	struct ScintillaMUIData *data = INST_DATA_CPP(cl, obj);
	data->sci->HandleHScroll();
	MUI_Redraw(obj, MADF_DRAWUPDATE);
	return 0;
}

static IPTR mSet(struct IClass *cl, Object *obj, struct opSet *msg) {
	struct ScintillaMUIData *data = INST_DATA_CPP(cl, obj);
	struct TagItem *tag;

	if (msg->ops_AttrList) {
		tag = FindTagItem(MUIA_Scintilla_Notification, msg->ops_AttrList);
		if (tag) {
			data->lastNotification = tag->ti_Data;
		}
	}

	return DoSuperMethodA(cl, obj, reinterpret_cast<Msg>(msg));
}

static IPTR mGet(struct IClass *cl, Object *obj, struct opGet *msg) {
	struct ScintillaMUIData *data = INST_DATA_CPP(cl, obj);

	switch (msg->opg_AttrID) {
	case MUIA_Scintilla_DirectPtr:
		*msg->opg_Storage = static_cast<IPTR>(data->sci->GetDirectPointer());
		return TRUE;
	case MUIA_Scintilla_Notification:
		*msg->opg_Storage = data->lastNotification;
		return TRUE;
	}

	return DoSuperMethodA(cl, obj, reinterpret_cast<Msg>(msg));
}

AROS_UFH3S(IPTR, ScintillaDispatcher,
	AROS_UFHA(struct IClass *, cl, A0),
	AROS_UFHA(Object *, obj, A2),
	AROS_UFHA(Msg, msg, A1))
{
	AROS_USERFUNC_INIT

	switch (msg->MethodID) {
	case OM_NEW:          return mNew(cl, obj, reinterpret_cast<struct opSet *>(msg));
	case OM_SET:          return mSet(cl, obj, reinterpret_cast<struct opSet *>(msg));
	case OM_GET:          return mGet(cl, obj, reinterpret_cast<struct opGet *>(msg));
	case OM_DISPOSE:      return mDispose(cl, obj, msg);
	case MUIM_Setup:      return mSetup(cl, obj, reinterpret_cast<struct MUIP_Setup *>(msg));
	case MUIM_Cleanup:    return mCleanup(cl, obj, reinterpret_cast<struct MUIP_Cleanup *>(msg));
	case MUIM_Show:       return mShow(cl, obj, msg);
	case MUIM_Hide:       return mHide(cl, obj, msg);
	case MUIM_AskMinMax:  return mAskMinMax(cl, obj, reinterpret_cast<struct MUIP_AskMinMax *>(msg));
	case MUIM_Draw:       return mDraw(cl, obj, reinterpret_cast<struct MUIP_Draw *>(msg));
	case MUIM_HandleInput: return mHandleInput(cl, obj, reinterpret_cast<struct MUIP_HandleInput *>(msg));
	case MUIM_GoActive:   return mGoActive(cl, obj, msg);
	case MUIM_GoInactive: return mGoInactive(cl, obj, msg);
	case MUIM_Scintilla_VScroll: return mVScroll(cl, obj, msg);
	case MUIM_Scintilla_HScroll: return mHScroll(cl, obj, msg);
	}

	return DoSuperMethodA(cl, obj, msg);

	AROS_USERFUNC_EXIT
}

//----------------------------------------------------------------------
// Public API
//----------------------------------------------------------------------

static struct MUI_CustomClass *ScintillaMUIClass = nullptr;

struct IClass *GetScintillaMUIClass() {
	if (!ScintillaMUIClass) {
		ScintillaMUIClass = MUI_CreateCustomClass(
			nullptr,
			const_cast<STRPTR>(MUIC_Group),
			nullptr,
			sizeof(struct ScintillaMUIData),
			reinterpret_cast<APTR>(ScintillaDispatcher));
	}
	return ScintillaMUIClass ? ScintillaMUIClass->mcc_Class : nullptr;
}

void FreeScintillaMUIClass() {
	if (ScintillaMUIClass) {
		MUI_DeleteCustomClass(ScintillaMUIClass);
		ScintillaMUIClass = nullptr;
	}
}

sptr_t ScintillaAROS_SendMessage(Object *obj, unsigned int iMessage, uptr_t wParam, sptr_t lParam) {
	struct ScintillaMUIData *data = static_cast<struct ScintillaMUIData *>(INST_DATA(OCLASS(obj), obj));
	if (data && data->sci) {
		return data->sci->SendMessage(iMessage, wParam, lParam);
	}
	return 0;
}

void ScintillaAROS_SetNotify(Object *obj, ScintillaNotifyFunc func, void *userData) {
	struct ScintillaMUIData *data = static_cast<struct ScintillaMUIData *>(INST_DATA(OCLASS(obj), obj));
	if (data && data->sci) {
		data->sci->SetNotifyCallback(func, userData);
	}
}
