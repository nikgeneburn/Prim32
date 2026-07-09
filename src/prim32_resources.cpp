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
    uint32_t  gen;
    bool      used;
    bool      ownsAtlas;   // built-in font's atlas is owned by the Context
    uint32_t  texSlot;
    FontAtlas atlas;
    uint64_t  bytes;
    char      name[48];
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
    f.texSlot = 0;                       // backend always uploads it to slot 0
    f.atlas = ctx->font;                 // shared pointers, not owned
    f.bytes = (uint64_t)ctx->font.width * ctx->font.height;
    snprintf(f.name, sizeof(f.name), "built-in (%.0fpx)", ctx->font.size);
    if (s_fontHigh < 2) s_fontHigh = 2;
}

ResolvedFont Prim32ResolveFont(FontHandle h) {
    Context* c = GetContext();
    if (FontRes* r = RegGetFont(h)) return { &r->atlas, r->texSlot };
    if (c && c->fontStackTop >= 0)
        if (FontRes* r = RegGetFont(c->fontStack[c->fontStackTop]))
            return { &r->atlas, r->texSlot };
    if (c)
        if (FontRes* r = RegGetFont(c->defaultFont))
            return { &r->atlas, r->texSlot };
    return { &s_fonts[1].atlas, s_fonts[1].texSlot };    // built-in fallback
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

    wchar_t family[64];
    if (!TtfFamilyName((const uint8_t*)data, size, family)) {
        SetErr("LoadFontFromMemory: could not parse font family name (not a TTF/OTF/TTC?)");
        return InvalidFontHandle;
    }
    // GDI copies the font data — caller's buffer is free to go after this call.
    DWORD installed = 0;
    HANDLE mem = AddFontMemResourceEx((PVOID)data, (DWORD)size, nullptr, &installed);
    if (!mem || !installed)          { SetErr("LoadFontFromMemory: AddFontMemResourceEx failed"); return InvalidFontHandle; }

    FontAtlas atlas = {};
    bool ok = Prim32BakeFont(&atlas, family, sizePixels, true);
    RemoveFontMemResourceEx(mem);    // atlas is rasterized; GDI font no longer needed
    if (!ok)                         { SetErr("LoadFontFromMemory: rasterization failed"); return InvalidFontHandle; }

    uint32_t slot = s_hooks->createTexture(atlas.pixels, atlas.width, atlas.height, 0, "font atlas");
    if (slot == 0xFFFFFFFFu) {
        Prim32FreeFontAtlas(&atlas);
        SetErr("LoadFontFromMemory: GPU atlas creation failed");
        return InvalidFontHandle;
    }
    uint32_t idx = RegAllocFont();
    if (!idx) { s_hooks->destroyTexture(slot); Prim32FreeFontAtlas(&atlas); SetErr("LoadFontFromMemory: font table full (%u)", MAX_FONTS); return InvalidFontHandle; }

    FontRes& f = s_fonts[idx];
    uint32_t gen = f.gen + 1; if (!gen) gen = 1;
    f = {};
    f.gen = gen; f.used = true; f.ownsAtlas = true;
    f.texSlot = slot; f.atlas = atlas;
    f.bytes = (uint64_t)atlas.width * atlas.height;
    char nameA[64]; int k = 0;
    for (; family[k] && k < 47; k++) nameA[k] = family[k] < 128 ? (char)family[k] : '?';
    nameA[k] = 0;
    snprintf(f.name, sizeof(f.name), "%s %.0fpx", nameA, sizePixels);
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
    if (s_hooks) s_hooks->destroyTexture(r->texSlot);
    if (r->ownsAtlas) Prim32FreeFontAtlas(&r->atlas);
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
