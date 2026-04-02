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

	// Clipboard
	std::string clipboardContent;

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
		ScintillaBase::Finalise();
		ReleaseRenderContext();
	}

	void SetVerticalScrollPos() override {
		// TODO: Update MUI scrollbar position
	}

	void SetHorizontalScrollPos() override {
		// TODO: Update MUI scrollbar position
	}

	bool ModifyScrollBars(Sci::Line nMax, Sci::Line nPage) override {
		// TODO: Update MUI scrollbar ranges
		return true;
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
		// Notify MUI parent that content changed
	}

	void NotifyParent(NotificationData scn) override {
		// Send notification to MUI parent
		// TODO: MUI notification mechanism
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

	void CreateCallTipWindow(PRectangle rc) override {
		// TODO: Create MUI popup window for calltips
	}

	void AddToPopUp(const char *label, int cmd, bool enabled) override {
		// TODO: Add item to MUI popup menu
	}

	bool SetIdle(bool on) override {
		hasIdler = on;
		return true;
	}

	bool FineTickerRunning(TickReason reason) override {
		return tickTimers[static_cast<int>(reason)];
	}

	void FineTickerStart(TickReason reason, int millis, int tolerance) override {
		tickTimers[static_cast<int>(reason)] = true;
		// TODO: Use MUI timer/MUIA_Timer to trigger callbacks
	}

	void FineTickerCancel(TickReason reason) override {
		tickTimers[static_cast<int>(reason)] = false;
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

	void SetupRenderContext(struct ::Window *win) {
		if (rctx) return;
		intuiWin = win;
		rctx = ZuneCreateRenderContextForWindow(win, win->WScreen->ViewPort.ColorMap, BACKEND_CYBERGFX);
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

	void DoPaint(WORD left, WORD top, WORD width, WORD height) {
		if (!rctx) return;

		EnsureBoard(width, height);
		if (!board) return;

		ZuneSetTarget(rctx, board);
		ZuneBeginBatch(rctx);

		PRectangle rcPaint(0, 0, width, height);
		std::unique_ptr<Surface> surfaceWindow = Surface::Allocate(Technology::Default);
		surfaceWindow->Init(static_cast<SurfaceID>(rctx), nullptr);

		Paint(surfaceWindow.get(), rcPaint);
		surfaceWindow->Release();

		ZuneEndBatch(rctx);

		// Present via ZunePresent
		ZuneSetTarget(rctx, board);
		ZunePresent(rctx, 0, 0, left, top,
			static_cast<UWORD>(width), static_cast<UWORD>(height));
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
		} else if (charLen > 0) {
			// Regular character input
			std::string_view sv(chars, charLen);
			InsertCharacter(sv, CharacterSource::DirectInput);
		}
	}

	void HandleMouseDown(WORD x, WORD y, unsigned int time, UWORD qualifier) {
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

	void HandleTick() {
		// Called periodically from MUI timer
		for (int i = 0; i < 5; i++) {
			if (tickTimers[i]) {
				TickFor(static_cast<TickReason>(i));
			}
		}
		if (hasIdler) {
			Idle();
		}
	}

	//------------------------------------------------------------------
	// Public API (SciFnDirect)
	//------------------------------------------------------------------

	sptr_t SendMessage(unsigned int iMessage, uptr_t wParam, sptr_t lParam) {
		D(bug("SCI: SendMessage msg=%u wParam=%lu lParam=%ld\n",
			iMessage, (unsigned long)wParam, (long)lParam));
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
};

#define MUIA_Scintilla_DirectPtr    (MUIA_UserData + 1)
#define MUIM_Scintilla_SendMessage  (MUIA_UserData + 2)

static IPTR mNew(struct IClass *cl, Object *obj, struct opSet *msg) {
	D(bug("SCI: mNew\n"));
	obj = reinterpret_cast<Object *>(DoSuperMethodA(cl, obj, reinterpret_cast<Msg>(msg)));
	if (!obj) { D(bug("SCI: mNew - super failed\n")); return 0; }

	struct ScintillaMUIData *data = INST_DATA_CPP(cl, obj);
	data->sci = new ScintillaAROS();
	data->sci->SetMUIObject(obj);

	D(bug("SCI: mNew done, obj=%p sci=%p\n", (void*)obj, (void*)data->sci));
	return reinterpret_cast<IPTR>(obj);
}

static IPTR mDispose(struct IClass *cl, Object *obj, Msg msg) {
	D(bug("SCI: mDispose\n"));
	struct ScintillaMUIData *data = INST_DATA_CPP(cl, obj);
	delete data->sci;
	data->sci = nullptr;
	return DoSuperMethodA(cl, obj, msg);
}

static IPTR mSetup(struct IClass *cl, Object *obj, struct MUIP_Setup *msg) {
	D(bug("SCI: mSetup\n"));
	if (!DoSuperMethodA(cl, obj, reinterpret_cast<Msg>(msg)))
		return FALSE;

	struct ScintillaMUIData *data = INST_DATA_CPP(cl, obj);
	data->sci->Initialise();

	// Request input events
	MUI_RequestIDCMP(obj, IDCMP_RAWKEY | IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE);

	D(bug("SCI: mSetup done\n"));
	return TRUE;
}

static IPTR mShow(struct IClass *cl, Object *obj, Msg msg) {
	D(bug("SCI: mShow\n"));
	IPTR ret = DoSuperMethodA(cl, obj, msg);
	if (!ret) { fprintf(stderr, "SCI: mShow - super failed\n"); return ret; }

	struct ScintillaMUIData *data = INST_DATA_CPP(cl, obj);

	// _window(obj) is now valid — the Intuition window is open
	struct ::Window *win = _window(obj);
	D(bug("SCI: mShow window=%p\n", (void*)win));
	if (win) {
		data->sci->SetupRenderContext(win);
	}

	D(bug("SCI: mShow done, rctx ready\n"));
	return ret;
}

static IPTR mHide(struct IClass *cl, Object *obj, Msg msg) {
	D(bug("SCI: mHide\n"));
	struct ScintillaMUIData *data = INST_DATA_CPP(cl, obj);
	data->sci->ReleaseRenderContext();

	return DoSuperMethodA(cl, obj, msg);
}

static IPTR mCleanup(struct IClass *cl, Object *obj, struct MUIP_Cleanup *msg) {
	D(bug("SCI: mCleanup\n"));
	struct ScintillaMUIData *data = INST_DATA_CPP(cl, obj);

	MUI_RejectIDCMP(obj, IDCMP_RAWKEY | IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE);

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

	WORD left = _mleft(obj);
	WORD top = _mtop(obj);
	WORD width = _mwidth(obj);
	WORD height = _mheight(obj);

	D(bug("SCI: mDraw flags=%x left=%d top=%d %dx%d rctx=%p\n",
		msg->flags, left, top, width, height, (void*)data->sci->GetRenderContext()));

	data->sci->DoPaint(left, top, width, height);

	D(bug("SCI: mDraw done\n"));
	return 0;
}

static IPTR mHandleInput(struct IClass *cl, Object *obj, struct MUIP_HandleInput *msg) {
	struct ScintillaMUIData *data = INST_DATA_CPP(cl, obj);

	if (!msg->imsg)
		return 0;

	struct IntuiMessage *imsg = msg->imsg;
	WORD mx = imsg->MouseX - _mleft(obj);
	WORD my = imsg->MouseY - _mtop(obj);
	unsigned int time = imsg->Seconds * 1000 + imsg->Micros / 1000;

	switch (imsg->Class) {
	case IDCMP_RAWKEY: {
		// Decode the key
		struct InputEvent ie = {};
		ie.ie_Class = IECLASS_RAWKEY;
		ie.ie_Code = imsg->Code;
		ie.ie_Qualifier = imsg->Qualifier;
		ie.ie_EventAddress = imsg->IAddress;

		// Check for key-up (bit 7 set)
		if (imsg->Code & IECODE_UP_PREFIX)
			break;

		char buf[8] = {};
		int charLen = KeymapMapRawKey(&ie, buf, sizeof(buf) - 1, nullptr);
		if (charLen < 0) charLen = 0;

		data->sci->HandleKeyDown(imsg->Code & ~IECODE_UP_PREFIX,
			imsg->Qualifier, buf, charLen);

		MUI_Redraw(obj, MADF_DRAWUPDATE);
		break;
	}

	case IDCMP_MOUSEBUTTONS:
		if (imsg->Code == IECODE_LBUTTON) {
			data->sci->HandleMouseDown(mx, my, time, imsg->Qualifier);
			MUI_Redraw(obj, MADF_DRAWUPDATE);
		} else if (imsg->Code == (IECODE_LBUTTON | IECODE_UP_PREFIX)) {
			data->sci->HandleMouseUp(mx, my, time, imsg->Qualifier);
			MUI_Redraw(obj, MADF_DRAWUPDATE);
		} else if (imsg->Code == IECODE_RBUTTON) {
			data->sci->HandleRightClick(mx, my, time, imsg->Qualifier);
		}
		break;

	case IDCMP_MOUSEMOVE:
		data->sci->HandleMouseMove(mx, my, time, imsg->Qualifier);
		break;
	}

	return 0;
}

static IPTR mGet(struct IClass *cl, Object *obj, struct opGet *msg) {
	struct ScintillaMUIData *data = INST_DATA_CPP(cl, obj);

	switch (msg->opg_AttrID) {
	case MUIA_Scintilla_DirectPtr:
		*msg->opg_Storage = static_cast<IPTR>(data->sci->GetDirectPointer());
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
	case OM_DISPOSE:      return mDispose(cl, obj, msg);
	case MUIM_Setup:      return mSetup(cl, obj, reinterpret_cast<struct MUIP_Setup *>(msg));
	case MUIM_Cleanup:    return mCleanup(cl, obj, reinterpret_cast<struct MUIP_Cleanup *>(msg));
	case MUIM_Show:       return mShow(cl, obj, msg);
	case MUIM_Hide:       return mHide(cl, obj, msg);
	case MUIM_AskMinMax:  return mAskMinMax(cl, obj, reinterpret_cast<struct MUIP_AskMinMax *>(msg));
	case MUIM_Draw:       return mDraw(cl, obj, reinterpret_cast<struct MUIP_Draw *>(msg));
	case MUIM_HandleInput: return mHandleInput(cl, obj, reinterpret_cast<struct MUIP_HandleInput *>(msg));
	case OM_GET:          return mGet(cl, obj, reinterpret_cast<struct opGet *>(msg));
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
			const_cast<STRPTR>(MUIC_Area),
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
