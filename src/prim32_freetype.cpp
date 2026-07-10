// ============================================================================
// prim32 FreeType rasterizer — compiled in when PRIM32_HAS_FREETYPE is defined
// (add FreeType's include dir and link freetype.lib; e.g. via vcpkg:
//  `vcpkg install freetype` then define PRIM32_HAS_FREETYPE for the library).
//
// Why FreeType: coverage of every Unicode plane (GDI's lazy path is limited to
// the BMP), consistent hinting/AA across machines, and fonts that Windows'
// GDI cannot load. The rest of the engine — dynamic glyph cache, multi-page
// atlases, incremental uploads — is rasterizer-agnostic.
// ============================================================================
#ifdef PRIM32_HAS_FREETYPE

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#ifdef DrawText
#undef DrawText
#endif
#include <stdlib.h>
#include <string.h>
#include <prim32/prim32.h>
#include "prim32_internal.h"

#include <ft2build.h>
#include FT_FREETYPE_H

namespace prim32 {

static FT_Library s_ft;
static bool s_ftInit, s_ftOk;

static bool FtLib() {
    if (!s_ftInit) { s_ftInit = true; s_ftOk = FT_Init_FreeType(&s_ft) == 0; }
    return s_ftOk;
}

// rasterA = FT_Face, rasterB = owned copy of the font bytes (FT streams from it)
bool Prim32FontInitFT(FontAtlas* fa, const void* data, size_t size, float sizePx, bool kerning) {
    (void)kerning;
    memset(fa, 0, sizeof(*fa));
    fa->rasterKind = 1;
    if (!FtLib()) return false;

    void* copy = malloc(size);                 // FT_New_Memory_Face requires the
    if (!copy) return false;                   // bytes to stay alive; we own a copy
    memcpy(copy, data, size);

    FT_Face face = nullptr;
    if (FT_New_Memory_Face(s_ft, (const FT_Byte*)copy, (FT_Long)size, 0, &face) != 0) {
        free(copy);
        return false;
    }
    if (FT_Set_Pixel_Sizes(face, 0, (FT_UInt)(sizePx + 0.5f)) != 0) {
        FT_Done_Face(face);
        free(copy);
        return false;
    }
    fa->rasterA = face;
    fa->rasterB = copy;
    // FreeType reports 26.6 metrics. Truncating them puts hinted bitmap rows
    // outside the nominal line box at small sizes, clipping capitals and
    // descenders. Round out and reserve one pixel above/below each line.
    FT_Pos asc = face->size->metrics.ascender;
    FT_Pos desc = -face->size->metrics.descender;
    FT_Pos height = face->size->metrics.height;
    fa->ascent = (float)((asc + 63) >> 6) + 1.0f;
    fa->descent = (float)((desc + 63) >> 6) + 1.0f;
    fa->lineHeight = (float)((height + 63) >> 6);
    if (fa->lineHeight < fa->ascent + fa->descent)
        fa->lineHeight = fa->ascent + fa->descent;
    fa->size       = sizePx;

    // kerning table for Latin pairs (matches the binary-searched BMP table)
    if (FT_HAS_KERNING(face)) {
        uint32_t cap = 512, m = 0;
        fa->kernKeys = (uint32_t*)malloc(cap * 4);
        fa->kernVals = (float*)malloc(cap * 4);
        for (uint32_t a = 32; a < 127 && fa->kernKeys; a++) {
            FT_UInt ga = FT_Get_Char_Index(face, a);
            if (!ga) continue;
            for (uint32_t b = 32; b < 127; b++) {
                FT_UInt gb = FT_Get_Char_Index(face, b);
                if (!gb) continue;
                FT_Vector kv;
                if (FT_Get_Kerning(face, ga, gb, FT_KERNING_DEFAULT, &kv) == 0 && kv.x) {
                    if (m == cap) {
                        cap *= 2;
                        fa->kernKeys = (uint32_t*)realloc(fa->kernKeys, cap * 4);
                        fa->kernVals = (float*)realloc(fa->kernVals, cap * 4);
                        if (!fa->kernKeys || !fa->kernVals) break;
                    }
                    fa->kernKeys[m] = (a << 16) | b;   // already in ascending order
                    fa->kernVals[m] = (float)(kv.x >> 6);
                    m++;
                }
            }
        }
        fa->kernCount = m;
    }
    if (!Prim32AtlasInit(fa, sizePx)) {
        Prim32RasterFreeFT(fa);
        return false;
    }
    return true;
}

bool Prim32RasterGlyphFT(FontAtlas* fa, uint32_t cp, GlyphBitmap* out) {
    FT_Face face = (FT_Face)fa->rasterA;
    if (!face) return false;
    FT_UInt gi = FT_Get_Char_Index(face, (FT_ULong)cp);
    if (!gi) return false;                     // no glyph -> caller uses notdef
    if (FT_Load_Glyph(face, gi, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL) != 0) return false;
    FT_GlyphSlot g = face->glyph;
    out->w        = (int)g->bitmap.width;
    out->h        = (int)g->bitmap.rows;
    out->bearingX = g->bitmap_left;
    out->bearingY = g->bitmap_top;
    out->advance  = (float)(g->advance.x >> 6);
    out->pixels   = g->bitmap.buffer;          // FT owns; valid until next load
    out->pitch    = g->bitmap.pitch;
    return out->pitch >= 0;                    // (negative-pitch bitmaps unsupported)
}

void Prim32RasterFreeFT(FontAtlas* fa) {
    if (fa->rasterA) FT_Done_Face((FT_Face)fa->rasterA);
    free(fa->rasterB);
    fa->rasterA = fa->rasterB = nullptr;
}

} // namespace prim32

#endif // PRIM32_HAS_FREETYPE
