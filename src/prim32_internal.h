// ============================================================================
// prim32 internal — shared between prim32.cpp, prim32_resources.cpp and the backend.
// Not part of the public API.
// ============================================================================
#pragma once
#include <prim32/prim32.h>

namespace prim32 {

// GDI font baker (prim32.cpp). face may be a private face installed via
// AddFontMemResourceEx. Fills atlas pixels (caller owns via FreeFontAtlas).
bool Prim32BakeFont(FontAtlas* fa, const wchar_t* face, float sizePx, bool kerning);
void Prim32FreeFontAtlas(FontAtlas* fa);

// Backend services registered by the renderer at Init. fmt: 0 = R8, 1 = RGBA8.
// createTexture uploads immediately (load-time op) and returns a texture slot
// usable in Prim meta, or 0xFFFFFFFF on failure. destroyTexture recycles the
// slot fence-safely.
struct Prim32BackendHooks {
    uint32_t (*createTexture)(const void* pixels, int w, int h, int fmt, const char* debugName);
    void     (*destroyTexture)(uint32_t slot);
};
void                    Prim32SetBackendHooks(const Prim32BackendHooks* hooks);
const Prim32BackendHooks* Prim32GetBackendHooks();

// Registry internals shared with the core text path (prim32_resources.cpp).
struct ResolvedFont { const FontAtlas* atlas; uint32_t texSlot; };
ResolvedFont Prim32ResolveFont(FontHandle explicitFont);   // full lookup chain

// Pure text core (prim32.cpp) — measurement/wrapping shared with DrawTextOpt
float       Prim32LineWidth(const FontAtlas* fa, const char* s, const char* end);
const char* Prim32WrapBreak(const FontAtlas* fa, const char* s, const char* end, float wrapW, float* outWidth);
Vec2        Prim32MeasureAtlas(const FontAtlas* fa, const char* text, const char* end, float wrapW, float lineSpacing);
void         Prim32RegisterBuiltinFont(Context* ctx);      // wires ctx->font as font #1

} // namespace prim32
