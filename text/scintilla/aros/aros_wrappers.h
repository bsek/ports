// aros_wrappers.h — Wrappers for AROS functions whose macros were #undef'd
// by aros_compat.h. Platform files include this to call these functions.
//
// Each wrapper calls through the library base directly using AROS_LC macros.

#ifndef AROS_WRAPPERS_H
#define AROS_WRAPPERS_H

#include <aros/libcall.h>
#include <exec/types.h>
#include <graphics/rastport.h>
#include <graphics/text.h>

// graphics.library wrappers
inline void GfxMove(struct RastPort *rp, WORD x, WORD y) {
	AROS_LC3NR(void, Move,
		AROS_LCA(struct RastPort *, rp, A1),
		AROS_LCA(WORD, x, D0),
		AROS_LCA(WORD, y, D1),
		struct GfxBase *, GfxBase, 40, Graphics);
}

inline void GfxText(struct RastPort *rp, CONST_STRPTR string, UWORD count) {
	AROS_LC3NR(void, Text,
		AROS_LCA(struct RastPort *, rp, A1),
		AROS_LCA(CONST_STRPTR, string, A0),
		AROS_LCA(UWORD, count, D0),
		struct GfxBase *, GfxBase, 10, Graphics);
}

inline WORD GfxTextLength(struct RastPort *rp, CONST_STRPTR string, UWORD count) {
	return AROS_LC3(WORD, TextLength,
		AROS_LCA(struct RastPort *, rp, A1),
		AROS_LCA(CONST_STRPTR, string, A0),
		AROS_LCA(UWORD, count, D0),
		struct GfxBase *, GfxBase, 9, Graphics);
}

inline void GfxSetFont(struct RastPort *rp, struct TextFont *font) {
	AROS_LC2NR(void, SetFont,
		AROS_LCA(struct RastPort *, rp, A1),
		AROS_LCA(struct TextFont *, font, A0),
		struct GfxBase *, GfxBase, 11, Graphics);
}

inline void GfxDraw(struct RastPort *rp, WORD x, WORD y) {
	AROS_LC3NR(void, Draw,
		AROS_LCA(struct RastPort *, rp, A1),
		AROS_LCA(WORD, x, D0),
		AROS_LCA(WORD, y, D1),
		struct GfxBase *, GfxBase, 41, Graphics);
}

// exec.library wrappers
inline APTR ExecAllocate(struct MemHeader *mh, ULONG byteSize) {
	return AROS_LC2(APTR, Allocate,
		AROS_LCA(struct MemHeader *, mh, A0),
		AROS_LCA(ULONG, byteSize, D0),
		struct ExecBase *, SysBase, 31, Exec);
}

inline void ExecDeallocate(struct MemHeader *mh, APTR memoryBlock, ULONG byteSize) {
	AROS_LC3NR(void, Deallocate,
		AROS_LCA(struct MemHeader *, mh, A0),
		AROS_LCA(APTR, memoryBlock, A1),
		AROS_LCA(ULONG, byteSize, D0),
		struct ExecBase *, SysBase, 32, Exec);
}

inline void ExecInsert(struct List *list, struct Node *node, struct Node *pred) {
	AROS_LC3NR(void, Insert,
		AROS_LCA(struct List *, list, A0),
		AROS_LCA(struct Node *, node, A1),
		AROS_LCA(struct Node *, pred, A2),
		struct ExecBase *, SysBase, 39, Exec);
}

inline void ExecRemove(struct Node *node) {
	AROS_LC1NR(void, Remove,
		AROS_LCA(struct Node *, node, A1),
		struct ExecBase *, SysBase, 42, Exec);
}

// intuition.library wrappers
inline BOOL IntuiDoubleClick(ULONG sSeconds, ULONG sMicros,
                              ULONG cSeconds, ULONG cMicros) {
	return AROS_LC4(BOOL, DoubleClick,
		AROS_LCA(ULONG, sSeconds, D0),
		AROS_LCA(ULONG, sMicros, D1),
		AROS_LCA(ULONG, cSeconds, D2),
		AROS_LCA(ULONG, cMicros, D3),
		struct IntuitionBase *, IntuitionBase, 17, Intuition);
}

// keymap.library wrappers
inline WORD KeymapMapRawKey(struct InputEvent *event, STRPTR buffer,
                             LONG length, struct ExecKeyMap *keyMap) {
	return AROS_LC4(WORD, MapRawKey,
		AROS_LCA(struct InputEvent *, event, A0),
		AROS_LCA(STRPTR, buffer, A1),
		AROS_LCA(LONG, length, D1),
		AROS_LCA(struct ExecKeyMap *, keyMap, A2),
		struct Library *, KeymapBase, 7, Keymap);
}

// intuition.library SetWindowPointer wrapper (macro undef'd in aros_compat.h)
inline void IntuiSetWindowPointer(struct ::Window *win, struct TagItem *tags) {
	AROS_LC2NR(void, SetWindowPointerA,
		AROS_LCA(struct ::Window *, win, A0),
		AROS_LCA(struct TagItem *, tags, A1),
		struct IntuitionBase *, IntuitionBase, 136, Intuition);
}

// MUI C++ helpers (get/set are C-only macros)
inline BOOL MUIGet(Object *obj, ULONG attr, IPTR *storage) {
	return GetAttr(attr, obj, storage);
}

inline void MUISet(Object *obj, ULONG attr, IPTR value) {
	SetAttrs(obj, attr, value, TAG_DONE);
}

template<typename T>
inline void MUIGet(Object *obj, ULONG attr, T *storage) {
	IPTR tmp = 0;
	GetAttr(attr, obj, &tmp);
	*storage = static_cast<T>(tmp);
}

#endif // AROS_WRAPPERS_H
