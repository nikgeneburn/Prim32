// ============================================================================
// Prim32 resource layer — images, fonts, handles.
//
//   * Generational handles: stale handles resolve to invalid, never crash.
//   * Decode ONCE at load (WIC: PNG/JPG/BMP/GIF/TIFF — zero extra deps,
//     isolated in DecodeImageWIC below). Upload ONCE via the backend hook.
//   * All loaders copy what they need; callers may free their buffers the
//     moment the call returns (GDI copies font bytes, WIC decodes to our own
//     pixel buffer which is freed after upload).
//   * Nothing here runs in the per-frame hot path: draw->Image/Text resolve a
//     handle with an array index + generation compare (~2 ns).
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#ifdef DrawText
#undef DrawText
#endif
#ifdef LoadImage
#undef LoadImage
#endif
#include <wincodec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <prim32/prim32.h>
#include "prim32_internal.h"

namespace prim32 {

// ------------------------------------------------------------- error channel
static char s_lastErr[256];
static void SetErr(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(s_lastErr, sizeof(s_lastErr), fmt, ap);
    va_end(ap);
    char line[300];
    snprintf(line, sizeof(line), "[Prim32] %s\n", s_lastErr);
    OutputDebugStringA(line);
}
const char* GetLastResourceError() { return s_lastErr; }

// ------------------------------------------------------------- backend hooks
static const Prim32BackendHooks* s_hooks = nullptr;
void Prim32SetBackendHooks(const Prim32BackendHooks* h) { s_hooks = h; }
const Prim32BackendHooks* Prim32GetBackendHooks() { return s_hooks; }

// ===== PRIM32_PURE_REG_BEGIN (no OS deps; extracted by the native tests) =====
static const uint32_t MAX_IMAGES = 256;
static const uint32_t MAX_FONTS  = 64;

struct ImageRes {
    uint32_t gen;          // bumped on destroy; 0 = never used
    bool     used;
    uint32_t texSlot;
    int      w, h;
    uint64_t bytes;        // estimated GPU memory
    char     name[48];     // debug name / path tail
};
struct FontRes {
    uint32_t   gen;
    bool       used;
    bool       ownsAtlas;   // built-in font's atlas is owned by the Context
    uint32_t   texSlot;     // first page slot (info only; glyphs carry their own)
    FontAtlas* atlas;       // canonical — the cache mutates it, so never copy
    uint64_t   bytes;
    char       name[48];
};

static ImageRes s_images[MAX_IMAGES];   // index 0 reserved (invalid)
static FontRes  s_fonts[MAX_FONTS];    // index 0 reserved; 1 = built-in font
static uint32_t s_imageHigh = 1, s_fontHigh = 1;

static uint32_t RegAllocImage() {
    for (uint32_t i = 1; i < MAX_IMAGES; i++)
        if (!s_images[i].used && (i >= s_imageHigh || s_images[i].gen)) {
            if (!s_images[i].used) { if (i >= s_imageHigh) s_imageHigh = i + 1; return i; }
        }
    return 0;
}
static uint32_t RegAllocFont() {
    for (uint32_t i = 2; i < MAX_FONTS; i++)      // 1 is the built-in font
        if (!s_fonts[i].used) { if (i >= s_fontHigh) s_fontHigh = i + 1; return i; }
    return 0;
}
static ImageRes* RegGetImage(ImageHandle h) {
    if (h.index == 0 || h.index >= MAX_IMAGES) return nullptr;
    ImageRes* r = &s_images[h.index];
    return (r->used && r->gen == h.gen) ? r : nullptr;
}
static FontRes* RegGetFont(FontHandle h) {
    if (h.index == 0 || h.index >= MAX_FONTS) return nullptr;
    FontRes* r = &s_fonts[h.index];
    return (r->used && r->gen == h.gen) ? r : nullptr;
}
// ===== PRIM32_PURE_REG_END =====

bool IsValid(ImageHandle h) { return RegGetImage(h) != nullptr; }
bool IsValid(FontHandle h)  { return RegGetFont(h) != nullptr; }

Vec2 GetImageSize(ImageHandle h) {
    ImageRes* r = RegGetImage(h);
    return r ? Vec2{ (float)r->w, (float)r->h } : Vec2{ 0, 0 };
}

ResourceStats GetResourceStats() {
    ResourceStats st = {};
    for (uint32_t i = 1; i < MAX_IMAGES; i++)
        if (s_images[i].used) { st.images++; st.textureBytes += s_images[i].bytes; }
    for (uint32_t i = 1; i < MAX_FONTS; i++)
        if (s_fonts[i].used) { st.fonts++; st.atlasBytes += s_fonts[i].bytes; }
    return st;
}

static void CopyNameTail(char* dst, size_t cap, const char* src) {
    size_t n = strlen(src);
    const char* tail = n >= cap ? src + (n - cap + 1) : src;
    snprintf(dst, cap, "%s", tail);
}

// --------------------------------------------------------------- font lookup
void Prim32RegisterBuiltinFont(Context* ctx) {
    FontRes& f = s_fonts[1];
    f.used = true;
    f.gen = f.gen ? f.gen : 1;
    f.ownsAtlas = false;                 // Context owns the built-in atlas
    f.texSlot = ctx->font.pageSlot;
    f.atlas = &ctx->font;                // canonical pointer, not a copy
    f.bytes = (uint64_t)ctx->font.pageW * ctx->font.pageH;
    snprintf(f.name, sizeof(f.name), "built-in (%.0fpx)", ctx->font.size);
    if (s_fontHigh < 2) s_fontHigh = 2;
}

ResolvedFont Prim32ResolveFont(FontHandle h) {
    Context* c = GetContext();
    if (FontRes* r = RegGetFont(h)) return { r->atlas };
    if (c && c->fontStackTop >= 0)
        if (FontRes* r = RegGetFont(c->fontStack[c->fontStackTop]))
            return { r->atlas };
    if (c)
        if (FontRes* r = RegGetFont(c->defaultFont))
            return { r->atlas };
    return { s_fonts[1].atlas ? s_fonts[1].atlas : &GetContext()->font };
}

void SetDefaultFont(FontHandle h) {
    Context* c = GetContext();
    if (!c) return;
    if (h != InvalidFontHandle && !RegGetFont(h)) { SetErr("SetDefaultFont: invalid handle {%u,%u}", h.index, h.gen); return; }
    c->defaultFont = h;
}
FontHandle GetDefaultFont() {
    Context* c = GetContext();
    if (c && RegGetFont(c->defaultFont)) return c->defaultFont;
    return FontHandle{ 1, s_fonts[1].gen };              // built-in
}
void PushFont(FontHandle h) {
    Context* c = GetContext();
    if (!c) return;
    if (!RegGetFont(h)) { SetErr("PushFont: invalid handle {%u,%u} (rejected)", h.index, h.gen); return; }
    if (c->fontStackTop < 15) c->fontStack[++c->fontStackTop] = h;
    else SetErr("PushFont: stack full (16)");
}
void PopFont() {
    Context* c = GetContext();
    if (c && c->fontStackTop >= 0) c->fontStackTop--;    // underflow-safe
}

Vec2 MeasureText(const char* text, const char* end) {
    ResolvedFont rf = Prim32ResolveFont(InvalidFontHandle);
    return Prim32MeasureAtlas(rf.atlas, text, end, 0.0f, 1.0f);
}
Vec2 MeasureText(FontHandle font, const char* text, const char* end) {
    ResolvedFont rf = Prim32ResolveFont(font);
    return Prim32MeasureAtlas(rf.atlas, text, end, 0.0f, 1.0f);
}
float GetFontLineHeight(FontHandle font) {
    return Prim32ResolveFont(font).atlas->lineHeight;
}

// ======================================================= dynamic glyph engine
// Rasterizers own a shared scratch (single-threaded, like the rest of prim32).
static uint8_t* s_ggoBuf;  static size_t s_ggoCap;    // raw GGO output
static uint8_t* s_a8Buf;   static size_t s_a8Cap;     // converted A8

static uint8_t* Scratch(uint8_t** buf, size_t* cap, size_t need) {
    if (need > *cap) {
        size_t n = *cap ? *cap : 4096;
        while (n < need) n *= 2;
        *buf = (uint8_t*)realloc(*buf, n);
        *cap = n;
    }
    return *buf;
}

// ---- GDI rasterizer: full BMP coverage (CJK, Cyrillic, PUA icons, ...)
bool Prim32RasterGlyphGDI(FontAtlas* fa, uint32_t cp, GlyphBitmap* out) {
    if (cp > 0xFFFF) return false;                     // GDI path is BMP-only (FreeType covers the rest)
    HDC dc = (HDC)fa->rasterA;
    if (!dc) return false;
    const MAT2 mat = { {0,1},{0,0},{0,0},{0,1} };
    GLYPHMETRICS gm = {};
    DWORD sz = GetGlyphOutlineW(dc, (UINT)cp, GGO_GRAY8_BITMAP, &gm, 0, nullptr, &mat);
    if (sz == GDI_ERROR) return false;
    int w = (int)gm.gmBlackBoxX, h = (int)gm.gmBlackBoxY;
    out->advance  = (float)gm.gmCellIncX;
    out->bearingX = gm.gmptGlyphOrigin.x;
    out->bearingY = gm.gmptGlyphOrigin.y;
    out->w = 0; out->h = 0; out->pixels = nullptr; out->pitch = 0;
    if (!sz || w <= 0 || h <= 0) return true;          // advance-only (space etc.)
    uint8_t* raw = Scratch(&s_ggoBuf, &s_ggoCap, sz);
    GLYPHMETRICS gm2;
    if (GetGlyphOutlineW(dc, (UINT)cp, GGO_GRAY8_BITMAP, &gm2, sz, raw, &mat) == GDI_ERROR) return false;
    int pitch = (w + 3) & ~3;                          // GGO rows are DWORD aligned
    uint8_t* a8 = Scratch(&s_a8Buf, &s_a8Cap, (size_t)w * h);
    for (int r = 0; r < h; r++)
        for (int x = 0; x < w; x++) {
            uint32_t v = raw[(size_t)r * pitch + x] * 255u / 64u;   // 0..64 -> 0..255
            a8[(size_t)r * w + x] = v > 255 ? 255 : (uint8_t)v;
        }
    out->w = w; out->h = h; out->pixels = a8; out->pitch = w;
    return true;
}

static void RasterKernGDI(FontAtlas* fa) {
    HDC dc = (HDC)fa->rasterA;
    fa->kernCount = 0; fa->kernKeys = nullptr; fa->kernVals = nullptr;
    DWORD n = GetKerningPairsW(dc, 0, nullptr);
    if (!n || n == GDI_ERROR) return;
    KERNINGPAIR* kp = (KERNINGPAIR*)malloc(n * sizeof(KERNINGPAIR));
    n = GetKerningPairsW(dc, n, kp);
    if (n && n != GDI_ERROR) {
        fa->kernKeys = (uint32_t*)malloc(n * 4);
        fa->kernVals = (float*)malloc(n * 4);
        uint32_t m = 0;
        for (DWORD k = 0; k < n; k++)
            if (kp[k].iKernAmount) {
                fa->kernKeys[m] = ((uint32_t)kp[k].wFirst << 16) | kp[k].wSecond;
                fa->kernVals[m] = (float)kp[k].iKernAmount; m++;
            }
        fa->kernCount = m;
        for (uint32_t a = 1; a < m; a++) {             // insertion sort (nearly sorted)
            uint32_t key = fa->kernKeys[a]; float val = fa->kernVals[a]; uint32_t b = a;
            while (b && fa->kernKeys[b - 1] > key) { fa->kernKeys[b] = fa->kernKeys[b-1]; fa->kernVals[b] = fa->kernVals[b-1]; b--; }
            fa->kernKeys[b] = key; fa->kernVals[b] = val;
        }
    }
    free(kp);
}

// ---- atlas pages
static bool NewPage(FontAtlas* fa) {
    if (fa->pageCount >= 8) return false;              // page cap (see docs)
    uint8_t* px = (uint8_t*)calloc(1, (size_t)fa->pageW * fa->pageH);
    if (!px) return false;
    uint32_t slot = 0;
    const Prim32BackendHooks* hk = Prim32GetBackendHooks();
    if (hk && hk->createTexture) {
        slot = hk->createTexture(px, fa->pageW, fa->pageH, 0, "glyph atlas page");
        if (slot == 0xFFFFFFFFu) { free(px); return false; }
    }
    free(fa->pagePixels);                              // old page fully uploaded already
    fa->pagePixels = px;
    fa->pageSlot = slot;
    fa->pageSlots[fa->pageCount++] = slot;
    fa->penX = fa->penY = 2; fa->shelfH = 0;
    return true;
}

bool Prim32AtlasInit(FontAtlas* fa, float sizePx) {
    fa->glyphCap = 256;
    fa->glyphs = (Glyph*)calloc(fa->glyphCap, sizeof(Glyph));
    fa->glyphCount = 1;                                // [0] = notdef
    memset(fa->asciiMap, 0, sizeof(fa->asciiMap));
    fa->cpKeys = nullptr; fa->cpVals = nullptr; fa->cpCap = 0; fa->cpCount = 0;
    fa->pageW = fa->pageH = sizePx <= 24.0f ? 512 : 1024;
    fa->pagePixels = nullptr; fa->pageCount = 0;
    if (!fa->glyphs || !NewPage(fa)) return false;
    // notdef: a hollow box baked procedurally into page 0
    int bw = (int)(sizePx * 0.50f); if (bw < 4) bw = 4;
    int bh = (int)(sizePx * 0.64f); if (bh < 5) bh = 5;
    int x, y;
    if (Prim32PackRect(fa, bw, bh, &x, &y)) {
        for (int r = 0; r < bh; r++)
            for (int cxi = 0; cxi < bw; cxi++) {
                bool edge = r == 0 || r == bh - 1 || cxi == 0 || cxi == bw - 1;
                fa->pagePixels[(size_t)(y + r) * fa->pageW + (x + cxi)] = edge ? 255 : 0;
            }
        const Prim32BackendHooks* hk = Prim32GetBackendHooks();
        if (hk && hk->updateTexture)
            hk->updateTexture(fa->pageSlot, x, y, bw, bh,
                              fa->pagePixels + (size_t)y * fa->pageW + x, fa->pageW);
        Glyph* nd = &fa->glyphs[0];
        nd->uv0 = PackUV((float)x / fa->pageW, (float)y / fa->pageH);
        nd->uv1 = PackUV((float)(x + bw) / fa->pageW, (float)(y + bh) / fa->pageH);
        nd->x0 = 1.0f; nd->y0 = (float)-bh;
        nd->x1 = 1.0f + bw; nd->y1 = 0.0f;
        nd->advance = bw + 2.0f;
        nd->texSlot = fa->pageSlot;
    }
    return true;
}

// ---- rasterize-on-miss. Never null; pointer valid until the next call.
Glyph* Prim32GetGlyph(FontAtlas* fa, uint32_t cp) {
    uint32_t idx = Prim32CacheFind(fa, cp);
    if (idx) return &fa->glyphs[idx];
    if (!fa->glyphs) return nullptr;                   // uninitialized atlas (never in practice)

    GlyphBitmap bm = {};
    bool ok = false;
    if (fa->rasterKind == 0) ok = Prim32RasterGlyphGDI(fa, cp, &bm);
#ifdef PRIM32_HAS_FREETYPE
    else if (fa->rasterKind == 1) ok = Prim32RasterGlyphFT(fa, cp, &bm);
#endif
    if (!ok) { Prim32CacheInsert(fa, cp, 0); return &fa->glyphs[0]; }   // notdef

    if (fa->glyphCount == fa->glyphCap) {
        uint32_t nc = fa->glyphCap * 2;
        Glyph* ng = (Glyph*)realloc(fa->glyphs, (size_t)nc * sizeof(Glyph));
        if (!ng) { Prim32CacheInsert(fa, cp, 0); return &fa->glyphs[0]; }
        fa->glyphs = ng; fa->glyphCap = nc;
    }
    uint32_t gi = fa->glyphCount;
    Glyph* gl = &fa->glyphs[gi];
    memset(gl, 0, sizeof(*gl));
    gl->advance = bm.advance;
    gl->texSlot = fa->pageSlot;

    if (bm.w > 0 && bm.h > 0) {
        int x, y;
        if (!Prim32PackRect(fa, bm.w, bm.h, &x, &y)) {
            if (!NewPage(fa) || !Prim32PackRect(fa, bm.w, bm.h, &x, &y)) {
                Prim32CacheInsert(fa, cp, 0);          // atlas exhausted: notdef
                return &fa->glyphs[0];
            }
        }
        for (int r = 0; r < bm.h; r++)
            memcpy(fa->pagePixels + (size_t)(y + r) * fa->pageW + x,
                   bm.pixels + (size_t)r * bm.pitch, (size_t)bm.w);
        const Prim32BackendHooks* hk = Prim32GetBackendHooks();
        if (hk && hk->updateTexture)
            hk->updateTexture(fa->pageSlot, x, y, bm.w, bm.h,
                              fa->pagePixels + (size_t)y * fa->pageW + x, fa->pageW);
        gl->uv0 = PackUV((float)x / fa->pageW, (float)y / fa->pageH);
        gl->uv1 = PackUV((float)(x + bm.w) / fa->pageW, (float)(y + bm.h) / fa->pageH);
        gl->x0 = (float)bm.bearingX;      gl->y0 = (float)-bm.bearingY;
        gl->x1 = gl->x0 + bm.w;           gl->y1 = gl->y0 + bm.h;
        gl->texSlot = fa->pageSlot;
    }
    fa->glyphCount++;
    Prim32CacheInsert(fa, cp, gi);
    return &fa->glyphs[gi];
}

void Prim32PrewarmAscii(FontAtlas* fa) {
    for (uint32_t cp = 32; cp < 127; cp++) Prim32GetGlyph(fa, cp);
}

// ---- font lifecycle
bool Prim32FontInitGDI(FontAtlas* fa, const void* data, size_t size,
                       const wchar_t* face, float sizePx, bool kerning) {
    memset(fa, 0, sizeof(*fa));
    fa->rasterKind = 0;
    if (data) {                                        // memory font: GDI copies the bytes
        DWORD installed = 0;
        HANDLE mem = AddFontMemResourceEx((PVOID)data, (DWORD)size, nullptr, &installed);
        if (!mem || !installed) return false;
        fa->rasterC = mem;                             // kept until FreeFontAtlas (lazy raster!)
    }
    HDC dc = CreateCompatibleDC(nullptr);
    HFONT font = CreateFontW(-(int)(sizePx + 0.5f), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                             DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                             ANTIALIASED_QUALITY, DEFAULT_PITCH, face);
    if (!dc || !font) {
        if (dc) DeleteDC(dc);
        if (font) DeleteObject(font);
        if (fa->rasterC) RemoveFontMemResourceEx((HANDLE)fa->rasterC);
        return false;
    }
    SelectObject(dc, font);
    fa->rasterA = dc; fa->rasterB = font;
    TEXTMETRICW tm; GetTextMetricsW(dc, &tm);
    fa->ascent = (float)tm.tmAscent; fa->descent = (float)tm.tmDescent;
    fa->lineHeight = (float)(tm.tmHeight + tm.tmExternalLeading);
    fa->size = sizePx;
    if (kerning) RasterKernGDI(fa);
    return Prim32AtlasInit(fa, sizePx);
}

void Prim32FreeFontAtlas(FontAtlas* fa) {
    if (!fa) return;
    if (fa->rasterKind == 0) {
        if (fa->rasterB) DeleteObject((HFONT)fa->rasterB);
        if (fa->rasterA) DeleteDC((HDC)fa->rasterA);
        if (fa->rasterC) RemoveFontMemResourceEx((HANDLE)fa->rasterC);
    }
#ifdef PRIM32_HAS_FREETYPE
    else if (fa->rasterKind == 1) Prim32RasterFreeFT(fa);
#endif
    free(fa->glyphs); free(fa->cpKeys); free(fa->cpVals);
    free(fa->pagePixels);
    free(fa->kernKeys); free(fa->kernVals);
    memset(fa, 0, sizeof(*fa));
}

// ---------------------------------------------------- TTF/OTF family name
// ===== PRIM32_PURE_TTF_BEGIN =====
static uint32_t BE32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint16_t BE16(const uint8_t* p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }

// Family name (nameID 1), preferring Windows/Unicode. Handles TTC collections.
static bool TtfFamilyName(const uint8_t* d, size_t n, wchar_t out[64]) {
    if (!d || n < 12) return false;
    size_t base = 0;
    if (BE32(d) == 0x74746366) {                       // 'ttcf' collection
        if (n < 16) return false;
        base = BE32(d + 12);
        if (base + 12 > n) return false;
    }
    uint16_t numTables = BE16(d + base + 4);
    size_t rec = base + 12;
    for (uint16_t t = 0; t < numTables; t++, rec += 16) {
        if (rec + 16 > n) return false;
        if (BE32(d + rec) != 0x6E616D65) continue;     // 'name'
        size_t off = BE32(d + rec + 8), len = BE32(d + rec + 12);
        if (off + len > n || len < 6) return false;
        const uint8_t* nt = d + off;
        uint16_t count = BE16(nt + 2), strOff = BE16(nt + 4);
        int best = -1, bestScore = -1;
        for (uint16_t i = 0; i < count; i++) {
            const uint8_t* e = nt + 6 + i * 12;
            if ((size_t)(e - d) + 12 > n) break;
            uint16_t plat = BE16(e), nameId = BE16(e + 6);
            if (nameId != 1) continue;
            int score = plat == 3 ? 2 : plat == 0 ? 1 : 0;   // prefer Windows, then Unicode
            if (score > bestScore) { bestScore = score; best = i; }
        }
        if (best < 0) return false;
        const uint8_t* e = nt + 6 + best * 12;
        uint16_t plat = BE16(e), slen = BE16(e + 8), soff = BE16(e + 10);
        const uint8_t* s = nt + strOff + soff;
        if ((size_t)(s - d) + slen > n) return false;
        int m = 0;
        if (plat == 3 || plat == 0) {                  // UTF-16BE
            for (uint16_t i = 0; i + 1 < slen && m < 63; i += 2)
                out[m++] = (wchar_t)((s[i] << 8) | s[i + 1]);
        } else {                                       // Mac Roman-ish: bytes
            for (uint16_t i = 0; i < slen && m < 63; i++)
                out[m++] = (wchar_t)s[i];
        }
        out[m] = 0;
        return m > 0;
    }
    return false;
}
// ===== PRIM32_PURE_TTF_END =====

// -------------------------------------------------------------- font loading
FontHandle LoadFontFromMemory(const void* data, size_t size, float sizePixels) {
    s_lastErr[0] = 0;
    if (!data || size < 12)          { SetErr("LoadFontFromMemory: empty/short data"); return InvalidFontHandle; }
    if (sizePixels < 4 || sizePixels > 256) { SetErr("LoadFontFromMemory: bad size %.1f px", sizePixels); return InvalidFontHandle; }
    if (!s_hooks)                    { SetErr("LoadFontFromMemory: backend not initialized"); return InvalidFontHandle; }

    FontAtlas* atlas = (FontAtlas*)calloc(1, sizeof(FontAtlas));
    if (!atlas) { SetErr("LoadFontFromMemory: out of memory"); return InvalidFontHandle; }

    bool ok = false;
    char rasterName[16];
#ifdef PRIM32_HAS_FREETYPE
    // FreeType covers every plane (astral CJK extensions, etc.)
    ok = Prim32FontInitFT(atlas, data, size, sizePixels, true);
    snprintf(rasterName, sizeof(rasterName), "freetype");
    if (!ok) SetErr("LoadFontFromMemory: FreeType could not load the face");
#else
    // GDI (zero dependencies): full Basic Multilingual Plane — CJK, Cyrillic,
    // Greek, Arabic glyphs, icon fonts in the PUA, ...
    wchar_t family[64];
    if (!TtfFamilyName((const uint8_t*)data, size, family)) {
        free(atlas);
        SetErr("LoadFontFromMemory: could not parse font family name (not a TTF/OTF/TTC?)");
        return InvalidFontHandle;
    }
    ok = Prim32FontInitGDI(atlas, data, size, family, sizePixels, true);
    snprintf(rasterName, sizeof(rasterName), "gdi");
    if (!ok) SetErr("LoadFontFromMemory: GDI could not load the face");
#endif
    if (!ok) { free(atlas); return InvalidFontHandle; }

    Prim32PrewarmAscii(atlas);              // basic Latin never rasterizes mid-frame

    uint32_t idx = RegAllocFont();
    if (!idx) {
        Prim32FreeFontAtlas(atlas); free(atlas);
        SetErr("LoadFontFromMemory: font table full (%u)", MAX_FONTS);
        return InvalidFontHandle;
    }
    FontRes& f = s_fonts[idx];
    uint32_t gen = f.gen + 1; if (!gen) gen = 1;
    f = {};
    f.gen = gen; f.used = true; f.ownsAtlas = true;
    f.texSlot = atlas->pageSlot;
    f.atlas = atlas;
    f.bytes = (uint64_t)atlas->pageW * atlas->pageH * atlas->pageCount;
    snprintf(f.name, sizeof(f.name), "font %.0fpx (%s)", sizePixels, rasterName);
    return { idx, f.gen };
}

FontHandle LoadFontFromFile(const char* path, float sizePixels) {
    s_lastErr[0] = 0;
    FILE* fp = fopen(path, "rb");
    if (!fp) { SetErr("LoadFontFromFile: cannot open '%s'", path); return InvalidFontHandle; }
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (n <= 0) { fclose(fp); SetErr("LoadFontFromFile: empty file '%s'", path); return InvalidFontHandle; }
    void* buf = malloc((size_t)n);
    size_t rd = fread(buf, 1, (size_t)n, fp);
    fclose(fp);
    if (rd != (size_t)n) { free(buf); SetErr("LoadFontFromFile: read failed '%s'", path); return InvalidFontHandle; }
    FontHandle h = LoadFontFromMemory(buf, (size_t)n, sizePixels);
    free(buf);
    if (h != InvalidFontHandle) {
        FontRes& f = s_fonts[h.index];
        char full[64]; snprintf(full, sizeof(full), "%s", f.name);
        CopyNameTail(f.name, sizeof(f.name), path);
        (void)full;
    }
    return h;
}

void DestroyFont(FontHandle h) {
    FontRes* r = RegGetFont(h);
    if (!r) return;
    if (h.index == 1) { SetErr("DestroyFont: the built-in font cannot be destroyed"); return; }
    if (s_hooks && r->atlas)
        for (int p = 0; p < r->atlas->pageCount; p++)
            s_hooks->destroyTexture(r->atlas->pageSlots[p]);   // fence-deferred
    if (r->ownsAtlas && r->atlas) { Prim32FreeFontAtlas(r->atlas); free(r->atlas); }
    r->atlas = nullptr;
    r->used = false;
    r->gen++; if (!r->gen) r->gen = 1;                  // stale handles now invalid
    // font-stack / default entries pointing here now safely resolve to fallback
}

// ------------------------------------------------------------- image decode
// WIC decoder, fully isolated. Everything OS/format-specific lives here.
static const CLSID kCLSID_WICImagingFactory = { 0xcacaf262, 0x9370, 0x4615, { 0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a } };
static const IID   kIID_IWICImagingFactory  = { 0xec5ec8a9, 0xc395, 0x4314, { 0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70 } };
static const GUID  kGUID_WICPixelFormat32bppRGBA = { 0xf5c7ad2d, 0x6a8d, 0x43dd, { 0xa7, 0xa8, 0xa2, 0x99, 0x35, 0x26, 0x1a, 0xe9 } };

static IWICImagingFactory* WicFactory() {
    static IWICImagingFactory* fac = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        (void)hr;   // S_FALSE / RPC_E_CHANGED_MODE are fine — COM is up either way
        CoCreateInstance(kCLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                         kIID_IWICImagingFactory, (void**)&fac);
    }
    return fac;
}

// Decodes to a malloc'd RGBA8 buffer (caller frees). Source can be a file
// path (utf8) or a memory blob.
static uint8_t* DecodeImageWIC(const char* path, const void* data, size_t size, int* outW, int* outH) {
    IWICImagingFactory* fac = WicFactory();
    if (!fac) { SetErr("image decode: WIC unavailable"); return nullptr; }

    IWICBitmapDecoder* dec = nullptr;
    IWICStream* stream = nullptr;
    if (path) {
        wchar_t wpath[512];
        MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 512);
        if (FAILED(fac->CreateDecoderFromFilename(wpath, nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnDemand, &dec))) {
            SetErr("image decode: cannot open/decode '%s'", path);
            return nullptr;
        }
    } else {
        if (FAILED(fac->CreateStream(&stream)) ||
            FAILED(stream->InitializeFromMemory((BYTE*)data, (DWORD)size)) ||
            FAILED(fac->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &dec))) {
            if (stream) stream->Release();
            SetErr("image decode: invalid/unsupported encoded bytes (%zu bytes)", size);
            return nullptr;
        }
    }

    uint8_t* pixels = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapSource* rgba = nullptr;
    UINT w = 0, h = 0;
    if (SUCCEEDED(dec->GetFrame(0, &frame)) &&
        SUCCEEDED(WICConvertBitmapSource(kGUID_WICPixelFormat32bppRGBA, frame, &rgba)) &&
        SUCCEEDED(rgba->GetSize(&w, &h)) && w && h && w <= 16384 && h <= 16384) {
        pixels = (uint8_t*)malloc((size_t)w * h * 4);
        if (pixels && FAILED(rgba->CopyPixels(nullptr, w * 4, w * h * 4, pixels))) {
            free(pixels); pixels = nullptr;
        }
    }
    if (!pixels) SetErr("image decode: frame conversion failed%s%s", path ? " for " : "", path ? path : "");
    if (rgba) rgba->Release();
    if (frame) frame->Release();
    dec->Release();
    if (stream) stream->Release();
    *outW = (int)w; *outH = (int)h;
    return pixels;
}

// ------------------------------------------------------------- image loading
static ImageHandle RegisterDecoded(uint8_t* pixels, int w, int h, const char* debugName) {
    if (!s_hooks) { free(pixels); SetErr("LoadImage: backend not initialized"); return InvalidImageHandle; }
    uint32_t slot = s_hooks->createTexture(pixels, w, h, 1, debugName);
    free(pixels);                                        // uploaded; CPU copy gone
    if (slot == 0xFFFFFFFFu) { SetErr("LoadImage: GPU texture creation failed (%dx%d)", w, h); return InvalidImageHandle; }
    uint32_t idx = RegAllocImage();
    if (!idx) { s_hooks->destroyTexture(slot); SetErr("LoadImage: image table full (%u)", MAX_IMAGES); return InvalidImageHandle; }
    ImageRes& r = s_images[idx];
    uint32_t gen = r.gen + 1; if (!gen) gen = 1;
    r = {};
    r.gen = gen; r.used = true;
    r.texSlot = slot; r.w = w; r.h = h;
    r.bytes = (uint64_t)w * h * 4;
    CopyNameTail(r.name, sizeof(r.name), debugName);
    return { idx, r.gen };
}

ImageHandle LoadImageFromFile(const char* path) {
    s_lastErr[0] = 0;
    if (!path || !*path) { SetErr("LoadImageFromFile: empty path"); return InvalidImageHandle; }
    int w = 0, h = 0;
    uint8_t* px = DecodeImageWIC(path, nullptr, 0, &w, &h);
    if (!px) return InvalidImageHandle;
    return RegisterDecoded(px, w, h, path);
}

ImageHandle LoadImageFromMemory(const void* data, size_t size) {
    s_lastErr[0] = 0;
    if (!data || size < 8) { SetErr("LoadImageFromMemory: empty/short data"); return InvalidImageHandle; }
    int w = 0, h = 0;
    uint8_t* px = DecodeImageWIC(nullptr, data, size, &w, &h);
    if (!px) return InvalidImageHandle;
    return RegisterDecoded(px, w, h, "(memory image)");
}

void DestroyImage(ImageHandle h) {
    ImageRes* r = RegGetImage(h);
    if (!r) return;
    if (s_hooks) s_hooks->destroyTexture(r->texSlot);
    r->used = false;
    r->gen++; if (!r->gen) r->gen = 1;
}

// ------------------------------------------------------- DrawList image draws
static void EmitImage(Context* c, uint32_t slot, float x0, float y0, float x1, float y1,
                      Vec2 uv0, Vec2 uv1, Col tint) {
    Prim* p = AddPrims(c, 1); if (!p) return;
    p->x0 = x0; p->y0 = y0; p->x1 = x1; p->y1 = y1;
    p->uv0 = PackUV(uv0.x, uv0.y); p->uv1 = PackUV(uv1.x, uv1.y);
    p->color = tint;
    p->meta = PackMeta(PRIM_IMAGE, c->curClip, slot);
}
static void ImageFallback(Context* c, float x0, float y0, float x1, float y1, ImageHandle h) {
#ifndef NDEBUG
    // debug: loud magenta placeholder + one-shot log
    DrawRect(c, x0, y0, x1, y1, COL32(255, 0, 255, 160));
    DrawRectStroke(c, x0, y0, x1, y1, COL32(0, 0, 0, 200), 0, 1.0f);
    SetErr("draw->Image: invalid handle {%u,%u}", h.index, h.gen);
#else
    (void)c; (void)x0; (void)y0; (void)x1; (void)y1; (void)h;   // release: draw nothing
#endif
}

void DrawList::Image(ImageHandle img, const prim32::Rect& r) { Image(img, r, Vec2{0,0}, Vec2{1,1}, 0xFFFFFFFFu); }
void DrawList::Image(ImageHandle img, const prim32::Rect& r, Col tint) { Image(img, r, Vec2{0,0}, Vec2{1,1}, tint); }
void DrawList::Image(ImageHandle img, const prim32::Rect& r, Vec2 uv0, Vec2 uv1, Col tint) {
    ImageRes* res = RegGetImage(img);
    if (!res) { ImageFallback(ctx, r.x, r.y, r.x + r.w, r.y + r.h, img); return; }
    EmitImage(ctx, res->texSlot, r.x, r.y, r.x + r.w, r.y + r.h, uv0, uv1, tint);
}
void DrawList::Image(ImageHandle img, Vec2 pos) {
    ImageRes* res = RegGetImage(img);
    if (!res) { ImageFallback(ctx, pos.x, pos.y, pos.x + 32, pos.y + 32, img); return; }
    EmitImage(ctx, res->texSlot, pos.x, pos.y, pos.x + res->w, pos.y + res->h, {0,0}, {1,1}, 0xFFFFFFFFu);
}
void DrawList::Image(ImageHandle img, Vec2 pos, Vec2 size, Col tint) {
    Image(img, prim32::Rect{ pos.x, pos.y, size.x, size.y }, tint);
}

} // namespace prim32
