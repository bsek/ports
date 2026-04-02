// Scintilla AROS test application
// Minimal MUI app that creates a Scintilla editor widget.

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {

#define DEBUG 1
#include <aros/debug.h>
#include <exec/types.h>
#include <intuition/intuition.h>
#include <libraries/mui.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/muimaster.h>
#include <libraries/zunegfx.h>
#include <proto/zunegfx.h>
#include <clib/alib_protos.h>
}

#include "ScintillaAROS.h"
#include "Scintilla.h"

// Request 256KB stack
extern "C" { __attribute__((used)) static const char __stack_cookie[] = "$STACK:262144"; }

// Sample C source code to display
static const char sampleText[] =
	"#include <stdio.h>\n"
	"\n"
	"int main(int argc, char *argv[])\n"
	"{\n"
	"    printf(\"Hello AROS!\\n\");\n"
	"    return 0;\n"
	"}\n";

int main(int argc, char *argv[])
{
	ZuneGfxBase = OpenLibrary("zunegfx.library", 0);
	if (!ZuneGfxBase) {
		D(bug("Failed to open zunegfx.library\n"));
		return 1;
	}

	struct IClass *sciClass = GetScintillaMUIClass();
	if (!sciClass) {
		D(bug("Failed to create Scintilla MUI class\n"));
		CloseLibrary(ZuneGfxBase);
		return 1;
	}

	Object *editor = reinterpret_cast<Object *>(NewObject(sciClass, NULL,
		MUIA_Frame, MUIV_Frame_Text,
		MUIA_Background, MUII_TextBack,
		TAG_DONE));

	if (!editor) {
		D(bug("Failed to create Scintilla editor object\n"));
		FreeScintillaMUIClass();
		CloseLibrary(ZuneGfxBase);
		return 1;
	}

	Object *win = reinterpret_cast<Object *>(MUI_NewObject(MUIC_Window,
		MUIA_Window_Title, (IPTR)"Scintilla Test",
		MUIA_Window_ID, MAKE_ID('S','C','T','E'),

		WindowContents, MUI_NewObject(MUIC_Group,
			Child, (IPTR)editor,
			TAG_DONE),
		TAG_DONE));

	Object *app = reinterpret_cast<Object *>(MUI_NewObject(MUIC_Application,
		MUIA_Application_Title, (IPTR)"ScintillaTest",
		MUIA_Application_Version, (IPTR)"$VER: ScintillaTest 1.0 (01.04.2026)",
		MUIA_Application_Description, (IPTR)"Scintilla editor test",

		SubWindow, (IPTR)win,
		TAG_DONE));

	if (!app) {
		D(bug("Failed to create MUI application\n"));
		FreeScintillaMUIClass();
		CloseLibrary(ZuneGfxBase);
		return 1;
	}

	// Configure the editor
	ScintillaAROS_SendMessage(editor, SCI_STYLESETFONT, STYLE_DEFAULT,
		(sptr_t)"ttcourier.font");
	ScintillaAROS_SendMessage(editor, SCI_STYLESETSIZE, STYLE_DEFAULT, 13);
	ScintillaAROS_SendMessage(editor, SCI_STYLECLEARALL, 0, 0);

	// Set colors
	ScintillaAROS_SendMessage(editor, SCI_STYLESETBACK, STYLE_DEFAULT, 0xFFFFFF);
	ScintillaAROS_SendMessage(editor, SCI_STYLESETFORE, STYLE_DEFAULT, 0x000000);

	// Set the text
	ScintillaAROS_SendMessage(editor, SCI_SETTEXT, 0, (sptr_t)sampleText);

	// Show line numbers
	ScintillaAROS_SendMessage(editor, SCI_SETMARGINTYPEN, 0, SC_MARGIN_NUMBER);
	ScintillaAROS_SendMessage(editor, SCI_SETMARGINWIDTHN, 0, 10);

	// Enable caret
	ScintillaAROS_SendMessage(editor, SCI_SETCARETPERIOD, 500, 0);

	// Open the window
	if (win) {
		SetAttrs(win, MUIA_Window_Open, TRUE, TAG_DONE);
	}

	// Check window opened
	IPTR open = FALSE;
	if (win) {
		GetAttr(MUIA_Window_Open, win, &open);
	}

	if (!open) {
		D(bug("Failed to open window\n"));
		MUI_DisposeObject(app);
		FreeScintillaMUIClass();
		CloseLibrary(ZuneGfxBase);
		return 1;
	}

	// Setup close gadget notification
	DoMethod(win, MUIM_Notify, MUIA_Window_CloseRequest, TRUE,
		(IPTR)app, 2, MUIM_Application_ReturnID, MUIV_Application_ReturnID_Quit);

	// Main event loop
	ULONG sigs = 0;
	ULONG id;
	while ((id = DoMethod(app, MUIM_Application_NewInput, (IPTR)&sigs))
		!= MUIV_Application_ReturnID_Quit)
	{
		if (sigs) {
			sigs = Wait(sigs | SIGBREAKF_CTRL_C);
			if (sigs & SIGBREAKF_CTRL_C)
				break;
		}
	}

	// Cleanup
	SetAttrs(win, MUIA_Window_Open, FALSE, TAG_DONE);
	MUI_DisposeObject(app);
	FreeScintillaMUIClass();
	CloseLibrary(ZuneGfxBase);

	return 0;
}
