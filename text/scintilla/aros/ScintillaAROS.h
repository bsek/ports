// Scintilla source code edit control
/** @file ScintillaAROS.h
 ** Public header for AROS MUI/Zune Scintilla widget.
 **/
// Copyright 2026 The AROS Development Team
// The License.txt file describes the conditions under which this software may be distributed.

#ifndef SCINTILLAAROS_H
#define SCINTILLAAROS_H

#include <stdint.h>
#include <intuition/classes.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Get the MUI custom class for the Scintilla editor widget.
 *
 * Usage:
 *   Object *editor = NewObject(GetScintillaMUIClass(), NULL,
 *       MUIA_Frame, MUIV_Frame_Text,
 *       TAG_DONE);
 *
 * Remember to call FreeScintillaMUIClass() when done (e.g. at program exit).
 */
struct IClass *GetScintillaMUIClass(void);

/**
 * Free the MUI custom class. Call once at program exit.
 */
void FreeScintillaMUIClass(void);

/**
 * Send a Scintilla message to the editor widget.
 *
 * @param obj       The MUI Scintilla object.
 * @param iMessage  Scintilla message ID (SCI_*).
 * @param wParam    Message parameter.
 * @param lParam    Message parameter.
 * @return          Message result.
 *
 * Example:
 *   ScintillaAROS_SendMessage(editor, SCI_SETTEXT, 0, (sptr_t)"Hello World");
 *   ScintillaAROS_SendMessage(editor, SCI_SETLEXER, SCLEX_CPP, 0);
 */
intptr_t ScintillaAROS_SendMessage(Object *obj, unsigned int iMessage,
                                    uintptr_t wParam, intptr_t lParam);

/**
 * Custom attribute: Get the direct pointer for SciFnDirect calls.
 *
 *   sptr_t ptr;
 *   get(editor, MUIA_Scintilla_DirectPtr, &ptr);
 */
#define MUIA_Scintilla_DirectPtr  (TAG_USER + 0x000F0001)

#ifdef __cplusplus
}
#endif

#endif /* SCINTILLAAROS_H */
