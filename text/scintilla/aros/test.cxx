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
#include "ILexer.h"
#include "SciLexer.h"
#include "Lexilla.h"

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
	ScintillaAROS_SendMessage(editor, SCI_STYLESETFONT, STYLE_DEFAULT, (sptr_t)"arial.font");
	ScintillaAROS_SendMessage(editor, SCI_STYLESETSIZE, STYLE_DEFAULT, 11);
	ScintillaAROS_SendMessage(editor, SCI_STYLECLEARALL, 0, 0);

	// Set colors
	ScintillaAROS_SendMessage(editor, SCI_STYLESETBACK, STYLE_DEFAULT, 0xFFFFFF);
	ScintillaAROS_SendMessage(editor, SCI_STYLESETFORE, STYLE_DEFAULT, 0x000000);

	// Set the text
	ScintillaAROS_SendMessage(editor, SCI_SETTEXT, 0, (sptr_t)sampleText);

	// Set up C++ lexer via Lexilla
	Scintilla::ILexer5 *lexer = CreateLexer("cpp");
	if (lexer) {
		ScintillaAROS_SendMessage(editor, SCI_SETILEXER, 0, (sptr_t)lexer);
		D(bug("Lexer set: cpp\n"));
	} else {
		D(bug("Failed to create cpp lexer\n"));
	}

	// Syntax highlighting colors
	ScintillaAROS_SendMessage(editor, SCI_STYLESETFORE, SCE_C_COMMENT, 0x008000);
	ScintillaAROS_SendMessage(editor, SCI_STYLESETFORE, SCE_C_COMMENTLINE, 0x008000);
	ScintillaAROS_SendMessage(editor, SCI_STYLESETFORE, SCE_C_COMMENTDOC, 0x008000);
	ScintillaAROS_SendMessage(editor, SCI_STYLESETFORE, SCE_C_NUMBER, 0x0080FF);
	ScintillaAROS_SendMessage(editor, SCI_STYLESETFORE, SCE_C_WORD, 0x0000FF);
	ScintillaAROS_SendMessage(editor, SCI_STYLESETBOLD, SCE_C_WORD, 1);
	ScintillaAROS_SendMessage(editor, SCI_STYLESETFORE, SCE_C_STRING, 0x800080);
	ScintillaAROS_SendMessage(editor, SCI_STYLESETFORE, SCE_C_CHARACTER, 0x800080);
	ScintillaAROS_SendMessage(editor, SCI_STYLESETFORE, SCE_C_PREPROCESSOR, 0x808000);
	ScintillaAROS_SendMessage(editor, SCI_STYLESETFORE, SCE_C_OPERATOR, 0x000080);

	// C/C++ keywords
	ScintillaAROS_SendMessage(editor, SCI_SETKEYWORDS, 0,
		(sptr_t)"auto break case char const continue default do double else enum "
		"extern float for goto if int long register return short signed sizeof "
		"static struct switch typedef union unsigned void volatile while "
		"class namespace template this new delete operator public private protected "
		"virtual inline friend try catch throw using");

	// Show line numbers
	ScintillaAROS_SendMessage(editor, SCI_SETMARGINTYPEN, 0, SC_MARGIN_NUMBER);
	ScintillaAROS_SendMessage(editor, SCI_SETMARGINWIDTHN, 0, 40);
	ScintillaAROS_SendMessage(editor, SCI_SETMARGINWIDTHN, 1, 0);  // Hide default symbol margin

	// Enable folding
	ScintillaAROS_SendMessage(editor, SCI_SETPROPERTY, (uptr_t)"fold", (sptr_t)"1");
	ScintillaAROS_SendMessage(editor, SCI_SETPROPERTY, (uptr_t)"fold.compact", (sptr_t)"0");
	ScintillaAROS_SendMessage(editor, SCI_SETMARGINTYPEN, 2, SC_MARGIN_SYMBOL);
	ScintillaAROS_SendMessage(editor, SCI_SETMARGINMASKN, 2, SC_MASK_FOLDERS);
	ScintillaAROS_SendMessage(editor, SCI_SETMARGINWIDTHN, 2, 16);
	ScintillaAROS_SendMessage(editor, SCI_SETMARGINSENSITIVEN, 2, 1);
	ScintillaAROS_SendMessage(editor, SCI_SETAUTOMATICFOLD, SC_AUTOMATICFOLD_SHOW | SC_AUTOMATICFOLD_CLICK | SC_AUTOMATICFOLD_CHANGE, 0);
	ScintillaAROS_SendMessage(editor, SCI_MARKERDEFINE, SC_MARKNUM_FOLDEROPEN, SC_MARK_BOXMINUS);
	ScintillaAROS_SendMessage(editor, SCI_MARKERDEFINE, SC_MARKNUM_FOLDER, SC_MARK_BOXPLUS);
	ScintillaAROS_SendMessage(editor, SCI_MARKERDEFINE, SC_MARKNUM_FOLDERSUB, SC_MARK_VLINE);
	ScintillaAROS_SendMessage(editor, SCI_MARKERDEFINE, SC_MARKNUM_FOLDERTAIL, SC_MARK_LCORNER);
	ScintillaAROS_SendMessage(editor, SCI_MARKERDEFINE, SC_MARKNUM_FOLDEREND, SC_MARK_BOXPLUSCONNECTED);
	ScintillaAROS_SendMessage(editor, SCI_MARKERDEFINE, SC_MARKNUM_FOLDEROPENMID, SC_MARK_BOXMINUSCONNECTED);
	ScintillaAROS_SendMessage(editor, SCI_MARKERDEFINE, SC_MARKNUM_FOLDERMIDTAIL, SC_MARK_TCORNER);

	// Autocomplete settings
	ScintillaAROS_SendMessage(editor, SCI_AUTOCSETIGNORECASE, 1, 0);
	ScintillaAROS_SendMessage(editor, SCI_AUTOCSETSEPARATOR, ' ', 0);
	ScintillaAROS_SendMessage(editor, SCI_AUTOCSETTYPESEPARATOR, '?', 0);

	// Register test icons for autocomplete (8x8 RGBA)
	{
		unsigned char blueIcon[8 * 8 * 4];
		unsigned char greenIcon[8 * 8 * 4];
		for (int i = 0; i < 8 * 8; i++) {
			// Blue square (type 0 = keyword)
			blueIcon[i * 4 + 0] = 50;   // R
			blueIcon[i * 4 + 1] = 100;  // G
			blueIcon[i * 4 + 2] = 200;  // B
			blueIcon[i * 4 + 3] = 255;  // A
			// Green square (type 1 = function)
			greenIcon[i * 4 + 0] = 50;
			greenIcon[i * 4 + 1] = 180;
			greenIcon[i * 4 + 2] = 50;
			greenIcon[i * 4 + 3] = 255;
		}
		ScintillaAROS_SendMessage(editor, SCI_RGBAIMAGESETWIDTH, 8, 0);
		ScintillaAROS_SendMessage(editor, SCI_RGBAIMAGESETHEIGHT, 8, 0);
		ScintillaAROS_SendMessage(editor, SCI_REGISTERRGBAIMAGE, 0, (sptr_t)blueIcon);
		ScintillaAROS_SendMessage(editor, SCI_REGISTERRGBAIMAGE, 1, (sptr_t)greenIcon);
	}

	// Enable caret
	ScintillaAROS_SendMessage(editor, SCI_SETCARETPERIOD, 500, 0);

	// Set up notification callback
	ScintillaAROS_SetNotify(editor,
		[](void *, intptr_t code, intptr_t position, intptr_t ch) {
			if (code == SCN_CHARADDED) {
				D(bug("SCN_CHARADDED: '%c'\n", (char)ch));
			}
		}, nullptr);

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
