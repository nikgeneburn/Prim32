// ============================================================================
// Prim32 core — context, font baking (GDI), text, widgets.
// The primitive emitters live inline in prim32.h; this file is everything else.
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#ifdef DrawText
#undef DrawText
#endif

#include <prim32/prim32.h>
#include "prim32_internal.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

namespace prim32 {

static Context* g = nullptr;
Context* GetContext()            { return g; }
void     SetContext(Context* c)  { g = c; }

// ------------------------------------------------------------------- hashing
static inline uint32_t Hash(const char* s, const char* end, uint32_t seed) {
    uint32_t h = seed ? seed : 2166136261u;
    while (s != end && *s) h = (h ^ (uint8_t)*s++) * 16777619u;
    return h ? h : 1;
}
static uint32_t GetID(const char* label) {
    uint32_t seed = g->idStackTop >= 0 ? g->idStack[g->idStackTop] : 0;
    return Hash(label, nullptr, seed);
}
// "visible##id" — hash the whole string, display only the part before "##"
static const char* LabelEnd(const char* s) {
    const char* p = strstr(s, "##");
    return p ? p : s + strlen(s);
}

void PushID(const char* id) { if (g->idStackTop < 31) g->idStack[++g->idStackTop] = GetID(id); }
void PushID(int id) { char b[16]; snprintf(b, 16, "%d", id); PushID(b); }
void PopID()        { if (g->idStackTop >= 0) g->idStackTop--; }

// --------------------------------------------------------------------- style
void Style::Dark() {
    windowRounding = 7.0f; frameRounding = 4.0f;
    windowPadding = { 10, 8 }; framePadding = { 8, 4 }; itemSpacing = { 8, 5 };
    titleBarHeight = 26.0f; frameHeight = 24.0f; borderSize = 1.0f;
    colors[ZC_WindowBg]        = COL32(21, 23, 28, 245);
    colors[ZC_TitleBar]        = COL32(31, 34, 42, 255);
    colors[ZC_TitleBarActive]  = COL32(46, 62, 92, 255);
    colors[ZC_Border]          = COL32(70, 76, 90, 128);
    colors[ZC_FrameBg]         = COL32(38, 42, 52, 255);
    colors[ZC_FrameBgHovered]  = COL32(50, 56, 70, 255);
    colors[ZC_FrameBgActive]   = COL32(60, 68, 86, 255);
    colors[ZC_Button]          = COL32(52, 84, 138, 255);
    colors[ZC_ButtonHovered]   = COL32(66, 104, 168, 255);
    colors[ZC_ButtonActive]    = COL32(88, 130, 200, 255);
    colors[ZC_SliderGrab]      = COL32(94, 134, 198, 255);
    colors[ZC_SliderGrabActive]= COL32(130, 170, 234, 255);
    colors[ZC_CheckMark]       = COL32(130, 180, 255, 255);
    colors[ZC_Text]            = COL32(226, 229, 235, 255);
    colors[ZC_TextDim]         = COL32(140, 146, 158, 255);
    colors[ZC_ProgressFill]    = COL32(74, 144, 110, 255);
}

// ===== PRIM32_PURE_TEXT_BEGIN (no OS deps; extracted by the native tests) =====
static inline float Kern(const FontAtlas* fa, uint32_t a, uint32_t b) {
    if (!fa->kernCount) return 0.0f;
    uint32_t key = (a << 16) | b, lo = 0, hi = fa->kernCount;
    while (lo < hi) { uint32_t mid = (lo + hi) >> 1; if (fa->kernKeys[mid] < key) lo = mid + 1; else hi = mid; }
    return (lo < fa->kernCount && fa->kernKeys[lo] == key) ? fa->kernVals[lo] : 0.0f;
}

// UTF-8 decode, one codepoint. Advances *s. Malformed bytes -> U+FFFD.
uint32_t Prim32Utf8Next(const char** s, const char* end) {
    const uint8_t* p = (const uint8_t*)*s;
    uint8_t c = *p;
    if (c < 0x80) { *s += 1; return c; }
    if ((c >> 5) == 0x6 && *s + 1 < end && (p[1] & 0xC0) == 0x80) {
        *s += 2; return ((uint32_t)(c & 0x1F) << 6) | (p[1] & 0x3F);
    }
    if ((c >> 4) == 0xE && *s + 2 < end && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        *s += 3; return ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    }
    if ((c >> 3) == 0x1E && *s + 3 < end && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
        *s += 4; return ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12)
                      | ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
    }
    *s += 1;
    return 0xFFFD;
}

// cp -> glyph pool index (0 = miss). ASCII takes the direct array.
uint32_t Prim32CacheFind(const FontAtlas* fa, uint32_t cp) {
    if (cp >= 32 && cp < 127) return fa->asciiMap[cp - 32];
    if (!fa->cpCap) return 0;
    uint32_t h = (cp * 2654435761u) & (fa->cpCap - 1);
    for (;;) {
        uint32_t k = fa->cpKeys[h];
        if (k == cp) return fa->cpVals[h];
        if (k == 0xFFFFFFFFu) return 0;
        h = (h + 1) & (fa->cpCap - 1);
    }
}

// insert cp -> pool index (grows + rehashes at 70% load)
bool Prim32CacheInsert(FontAtlas* fa, uint32_t cp, uint32_t glyphIdx) {
    if (cp >= 32 && cp < 127) { fa->asciiMap[cp - 32] = glyphIdx; return true; }
    if (fa->cpCount * 10 >= fa->cpCap * 7) {
        uint32_t newCap = fa->cpCap ? fa->cpCap * 2 : 256;
        uint32_t* nk = (uint32_t*)malloc(newCap * 4);
        uint32_t* nv = (uint32_t*)malloc(newCap * 4);
        if (!nk || !nv) { free(nk); free(nv); return false; }
        memset(nk, 0xFF, newCap * 4);
        for (uint32_t i = 0; i < fa->cpCap; i++) {
            if (fa->cpKeys[i] == 0xFFFFFFFFu) continue;
            uint32_t h = (fa->cpKeys[i] * 2654435761u) & (newCap - 1);
            while (nk[h] != 0xFFFFFFFFu) h = (h + 1) & (newCap - 1);
            nk[h] = fa->cpKeys[i]; nv[h] = fa->cpVals[i];
        }
        free(fa->cpKeys); free(fa->cpVals);
        fa->cpKeys = nk; fa->cpVals = nv; fa->cpCap = newCap;
    }
    uint32_t h = (cp * 2654435761u) & (fa->cpCap - 1);
    while (fa->cpKeys[h] != 0xFFFFFFFFu) {
        if (fa->cpKeys[h] == cp) { fa->cpVals[h] = glyphIdx; return true; }
        h = (h + 1) & (fa->cpCap - 1);
    }
    fa->cpKeys[h] = cp; fa->cpVals[h] = glyphIdx; fa->cpCount++;
    return true;
}

// shelf packer on the CURRENT page. false = page full (caller opens a new one).
bool Prim32PackRect(FontAtlas* fa, int w, int h, int* outX, int* outY) {
    if (w + 4 > fa->pageW || h + 4 > fa->pageH) return false;      // absurd glyph
    if (fa->penX + w + 2 > fa->pageW) {                             // new shelf
        fa->penX = 2;
        fa->penY += fa->shelfH + 2;
        fa->shelfH = 0;
    }
    if (fa->penY + h + 2 > fa->pageH) return false;                 // page full
    *outX = fa->penX; *outY = fa->penY;
    fa->penX += w + 2;
    if (h > fa->shelfH) fa->shelfH = h;
    return true;
}

// breakable before CJK ideographs / kana / fullwidth forms
static inline bool CjkBreakable(uint32_t cp) {
    return (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0xFF00 && cp <= 0xFFEF) || (cp >= 0x20000 && cp <= 0x3FFFF);
}

// width of a single line, stopping at \n or `end`
float Prim32LineWidth(FontAtlas* fa, const char* s, const char* end) {
    float w = 0; uint32_t prev = 0;
    while (s < end && *s) {
        const char* at = s;
        uint32_t cp = Prim32Utf8Next(&s, end);
        (void)at;
        if (cp == (uint32_t)'\n') break;
        if (cp < 32) continue;
        Glyph* gl = Prim32GetGlyph(fa, cp);
        if (prev) w += Kern(fa, prev, cp);
        w += gl->advance;
        prev = cp < 0x10000 ? cp : 0;
    }
    return w;
}

// Greedy word wrap: returns the exclusive end of the next line. Breaks at \n
// always; at spaces or before CJK ideographs when wrapW > 0; mid-word as a
// last resort so progress is guaranteed.
const char* Prim32WrapBreak(FontAtlas* fa, const char* s, const char* end, float wrapW, float* outWidth) {
    float w = 0; uint32_t prev = 0;
    const char* lastBreak = nullptr; float wAtBreak = 0;
    const char* p = s;
    while (p < end && *p) {
        const char* at = p;
        uint32_t cp = Prim32Utf8Next(&p, end);
        if (cp == (uint32_t)'\n') { p = at; break; }
        if (cp < 32) continue;
        Glyph* gl = Prim32GetGlyph(fa, cp);
        float adv = (prev ? Kern(fa, prev, cp) : 0.0f) + gl->advance;
        if (wrapW > 0 && CjkBreakable(cp) && at > s) { lastBreak = at; wAtBreak = w; }
        if (wrapW > 0 && w + adv > wrapW && at > s) {
            if (lastBreak) { if (outWidth) *outWidth = wAtBreak; return lastBreak; }
            if (outWidth) *outWidth = w;
            return at;                                  // mid-word break
        }
        w += adv;
        prev = cp < 0x10000 ? cp : 0;
        if (cp == ' ') { lastBreak = at; wAtBreak = w - adv; }
    }
    if (outWidth) *outWidth = w;
    return p;
}

// full measurement: honors \n, optional wrap, line-spacing multiplier
Vec2 Prim32MeasureAtlas(FontAtlas* fa, const char* text, const char* end, float wrapW, float lineSpacing) {
    if (!end) end = text + strlen(text);
    float maxW = 0; int lines = 0;
    const char* s = text;
    while (s < end && *s) {
        float w; const char* brk = Prim32WrapBreak(fa, s, end, wrapW, &w);
        if (w > maxW) maxW = w;
        lines++;
        s = brk;
        if (s < end && *s == '\n') s++;
        else while (s < end && *s == ' ') s++;
    }
    if (lines == 0) lines = 1;
    return { maxW, fa->lineHeight * lineSpacing * lines };
}
// ===== PRIM32_PURE_TEXT_END =====

// ---------------------------------------------------------------------- text
// Hot loop: UTF-8 decode + cache lookup + one 32-byte store per glyph.
// Unseen glyphs rasterize once inside Prim32GetGlyph (any script, any plane).
static void DrawTextAtlas(Context* c, FontAtlas* fa, float x, float y, Col col,
                          const char* text, const char* end) {
    float penX = x, baseline = y + fa->ascent;
    const uint32_t clip = c->curClip;
    uint32_t prev = 0;
    if (!end) end = text + strlen(text);
    const char* s = text;
    while (s < end && *s) {
        uint32_t cp = Prim32Utf8Next(&s, end);
        if (cp == (uint32_t)'\n') { penX = x; baseline += fa->lineHeight; prev = 0; continue; }
        if (cp < 32) continue;
        Glyph* gl = Prim32GetGlyph(fa, cp);
        if (prev) penX += Kern(fa, prev, cp);
        if (gl->x1 > gl->x0) {
            Prim* p = AddPrims(c, 1); if (!p) return;
            p->x0 = penX + gl->x0; p->y0 = baseline + gl->y0;
            p->x1 = penX + gl->x1; p->y1 = baseline + gl->y1;
            p->uv0 = gl->uv0; p->uv1 = gl->uv1;
            p->color = col;
            p->meta = PackMeta(PRIM_GLYPH, clip, gl->texSlot);
        }
        penX += gl->advance;
        prev = cp < 0x10000 ? cp : 0;
    }
}

void DrawText(Context* c, float x, float y, Col col, const char* text, const char* end) {
    ResolvedFont rf = Prim32ResolveFont(InvalidFontHandle);
    DrawTextAtlas(c, rf.atlas, x, y, col, text, end);
}
void DrawText(Context* c, FontHandle font, float x, float y, Col col, const char* text, const char* end) {
    ResolvedFont rf = Prim32ResolveFont(font);
    DrawTextAtlas(c, rf.atlas, x, y, col, text, end);
}

Vec2 TextSize(Context* c, const char* text, const char* end) {
    (void)c;
    ResolvedFont rf = Prim32ResolveFont(InvalidFontHandle);
    return Prim32MeasureAtlas(rf.atlas, text, end, 0.0f, 1.0f);
}

// bounded / aligned / wrapped text
void DrawTextOpt(Context* c, FontHandle font, const char* text, const prim32::Rect& b, const TextOptions& opt) {
    if (!text || !*text) return;
    ResolvedFont rf = Prim32ResolveFont(font);
    Col col = opt.color ? opt.color : c->style.colors[ZC_Text];
    float lh = rf.atlas->lineHeight * opt.lineSpacing;
    float wrapW = opt.wrapWidth;
    Vec2 total = Prim32MeasureAtlas(rf.atlas, text, nullptr, wrapW, opt.lineSpacing);
    float y = b.y;
    if (opt.valign == VALIGN_MIDDLE)      y += (b.h - total.y) * 0.5f;
    else if (opt.valign == VALIGN_BOTTOM) y += b.h - total.y;
    if (opt.clip) PushClip(b.x, b.y, b.x + b.w, b.y + b.h, true);
    const char* s = text; const char* end = text + strlen(text);
    while (s < end && *s) {
        float w; const char* brk = Prim32WrapBreak(rf.atlas, s, end, wrapW, &w);
        float x = b.x;
        if (opt.align == ALIGN_CENTER)     x += (b.w - w) * 0.5f;
        else if (opt.align == ALIGN_RIGHT) x += b.w - w;
        DrawTextAtlas(c, rf.atlas, x, y, col, s, brk);
        y += lh;
        s = brk;
        if (s < end && *s == '\n') s++;
        else while (s < end && *s == ' ') s++;
    }
    if (opt.clip) PopClip();
}

// ------------------------------------------------------- DrawList text/misc
DrawList* GetDrawList() { return &g->drawList; }

void DrawList::Text(const char* t, Vec2 p)                 { prim32::DrawText(ctx, p.x, p.y, ctx->style.colors[ZC_Text], t); }
void DrawList::Text(const char* t, Vec2 p, Col col)        { prim32::DrawText(ctx, p.x, p.y, col, t); }
void DrawList::Text(FontHandle f, const char* t, Vec2 p)   { prim32::DrawText(ctx, f, p.x, p.y, ctx->style.colors[ZC_Text], t); }
void DrawList::Text(FontHandle f, const char* t, Vec2 p, Col col) { prim32::DrawText(ctx, f, p.x, p.y, col, t); }
void DrawList::Text(const char* t, const prim32::Rect& b, const TextOptions& o)               { DrawTextOpt(ctx, InvalidFontHandle, t, b, o); }
void DrawList::Text(FontHandle f, const char* t, const prim32::Rect& b, const TextOptions& o) { DrawTextOpt(ctx, f, t, b, o); }
Vec2 DrawList::MeasureText(const char* t)                  { return prim32::MeasureText(t); }
Vec2 DrawList::MeasureText(FontHandle f, const char* t)    { return prim32::MeasureText(f, t); }

// ------------------------------------------------------------------- clipping
// CPU keeps shadow copies for intersection — the GPU buffer is write-only!
static ClipRect s_clipShadow[32];

static uint32_t AddClip(const ClipRect& r) {
    if (g->clipCount >= g->clipCap) return g->curClip;
    g->clips[g->clipCount] = r;                    // single write to WC memory
    return g->clipCount++;
}
void PushClip(float x0, float y0, float x1, float y1, bool intersect) {
    ClipRect r = { x0, y0, x1, y1 };
    if (intersect && g->clipStackTop >= 0) {
        const ClipRect& c = s_clipShadow[g->clipStackTop];
        if (r.x0 < c.x0) r.x0 = c.x0;
        if (r.y0 < c.y0) r.y0 = c.y0;
        if (r.x1 > c.x1) r.x1 = c.x1;
        if (r.y1 > c.y1) r.y1 = c.y1;
    }
    if (r.x1 < r.x0) r.x1 = r.x0;
    if (r.y1 < r.y0) r.y1 = r.y0;
    uint32_t idx = AddClip(r);
    if (g->clipStackTop < 31) { g->clipStackTop++; s_clipShadow[g->clipStackTop] = r; g->clipStack[g->clipStackTop] = idx; }
    g->curClip = idx;
}
void PopClip() {
    if (g->clipStackTop > 0) g->clipStackTop--;
    g->curClip = g->clipStack[g->clipStackTop];
}
static const ClipRect& CurClipRect() { return s_clipShadow[g->clipStackTop]; }

// ------------------------------------------------------------------ lifecycle
Context* CreateContext(const FontDesc* fd) {
    Context* c = (Context*)calloc(1, sizeof(Context));
    c->style.Dark();
    FontDesc def = { L"Segoe UI", 17.0f, true };
    if (!fd) fd = &def;
    if (!Prim32FontInitGDI(&c->font, nullptr, 0, fd->face, fd->sizePx, fd->kerning)) {
        Prim32FontInitGDI(&c->font, nullptr, 0, L"Arial", fd->sizePx, fd->kerning);
    }
    Prim32PrewarmAscii(&c->font);   // first frame never rasterizes basic Latin
    c->drawList.ctx = c;
    c->fontStackTop = -1;
    c->defaultFont = InvalidFontHandle;   // resolves to the built-in font
    Prim32RegisterBuiltinFont(c);
    c->idStackTop = -1;
    g = c;
    return c;
}
// Prim32FreeFontAtlas lives in prim32_resources.cpp (rasterizer teardown)

void DestroyContext(Context* c) {
    if (!c) return;
    Prim32FreeFontAtlas(&c->font);
    if (g == c) g = nullptr;
    free(c);
}

static int64_t QPC() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
static double  QPFreq() { static double f = 0; if (!f) { LARGE_INTEGER q; QueryPerformanceFrequency(&q); f = (double)q.QuadPart; } return f; }

// ------------------------------------------------------------- frame plumbing
// Non-window draws are collected as ordered items per layer. An item is
// either a dynamic segment of this frame's prim stream or a cached layer.
struct DrawItem { uint32_t start, count; CachedLayer* cache; float ox, oy; uint32_t clipBase; uint8_t layer; };
static DrawItem s_items[96]; static uint32_t s_itemCount; static uint32_t s_segStart;

void NewFrame(const FrameMem& mem, const IO& io) {
    Context* c = g;
    c->curLayer = LAYER_BACKGROUND;
    c->recLayer = nullptr;
    c->frameT0 = QPC();
    c->frameNum++;

    // input edges
    for (int i = 0; i < 3; i++) {
        c->mouseClicked[i]  = io.mouseDown[i] && !c->io.mouseDown[i];
        c->mouseReleased[i] = !io.mouseDown[i] && c->io.mouseDown[i];
    }
    if (c->mouseClicked[0]) c->mouseClickPos = io.mousePos;
    c->io = io;

    // frame memory (mapped GPU upload heap — write-only!)
    c->prims = mem.prims; c->primCap = mem.primCap; c->primCount = 0;
    c->clips = mem.clips; c->clipCap = mem.clipCap; c->clipCount = 0;
    c->overflow = false;
    c->insideFrame = true;

    // clip 0 = whole display
    c->clipStackTop = -1; c->curClip = 0;
    PushClip(0, 0, io.displaySize.x, io.displaySize.y, false);

    // hovered window: topmost window (from last frame) under the mouse
    c->hoveredWindow = 0;
    int bestZ = -0x7FFFFFFF;
    for (uint32_t i = 0; i < c->windowCount; i++) {
        Window* w = &c->windows[i];
        if (!w->inUse || w->lastFrame + 1 < c->frameNum) continue;
        int z = w->z + ((w->flags & WF_Topmost) ? 0x100000 : 0);
        if (io.mousePos.x >= w->pos.x && io.mousePos.y >= w->pos.y &&
            io.mousePos.x < w->pos.x + w->size.x && io.mousePos.y < w->pos.y + w->size.y &&
            z > bestZ) { bestZ = z; c->hoveredWindow = w->id; }
    }
    c->hotId = 0;

    s_itemCount = 0; s_segStart = 0;
    c->curWindow = nullptr;
}

static void FlushLooseSeg() {
    if (g->recLayer) return;                       // cache recording: no segments
    if (g->primCount > s_segStart && s_itemCount < 96)
        s_items[s_itemCount++] = { s_segStart, g->primCount - s_segStart, nullptr, 0, 0, 0, (uint8_t)g->curLayer };
    s_segStart = g->primCount;
}

// ------------------------------------------------------------------- layers
void BeginLayer(LayerId layer) { FlushLooseSeg(); g->curLayer = layer; }
void EndLayer()                { FlushLooseSeg(); g->curLayer = LAYER_BACKGROUND; }

// ------------------------------------------------------------- cached layers
static uint32_t s_nextLayerId = 0;

CachedLayer* CreateCachedLayer(uint32_t maxPrims, const char* name) {
    if (s_nextLayerId >= 256) return nullptr;
    CachedLayer* l = (CachedLayer*)calloc(1, sizeof(CachedLayer));
    l->prims = (Prim*)malloc((size_t)maxPrims * sizeof(Prim));
    l->primCap = maxPrims;
    l->id = s_nextLayerId++;
    l->name = name ? name : "(cached)";
    return l;
}
void DestroyCachedLayer(CachedLayer* l) {
    if (!l) return;
    free(l->prims);
    free(l);
}

// Redirect the context's emission into the layer's CPU staging. All draw
// APIs work unchanged; coordinates are layer-local (offset at DrawCached).
static ClipRect s_clipShadowSave[32];

void BeginCache(CachedLayer* l) {
    Context* c = g;
    if (!l || c->recLayer || c->curWindow) return;
    c->recLayer = l;
    l->recording = true;
    memcpy(s_clipShadowSave, s_clipShadow, sizeof(s_clipShadow));
    c->savePrims = c->prims;   c->savePrimCount = c->primCount; c->savePrimCap = c->primCap;
    c->saveClips = c->clips;   c->saveClipCount = c->clipCount; c->saveClipCap = c->clipCap;
    c->saveCurClip = c->curClip; c->saveClipStackTop = c->clipStackTop;
    c->prims = l->prims;  c->primCount = 0; c->primCap = l->primCap;
    c->clips = l->clips;  c->clipCount = 0; c->clipCap = 16;
    c->clipStackTop = -1;
    PushClip(-1e7f, -1e7f, 1e7f, 1e7f, false);     // clip 0: unbounded
}
void EndCache() {
    Context* c = g;
    CachedLayer* l = c->recLayer;
    if (!l) return;
    l->primCount = c->primCount;
    l->clipCount = c->clipCount;
    l->version++;
    l->recording = false;
    c->recLayer = nullptr;
    c->prims = c->savePrims;   c->primCount = c->savePrimCount; c->primCap = c->savePrimCap;
    c->clips = c->saveClips;   c->clipCount = c->saveClipCount; c->clipCap = c->saveClipCap;
    c->curClip = c->saveCurClip; c->clipStackTop = c->saveClipStackTop;
    memcpy(s_clipShadow, s_clipShadowSave, sizeof(s_clipShadow));
}

// Enqueue a cached layer at the current point in the current layer's order.
// Cost per frame: copying its few clip rects; the prims never move.
void DrawCached(CachedLayer* l, Vec2 offset) {
    Context* c = g;
    if (!l || c->recLayer || !l->primCount || s_itemCount >= 96) return;
    FlushLooseSeg();
    uint32_t base = c->clipCount;
    if (base + l->clipCount > c->clipCap) return;  // clip table full
    for (uint32_t i = 0; i < l->clipCount; i++) {  // rebased + offset copies
        ClipRect r = l->clips[i];
        r.x0 += offset.x; r.y0 += offset.y; r.x1 += offset.x; r.y1 += offset.y;
        c->clips[base + i] = r;                    // single WC write each
    }
    c->clipCount = base + l->clipCount;
    s_items[s_itemCount++] = { 0, l->primCount, l, offset.x, offset.y, base, (uint8_t)c->curLayer };
}

DrawData* EndFrame() {
    Context* c = g;
    FlushLooseSeg();
    DrawData* dd = &c->drawData;
    dd->rangeCount = 0; dd->displaySize = c->io.displaySize;

    // background items first, in submission order
    for (uint32_t i = 0; i < s_itemCount && dd->rangeCount < 160; i++) {
        const DrawItem& it = s_items[i];
        if (it.layer != LAYER_BACKGROUND) continue;
        dd->ranges[dd->rangeCount++] = { it.start, it.count,
            it.cache ? it.cache->name : "(background)", it.cache, it.ox, it.oy, it.clipBase };
    }

    // windows sorted by z (insertion sort; window count is small), topmost last
    Window* order[64]; uint32_t n = 0;
    for (uint32_t i = 0; i < c->windowCount; i++) {
        Window* w = &c->windows[i];
        if (w->inUse && w->lastFrame == c->frameNum && w->primEnd > w->primStart) order[n++] = w;
    }
    for (uint32_t a = 1; a < n; a++) {
        Window* w = order[a]; uint32_t b = a;
        auto key = [](Window* x) { return (int64_t)x->z + (((x->flags & WF_Topmost) != 0) ? (1ll << 40) : 0); };
        while (b && key(order[b - 1]) > key(w)) { order[b] = order[b - 1]; b--; }
        order[b] = w;
    }
    for (uint32_t i = 0; i < n && dd->rangeCount < 160; i++)
        dd->ranges[dd->rangeCount++] = { order[i]->primStart, order[i]->primEnd - order[i]->primStart,
                                         order[i]->name, nullptr, 0, 0, 0 };

    // foreground items last: above every window
    for (uint32_t i = 0; i < s_itemCount && dd->rangeCount < 160; i++) {
        const DrawItem& it = s_items[i];
        if (it.layer != LAYER_FOREGROUND) continue;
        dd->ranges[dd->rangeCount++] = { it.start, it.count,
            it.cache ? it.cache->name : "(foreground)", it.cache, it.ox, it.oy, it.clipBase };
    }

    dd->totalPrims = c->primCount;
    dd->clipCount  = c->clipCount;

    c->stats.prims  = c->primCount;
    c->stats.clips  = c->clipCount;
    c->stats.ranges = dd->rangeCount;
    c->stats.overflow = c->overflow;
    c->stats.buildMs = (float)((QPC() - c->frameT0) * 1000.0 / QPFreq());
    c->insideFrame = false;
    return dd;
}
Stats GetStats() { return g->stats; }

// ---------------------------------------------------------------- interaction
struct BtnState { bool hovered, held, clicked; };
static BtnState ButtonBehavior(float x0, float y0, float x1, float y1, uint32_t id) {
    Context* c = g;
    BtnState r = { false, false, false };
    const ClipRect& cl = CurClipRect();
    Vec2 m = c->io.mousePos;
    bool inRect = m.x >= x0 && m.y >= y0 && m.x < x1 && m.y < y1 &&
                  m.x >= cl.x0 && m.y >= cl.y0 && m.x < cl.x1 && m.y < cl.y1;
    bool winHovered = c->curWindow && c->curWindow->id == c->hoveredWindow;
    r.hovered = inRect && winHovered && (c->activeId == 0 || c->activeId == id);
    if (r.hovered) {
        c->hotId = id;
        if (c->mouseClicked[0]) { c->activeId = id; c->activeWindow = c->curWindow->id; c->activeGrabOff = { m.x - x0, m.y - y0 }; }
    }
    if (c->activeId == id) {
        r.held = c->io.mouseDown[0];
        if (c->mouseReleased[0]) { if (r.hovered) r.clicked = true; c->activeId = 0; }
    }
    c->lastItem.x0 = x0; c->lastItem.y0 = y0; c->lastItem.x1 = x1; c->lastItem.y1 = y1;
    c->lastItem.id = id; c->lastItem.hovered = r.hovered; c->lastItem.active = r.held; c->lastItem.clicked = r.clicked;
    c->lastItemFromBehavior = true;
    return r;
}
bool IsItemHovered() { return g->lastItem.hovered; }
bool IsItemActive()  { return g->lastItem.active; }

// -------------------------------------------------------------------- layout
static void ItemAdd(float w, float h) {
    Window* win = g->curWindow; if (!win) return;
    float x1 = win->cursor.x + w, y1 = win->cursor.y + h;
    if (g->lastItemFromBehavior) {
        g->lastItemFromBehavior = false;      // rect already set by ButtonBehavior
    } else {
        g->lastItem.x0 = win->cursor.x; g->lastItem.y0 = win->cursor.y;
        g->lastItem.x1 = x1;            g->lastItem.y1 = y1;
        g->lastItem.id = 0;
        g->lastItem.hovered = g->lastItem.active = g->lastItem.clicked = false;
    }
    if (x1 > win->contentMax.x) win->contentMax.x = x1;
    if (y1 > win->contentMax.y) win->contentMax.y = y1;
    win->prevLineH = h > win->lineH ? h : win->lineH;
    win->lineStart = { win->cursor.x, win->cursor.y };
    win->cursor.y = y1 + g->style.itemSpacing.y;
    win->cursor.x = win->pos.x + g->style.windowPadding.x;
    win->lineH = 0;
}
void SameLine(float spacing) {
    Window* win = g->curWindow; if (!win) return;
    if (spacing < 0) spacing = g->style.itemSpacing.x;
    win->cursor.x = g->lastItem.x1 + spacing;
    win->cursor.y = win->lineStart.y;
    win->lineH = win->prevLineH;
}
void Spacing() { Window* w = g->curWindow; if (w) w->cursor.y += g->style.itemSpacing.y; }
void Dummy(Vec2 size) { ItemAdd(size.x, size.y); }
Vec2 GetCursorScreenPos() { return g->curWindow ? g->curWindow->cursor : Vec2{ 0, 0 }; }
void SetCursorScreenPos(Vec2 p) { if (g->curWindow) g->curWindow->cursor = p; }
Vec2 GetContentAvail() {
    Window* w = g->curWindow; if (!w) return { 0, 0 };
    return { w->pos.x + w->size.x - g->style.windowPadding.x - w->cursor.x,
             w->pos.y + w->size.y - g->style.windowPadding.y - w->cursor.y };
}
float GetFrameHeight() { return g->style.frameHeight; }

// ------------------------------------------------------------------- windows
static Vec2 s_nextPos, s_nextSize; static int s_nextPosMode, s_nextSizeMode; // 0=no,1=once,2=always
void SetNextWindowPos(Vec2 p, bool always)  { s_nextPos = p;  s_nextPosMode = always ? 2 : 1; }
void SetNextWindowSize(Vec2 s, bool always) { s_nextSize = s; s_nextSizeMode = always ? 2 : 1; }

static Window* FindOrCreateWindow(uint32_t id, const char* name) {
    Context* c = g;
    for (uint32_t i = 0; i < c->windowCount; i++)
        if (c->windows[i].inUse && c->windows[i].id == id) return &c->windows[i];
    Window* w = nullptr;
    if (c->windowCount < 64) w = &c->windows[c->windowCount++];
    else return nullptr;
    memset(w, 0, sizeof(*w));
    w->inUse = true; w->id = id;
    const char* e = LabelEnd(name);
    size_t n = (size_t)(e - name); if (n > 23) n = 23;
    memcpy(w->name, name, n); w->name[n] = 0;
    if (!w->name[0]) { memcpy(w->name, "(window)", 9); }
    w->pos = { 60.0f + (c->windowCount % 8) * 40.0f, 60.0f + (c->windowCount % 8) * 40.0f };
    w->size = { 320, 240 };
    w->z = c->zNext++;
    return w;
}

bool Begin(const char* name, uint32_t flags) {
    Context* c = g;
    if (c->recLayer) return false;    // no windows inside cache recording
    FlushLooseSeg();
    uint32_t id = Hash(name, nullptr, 0);
    Window* w = FindOrCreateWindow(id, name);
    if (!w) return false;
    c->curWindow = w;
    w->flags = flags;
    bool firstUse = w->lastFrame == 0;
    w->lastFrame = c->frameNum;

    if (s_nextPosMode)  { if (s_nextPosMode == 2 || firstUse) w->pos = s_nextPos; s_nextPosMode = 0; }
    if (s_nextSizeMode) { w->size = s_nextSize; s_nextSizeMode = 0; }
    if (flags & WF_AutoSize) {
        if (w->autoSizeStore.x > 0) w->size = w->autoSizeStore;
    }

    const Style& st = c->style;
    float tbh = (flags & WF_NoTitleBar) ? 0.0f : st.titleBarHeight;

    // focus: click anywhere in the window brings it to front
    if (c->mouseClicked[0] && c->hoveredWindow == id && !(flags & WF_NoBringToFront))
        w->z = c->zNext++;

    // title-bar drag
    if (!(flags & WF_NoMove) && tbh > 0) {
        uint32_t moveId = id ^ 0x9E3779B9u;
        Vec2 m = c->io.mousePos;
        bool overBar = m.x >= w->pos.x && m.x < w->pos.x + w->size.x &&
                       m.y >= w->pos.y && m.y < w->pos.y + tbh;
        if (overBar && c->hoveredWindow == id && c->mouseClicked[0] && c->activeId == 0) {
            c->activeId = moveId; c->activeWindow = id;
            c->activeGrabOff = { m.x - w->pos.x, m.y - w->pos.y };
        }
        if (c->activeId == moveId) {
            if (c->io.mouseDown[0]) w->pos = { m.x - c->activeGrabOff.x, m.y - c->activeGrabOff.y };
            else c->activeId = 0;
        }
    }

    w->primStart = c->primCount;
    bool focused = (c->activeWindow == id) || (c->hoveredWindow == id);

    if (!(flags & WF_NoBackground)) {
        DrawShadow(c, w->pos.x + 4, w->pos.y + 6, w->pos.x + w->size.x + 4, w->pos.y + w->size.y + 6,
                   COL32(0, 0, 0, 130), st.windowRounding, 14.0f);
        DrawRectRounded(c, w->pos.x, w->pos.y, w->pos.x + w->size.x, w->pos.y + w->size.y,
                        st.colors[ZC_WindowBg], st.windowRounding);
        if (tbh > 0) {
            PushClip(w->pos.x, w->pos.y, w->pos.x + w->size.x, w->pos.y + tbh, false);
            DrawRectRounded(c, w->pos.x, w->pos.y, w->pos.x + w->size.x, w->pos.y + tbh + st.windowRounding,
                            st.colors[focused ? ZC_TitleBarActive : ZC_TitleBar], st.windowRounding);
            PopClip();
            DrawText(c, w->pos.x + st.windowPadding.x,
                     w->pos.y + (tbh - GetFontLineHeight()) * 0.5f, st.colors[ZC_Text], name, LabelEnd(name));
        }
        if (st.borderSize > 0)
            DrawRectStroke(c, w->pos.x, w->pos.y, w->pos.x + w->size.x, w->pos.y + w->size.y,
                           st.colors[ZC_Border], st.windowRounding, st.borderSize);
    }

    PushClip(w->pos.x, w->pos.y + tbh, w->pos.x + w->size.x, w->pos.y + w->size.y, false);
    w->cursor = { w->pos.x + st.windowPadding.x, w->pos.y + tbh + st.windowPadding.y };
    w->contentMax = w->cursor;
    w->lineH = w->prevLineH = 0;
    w->clipIdx = c->curClip;
    return true;
}

void End() {
    Context* c = g;
    Window* w = c->curWindow; if (!w) return;
    PopClip();
    w->primEnd = c->primCount;
    if (w->flags & WF_AutoSize)
        w->autoSizeStore = { w->contentMax.x - w->pos.x + c->style.windowPadding.x,
                             w->contentMax.y - w->pos.y + c->style.windowPadding.y };
    c->curWindow = nullptr;
    s_segStart = c->primCount;
}

// ------------------------------------------------------------------- widgets
void Text(const char* text) { TextColored(g->style.colors[ZC_Text], text); }
void TextColored(Col col, const char* text) {
    Context* c = g; Window* w = c->curWindow; if (!w) return;
    Vec2 sz = TextSize(c, text);
    DrawText(c, w->cursor.x, w->cursor.y, col, text);
    ItemAdd(sz.x, sz.y);
}
void TextF(const char* fmt, ...) {
    Context* c = g;
    va_list ap; va_start(ap, fmt);
    vsnprintf(c->textScratch, sizeof(c->textScratch), fmt, ap);
    va_end(ap);
    Text(c->textScratch);
}

bool Button(const char* label, Vec2 size) {
    Context* c = g; Window* w = c->curWindow; if (!w) return false;
    const Style& st = c->style;
    const char* lblEnd = LabelEnd(label);
    Vec2 ts = TextSize(c, label, lblEnd);
    float bw = size.x > 0 ? size.x : ts.x + st.framePadding.x * 2;
    float bh = size.y > 0 ? size.y : st.frameHeight;
    float x0 = w->cursor.x, y0 = w->cursor.y;
    uint32_t id = GetID(label);
    BtnState bs = ButtonBehavior(x0, y0, x0 + bw, y0 + bh, id);
    Col col = st.colors[bs.held ? ZC_ButtonActive : bs.hovered ? ZC_ButtonHovered : ZC_Button];
    DrawRectRounded(c, x0, y0, x0 + bw, y0 + bh, col, st.frameRounding);
    DrawText(c, x0 + (bw - ts.x) * 0.5f, y0 + (bh - ts.y) * 0.5f, st.colors[ZC_Text], label, lblEnd);
    ItemAdd(bw, bh);
    return bs.clicked;
}

bool InvisibleButton(const char* idStr, Vec2 size) {
    Context* c = g; Window* w = c->curWindow; if (!w) return false;
    uint32_t id = GetID(idStr);
    float x0 = w->cursor.x, y0 = w->cursor.y;
    BtnState bs = ButtonBehavior(x0, y0, x0 + size.x, y0 + size.y, id);
    ItemAdd(size.x, size.y);
    return bs.clicked;
}

bool Checkbox(const char* label, bool* v) {
    Context* c = g; Window* w = c->curWindow; if (!w) return false;
    const Style& st = c->style;
    const char* lblEnd = LabelEnd(label);
    float sz = st.frameHeight - 4;
    float x0 = w->cursor.x, y0 = w->cursor.y + 2;
    uint32_t id = GetID(label);
    Vec2 ts = TextSize(c, label, lblEnd);
    BtnState bs = ButtonBehavior(x0, y0, x0 + sz + st.itemSpacing.x + ts.x, y0 + sz, id);
    if (bs.clicked) *v = !*v;
    Col bg = st.colors[bs.held ? ZC_FrameBgActive : bs.hovered ? ZC_FrameBgHovered : ZC_FrameBg];
    DrawRectRounded(c, x0, y0, x0 + sz, y0 + sz, bg, 3.0f);
    if (*v) {
        float p = sz * 0.22f;
        DrawLine(c, x0 + p, y0 + sz * 0.55f, x0 + sz * 0.42f, y0 + sz - p, st.colors[ZC_CheckMark], 2.2f);
        DrawLine(c, x0 + sz * 0.42f, y0 + sz - p, x0 + sz - p, y0 + p, st.colors[ZC_CheckMark], 2.2f);
    }
    DrawText(c, x0 + sz + st.itemSpacing.x, y0 + (sz - ts.y) * 0.5f, st.colors[ZC_Text], label, lblEnd);
    ItemAdd(sz + st.itemSpacing.x + ts.x, st.frameHeight - 2);
    return bs.clicked;
}

static bool SliderScalar(const char* label, float* v, float vMin, float vMax, const char* valText) {
    Context* c = g; Window* w = c->curWindow; if (!w) return false;
    const Style& st = c->style;
    const char* lblEnd = LabelEnd(label);
    float availW = GetContentAvail().x;
    float sw = availW * 0.60f; if (sw < 60) sw = 60;
    float x0 = w->cursor.x, y0 = w->cursor.y, x1 = x0 + sw, y1 = y0 + st.frameHeight;
    uint32_t id = GetID(label);
    BtnState bs = ButtonBehavior(x0, y0, x1, y1, id);
    bool changed = false;
    if (c->activeId == id && c->io.mouseDown[0]) {
        float t = (c->io.mousePos.x - x0 - 6) / (sw - 12);
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        float nv = vMin + t * (vMax - vMin);
        if (nv != *v) { *v = nv; changed = true; }
    }
    Col bg = st.colors[bs.held ? ZC_FrameBgActive : bs.hovered ? ZC_FrameBgHovered : ZC_FrameBg];
    DrawRectRounded(c, x0, y0, x1, y1, bg, st.frameRounding);
    float t = (vMax > vMin) ? (*v - vMin) / (vMax - vMin) : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    float gx = x0 + 6 + t * (sw - 12);
    DrawRectRounded(c, gx - 5, y0 + 3, gx + 5, y1 - 3,
                    st.colors[bs.held ? ZC_SliderGrabActive : ZC_SliderGrab], 3.0f);
    Vec2 vts = TextSize(c, valText);
    DrawText(c, x0 + (sw - vts.x) * 0.5f, y0 + (st.frameHeight - vts.y) * 0.5f, st.colors[ZC_Text], valText);
    DrawText(c, x1 + st.itemSpacing.x, y0 + (st.frameHeight - GetFontLineHeight()) * 0.5f,
             st.colors[ZC_Text], label, lblEnd);
    Vec2 lts = TextSize(c, label, lblEnd);
    ItemAdd(sw + st.itemSpacing.x + lts.x, st.frameHeight);
    return changed;
}
bool SliderFloat(const char* label, float* v, float vMin, float vMax, const char* fmt) {
    char buf[64]; snprintf(buf, 64, fmt, *v);
    return SliderScalar(label, v, vMin, vMax, buf);
}
bool SliderInt(const char* label, int* v, int vMin, int vMax) {
    float f = (float)*v;
    char buf[64]; snprintf(buf, 64, "%d", *v);
    bool ch = SliderScalar(label, &f, (float)vMin, (float)vMax, buf);
    if (ch) *v = (int)(f + (f >= 0 ? 0.5f : -0.5f));
    return ch;
}

void ProgressBar(float frac, Vec2 size) {
    Context* c = g; Window* w = c->curWindow; if (!w) return;
    const Style& st = c->style;
    float bw = size.x > 0 ? size.x : GetContentAvail().x;
    float bh = size.y > 0 ? size.y : st.frameHeight * 0.6f;
    float x0 = w->cursor.x, y0 = w->cursor.y;
    frac = frac < 0 ? 0 : frac > 1 ? 1 : frac;
    DrawRectRounded(c, x0, y0, x0 + bw, y0 + bh, st.colors[ZC_FrameBg], st.frameRounding);
    if (frac > 0.001f)
        DrawRectRounded(c, x0, y0, x0 + bw * frac, y0 + bh, st.colors[ZC_ProgressFill], st.frameRounding);
    ItemAdd(bw, bh);
}

void Separator() {
    Context* c = g; Window* w = c->curWindow; if (!w) return;
    float x0 = w->pos.x + c->style.windowPadding.x;
    float x1 = w->pos.x + w->size.x - c->style.windowPadding.x;
    DrawLine(c, x0, w->cursor.y + 3, x1, w->cursor.y + 3, c->style.colors[ZC_Border], 1.0f);
    w->cursor.y += 7;
}

} // namespace prim32
