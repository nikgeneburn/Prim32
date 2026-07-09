// ============================================================================
// Prim32 — extreme-throughput immediate-mode GUI for Direct3D 12
// ----------------------------------------------------------------------------
// Design for maximum primitives/frame at minimum cost:
//
//   * Every drawable (rect, rounded rect, circle, line, glyph, image, shadow)
//     is ONE 32-byte GPU primitive. No CPU tessellation, no vertex/index
//     buffers. A rounded rect that costs Dear ImGui ~40 vertices (~1.3 KB)
//     costs Prim32 32 bytes.
//   * Widgets write primitives DIRECTLY into persistently-mapped GPU upload
//     memory (write-combined). Zero intermediate draw lists, zero copies.
//   * The GPU expands primitives in the vertex shader (vertex pulling from a
//     StructuredBuffer via SV_VertexID). One PSO, one root signature, ~one
//     DrawInstanced for the whole UI (one per overlapping window range).
//   * Rounded corners / circles / lines / borders / shadows are analytic
//     signed-distance fields evaluated in the pixel shader: perfect AA,
//     no MSAA, no geometry cost.
//   * Clipping happens in the vertex shader by clamping quads against a clip
//     table — zero scissor changes, the draw never breaks.
//
// Threading: one context = one thread (like Dear ImGui).
// ============================================================================
#pragma once
#include <stdint.h>
#include <stddef.h>

// windows.h #defines DrawText — we want the name. Include windows.h BEFORE
// this header (or use ::DrawTextW explicitly for the Win32 one).
#ifdef DrawText
#undef DrawText
#endif

#ifndef PRIM32_API
#define PRIM32_API
#endif

namespace prim32 {

// ---------------------------------------------------------------- basic types
struct Vec2 { float x, y; };
static inline Vec2 V2(float x, float y) { return Vec2{ x, y }; }

// Color: 0xAABBGGRR in memory => byte order R,G,B,A (same as IM_COL32).
typedef uint32_t Col;
static inline Col COL32(uint32_t r, uint32_t g, uint32_t b, uint32_t a = 255) {
    return (a << 24) | (b << 16) | (g << 8) | r;
}
static inline Col ColA(Col c, uint32_t a) { return (c & 0x00FFFFFFu) | (a << 24); }

// ------------------------------------------------------------- GPU primitive
// The unit of everything. 32 bytes, written once, read by the vertex shader.
enum PrimType : uint32_t {
    PRIM_RECT          = 0,  // solid rect                (no params)
    PRIM_GLYPH         = 1,  // font-atlas alpha * color  (uv0/uv1 = atlas uvs)
    PRIM_RECT_ROUNDED  = 2,  // SDF fill                  (pa=radius, color2=gradient bottom)
    PRIM_RECT_STROKE   = 3,  // SDF border                (pa=radius, pb=thickness)
    PRIM_CIRCLE        = 4,  // SDF fill, r=min half-ext  (color2=gradient bottom)
    PRIM_CIRCLE_STROKE = 5,  // SDF ring                  (pb=thickness)
    PRIM_LINE          = 6,  // capsule x0y0->x1y1        (pb=thickness)
    PRIM_IMAGE         = 7,  // rgba texture * color      (uv0/uv1 = uvs, tex=slot)
    PRIM_SHADOW        = 8,  // rounded-box gaussian falloff (pa=radius, pb=softness)
};

// meta word: [0:4]=type  [4:16]=clip index (4096)  [16:24]=texture slot  [24:32]=reserved
struct Prim {
    float    x0, y0, x1, y1;   // dest rect in pixels (line: the two endpoints)
    uint32_t uv0, uv1;         // textured: unorm16x2 uv min/max
                               // SDF:      uv0 = f16 pa | f16 pb, uv1 = color2
    Col      color;
    uint32_t meta;
};
static_assert(sizeof(Prim) == 32, "Prim must be exactly 32 bytes");

struct ClipRect { float x0, y0, x1, y1; };
struct Rect { float x, y, w, h; };      // position + size

// ------------------------------------------------------- resource handles
// Generational: cheap to copy/compare, safe to hold across Destroy calls
// ({index,gen} mismatch simply resolves to invalid).  {0,0} = invalid.
struct ImageHandle { uint32_t index, gen; };
struct FontHandle  { uint32_t index, gen; };
constexpr ImageHandle InvalidImageHandle = { 0, 0 };
constexpr FontHandle  InvalidFontHandle  = { 0, 0 };
inline bool operator==(ImageHandle a, ImageHandle b) { return a.index == b.index && a.gen == b.gen; }
inline bool operator!=(ImageHandle a, ImageHandle b) { return !(a == b); }
inline bool operator==(FontHandle a, FontHandle b)   { return a.index == b.index && a.gen == b.gen; }
inline bool operator!=(FontHandle a, FontHandle b)   { return !(a == b); }

// ----------------------------------------------------------- text options
enum TextAlign  : uint8_t { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT };
enum TextVAlign : uint8_t { VALIGN_TOP, VALIGN_MIDDLE, VALIGN_BOTTOM };
struct TextOptions {
    Col        color       = 0;            // 0 = style text color
    TextAlign  align       = ALIGN_LEFT;
    TextVAlign valign      = VALIGN_TOP;
    float      wrapWidth   = 0;            // 0 = no wrap (\n still honored)
    bool       clip        = false;        // clip to the bounds rect
    float      lineSpacing = 1.0f;         // multiplier on font line height
    // TODO: bool ellipsis — planned, see README
};

static inline uint32_t PackMeta(uint32_t type, uint32_t clip, uint32_t tex) {
    return type | (clip << 4) | (tex << 16);
}
// fast f32->f16 (values are small positive pixel counts; no inf/nan handling needed)
static inline uint32_t F16(float f) {
    union { float f; uint32_t u; } v; v.f = f * 1.0f;
    uint32_t u = v.u, s = (u >> 16) & 0x8000u;
    int32_t  e = (int32_t)((u >> 23) & 0xFF) - 127 + 15;
    if (e <= 0) return s;
    if (e >= 31) return s | 0x7BFFu;
    return s | (uint32_t)(e << 10) | ((u >> 13) & 0x3FFu);
}
static inline uint32_t PackF16x2(float a, float b) { return F16(a) | (F16(b) << 16); }
static inline uint32_t PackUV(float u, float v) {   // unorm16x2
    return (uint32_t)(u * 65535.0f + 0.5f) | ((uint32_t)(v * 65535.0f + 0.5f) << 16);
}

// ------------------------------------------------------------------ font data
struct Glyph {
    uint32_t uv0, uv1;      // pre-packed atlas uvs (unorm16x2)
    float    x0, y0, x1, y1;// draw rect relative to pen (x, baseline)
    float    advance;
};
struct FontAtlas {
    uint8_t* pixels;        // R8, owned by context
    int      width, height;
    Glyph    glyphs[224];   // codepoints 32..255
    float    ascent, descent, lineHeight, size;
    uint32_t kernCount;
    uint32_t* kernKeys;     // (a<<16)|b, sorted
    float*    kernVals;
};

// ------------------------------------------------------------------------ IO
struct IO {
    Vec2  displaySize;
    Vec2  mousePos;
    bool  mouseDown[3];
    float mouseWheel;
    float deltaTime;
};

// --------------------------------------------------------------------- style
enum ColorId : int {
    ZC_WindowBg, ZC_TitleBar, ZC_TitleBarActive, ZC_Border,
    ZC_FrameBg, ZC_FrameBgHovered, ZC_FrameBgActive,
    ZC_Button, ZC_ButtonHovered, ZC_ButtonActive,
    ZC_SliderGrab, ZC_SliderGrabActive, ZC_CheckMark,
    ZC_Text, ZC_TextDim, ZC_ProgressFill, ZC_COUNT
};
struct Style {
    Col   colors[ZC_COUNT];
    float windowRounding, frameRounding;
    Vec2  windowPadding, framePadding, itemSpacing;
    float titleBarHeight, frameHeight, borderSize;
    PRIM32_API void Dark();   // default dark theme
};

// ------------------------------------------------------------------- windows
enum WindowFlags : uint32_t {
    WF_None        = 0,
    WF_NoTitleBar  = 1 << 0,
    WF_NoMove      = 1 << 1,
    WF_NoBackground= 1 << 2,
    WF_AutoSize    = 1 << 3,
    WF_Topmost     = 1 << 4,    // always drawn last (overlays)
    WF_NoBringToFront = 1 << 5,
};

struct Window {
    uint32_t id;
    char     name[24];          // visible label (for GPU zones / debugging)
    Vec2     pos, size;
    Vec2     cursor, lineStart;  // layout
    float    lineH, prevLineH;
    Vec2     contentMax;         // for autosize
    Vec2     autoSizeStore;
    uint32_t flags;
    uint32_t primStart, primEnd; // contiguous range in this frame's prim stream
    uint32_t clipIdx;
    int      z;                  // draw order, higher = on top
    uint32_t lastFrame;
    bool     inUse;
};

// ------------------------------------------------------------------- layers
// Draw order: BACKGROUND items -> windows (z-sorted) -> FOREGROUND items.
enum LayerId : uint8_t { LAYER_BACKGROUND = 0, LAYER_FOREGROUND = 1 };

// A cached layer is a retained block of primitives. Record once with
// BeginCache/EndCache (any draw API, layer-local coordinates), then
// DrawCached() every frame: zero CPU emit, zero streaming — the backend
// keeps the prims in a VRAM buffer and re-uploads only when the version
// changes. Moving a cached layer is free: the offset is a root constant.
struct CachedLayer {
    Prim*     prims;          // CPU staging (authoritative copy)
    uint32_t  primCount, primCap;
    ClipRect  clips[16];      // layer-local clip rects captured at bake
    uint32_t  clipCount;
    uint32_t  id;             // backend slot
    uint32_t  version;        // bumped by EndCache -> backend re-uploads
    const char* name;         // for GPU zones
    bool      recording;
};

// ------------------------------------------------------------------ draw data
struct DrawRange {
    uint32_t start, count;
    const char* label;
    const CachedLayer* cache;   // null = dynamic (this frame's ring)
    float ox, oy;               // offset applied in the vertex shader
    uint32_t clipBase;          // added to prim clip indices (cache rebase)
};
struct DrawData {
    DrawRange ranges[160];
    uint32_t  rangeCount;
    uint32_t  totalPrims;
    uint32_t  clipCount;
    Vec2      displaySize;
};

struct Stats {
    uint32_t prims, clips, ranges;
    float    buildMs;         // CPU time NewFrame..Render this frame
    bool     overflow;        // prim buffer ran out (increase backend capacity)
};

// ------------------------------------------------------------------ DrawList
// The immediate drawing interface:  auto* draw = prim32::GetDrawList();
// Primitive methods are zero-cost inline forwards to the 32-byte emitters.
// Image/Text resolve generational handles (array index + gen check, ~2 ns)
// and NEVER load, decode, or create GPU resources — that is the resource
// layer's job (LoadImageFromFile / LoadFontFromMemory / ...).
struct Context;
struct DrawList {
    Context* ctx;

    // ---- primitives (inline, defined after the emitters below)
    void FilledRect(const prim32::Rect& r, Col col, float rounding = 0.0f);
    void Rect(const prim32::Rect& r, Col col, float thickness = 1.0f, float rounding = 0.0f);
    void Line(Vec2 a, Vec2 b, Col col, float thickness = 1.0f);
    void Circle(Vec2 center, float radius, Col col, float thickness = 1.0f);
    void FilledCircle(Vec2 center, float radius, Col col);
    void Shadow(const prim32::Rect& r, Col col, float rounding, float softness);
    // NOTE: Triangle is intentionally absent — arbitrary triangles do not fit
    // the 32-byte quad-primitive model (see README).

    // ---- images (resolve handle -> texture slot -> one PRIM_IMAGE)
    PRIM32_API void Image(ImageHandle img, const prim32::Rect& r);
    PRIM32_API void Image(ImageHandle img, const prim32::Rect& r, Col tint);
    PRIM32_API void Image(ImageHandle img, const prim32::Rect& r, Vec2 uv0, Vec2 uv1, Col tint = 0xFFFFFFFFu);
    PRIM32_API void Image(ImageHandle img, Vec2 pos);                 // natural size
    PRIM32_API void Image(ImageHandle img, Vec2 pos, Vec2 size, Col tint = 0xFFFFFFFFu);

    // ---- text. Without a font: font stack top > default font > built-in.
    // Without a color: the style's text color.
    PRIM32_API void Text(const char* text, Vec2 pos);
    PRIM32_API void Text(const char* text, Vec2 pos, Col color);
    PRIM32_API void Text(FontHandle font, const char* text, Vec2 pos);
    PRIM32_API void Text(FontHandle font, const char* text, Vec2 pos, Col color);
    PRIM32_API void Text(const char* text, const prim32::Rect& bounds, const TextOptions& opt);
    PRIM32_API void Text(FontHandle font, const char* text, const prim32::Rect& bounds, const TextOptions& opt);

    // ---- measurement (matches rendering: same font resolution)
    PRIM32_API Vec2 MeasureText(const char* text);
    PRIM32_API Vec2 MeasureText(FontHandle font, const char* text);

    // ---- clipping (forwards to the clip stack; VS-clipped, no draw breaks)
    void PushClipRect(const prim32::Rect& r, bool intersect = true);
    void PopClipRect();
};

// -------------------------------------------------------------------- context
// Fully public: hot-path emitters are inlined below and need the fields.
struct Context {
    // ---- frame memory (points into the backend's mapped GPU upload buffer)
    Prim*     prims;      uint32_t primCount, primCap;
    ClipRect* clips;      uint32_t clipCount, clipCap;
    uint32_t  curClip;
    bool      overflow;

    IO        io;
    Style     style;
    FontAtlas font;

    // ---- interaction
    uint32_t hotId, activeId, activeWindow, hoveredWindow;
    Vec2     activeGrabOff;       // drag anchor
    bool     mouseClicked[3], mouseReleased[3];
    Vec2     mouseClickPos;

    // ---- windows
    Window   windows[64];
    uint32_t windowCount;
    Window*  curWindow;
    int      zNext;
    uint32_t frameNum;

    // ---- fonts (resource system)
    FontHandle defaultFont;
    FontHandle fontStack[16];
    int        fontStackTop;      // -1 = empty

    // ---- layers / cache recording
    LayerId      curLayer;
    CachedLayer* recLayer;
    Prim*        savePrims;  uint32_t savePrimCount, savePrimCap;
    ClipRect*    saveClips;  uint32_t saveClipCount, saveClipCap, saveCurClip;
    int          saveClipStackTop;

    // ---- id stack
    uint32_t idStack[32]; int idStackTop;

    // ---- clip stack
    uint32_t clipStack[32]; int clipStackTop;

    // ---- last item
    struct { float x0, y0, x1, y1; uint32_t id; bool hovered, active, clicked; } lastItem;
    bool lastItemFromBehavior;   // set by ButtonBehavior, consumed by ItemAdd

    // ---- misc
    char      textScratch[4096];
    DrawList  drawList;           // returned by GetDrawList()
    DrawData  drawData;
    Stats     stats;
    int64_t   frameT0;
    bool      insideFrame;
};

// ============================================================ lifecycle / frame
struct FontDesc { const wchar_t* face; float sizePx; bool kerning; };

PRIM32_API Context* CreateContext(const FontDesc* font = nullptr); // bakes atlas (GDI)
PRIM32_API void     DestroyContext(Context* ctx);
PRIM32_API Context* GetContext();
PRIM32_API void     SetContext(Context* ctx);

// Backend hands the context raw mapped GPU memory for this frame:
struct FrameMem { Prim* prims; uint32_t primCap; ClipRect* clips; uint32_t clipCap; };
PRIM32_API void      NewFrame(const FrameMem& mem, const IO& io);
PRIM32_API DrawData* EndFrame();   // sorts window ranges by z; returns draw data
PRIM32_API Stats     GetStats();

// ============================================================== immediate draw
// The hot path. These compile to ~a dozen instructions each: pack fields and
// do one 32-byte store into write-combined GPU memory. NEVER read *p back.
static inline Prim* AddPrims(Context* c, uint32_t n) {
    uint32_t i = c->primCount;
    if (i + n > c->primCap) { c->overflow = true; return nullptr; }
    c->primCount = i + n;
    return c->prims + i;
}
static inline void DrawRect(Context* c, float x0, float y0, float x1, float y1, Col col) {
    Prim* p = AddPrims(c, 1); if (!p) return;
    p->x0 = x0; p->y0 = y0; p->x1 = x1; p->y1 = y1;
    p->uv0 = 0; p->uv1 = 0; p->color = col;
    p->meta = PackMeta(PRIM_RECT, c->curClip, 0);
}
static inline void DrawRectRounded(Context* c, float x0, float y0, float x1, float y1,
                                   Col col, float radius, Col gradBottom = 0) {
    Prim* p = AddPrims(c, 1); if (!p) return;
    p->x0 = x0; p->y0 = y0; p->x1 = x1; p->y1 = y1;
    p->uv0 = PackF16x2(radius, 0.0f); p->uv1 = gradBottom; p->color = col;
    p->meta = PackMeta(PRIM_RECT_ROUNDED, c->curClip, 0);
}
static inline void DrawRectStroke(Context* c, float x0, float y0, float x1, float y1,
                                  Col col, float radius, float thickness) {
    Prim* p = AddPrims(c, 1); if (!p) return;
    p->x0 = x0; p->y0 = y0; p->x1 = x1; p->y1 = y1;
    p->uv0 = PackF16x2(radius, thickness); p->uv1 = 0; p->color = col;
    p->meta = PackMeta(PRIM_RECT_STROKE, c->curClip, 0);
}
static inline void DrawCircle(Context* c, float cx, float cy, float r, Col col, Col gradBottom = 0) {
    Prim* p = AddPrims(c, 1); if (!p) return;
    p->x0 = cx - r; p->y0 = cy - r; p->x1 = cx + r; p->y1 = cy + r;
    p->uv0 = 0; p->uv1 = gradBottom; p->color = col;
    p->meta = PackMeta(PRIM_CIRCLE, c->curClip, 0);
}
static inline void DrawCircleStroke(Context* c, float cx, float cy, float r, Col col, float thickness) {
    Prim* p = AddPrims(c, 1); if (!p) return;
    p->x0 = cx - r; p->y0 = cy - r; p->x1 = cx + r; p->y1 = cy + r;
    p->uv0 = PackF16x2(0.0f, thickness); p->uv1 = 0; p->color = col;
    p->meta = PackMeta(PRIM_CIRCLE_STROKE, c->curClip, 0);
}
static inline void DrawLine(Context* c, float x0, float y0, float x1, float y1, Col col, float thickness) {
    Prim* p = AddPrims(c, 1); if (!p) return;
    p->x0 = x0; p->y0 = y0; p->x1 = x1; p->y1 = y1;   // endpoints; VS builds the AABB
    p->uv0 = PackF16x2(0.0f, thickness); p->uv1 = 0; p->color = col;
    p->meta = PackMeta(PRIM_LINE, c->curClip, 0);
}
static inline void DrawShadow(Context* c, float x0, float y0, float x1, float y1,
                              Col col, float radius, float softness) {
    Prim* p = AddPrims(c, 1); if (!p) return;
    p->x0 = x0; p->y0 = y0; p->x1 = x1; p->y1 = y1;
    p->uv0 = PackF16x2(radius, softness); p->uv1 = 0; p->color = col;
    p->meta = PackMeta(PRIM_SHADOW, c->curClip, 0);
}
static inline void DrawImage(Context* c, float x0, float y0, float x1, float y1,
                             uint32_t texSlot, Col tint = 0xFFFFFFFFu,
                             float u0 = 0, float v0 = 0, float u1 = 1, float v1 = 1) {
    Prim* p = AddPrims(c, 1); if (!p) return;
    p->x0 = x0; p->y0 = y0; p->x1 = x1; p->y1 = y1;
    p->uv0 = PackUV(u0, v0); p->uv1 = PackUV(u1, v1); p->color = tint;
    p->meta = PackMeta(PRIM_IMAGE, c->curClip, texSlot);
}

PRIM32_API void  DrawText(Context* c, float x, float y, Col col, const char* text, const char* end = nullptr);
PRIM32_API void  DrawText(Context* c, FontHandle font, float x, float y, Col col, const char* text, const char* end = nullptr);
PRIM32_API Vec2  TextSize(Context* c, const char* text, const char* end = nullptr);

// ---- DrawList inline primitive bodies (zero-cost forwards)
inline void DrawList::FilledRect(const prim32::Rect& r, Col col, float rounding) {
    if (rounding > 0) DrawRectRounded(ctx, r.x, r.y, r.x + r.w, r.y + r.h, col, rounding);
    else              DrawRect(ctx, r.x, r.y, r.x + r.w, r.y + r.h, col);
}
inline void DrawList::Rect(const prim32::Rect& r, Col col, float thickness, float rounding) {
    DrawRectStroke(ctx, r.x, r.y, r.x + r.w, r.y + r.h, col, rounding, thickness);
}
inline void DrawList::Line(Vec2 a, Vec2 b, Col col, float thickness) {
    DrawLine(ctx, a.x, a.y, b.x, b.y, col, thickness);
}
inline void DrawList::Circle(Vec2 c, float radius, Col col, float thickness) {
    DrawCircleStroke(ctx, c.x, c.y, radius, col, thickness);
}
inline void DrawList::FilledCircle(Vec2 c, float radius, Col col) {
    DrawCircle(ctx, c.x, c.y, radius, col);
}
inline void DrawList::Shadow(const prim32::Rect& r, Col col, float rounding, float softness) {
    DrawShadow(ctx, r.x, r.y, r.x + r.w, r.y + r.h, col, rounding, softness);
}

PRIM32_API DrawList* GetDrawList();   // the current context's draw list

PRIM32_API void  PushClip(float x0, float y0, float x1, float y1, bool intersect = true);
PRIM32_API void  PopClip();

inline void DrawList::PushClipRect(const prim32::Rect& r, bool intersect) { PushClip(r.x, r.y, r.x + r.w, r.y + r.h, intersect); }
inline void DrawList::PopClipRect() { PopClip(); }

// Bounded/aligned/wrapped text (namespace form; DrawList::Text wraps this)
PRIM32_API void DrawTextOpt(Context* c, FontHandle font, const char* text, const prim32::Rect& bounds, const TextOptions& opt);

// ---- layers: arbitrary drawing under / above all windows
PRIM32_API void  BeginLayer(LayerId layer);   // FOREGROUND draws above everything
PRIM32_API void  EndLayer();                  // back to BACKGROUND

// ---- cached (retained) layers
PRIM32_API CachedLayer* CreateCachedLayer(uint32_t maxPrims, const char* name);
PRIM32_API void  DestroyCachedLayer(CachedLayer* layer);
PRIM32_API void  BeginCache(CachedLayer* layer);  // redirect all draws into the layer
PRIM32_API void  EndCache();                      // bump version (re-upload once)
PRIM32_API void  DrawCached(CachedLayer* layer, Vec2 offset);  // free per frame

// ==================================================================== widgets
PRIM32_API bool  Begin(const char* name, uint32_t flags = 0);
PRIM32_API void  End();
PRIM32_API void  SetNextWindowPos(Vec2 pos, bool always = false);
PRIM32_API void  SetNextWindowSize(Vec2 size, bool always = false);

PRIM32_API void  Text(const char* text);
PRIM32_API void  TextColored(Col col, const char* text);
PRIM32_API void  TextF(const char* fmt, ...);
PRIM32_API bool  Button(const char* label, Vec2 size = { 0, 0 });
PRIM32_API bool  Checkbox(const char* label, bool* v);
PRIM32_API bool  SliderFloat(const char* label, float* v, float vMin, float vMax, const char* fmt = "%.2f");
PRIM32_API bool  SliderInt(const char* label, int* v, int vMin, int vMax);
PRIM32_API void  ProgressBar(float frac, Vec2 size = { 0, 0 });
PRIM32_API void  Separator();
PRIM32_API void  SameLine(float spacing = -1.0f);
PRIM32_API void  Spacing();
PRIM32_API bool  InvisibleButton(const char* id, Vec2 size);
PRIM32_API void  Dummy(Vec2 size);

PRIM32_API bool  IsItemHovered();
PRIM32_API bool  IsItemActive();
PRIM32_API Vec2  GetCursorScreenPos();
PRIM32_API void  SetCursorScreenPos(Vec2 p);
PRIM32_API Vec2  GetContentAvail();
PRIM32_API float GetFrameHeight();

PRIM32_API void  PushID(const char* id);
PRIM32_API void  PushID(int id);
PRIM32_API void  PopID();

// ================================================================ resources
// Loading happens HERE, never in draw calls: decode once, upload once, then
// draw with the handle every frame for free. All loaders copy what they
// need — the caller may free `data` as soon as the call returns.
// On failure they return the Invalid handle; see GetLastResourceError().
PRIM32_API ImageHandle LoadImageFromFile(const char* path);                     // PNG/JPG/BMP/GIF/TIFF (WIC)
PRIM32_API ImageHandle LoadImageFromMemory(const void* data, size_t size);      // encoded bytes
PRIM32_API void        DestroyImage(ImageHandle image);
PRIM32_API bool        IsValid(ImageHandle image);
PRIM32_API Vec2        GetImageSize(ImageHandle image);

PRIM32_API FontHandle  LoadFontFromFile(const char* path, float sizePixels);    // TTF/OTF/TTC
PRIM32_API FontHandle  LoadFontFromMemory(const void* data, size_t size, float sizePixels);
PRIM32_API void        DestroyFont(FontHandle font);
PRIM32_API bool        IsValid(FontHandle font);

// Font selection for draw->Text(...) without an explicit font:
//   1. explicit font argument   2. PushFont stack top
//   3. SetDefaultFont           4. built-in font (baked at CreateContext)
PRIM32_API void        SetDefaultFont(FontHandle font);
PRIM32_API FontHandle  GetDefaultFont();
PRIM32_API void        PushFont(FontHandle font);   // invalid handles rejected
PRIM32_API void        PopFont();                   // underflow-safe

PRIM32_API Vec2        MeasureText(const char* text, const char* end = nullptr);            // current font
PRIM32_API Vec2        MeasureText(FontHandle font, const char* text, const char* end = nullptr);
PRIM32_API float       GetFontLineHeight(FontHandle font = InvalidFontHandle);

PRIM32_API const char* GetLastResourceError();      // "" when none; static storage
struct ResourceStats { uint32_t images, fonts; uint64_t textureBytes, atlasBytes; };
PRIM32_API ResourceStats GetResourceStats();

} // namespace prim32
