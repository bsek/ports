# Scintilla AROS Port — Analyse og Plan

## Oversikt

Scintilla er en plattform-uavhengig kode-redigeringskomponent skrevet i C++17.
Kjernen er fullstendig portabel — all plattform-spesifikk kode er isolert i egne filer.

Denne porten bruker **MUI/Zune** som widget-system og **zunegfx** som grafikkbibliotek.

---

## Arkitektur

```
+--------------------------------------------------+
|  Platform-spesifikk kode  (DET VI LAGER)         |
|  aros/ScintillaAROS.cxx   (widget + hendelser)   |
|  aros/PlatAROS.cxx        (tegning + fonter)     |
+--------------------------------------------------+
|  src/ - Portabel kjernekode  (UENDRET)            |
|  Editor.cxx, Document.cxx, ScintillaBase.cxx ... |
+--------------------------------------------------+
|  include/ - Offentlig API  (UENDRET)              |
|  Scintilla.h, ScintillaTypes.h, ...              |
+--------------------------------------------------+
```

To nye filer implementerer alt plattform-spesifikt:

- **`aros/PlatAROS.cxx`** — Implementerer `Surface`, `Font`, `Window`, `ListBox`, `Menu`
- **`aros/ScintillaAROS.cxx`** — MUI Area-subklasse, kobler widget til Scintilla-kjernen

---

## Avhengigheter

| Bibliotek | Bruk |
|---|---|
| `zunegfx.library` | Tegning, tekst, clipping, texturer, polylinjer, polygoner |
| `diskfont.library` | Åpne fonter (`OpenDiskFont`) |
| `muimaster.library` | Widget-system, input, timere |
| `cybergraphics.library` | Indirekte via zunegfx-backend |

---

## PlatAROS.cxx

### SurfaceID og WindowID

```cpp
typedef struct RenderContext *  SurfaceID;   // zunegfx RenderContext
typedef struct Window *         WindowID;    // Intuition Window
```

### FontHandle

Bruker `graphics.library` TextFont via `diskfont.library`:

```cpp
class FontHandle : public Font {
public:
    struct TextFont *tf;   // fra OpenDiskFont() / diskfont.library
    struct TextAttr  ta;
    ~FontHandle() { CloseFont(tf); }
};
```

Fontmetrikk:

| Scintilla-metode | AROS-kilde |
|---|---|
| `Ascent(font)` | `font->tf_Baseline` |
| `Descent(font)` | `font->tf_YSize - font->tf_Baseline` |
| `Height(font)` | `font->tf_YSize` |
| `AverageCharWidth(font)` | `font->tf_XSize` |

### SurfaceAROS — tegning via zunegfx

`SurfaceID` er en `struct RenderContext *` fra zunegfx.

#### Fargekonvertering

Scintilla's `ColourRGBA` mappes direkte til zunegfx's `ULONG` ARGB32-format:

```cpp
ULONG toZuneColor(ColourRGBA c) {
    return ZUNE_COLOR_ARGB32(c.GetAlpha(), c.GetRed(), c.GetGreen(), c.GetBlue());
}
```

#### Komplett mapping av Surface-metoder

| Scintilla `Surface`-metode | zunegfx-kall |
|---|---|
| `FillRectangle(rc, fill)` | `ZuneDrawRectangle(rctx, rect, ZUNE_BRUSH_SOLID(color))` |
| `FillRectangleAligned(rc, fill)` | Som over |
| `RectangleDraw(rc, fillStroke)` | `ZuneDrawRectangleRoundedStyled(borderWidth=1)` |
| `RectangleFrame(rc, stroke)` | `ZuneDrawRectangleOutlineStyled` |
| `RoundedRectangle(rc, fillStroke)` | `ZuneDrawRectangleRoundedStyled` |
| `AlphaRectangle(rc, corner, fs)` | `ZuneFillRectangleRoundedAA` |
| `GradientRectangle(rc, stops, opt)` | `ZuneDrawRectangle` med `ZUNE_BRUSH_TYPE_LINEAR_GRADIENT` |
| `LineDraw(start, end, stroke)` | `ZuneDrawLineAA` / `ZuneDrawLine` |
| `PolyLine(pts, n, stroke)` | `ZuneDrawPolyline` / `ZuneDrawPolylineStyled` |
| `Polygon(pts, n, fillStroke)` | `ZuneFillPolygon` + `ZuneDrawPolyline` for outline |
| `Ellipse(rc, fillStroke)` | `ZuneFillCircleAA` |
| `Stadium(rc, fillStroke, ends)` | `ZuneFillRectangleRoundedAA` |
| `DrawRGBAImage(rc, w, h, pixels)` | `ZuneCreateTextureFromData` + `ZuneDrawTexture` |
| `AllocatePixMap(w, h)` | `ZuneCreateDrawingBoardForRenderContext` → ny `RenderContext` |
| `Copy(rc, from, src)` | `ZuneBlit(src_rctx, dst_rctx, ...)` |
| `SetClip(rc)` | `ZuneSetClipRegion` |
| `PopClip()` | `ZuneClearClipRegion` |
| `FlushDrawing()` | `ZuneFlushBatch` |
| `FlushCachedState()` | `ZuneFlushBatch` eller ingen-op |

#### Batching for ytelse

Scintilla tegner mange primitiver per redraw-syklus. `ZuneBeginBatch` / `ZuneEndBatch`
brukes rundt hele redraw-kallet for å samle operasjoner:

```cpp
void ScintillaAROS::Redraw() {
    ZuneBeginBatch(rctx);
    wEditor.DoPaint(surface, ...);
    ZuneEndBatch(rctx);
    ZunePresent(rctx, 0, 0, left, top, width, height);
}
```

#### Tekst-rendering via zunegfx

zunegfx har nå et komplett tekst-API. Fonten settes på `RenderContext`,
og all tekst-tegning og -måling går gjennom zunegfx-funksjoner:

```cpp
// Sett font på RenderContext
ZuneSetFont(rctx, fontHandle->tf);

// Tegn tekst (transparent bakgrunn, JAM1)
ZuneDrawText(rctx, ZUNE_POINT_PTR(x, y), text.data(), text.length(), fgColor);

// Tegn tekst med bakgrunnsfarge (JAM2) — for seleksjoner
ZuneDrawTextBackground(rctx, ZUNE_POINT_PTR(x, y), text.data(), text.length(),
                       fgColor, bgColor);
```

Merk: `position.y` er TOPPEN av tekstlinjen, ikke baseline. zunegfx legger til
`tf_Baseline` internt.

Tekstmåling:

```cpp
// Bredde av en streng
XYPOSITION WidthText(font, text) {
    return (XYPOSITION)ZuneTextLength(rctx, text.data(), text.length());
}

// Per-tegn-bredder (for MeasureWidths)
for (size_t i = 1; i <= text.length(); i++) {
    positions[i-1] = (XYPOSITION)ZuneTextLength(rctx, text.data(), i);
}

// Hvor mange tegn passer i en bredde (for line wrapping)
UWORD chars = ZuneTextFit(rctx, text.data(), text.length(), maxWidth);
```

#### Sync/Reload for blandet rendering

Når zunegfx bruker OpenGL-backend og man trenger å blande med legacy
bitmap-operasjoner (f.eks. font-rendering via bitmap):

```cpp
ZuneSync(rctx);    // Kopier FBO → bitmap (før legacy-ops)
// ... legacy bitmap-operasjoner ...
ZuneReload(rctx);  // Kopier bitmap → FBO (etter legacy-ops)
```

For CyberGfx-backend er disse no-ops.

#### Capture (les bakgrunn for alpha-blending)

For korrekt alpha-blending over eksisterende innhold:

```cpp
ZuneCapture(rctx, window->RPort, src_x, src_y, dst_x, dst_y, w, h);
```

---

## ScintillaAROS.cxx

MUI Area-subklasse som kobler MUI-widget-systemet til Scintilla-kjernen.
Arver fra `ScintillaBase` (som igjen arver fra `Editor`).

### Klasse-struktur

```cpp
class ScintillaAROS : public ScintillaBase {
    struct Window   *window;      // Intuition window
    struct RenderContext *rctx;   // zunegfx kontekst
    Object          *muiObject;   // MUI objekt-peker (self)

    // Overstyr ScintillaBase virtuelle metoder:
    void Initialise() override;
    void Finalise() override;
    void SetVerticalScrollPos() override;
    void SetHorizontalScrollPos() override;
    bool ModifyScrollBars(Sci::Line nMax, Sci::Line nPage) override;
    void Copy() override;
    void Paste() override;
    void ClaimSelection() override;
    void NotifyChange() override;
    void NotifyParent(NotificationData scn) override;
    void CopyToClipboard(const SelectionText &selectedText) override;
    void SetTicking(bool on) override;
    bool SetIdle(bool on) override;
    void SetMouseCapture(bool on) override;
    bool HaveMouseCapture() override;
    sptr_t DefWndProc(Message iMessage, uptr_t wParam, sptr_t lParam) override;
    void CreateCallTipWindow(PRectangle rc) override;
    void AddToPopUp(const char *label, int cmd=0, bool enabled=true) override;
};
```

### MUI-metoder som må implementeres

| MUI-metode | Funksjon |
|---|---|
| `MUIM_AskMinMax` | Returner min/max størrelse |
| `MUIM_Draw` | Kall Scintilla sin paint-logikk |
| `MUIM_HandleInput` | Tastatur og mus |
| `MUIM_Setup` | Opprett `RenderContext` |
| `MUIM_Cleanup` | Frigjør `RenderContext` |
| `MUIA_Timer` | Markørblinking (via `MUIA_Timer`-attributt) |

### Input-håndtering

```cpp
// Tastatur
case IDCMP_RAWKEY:
    ScintillaAROS::KeyDown(imsg->Code, imsg->Qualifier);

// Mus
case IDCMP_MOUSEBUTTONS:
    ScintillaAROS::ButtonDown(Point(x, y), time, shift, ctrl, alt);

case IDCMP_MOUSEMOVE:
    ScintillaAROS::MouseMove(Point(x, y), ...);
```

### Clipboard

AROS clipboard bruker `clipboard.device` med IFF-format (FTXT-chunk):

```c
// Skriv til clipboard
OpenDevice("clipboard.device", 0, &clipReq, 0);
// Skriv IFF FTXT-chunk med tekst

// Les fra clipboard
// Les IFF FTXT-chunk
```

### Timere (markørblinking)

MUI støtter timere via `MUIA_Timer`-attributtet på Area:

```c
set(muiObject, MUIA_Timer, 500);  // 500ms intervall
// Håndteres i MUIM_HandleInput med IECLASS_TIMER
```

---

## Byggkonfigurasjon

### Katalogstruktur

```
scintilla/
+-- aros/
|   +-- PlatAROS.cxx         (ny - tegning og fonter)
|   +-- ScintillaAROS.cxx    (ny - MUI widget)
|   +-- ScintillaAROS.h      (ny - offentlig header)
+-- src/                     (uendret)
+-- include/                 (uendret)
+-- mmakefile.src            (ny - AROS build)
```

### mmakefile.src (skisse)

```makefile
include $(SRCDIR)/config/aros-contrib.cfg

#MM- ports-text : ports-text-scintilla
#MM  ports-text-scintilla : includes linklibs

SCINTILLA_SRC := $(PORTSDIR)/scintilla/scintilla

USER_CPPFLAGS := -std=c++17 -DPLAT_AROS \
    -I$(SCINTILLA_SRC)/include \
    -I$(SCINTILLA_SRC)/src \
    -I$(AROS_INCLUDES)/libraries

FILES_CORE := \
    AutoComplete CaseConvert CaseFolder CellBuffer CharClassify \
    CharacterCategoryMap CharacterType ContractionState DBCS \
    Decoration Document EditModel EditView Editor Indicator \
    KeyMap LineMarker MarginView PerLine PositionCache RESearch \
    RunStyles ScintillaBase Selection Style UniConversion \
    UndoHistory UniqueString ViewStyle XPM

FILES_PLATFORM := \
    aros/PlatAROS \
    aros/ScintillaAROS

FILES := \
    $(addprefix $(SCINTILLA_SRC)/src/, $(FILES_CORE)) \
    $(addprefix $(SCINTILLA_SRC)/, $(FILES_PLATFORM))

%build_lib mmake=ports-text-scintilla \
    libname=scintilla files=$(FILES) \
    uselibs="zunegfx"
```

---

## Implementasjonsrekkefølge

### Fase 1 — Minimal prototype

1. `PlatAROS.cxx`: `Font::Allocate`, `Surface::Init`, `FillRectangle`, `DrawTextNoClip`, `WidthText`, `Ascent/Descent/Height`
2. `ScintillaAROS.cxx`: MUI Area-subklasse med `MUIM_Draw` og `MUIM_HandleInput`
3. Verifiser at tekst vises og grunnleggende tastaturinput fungerer

### Fase 2 — Full funksjonalitet

4. Alle resterende `Surface`-metoder (linjer, gradienter, alpha, bilder)
5. Clipboard (`clipboard.device` + IFF FTXT)
6. Scrollbars via MUI `Prop`-objekter
7. Timer for markørblinking

### Fase 3 — Polering

8. `ListBox` for autocomplete-popup (MUI List-objekt i popup-vindu)
9. `Menu` for høyreklikk-kontekstmeny (Intuition-menyer)
10. Ytelsesoptimalisering med zunegfx batching

---

## Kjente utfordringer

### C++17 på AROS

Scintilla bruker `std::optional`, `std::string_view`, structured bindings og
andre C++17-funksjoner. AROS sin GCC-toolchain må støtte C++17 (`-std=c++17`).
Verifiser med en enkel test-kompilering av `src/Editor.cxx` mot AROS-toolchain.

### IScreenLineLayout

Scintilla 5.x bruker dette interfacet for bi-directional tekst og Unicode-layout
(tilsvarende HarfBuzz/Pango i GTK). For initial port kan en minimal stub brukes
som bare støtter venstre-til-høyre ASCII/Latin.

### Proporsjonal tekst og MeasureWidths

`MeasureWidths` må gi kumulativ bredde for hvert tegn i en streng. For
proporsjonale fonter krever dette et `ZuneTextLength`-kall per prefiks.
Dette er O(n²) i verste fall — kan optimeres ved å begrense til monospace-fonter
i første omgang (der `AverageCharWidth * i` gir bredden direkte).
