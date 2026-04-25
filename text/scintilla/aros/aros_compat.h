// aros_compat.h — Resolve naming conflicts between AROS and Scintilla.
// Force-included via -include before all source files.
//
// AROS system headers define struct/typedef names and function-like macros
// that collide with Scintilla C++ identifiers. This header includes all
// AROS headers first (renaming colliding types), then undefs the macros.

#ifndef AROS_COMPAT_H
#define AROS_COMPAT_H

// --- Phase 1: Rename AROS struct/typedef names that collide ---
#define Message   ExecMessage
#define KeyMap    ExecKeyMap
#define Point     AROSPoint

// --- Phase 2: Include all AROS system headers under the renames ---
#ifdef __cplusplus
extern "C" {
#endif
#include <exec/exec.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <graphics/gfx.h>
#include <graphics/view.h>
#include <graphics/rastport.h>
#include <graphics/text.h>
#include <graphics/regions.h>
#include <utility/tagitem.h>
#include <utility/hooks.h>
#include <devices/keymap.h>
#include <devices/inputevent.h>
#include <devices/clipboard.h>
#include <intuition/intuition.h>
#include <intuition/classes.h>
#include <intuition/pointerclass.h>
#include <rexx/storage.h>
#include <libraries/mui.h>

// Proto headers (these pull in inline/ or defines/ with function macros)
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/utility.h>
#include <proto/layers.h>
#include <proto/keymap.h>
#include <proto/diskfont.h>
#include <proto/iffparse.h>
#include <proto/muimaster.h>

#include <clib/alib_protos.h>

#ifdef __cplusplus
} // extern "C"
#endif

// --- Phase 3: Undo type renames ---
#undef Message
#undef KeyMap
#undef Point

// --- Phase 4: Undef AROS function-like macros that collide with Scintilla ---

// From exec: defines/exec.h / inline/exec.h
#ifdef Allocate
#undef Allocate
#endif
#ifdef Deallocate
#undef Deallocate
#endif
#ifdef Insert
#undef Insert
#endif
#ifdef Remove
#undef Remove
#endif
#ifdef Signal
#undef Signal
#endif
#ifdef Wait
#undef Wait
#endif
#ifdef Debug
#undef Debug
#endif
#ifdef NewObject
#undef NewObject
#endif

// From graphics: defines/graphics.h / inline/graphics.h
#ifdef Move
#undef Move
#endif
#ifdef Draw
#undef Draw
#endif
#ifdef Text
#undef Text
#endif
#ifdef TextLength
#undef TextLength
#endif
#ifdef SetFont
#undef SetFont
#endif
#ifdef Flood
#undef Flood
#endif

// From dos: defines/dos.h / inline/dos.h
#ifdef Open
#undef Open
#endif
#ifdef Close
#undef Close
#endif
#ifdef Read
#undef Read
#endif
#ifdef Write
#undef Write
#endif
#ifdef Input
#undef Input
#endif
#ifdef Output
#undef Output
#endif
#ifdef Seek
#undef Seek
#endif
#ifdef Flush
#undef Flush
#endif
#ifdef Rename
#undef Rename
#endif
#ifdef Execute
#undef Execute
#endif
#ifdef SetMode
#undef SetMode
#endif
#ifdef Format
#undef Format
#endif

// From keymap: defines/keymap.h / inline/keymap.h
#ifdef MapRawKey
#undef MapRawKey
#endif

// From intuition: defines/intuition.h / inline/intuition.h
#ifdef DoubleClick
#undef DoubleClick
#endif
#ifdef SetWindowPointer
#undef SetWindowPointer
#endif

// From MUI: libraries/mui.h
#ifdef End
#undef End
#endif
#ifdef Child
#undef Child
#endif
#ifdef Window
#undef Window
#endif
#ifdef String
#undef String
#endif

// --- Phase 5: Fix missing PTRDIFF_MAX on AROS ---
#ifndef PTRDIFF_MAX
#include <climits>
#define PTRDIFF_MAX INT_MAX
#define PTRDIFF_MIN INT_MIN
#endif

#endif // AROS_COMPAT_H
