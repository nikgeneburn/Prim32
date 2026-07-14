// ============================================================================
// Prim32 benchmark & demo
//   - flip-model swapchain with tearing (true uncapped FPS)
//   - stress modes: solid rects / rounded / circles / glyphs / widget clusters
//     from 1k to 2,000,000 primitives, fully re-submitted every frame
//   - live stats: FPS, frame ms, CPU build ms, GPU ms, MB/s streamed
//   F11 = borderless fullscreen
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdio.h>
#include <math.h>
#include <emmintrin.h>   // SSE2: baseline on x64
#ifdef DrawText
#undef DrawText
#endif
#include <prim32/prim32.h>
#include <prim32/prim32_dx12.h>
#include <prim32/p32prof.h>
#include "embedded_png.h"

using namespace prim32;

template <class T> static void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

// ------------------------------------------------------------------ D3D state
static const UINT kFrames = 3;
static HWND                       g_hwnd;
static ID3D12Device*              g_device;
static ID3D12CommandQueue*        g_queue;
static IDXGISwapChain3*           g_swap;
static ID3D12DescriptorHeap*      g_rtvHeap;
static ID3D12Resource*            g_back[kFrames];
static D3D12_CPU_DESCRIPTOR_HANDLE g_rtv[kFrames];
static ID3D12CommandAllocator*    g_alloc[kFrames];
static ID3D12GraphicsCommandList* g_cl;
static ID3D12Fence*               g_fence;
static HANDLE                     g_fenceEvent;
static UINT64                     g_fenceCounter;
static UINT64                     g_slotFence[kFrames];
static UINT                       g_frame;
static bool                       g_tearing;
static int                        g_width = 1680, g_height = 945;
static bool                       g_occluded;
static float                      g_mouseWheel;

static void WaitIdle() {
    g_queue->Signal(g_fence, ++g_fenceCounter);
    if (g_fence->GetCompletedValue() < g_fenceCounter) {
        g_fence->SetEventOnCompletion(g_fenceCounter, g_fenceEvent);
        WaitForSingleObject(g_fenceEvent, INFINITE);
    }
}

static void CreateRTVs() {
    UINT stride = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kFrames; i++) {
        g_swap->GetBuffer(i, IID_PPV_ARGS(&g_back[i]));
        g_rtv[i] = { h.ptr + (size_t)i * stride };
        g_device->CreateRenderTargetView(g_back[i], nullptr, g_rtv[i]);
    }
    p32prof::TrackMem("gpu: swapchain buffers", (uint64_t)g_width * g_height * 4 * kFrames);
}

static void Resize(int w, int h) {
    if (!g_swap || w <= 0 || h <= 0) return;
    WaitIdle();
    for (UINT i = 0; i < kFrames; i++) SafeRelease(g_back[i]);
    g_swap->ResizeBuffers(kFrames, w, h, DXGI_FORMAT_UNKNOWN,
                          g_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    g_width = w; g_height = h;   // BEFORE CreateRTVs so TrackMem sees the new size
    CreateRTVs();
}

static bool InitD3D() {
#if defined(_DEBUG)
    { ID3D12Debug* dbg = nullptr;
      if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) { dbg->EnableDebugLayer(); dbg->Release(); } }
#endif
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_device)))) return false;

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(g_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_queue)))) return false;

    IDXGIFactory4* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
    { IDXGIFactory5* f5 = nullptr;
      if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&f5)))) {
          BOOL allow = FALSE;
          if (SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))))
              g_tearing = allow != 0;
          f5->Release();
      } }

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = g_width; sd.Height = g_height;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = kFrames;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = g_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    IDXGISwapChain1* sc1 = nullptr;
    if (FAILED(factory->CreateSwapChainForHwnd(g_queue, g_hwnd, &sd, nullptr, nullptr, &sc1))) { factory->Release(); return false; }
    sc1->QueryInterface(IID_PPV_ARGS(&g_swap));
    sc1->Release();
    factory->MakeWindowAssociation(g_hwnd, DXGI_MWA_NO_ALT_ENTER);
    factory->Release();

    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = kFrames;
    g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_rtvHeap));
    CreateRTVs();

    for (UINT i = 0; i < kFrames; i++)
        g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_alloc[i]));
    g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc[0], nullptr, IID_PPV_ARGS(&g_cl));
    g_cl->Close();

    g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));
    g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return true;
}

// ------------------------------------------------------------------ stress UI
static int   s_mode = 1;            // 0 rects 1 rounded 2 circles 3 glyphs 4 clusters 5 overdraw 6 shadows
static int   s_countExp = 60;       // slider 0..100 -> 1k..2M log scale
static bool  s_animate = true;
static bool  s_bigQuads = false;
static bool  s_vsync = false;
static bool  s_extraWindows = false;
static bool  s_profiler = true;

// ---- efficiency governor: highest fps that fits in a CPU/GPU budget ----
static bool  s_budget = false;
static float s_budgetCpu = 1.0f;    // % of the whole machine (Task-Manager style)
static float s_budgetGpu = 1.0f;    // % of the GPU (3D engine busy)
static float s_trimCpu = 1.0f, s_trimGpu = 1.0f;   // feedback correction
static float s_lastSleepMs = 0, s_lastWaitMs = 0;
static float s_activeMs = 0.5f;     // EMA of real CPU work per frame (wall - waits)
static float s_gpuCostMs = 0.2f;    // EMA of GPU work per frame (zone total)
static float s_periodMs = 0;        // governed frame period

static inline float ClampF(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

// avg ms of a top-level CPU scope, from the profiler's window stats
static float CpuScopeAvg(const char* name) {
    for (int i = p32prof::NodeFirstChild(0); i != -1; i = p32prof::NodeNextSibling(i)) {
        const p32prof::NodeView& v = p32prof::NodeGet(i);
        if (!strcmp(v.name, name)) return v.avgMs;
    }
    return 0.0f;
}

// Which wall are we on? Computed from measured splits so the answer is
// always in front of you instead of needing dump forensics.
static const char* BottleneckVerdict() {
    float frame = p32prof::FrameAvgMs();
    if (frame < 0.01f) return "warming up";
    p32prof::ZoneStats zs;
    float gpu = 0;
    if (p32prof::GetGpuZoneWindow("UI pass", &zs)) gpu += zs.avgMs;
    if (p32prof::GetGpuZoneWindow("Clear+Barrier", &zs)) gpu += zs.avgMs;
    float build = CpuScopeAvg("BuildUI");
    float wait  = CpuScopeAvg("WaitFrameFence");
    float pres  = CpuScopeAvg("Present");
    if (s_budget)               return "budget governor (sleeping by design)";
    if (s_vsync)                return "vsync (waiting for vblank by design)";
    if (gpu > 0.6f * frame)     return "GPU raster/fill -- caching can't help; fewer/smaller prims or lower res";
    if (build > 0.5f * frame)   return "CPU emit/build -- caching and SIMD emit help here";
    if (wait + pres > 0.4f * frame && gpu < 0.4f * frame)
                                return "present pacing (windowed compositor) -- try F11 fullscreen";
    return "mixed / idle";
}

// Feed-forward from measured per-frame cost + slow feedback against the
// process-wide measured usage (GetProcessTimes / PDH GPU engines), then
// sleep the remainder of the period on a high-resolution waitable timer.
static void GovernorSleep() {
    static HANDLE timer = nullptr; static bool tried = false;
    static LARGE_INTEGER qpf = {}; static uint64_t nextDue = 0;
    if (!qpf.QuadPart) QueryPerformanceFrequency(&qpf);
    if (!tried) {
        tried = true;
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
        timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer) timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }
    // reset the controller whenever the budgets change (no stale state)
    static float pbc = -1, pbg = -1;
    if (pbc != s_budgetCpu || pbg != s_budgetGpu) {
        pbc = s_budgetCpu; pbg = s_budgetGpu;
        s_trimCpu = 1.0f; s_trimGpu = 1.0f; nextDue = 0;
    }
    const p32prof::ProcStats& ps = p32prof::GetProcStats();
    const p32prof::GpuZone* z; int zn = p32prof::GetGpuZones(&z);
    float gpuMs = 0, gpuMinMs = 0;
    for (int i = 0; i < zn; i++) {
        if (z[i].depth != 0) continue;
        gpuMs += z[i].ms;
        p32prof::ZoneStats zs;
        if (p32prof::GetGpuZoneWindow(z[i].name, &zs)) gpuMinMs += zs.minMs;
    }
    if (gpuMs > 0.0f) s_gpuCostMs += (gpuMs - s_gpuCostMs) * 0.3f;

    int cores = ps.cores > 0 ? ps.cores : 1;
    // GPU duty cycle from the windowed MINIMUM zone cost — the cost at the
    // highest clock recently reached. Governing on instantaneous cost lets
    // idle downclocking (~10x slower) drag the period into single-digit fps.
    float gpuCost = gpuMinMs > 0.01f ? gpuMinMs : s_gpuCostMs;
    float pGpu = gpuCost / (s_budgetGpu * 0.01f);
    // CPU: feed-forward + small clamped trim vs measured process usage
    // (covers driver threads etc.). Deadband stops integrator windup.
    if (ps.cpuTotalPct > 0.001f) {
        float r = ps.cpuTotalPct / s_budgetCpu;
        if      (r > 1.15f) s_trimCpu = ClampF(s_trimCpu * 1.01f,  0.7f, 2.5f);
        else if (r < 0.85f) s_trimCpu = ClampF(s_trimCpu * 0.995f, 0.7f, 2.5f);
    }
    float pCpu = s_activeMs / (s_budgetCpu * 0.01f * cores) * s_trimCpu;
    s_periodMs = ClampF(pGpu > pCpu ? pGpu : pCpu, 0.0f, 500.0f);

    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    uint64_t periodTicks = (uint64_t)(s_periodMs * (double)qpf.QuadPart / 1000.0);
    if (!nextDue || nextDue + periodTicks < (uint64_t)now.QuadPart) nextDue = (uint64_t)now.QuadPart;
    nextDue += periodTicks;
    if ((uint64_t)now.QuadPart < nextDue) {
        double remainMs = (double)(nextDue - (uint64_t)now.QuadPart) * 1000.0 / (double)qpf.QuadPart;
        s_lastSleepMs = (float)remainMs;
        if (timer) {
            LARGE_INTEGER due; due.QuadPart = -(LONGLONG)(remainMs * 10000.0);
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, (DWORD)(remainMs + 32.0));
        } else {
            Sleep((DWORD)remainMs);
        }
    } else {
        s_lastSleepMs = 0;
    }
}

static uint32_t StressCount() {
    // log scale: 1'000 .. 2'000'000
    float t = s_countExp / 100.0f;
    return (uint32_t)(1000.0 * pow(2000.0, (double)t));
}

static inline uint32_t Hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}

static bool s_cacheStress = false;

// SIMD grid emitter (modes 0/1/2). Works identically when redirected into a
// cached layer by BeginCache().
static void EmitGrid(Context* c, uint32_t n, float W, float H,
                     const float* wobX, const float* wobY) {
    // rects / rounded / circles — SIMD emit path.
    // Everything invariant is hoisted into per-column tables once per frame;
    // the inner loop is one SSE2 add + xor and two non-temporal 16-byte
    // streams per primitive, written straight into mapped GPU memory.
    P32PROF_SCOPE("emit grid");
    int cols = (int)ceilf(sqrtf((float)n * (W / H)));
    if (cols < 1) cols = 1;
    if (cols > 4096) cols = 4096;
    int rows = (int)((n + cols - 1) / cols);
    float cellW = W / cols, cellH = H / (rows > 0 ? rows : 1);
    float sz = s_bigQuads ? 150.0f : (cellW < cellH ? cellW : cellH) * 0.9f;
    if (!s_bigQuads && sz > 22) sz = 22;
    if (sz < 0.75f) sz = 0.75f;

    const uint32_t type = s_mode == 0 ? PRIM_RECT : s_mode == 1 ? PRIM_RECT_ROUNDED : PRIM_CIRCLE;
    const uint32_t meta = PackMeta(type, c->curClip, 0);
    const uint32_t uv0  = s_mode == 1 ? PackF16x2(sz * 0.3f, 0.0f) : 0;
    const uint32_t alpha = (s_bigQuads ? 0x5Au : 0xFFu) << 24;

    static __m128  colRect[4096];   // {x0, 0, x1, 0}
    static __m128i colTail[4096];   // {uv0, uv1, colHash(no alpha), meta}
    {
        P32PROF_SCOPE("column tables");
        for (int col = 0; col < cols; col++) {
            float x = col * cellW + wobX[col & 255];
            colRect[col] = _mm_setr_ps(x, 0.0f, x + sz, 0.0f);
            uint32_t hc = Hash32((uint32_t)col * 2654435761u) & 0x00FFFFFFu;
            colTail[col] = _mm_setr_epi32((int)uv0, 0, (int)hc, (int)meta);
        }
    }

    uint32_t i = 0;
    for (int r = 0; r < rows && i < n; r++) {
        float y = r * cellH + wobY[r & 255];
        __m128 rowRect = _mm_setr_ps(0.0f, y, 0.0f, y + sz);
        uint32_t hr = (Hash32((uint32_t)r * 0x9E3779B9u) & 0x00FFFFFFu) | alpha;
        __m128i rowColor = _mm_slli_si128(_mm_cvtsi32_si128((int)hr), 8);   // lane 2
        uint32_t want = n - i; if (want > (uint32_t)cols) want = (uint32_t)cols;
        Prim* p = AddPrims(c, want);
        if (!p) break;
        for (uint32_t col = 0; col < want; col++, p++) {
            _mm_stream_si128((__m128i*)p,     _mm_castps_si128(_mm_add_ps(colRect[col], rowRect)));
            _mm_stream_si128((__m128i*)p + 1, _mm_xor_si128(colTail[col], rowColor));
        }
        i += want;
    }
    _mm_sfence();   // make NT stores visible before command submission
}

static void BuildStress(Context* c, float time) {
    const uint32_t n = StressCount();
    const float W = (float)g_width, H = (float)g_height;

    // per-frame wobble table: animation for millions of prims at ~zero CPU cost
    static float wobX[256], wobY[256];
    {
    P32PROF_SCOPE("wobble table");
    if (s_animate)
        for (int i = 0; i < 256; i++) {
            wobX[i] = sinf(time * 1.9f + i * 0.245f) * 9.0f;
            wobY[i] = cosf(time * 2.3f + i * 0.171f) * 7.0f;
        }
    else
        for (int i = 0; i < 256; i++) wobX[i] = wobY[i] = 0.0f;
    }

    if (s_mode == 3) {   // glyph storm
        P32PROF_SCOPE("emit glyphs");
        static const char* line = "Prim32 streams a million glyphs a frame as 32-byte prims. ";
        const int lineLen = (int)strlen(line);
        int perRow = (int)(W / 8.5f); if (perRow < 8) perRow = 8;
        if (perRow > 500) perRow = 500;
        static char row[512];
        for (int i = 0; i < perRow; i++) row[i] = line[i % lineLen];
        row[perRow] = 0;
        int rows = (int)((n + perRow - 1) / perRow);
        float lh = c->font.lineHeight * 0.999f;
        float visRows = H / lh;
        for (int r = 0; r < rows; r++) {
            float y = fmodf(r * lh, visRows * lh) - lh;
            float x = wobX[r & 255] - 12.0f;
            Col col = COL32(120 + (Hash32(r) & 63), 160, 200 + (Hash32(r * 7) & 55), 255);
            DrawText(c, x, y + wobY[(r * 3) & 255] * 0.25f, col, row);
            if ((uint32_t)c->primCount >= n) break;   // approximate cutoff
        }
        return;
    }

    if (s_mode == 4) {   // fake widget clusters: 8 prims each
        P32PROF_SCOPE("emit widget clusters");
        uint32_t clusters = n / 8;
        float cw = 150, ch = 64;
        int cols = (int)(W / cw); if (cols < 1) cols = 1;
        for (uint32_t i = 0; i < clusters; i++) {
            uint32_t h = Hash32(i);
            float x = (float)(i % cols) * cw + wobX[i & 255] * 0.4f;
            float y = fmodf((float)(i / cols) * ch, H + ch) - ch * 0.5f + wobY[i & 255] * 0.4f;
            DrawRectRounded(c, x, y, x + cw - 8, y + ch - 8, COL32(30, 34, 44, 230), 6, COL32(24, 26, 34, 230));
            DrawRectStroke(c, x, y, x + cw - 8, y + ch - 8, COL32(80, 90, 110, 120), 6, 1);
            DrawRect(c, x + 8, y + 8, x + 8 + (h & 63), y + 14, COL32(90, 140, 220, 255));
            DrawRectRounded(c, x + 8, y + 22, x + cw - 20, y + 34, COL32(50, 56, 70, 255), 3);
            float t = ((h >> 8) & 255) / 255.0f;
            DrawCircle(c, x + 12 + t * (cw - 36), y + 28, 7, COL32(140, 180, 250, 255));
            DrawLine(c, x + 8, y + 44, x + cw - 20, y + 44, COL32(90, 100, 120, 180), 1.5f);
            DrawCircleStroke(c, x + cw - 28, y + 50, 5, COL32(120, 200, 160, 255), 2);
            DrawRect(c, x + 8, y + 48, x + 40, y + 52, COL32(74, 144, 110, 255));
        }
        return;
    }

    if (s_mode == 5) {   // overdraw: big blended gradient rects — pure fill/blend rate
        P32PROF_SCOPE("emit overdraw");
        for (uint32_t i = 0; i < n; i++) {
            uint32_t h = Hash32(i * 2654435761u);
            float sz = 220.0f + (float)(h & 255);
            float x = (float)(h % (uint32_t)(W + 400.0f)) - 200.0f + wobX[i & 255] * 2.0f;
            float y = (float)((h >> 11) % (uint32_t)(H + 400.0f)) - 200.0f + wobY[i & 255] * 2.0f;
            Col ca = COL32(60 + (h & 127), 80 + ((h >> 7) & 127), 140 + ((h >> 14) & 63), 72);
            Col cb = COL32((h >> 3) & 255, 90, 200, 60);
            DrawRectRounded(c, x, y, x + sz, y + sz * 0.8f, ca, 48.0f, cb);
        }
        return;
    }

    if (s_mode == 6) {   // shadows: 3x-inflated quads + gaussian ALU per pixel
        P32PROF_SCOPE("emit shadows");
        for (uint32_t i = 0; i < n; i++) {
            uint32_t h = Hash32(i * 0x9E3779B9u);
            float x = (float)(h % (uint32_t)W) + wobX[i & 255];
            float y = (float)((h >> 12) % (uint32_t)H) + wobY[i & 255];
            DrawShadow(c, x, y, x + 110, y + 80, COL32(0, 0, 0, 190), 14.0f, 24.0f);
        }
        return;
    }

    if (s_cacheStress) {
        // Retained path: bake ONCE into a cached layer (VRAM), then draw it
        // every frame for free. Movement is a root-constant offset: zero
        // re-emit, zero streaming, zero rebake.
        P32PROF_SCOPE("cached stress");
        static CachedLayer* cacheL = nullptr;
        static uint64_t bakedKey = ~0ull;
        static const float zeroWob[256] = {};
        uint64_t key = ((uint64_t)s_mode << 52) ^ ((uint64_t)n << 12)
                     ^ ((uint64_t)(s_bigQuads ? 1 : 0) << 8)
                     ^ ((uint64_t)g_width << 24) ^ (uint64_t)g_height;
        if (!cacheL) {
            cacheL = CreateCachedLayer(2u * 1024u * 1024u, "stress cache");
            if (cacheL) p32prof::TrackMem("cpu: stress cache staging", (uint64_t)cacheL->primCap * sizeof(Prim));
        }
        if (cacheL && bakedKey != key) {
            P32PROF_SCOPE("bake (only on change)");
            bakedKey = key;
            BeginCache(cacheL);
            EmitGrid(c, n, W, H, zeroWob, zeroWob);
            EndCache();
        }
        Vec2 off = s_animate ? Vec2{ wobX[0] * 2.5f, wobY[0] * 2.5f } : Vec2{ 0, 0 };
        DrawCached(cacheL, off);
        return;
    }

    EmitGrid(c, n, W, H, wobX, wobY);
}

// ------------------------------------------------- DrawList / resources demo
// Exercises every load path: font from file, font from memory, image from
// memory (embedded PNG), image from file (missing -> clean error), invalid
// handles, PushFont/PopFont, SetDefaultFont, MeasureText, GetImageSize,
// TextOptions (wrap/align/clip) and tinted/uv-cropped image draws.
static void ShowDrawListDemo(Context* c) {
    static bool loaded = false;
    static FontHandle titleFont, memFont, cjkFont, iconFont;
    static ImageHandle memImg, fileImg;
    static bool useMemDefault = false;
    static char log0[96], log1[96], log2[96];
    if (!loaded) {
        loaded = true;
        titleFont = LoadFontFromFile("C:\\Windows\\Fonts\\arialbd.ttf", 24.0f);
        snprintf(log0, sizeof(log0), "font file:  %s", IsValid(titleFont) ? "OK (arialbd.ttf 24px)" : GetLastResourceError());
        FILE* f = fopen("C:\\Windows\\Fonts\\consola.ttf", "rb");   // read to RAM,
        if (f) {                                                        // load, FREE:
            fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
            void* buf = malloc((size_t)n);
            if (buf && fread(buf, 1, (size_t)n, f) == (size_t)n)        // caller keeps
                memFont = LoadFontFromMemory(buf, (size_t)n, 15.0f);    // ownership
            free(buf);
            fclose(f);
        }
        snprintf(log1, sizeof(log1), "font mem:   %s", IsValid(memFont) ? "OK (consola.ttf bytes, 15px)" : GetLastResourceError());
        memImg = LoadImageFromMemory(kEmbeddedPng, sizeof(kEmbeddedPng));
        // Unicode showcase fonts (stock Windows): CJK + the MDL2 icon font
        cjkFont = LoadFontFromFile("C:\\Windows\\Fonts\\msyh.ttc", 18.0f);      // Microsoft YaHei
        if (!IsValid(cjkFont)) cjkFont = LoadFontFromFile("C:\\Windows\\Fonts\\simsun.ttc", 18.0f);
        iconFont = LoadFontFromFile("C:\\Windows\\Fonts\\segmdl2.ttf", 22.0f);   // Segoe MDL2 Assets (PUA icons)
        fileImg = LoadImageFromFile("assets/logo.png");                 // error-path demo when absent
        snprintf(log2, sizeof(log2), "image file: %s", IsValid(fileImg) ? "OK (assets/logo.png)" : GetLastResourceError());
    }
    SetNextWindowPos({ 24, 640 });
    if (Begin("DrawList showcase##dl", WF_AutoSize)) {
        DrawList* draw = GetDrawList();
        Vec2 p = GetCursorScreenPos();
        Dummy({ 440, 196 });                       // reserve the free-draw area
        float x = p.x, y = p.y;
        draw->Text("draw->Text: default font", { x, y });                       y += 20;
        draw->Text(titleFont, "Explicit title font", { x, y }, COL32(255, 220, 120)); y += 32;
        PushFont(memFont);                          // invalid handles are rejected safely
        draw->Text("PushFont/PopFont: memory-loaded font", { x, y });
        PopFont();                                  y += 26;
        draw->Image(memImg, prim32::Rect{ x, y, 48, 48 });
        draw->Image(memImg, prim32::Rect{ x + 56, y, 48, 48 }, COL32(255, 255, 255, 110));       // tint alpha
        draw->Image(memImg, prim32::Rect{ x + 112, y, 48, 48 }, Vec2{ 0, 0 }, Vec2{ 0.5f, 0.5f });// uv crop
        draw->Image(fileImg, prim32::Rect{ x + 168, y, 48, 48 });   // invalid when absent: debug placeholder / release no-op
        y += 56;
        prim32::Rect box{ x, y, 300, 56 };
        draw->Rect(box, c->style.colors[ZC_Border], 1.0f, 4.0f);
        TextOptions to; to.align = ALIGN_CENTER; to.valign = VALIGN_MIDDLE;
        to.wrapWidth = 288; to.clip = true;
        draw->Text("Wrapped, centered, vertically-aligned and clipped text in a bounds rect via TextOptions.", box, to);

        // ---- Unicode: any script, glyphs rasterize on first use ----
        {
            Vec2 u = GetCursorScreenPos();
            Dummy({ 440, 118 });
            float uy = u.y;
            // Cyrillic through the DEFAULT font (Segoe UI covers it)
            draw->Text("\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82, \xD0\xBC\xD0\xB8\xD1\x80! (Cyrillic, default font)", { u.x, uy });
            uy += 22;
            // Chinese through an explicit CJK font
            if (IsValid(cjkFont))
                draw->Text(cjkFont, "\xE4\xB8\xAD\xE6\x96\x87\xE6\xB8\xB2\xE6\x9F\x93\xE6\xB5\x8B\xE8\xAF\x95 \xE4\xBD\xA0\xE5\xA5\xBD\xEF\xBC\x8C\xE4\xB8\x96\xE7\x95\x8C", { u.x, uy }, COL32(180, 235, 190));
            uy += 26;
            // icon font: PUA codepoints via EncodeUtf8
            if (IsValid(iconFont)) {
                static const uint32_t icons[] = { 0xE700, 0xE706, 0xE713, 0xE734, 0xE77B, 0xE8FB };
                float ix = u.x;
                for (uint32_t ic : icons) {
                    char b[5];
                    EncodeUtf8(ic, b);
                    draw->Text(iconFont, b, { ix, uy }, COL32(140, 200, 255));
                    ix += 34;
                }
                draw->Text("<- Segoe MDL2 icons (PUA)", { ix + 8, uy + 4 });
            }
            uy += 34;
            // CJK word wrap (breaks between ideographs)
            if (IsValid(cjkFont)) {
                prim32::Rect ubox{ u.x, uy, 200, 30 };
                TextOptions uo; uo.wrapWidth = 192; uo.clip = true;
                draw->Text(cjkFont, "\xE8\xBF\x99\xE6\xAE\xB5\xE4\xB8\xAD\xE6\x96\x87\xE6\x96\x87\xE6\x9C\xAC\xE4\xBC\x9A\xE8\x87\xAA\xE5\x8A\xA8\xE6\x8D\xA2\xE8\xA1\x8C", ubox, uo);
            }
        }

        Vec2 m = MeasureText("MeasureText sample");
        Vec2 isz = GetImageSize(memImg);
        TextF("MeasureText: %.0f x %.0f | GetImageSize: %.0f x %.0f", m.x, m.y, isz.x, isz.y);
        Text(log0); Text(log1); Text(log2);
        if (Checkbox("SetDefaultFont(memory font)##dfont", &useMemDefault))
            SetDefaultFont(useMemDefault ? memFont : InvalidFontHandle);   // Invalid = built-in
        ResourceStats rs = GetResourceStats();
        TextF("resources: %u images, %u fonts | %.2f MB tex, %.2f MB atlases",
              rs.images, rs.fonts, rs.textureBytes / 1048576.0, rs.atlasBytes / 1048576.0);
    }
    End();
}

// ---------------------------------------------------------- child scroll demo
static void ShowChildDemo() {
    SetNextWindowPos({ 500, 640 });
    if (Begin("Child regions##child", WF_AutoSize)) {
        Text("Wheel over the log to scroll it.");
        if (BeginChild("Event log", { 360, 150 }, CF_Border)) {
            for (int i = 0; i < 40; i++)
                TextF("event %02d: persistent child scroll state", i + 1);
            EndChild();
        }
    }
    End();
}

// -------------------------------------------------------------------- main UI
static void BuildUI(Context* c, float time, float fps, float frameMs) {
    {   P32PROF_SCOPE("Stress payload");
        BuildStress(c, time);
    }
    P32PROF_SCOPE("Windows");

    Stats st = GetStats();
    prim32_dx12::GpuStats gst = prim32_dx12::GetGpuStats();

    SetNextWindowPos({ 24, 24 });
    if (Begin("Prim32 Benchmark##main", WF_AutoSize)) {
        TextF("%.0f FPS   (%.3f ms)", fps, frameMs);
        TextColored(COL32(140, 200, 160, 255), "GPU pass");
        SameLine(); TextF("%.3f ms", gst.gpuMs);
        TextColored(COL32(140, 170, 220, 255), "CPU build");
        SameLine(); TextF("%.3f ms", st.buildMs);
        TextF("%u prims   %u draws   %.1f MB/frame",
              st.prims, st.ranges, gst.bytesStreamed / (1024.0 * 1024.0));
        TextColored(COL32(255, 200, 120, 255), "bound by:");
        SameLine(); TextF("%s", BottleneckVerdict());
        if (st.overflow) TextColored(COL32(255, 90, 90, 255), "PRIM BUFFER FULL — raise maxPrims");
        Separator();

        TextColored(COL32(140, 146, 158, 255), "Stress payload");
        SliderInt("count 1k..2M (log)##cnt", &s_countExp, 0, 100);
        TextF("primitives: %u", StressCount());
        if (Button("Rects##m0"))   { s_mode = 0; } SameLine();
        if (Button("Rounded##m1")) { s_mode = 1; } SameLine();
        if (Button("Circles##m2")) { s_mode = 2; }
        if (Button("Glyphs##m3"))  { s_mode = 3; } SameLine();
        if (Button("Widgets##m4")) { s_mode = 4; } SameLine();
        if (Button("Overdraw##m5")){ s_mode = 5; } SameLine();
        if (Button("Shadows##m6")) { s_mode = 6; }
        Checkbox("animate##an", &s_animate);
        SameLine(); Checkbox("cache stress (bake once, move free)##cs", &s_cacheStress);
        SameLine(); Checkbox("big alpha quads (fill-bound)##bq", &s_bigQuads);
        Checkbox("vsync##vs", &s_vsync);
        SameLine(); Checkbox("extra windows##xw", &s_extraWindows);
        Checkbox("profiler##pf", &s_profiler);
        Separator();

        TextColored(COL32(140, 146, 158, 255), "Efficiency budget (max fps within a usage cap)");
        Checkbox("limit CPU/GPU usage##eff", &s_budget);
        if (s_budget) {
            p32prof::ProcStats eps = p32prof::GetProcStats();
            SliderFloat("CPU %% of machine##bc", &s_budgetCpu, 0.25f, 25.0f, "%.2f%%");
            SliderFloat("GPU %%##bg", &s_budgetGpu, 0.25f, 25.0f, "%.2f%%");
            float gfps = s_periodMs > 0.01f ? 1000.0f / s_periodMs : 0.0f;
            TextF("fits in budget: %.1f fps  (period %.2f ms, sleep %.2f ms)", gfps, s_periodMs, s_lastSleepMs);
            TextF("frame cost: CPU %.3f ms | GPU %.3f ms", s_activeMs, s_gpuCostMs);
            TextF("measured: CPU %.2f%% of machine | GPU 3D %.2f%%", eps.cpuTotalPct, eps.gpu3D);
            TextColored(COL32(140, 146, 158, 255), "note: at low duty the GPU downclocks, so ms/frame rises");
        }
        Separator();

        TextColored(COL32(140, 146, 158, 255), "Widget sanity check");
        static bool cb = true; static float f1 = 0.42f; static int i1 = 7;
        static float prog = 0.0f; prog = fmodf(time * 0.2f, 1.0f);
        Checkbox("works##cb", &cb);
        SliderFloat("float##sf", &f1, 0.0f, 1.0f);
        SliderInt("int##si", &i1, 0, 20);
        ProgressBar(prog);
        if (Button("Click me##bt")) cb = !cb;
        SameLine(); TextF("buffer use: %.1f%%", 100.0f * st.prims / (float)(c->primCap ? c->primCap : 1));
    }
    End();

    if (s_extraWindows) {
        for (int w = 0; w < 6; w++) {
            char name[32]; snprintf(name, 32, "Window %d##xw%d", w, w);
            SetNextWindowPos({ 500.0f + w * 90.0f, 120.0f + w * 70.0f });
            if (Begin(name, WF_AutoSize)) {
                TextF("draggable, z-ordered");
                static float v[6];
                SliderFloat("v##s", &v[w], 0, 1);
                ProgressBar(fmodf(time * (0.1f + w * 0.07f), 1.0f), { 160, 0 });
            }
            End();
        }
    }

    {   P32PROF_SCOPE("DrawList demo");
        ShowDrawListDemo(c);
    }
    ShowChildDemo();

    {   // foreground layer: arbitrary drawing above ALL windows
        BeginLayer(LAYER_FOREGROUND);
        Vec2 m = c->io.mousePos;
        Col cc = COL32(255, 220, 120, 150);
        DrawCircleStroke(c, m.x, m.y, 13.0f, cc, 1.5f);
        DrawLine(c, m.x - 22, m.y, m.x - 6, m.y, cc, 1.5f);
        DrawLine(c, m.x + 6, m.y, m.x + 22, m.y, cc, 1.5f);
        DrawLine(c, m.x, m.y - 22, m.x, m.y - 6, cc, 1.5f);
        DrawLine(c, m.x, m.y + 6, m.x, m.y + 22, cc, 1.5f);
        EndLayer();
    }

    {   P32PROF_SCOPE("Profiler window");
        static char extra[256];
        Stats stp = GetStats();
        snprintf(extra, sizeof(extra),
                 "res %dx%d | prims %u | draws %u | streamed %.1f MB/frame | vsync %s | stress mode %d x %u"
                 " | budget %s (cpu %.2f%%, gpu %.2f%%, period %.2f ms)\nbound by: %s",
                 g_width, g_height, stp.prims, stp.ranges,
                 prim32_dx12::GetGpuStats().bytesStreamed / 1048576.0,
                 s_vsync ? "on" : "off", s_mode, StressCount(),
                 s_budget ? "ON" : "off", s_budgetCpu, s_budgetGpu, s_periodMs,
                 BottleneckVerdict());
        p32prof::ShowProfiler(c, &s_profiler, extra);
    }

    // topmost overlay
    SetNextWindowPos({ (float)g_width - 190.0f, 16.0f }, true);
    if (Begin("overlay##hud", WF_NoTitleBar | WF_AutoSize | WF_Topmost | WF_NoMove)) {
        TextColored(COL32(255, 220, 120, 255), "Prim32");
        TextF("%.0f fps", fps);
        TextF("%.2fM prims", GetStats().prims / 1000000.0f);
    }
    End();
}

// ------------------------------------------------------------------- win proc
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) Resize(LOWORD(lp), HIWORD(lp));
        g_occluded = wp == SIZE_MINIMIZED;
        return 0;
    case WM_MOUSEWHEEL:
        g_mouseWheel += GET_WHEEL_DELTA_WPARAM(wp) / 120.0f;
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_F11) {
            static WINDOWPLACEMENT wpPrev = { sizeof(wpPrev) };
            static bool fs = false;
            DWORD style = GetWindowLongW(h, GWL_STYLE);
            if (!fs) {
                MONITORINFO mi = { sizeof(mi) };
                GetWindowPlacement(h, &wpPrev);
                GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY), &mi);
                SetWindowLongW(h, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
                SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                             mi.rcMonitor.right - mi.rcMonitor.left,
                             mi.rcMonitor.bottom - mi.rcMonitor.top,
                             SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
                fs = true;
            } else {
                SetWindowLongW(h, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
                SetWindowPlacement(h, &wpPrev);
                SetWindowPos(h, nullptr, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
                fs = false;
            }
        }
        if (wp == VK_ESCAPE) DestroyWindow(h);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

// ----------------------------------------------------------------------- main
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    { // DPI awareness without requiring newer SDK headers
        typedef BOOL(WINAPI* Fn)(void);
        if (Fn f = (Fn)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDPIAware")) f();
    }

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = L"Prim32";
    RegisterClassExW(&wc);

    RECT r = { 0, 0, g_width, g_height };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowExW(0, L"Prim32", L"Prim32 — D3D12 immediate GUI benchmark",
                             WS_OVERLAPPEDWINDOW, 60, 40,
                             r.right - r.left, r.bottom - r.top,
                             nullptr, nullptr, hInst, nullptr);
    if (!InitD3D()) { MessageBoxW(nullptr, L"D3D12 init failed", L"Prim32", MB_OK); return 1; }

    FontDesc fd = { L"Segoe UI", 17.0f, true };
    Context* ctx = CreateContext(&fd);
    p32prof::TrackMem("cpu: Prim32 context", sizeof(Context));

    prim32_dx12::InitDesc bd = {};
    bd.device = g_device; bd.queue = g_queue;
    bd.rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    bd.framesInFlight = kFrames;
    bd.maxPrims = 2u * 1024u * 1024u;      // 64 MB per frame slot: room for 2M prims
    if (!prim32_dx12::Init(ctx, bd)) { MessageBoxW(nullptr, L"Prim32 backend init failed", L"Prim32", MB_OK); return 1; }

    ShowWindow(g_hwnd, SW_SHOWDEFAULT);
    UpdateWindow(g_hwnd);

    LARGE_INTEGER qpf, t0, tPrev;
    QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&t0);
    tPrev = t0;
    double fpsAccT = 0; int fpsAccN = 0; float fps = 0, frameMs = 0;

    for (;;) {
        MSG msg;
        bool quit = false;
        {
        P32PROF_SCOPE("PumpMessages");
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) quit = true;
        }
        }
        if (quit) break;
        if (g_occluded) { Sleep(16); continue; }

        LARGE_INTEGER tNow; QueryPerformanceCounter(&tNow);
        float dt = (float)((tNow.QuadPart - tPrev.QuadPart) / (double)qpf.QuadPart);
        float time = (float)((tNow.QuadPart - t0.QuadPart) / (double)qpf.QuadPart);
        tPrev = tNow;
        frameMs = frameMs * 0.95f + dt * 1000.0f * 0.05f;
        {   // real CPU work this frame = wall - fence wait - governor sleep
            float act = dt * 1000.0f - s_lastWaitMs - s_lastSleepMs;
            if (act < 0.01f) act = 0.01f;
            if (act < 100.0f) s_activeMs += (act - s_activeMs) * 0.1f;
        }
        fpsAccT += dt; fpsAccN++;
        if (fpsAccT >= 0.25) { fps = (float)(fpsAccN / fpsAccT); fpsAccT = 0; fpsAccN = 0; }

        // pace: wait for this frame slot's previous use (long time here = GPU-bound)
        {
            P32PROF_SCOPE("WaitFrameFence");
            LARGE_INTEGER w0, w1; QueryPerformanceCounter(&w0);
            if (g_slotFence[g_frame] && g_fence->GetCompletedValue() < g_slotFence[g_frame]) {
                g_fence->SetEventOnCompletion(g_slotFence[g_frame], g_fenceEvent);
                WaitForSingleObject(g_fenceEvent, INFINITE);
            }
            QueryPerformanceCounter(&w1);
            s_lastWaitMs = (float)((w1.QuadPart - w0.QuadPart) * 1000.0 / (double)qpf.QuadPart);
        }

        // backend frame slot (also publishes last GPU zone results)
        FrameMem mem;
        {
            P32PROF_SCOPE("Backend NewFrame");
            mem = prim32_dx12::NewFrame();
        }

        UINT bb = g_swap->GetCurrentBackBufferIndex();
        {
            P32PROF_SCOPE("Reset cmdlist");
            g_alloc[g_frame]->Reset();
            g_cl->Reset(g_alloc[g_frame], nullptr);
        }

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = g_back[bb];
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        prim32_dx12::GpuZoneBegin(g_cl, "Clear+Barrier");
        g_cl->ResourceBarrier(1, &barrier);

        const float clear[4] = { 0.045f, 0.05f, 0.065f, 1.0f };
        g_cl->OMSetRenderTargets(1, &g_rtv[bb], FALSE, nullptr);
        g_cl->ClearRenderTargetView(g_rtv[bb], clear, 0, nullptr);
        prim32_dx12::GpuZoneEnd(g_cl);

        // ---- Prim32 frame
        IO io = PollWin32Input(g_hwnd, { (float)g_width, (float)g_height }, dt);
        io.mouseWheel = g_mouseWheel;
        g_mouseWheel = 0;
        {
            P32PROF_SCOPE("Prim32 NewFrame");
            NewFrame(mem, io);
        }
        {
            P32PROF_SCOPE("BuildUI");
            BuildUI(ctx, time, fps, frameMs);
        }
        DrawData* dd;
        {
            P32PROF_SCOPE("Prim32 EndFrame");
            dd = EndFrame();
        }
        {
            P32PROF_SCOPE("Backend Render");
            prim32_dx12::Render(dd, g_cl);
        }

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        g_cl->ResourceBarrier(1, &barrier);
        g_cl->Close();

        ID3D12CommandList* lists[] = { g_cl };
        {
            P32PROF_SCOPE("ExecuteCommandLists");
            g_queue->ExecuteCommandLists(1, lists);
            prim32_dx12::NotifyExecuted();
        }
        {
            P32PROF_SCOPE("Present");
            g_swap->Present(s_vsync ? 1 : 0, (!s_vsync && g_tearing) ? DXGI_PRESENT_ALLOW_TEARING : 0);
        }

        g_queue->Signal(g_fence, ++g_fenceCounter);
        g_slotFence[g_frame] = g_fenceCounter;
        g_frame = (g_frame + 1) % kFrames;

        if (s_budget) {
            P32PROF_SCOPE("BudgetSleep");
            GovernorSleep();
        } else {
            s_lastSleepMs = 0; s_periodMs = 0; s_trimCpu = s_trimGpu = 1.0f;
        }

        p32prof::SampleProcess();
        p32prof::FrameMark();
    }

    WaitIdle();
    prim32_dx12::Shutdown();
    DestroyContext(ctx);
    for (UINT i = 0; i < kFrames; i++) { SafeRelease(g_back[i]); SafeRelease(g_alloc[i]); }
    SafeRelease(g_cl); SafeRelease(g_rtvHeap); SafeRelease(g_swap);
    SafeRelease(g_fence); SafeRelease(g_queue); SafeRelease(g_device);
    if (g_fenceEvent) CloseHandle(g_fenceEvent);
    return 0;
}
