// ============================================================================
// prim32 internal — shared between prim32.cpp, prim32_resources.cpp and the backend.
// Not part of the public API.
// ============================================================================
#pragma once
#include <prim32/prim32.h>

namespace prim32 {

// ---- dynamic glyph cache -------------------------------------------------
// One rasterized glyph, A8 coverage. pixels are valid until the next raster
// call on the same font (rasterizers own a scratch buffer).
struct GlyphBitmap {
    int   w, h;
    int   bearingX, bearingY;   // left offset, top offset (y up from baseline)
    float advance;
    const uint8_t* pixels;
    int   pitch;
};

// Pure cache core (prim32.cpp; natively unit-tested):
uint32_t    Prim32CacheFind(const FontAtlas* fa, uint32_t cp);       // pool idx, 0 = miss
bool        Prim32CacheInsert(FontAtlas* fa, uint32_t cp, uint32_t glyphIdx);
bool        Prim32PackRect(FontAtlas* fa, int w, int h, int* outX, int* outY);
uint32_t    Prim32Utf8Next(const char** s, const char* end);         // 0xFFFD on malformed

// Rasterize-on-miss (prim32_resources.cpp). Never returns null — missing
// glyphs resolve to the notdef box (pool index 0). The returned pointer is
// valid until the next Prim32GetGlyph call (the pool may grow).
Glyph*      Prim32GetGlyph(FontAtlas* fa, uint32_t cp);

// Atlas + rasterizer lifecycle (prim32_resources.cpp)
bool Prim32AtlasInit(FontAtlas* fa, float sizePx);                   // pool, maps, page 0, notdef
bool Prim32FontInitGDI(FontAtlas* fa, const void* data, size_t size, // data==null -> system face
                        const wchar_t* face, float sizePx, int weight, bool kerning);
#ifdef PRIM32_HAS_FREETYPE
bool Prim32FontInitFT(FontAtlas* fa, const void* data, size_t size, float sizePx, bool kerning);
bool Prim32RasterGlyphFT(FontAtlas* fa, uint32_t cp, GlyphBitmap* out);
void Prim32RasterFreeFT(FontAtlas* fa);
#endif
bool Prim32RasterGlyphGDI(FontAtlas* fa, uint32_t cp, GlyphBitmap* out);
void Prim32PrewarmAscii(FontAtlas* fa);                              // rasterize 32..126 now
void Prim32FreeFontAtlas(FontAtlas* fa);                             // pool/maps/page/kern/raster

// Backend services registered by the renderer at Init. fmt: 0 = R8, 1 = RGBA8.
// createTexture uploads immediately (load-time op) and returns a texture slot
// usable in Prim meta, or 0xFFFFFFFF on failure. destroyTexture recycles the
// slot fence-safely.
struct Prim32BackendHooks {
    uint32_t (*createTexture)(const void* pixels, int w, int h, int fmt, const char* debugName);
    void     (*destroyTexture)(uint32_t slot);
    // Queues a region update (backend copies pixels immediately and flushes
    // all queued regions at the start of the next Render). Used for
    // incremental glyph-atlas uploads.
    void     (*updateTexture)(uint32_t slot, int x, int y, int w, int h, const uint8_t* pixels, int pitch);
};
void                    Prim32SetBackendHooks(const Prim32BackendHooks* hooks);
const Prim32BackendHooks* Prim32GetBackendHooks();

// Registry internals shared with the core text path (prim32_resources.cpp).
// Non-const: text can lazily rasterize new glyphs during draw AND measure.
struct ResolvedFont { FontAtlas* atlas; };
ResolvedFont Prim32ResolveFont(FontHandle explicitFont);   // full lookup chain

// Text core (prim32.cpp) — measurement/wrapping shared with DrawTextOpt.
// UTF-8 aware; CJK ideographs are treated as breakable for wrapping.
float       Prim32LineWidth(FontAtlas* fa, const char* s, const char* end);
const char* Prim32WrapBreak(FontAtlas* fa, const char* s, const char* end, float wrapW, float* outWidth);
Vec2        Prim32MeasureAtlas(FontAtlas* fa, const char* text, const char* end, float wrapW, float lineSpacing);
void         Prim32RegisterBuiltinFont(Context* ctx);

} // namespace prim32
