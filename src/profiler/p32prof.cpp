// ============================================================================
// p32prof implementation. See p32prof.h for the tour.
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <stdint.h>
#include <windows.h>
#include <psapi.h>
#include <pdh.h>
#ifndef PDH_MORE_DATA
#define PDH_MORE_DATA ((PDH_STATUS)0x800007D2L)
#endif
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_MSC_VER)
#include <intrin.h>
static inline uint64_t Tsc() { return __rdtsc(); }
#else
static inline uint64_t Tsc() { unsigned lo, hi; __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi)); return ((uint64_t)hi << 32) | lo; }
#endif
#include <prim32/p32prof.h>

namespace p32prof {

// =============================================================== CPU tree
static const int MAX_NODES = 512;
static const int MAX_DEPTH = 64;
static const int HISTORY   = 240;

struct Node {
    const char* name;
    int parent, firstChild, nextSibling, depth;
    uint64_t accTicks;                    // live accumulation
    uint32_t accCalls;
    float dTotal, dSelf; uint32_t dCalls; // display snapshot
    float avg, dispMin, dispMax;
    float wMin, wMax; double wSum; int wN;
    bool open;
};

static Node   s_nodes[MAX_NODES];
static int    s_nodeCount = 0;
static struct { uint64_t key; int idx; } s_map[2048];   // open addressing
static int    s_stack[MAX_DEPTH];
static uint64_t s_start[MAX_DEPTH];
static int    s_sp = 0;
static bool   s_paused = false;
static int    s_statWindow = 120;

static uint64_t s_tsc0, s_qpc0, s_lastFrameQpc, s_lastFrameTsc;
static double  s_qpf = 0, s_ticksPerMs = 0;
static float   s_hist[HISTORY]; static int s_histN = 0, s_histHead = 0;
static float   s_fps = 0, s_avgMs = 0; static double s_fpsAccT = 0; static int s_fpsAccN = 0;

static uint64_t Qpc() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return (uint64_t)t.QuadPart; }

static void InitOnce() {
    static bool done = false;
    if (done) return;
    done = true;
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); s_qpf = (double)f.QuadPart;
    s_tsc0 = Tsc(); s_qpc0 = Qpc();
    s_lastFrameQpc = s_qpc0; s_lastFrameTsc = s_tsc0;
    Node& r = s_nodes[0];
    memset(&r, 0, sizeof(r));
    r.name = "Frame"; r.parent = -1; r.firstChild = -1; r.nextSibling = -1;
    r.open = true; r.wMin = 1e30f;
    s_nodeCount = 1;
    memset(s_map, 0, sizeof(s_map));
    s_stack[0] = 0; s_sp = 0;
}

static int FindOrCreate(int parent, const char* name) {
    uint64_t key = ((uint64_t)(uint32_t)parent << 48) ^ (uint64_t)(uintptr_t)name * 0x9E3779B97F4A7C15ull;
    if (!key) key = 1;
    uint32_t h = (uint32_t)(key >> 32) & 2047;
    for (;;) {
        if (s_map[h].key == key) return s_map[h].idx;
        if (s_map[h].key == 0) break;
        h = (h + 1) & 2047;
    }
    if (s_nodeCount >= MAX_NODES) return 0;
    int idx = s_nodeCount++;
    Node& n = s_nodes[idx];
    memset(&n, 0, sizeof(n));
    n.name = name; n.parent = parent; n.firstChild = -1; n.nextSibling = -1;
    n.depth = s_nodes[parent].depth + 1;
    n.open = n.depth < 3;
    n.wMin = 1e30f;
    // append to parent's child list (will be re-sorted by FrameMark)
    int* link = &s_nodes[parent].firstChild;
    while (*link != -1) link = &s_nodes[*link].nextSibling;
    *link = idx;
    s_map[h].key = key; s_map[h].idx = idx;
    return idx;
}

void Begin(const char* name) {
    InitOnce();
    if (s_sp >= MAX_DEPTH - 1) return;
    int idx = FindOrCreate(s_stack[s_sp], name);
    s_nodes[idx].accCalls++;
    s_sp++;
    s_stack[s_sp] = idx;
    s_start[s_sp] = Tsc();
}
void End() {
    if (s_sp <= 0) return;
    uint64_t t = Tsc() - s_start[s_sp];
    s_nodes[s_stack[s_sp]].accTicks += t;
    s_sp--;
}

static void SortChildren(int parent) {
    // insertion sort of sibling list by dTotal, descending
    int sorted = -1;
    int cur = s_nodes[parent].firstChild;
    while (cur != -1) {
        int next = s_nodes[cur].nextSibling;
        if (sorted == -1 || s_nodes[cur].dTotal >= s_nodes[sorted].dTotal) {
            s_nodes[cur].nextSibling = sorted; sorted = cur;
        } else {
            int p = sorted;
            while (s_nodes[p].nextSibling != -1 && s_nodes[s_nodes[p].nextSibling].dTotal > s_nodes[cur].dTotal)
                p = s_nodes[p].nextSibling;
            s_nodes[cur].nextSibling = s_nodes[p].nextSibling;
            s_nodes[p].nextSibling = cur;
        }
        cur = next;
    }
    s_nodes[parent].firstChild = sorted;
}

static void CsvTick(float frameMs);

void FrameMark() {
    InitOnce();
    while (s_sp > 0) End();               // tolerate unbalanced scopes

    uint64_t qpc = Qpc(), tsc = Tsc();
    double frameMs = (qpc - s_lastFrameQpc) * 1000.0 / s_qpf;
    uint64_t frameTicks = tsc - s_lastFrameTsc;
    s_lastFrameQpc = qpc; s_lastFrameTsc = tsc;

    double totalS = (qpc - s_qpc0) / s_qpf;
    if (totalS > 0.01) s_ticksPerMs = (double)(tsc - s_tsc0) / (totalS * 1000.0);
    if (s_ticksPerMs <= 0) { for (int i = 0; i < s_nodeCount; i++) { s_nodes[i].accTicks = 0; s_nodes[i].accCalls = 0; } return; }

    // root = whole frame
    s_nodes[0].accTicks = frameTicks;
    s_nodes[0].accCalls = 1;

    if (!s_paused) {
        for (int i = 0; i < s_nodeCount; i++) {
            Node& n = s_nodes[i];
            n.dTotal = (float)(n.accTicks / s_ticksPerMs);
            n.dCalls = n.accCalls;
            n.wSum += n.dTotal;
            if (n.dTotal < n.wMin) n.wMin = n.dTotal;
            if (n.dTotal > n.wMax) n.wMax = n.dTotal;
            n.wN++;
            n.avg = (float)(n.wSum / n.wN);          // exact mean of current window
            if (n.wN >= s_statWindow) {              // roll the window
                n.dispMin = n.wMin; n.dispMax = n.wMax;
                n.wMin = 1e30f; n.wMax = 0; n.wSum = 0; n.wN = 0;
            }
        }
        for (int i = 0; i < s_nodeCount; i++) {
            float cs = 0;
            for (int c = s_nodes[i].firstChild; c != -1; c = s_nodes[c].nextSibling) cs += s_nodes[c].dTotal;
            s_nodes[i].dSelf = s_nodes[i].dTotal - cs;
            if (s_nodes[i].dSelf < 0) s_nodes[i].dSelf = 0;
        }
        for (int i = 0; i < s_nodeCount; i++) SortChildren(i);

        s_hist[s_histHead] = (float)frameMs;
        s_histHead = (s_histHead + 1) % HISTORY;
        if (s_histN < HISTORY) s_histN++;
    }
    for (int i = 0; i < s_nodeCount; i++) { s_nodes[i].accTicks = 0; s_nodes[i].accCalls = 0; }

    s_fpsAccT += frameMs / 1000.0; s_fpsAccN++;
    if (s_fpsAccT >= 0.25) {
        s_fps = (float)(s_fpsAccN / s_fpsAccT);
        s_avgMs = (float)(s_fpsAccT * 1000.0 / s_fpsAccN);
        s_fpsAccT = 0; s_fpsAccN = 0;
    }
    CsvTick((float)frameMs);
}

void SetStatWindow(int frames) {
    if (frames < 10) frames = 10;
    if (frames > 1000) frames = 1000;
    if (frames == s_statWindow) return;
    s_statWindow = frames;
    for (int i = 0; i < s_nodeCount; i++) {
        Node& n = s_nodes[i];
        n.wMin = 1e30f; n.wMax = 0; n.wSum = 0; n.wN = 0;
    }
}
int GetStatWindow() { return s_statWindow; }

int  NodeCount()            { return s_nodeCount; }
int  NodeFirstChild(int i)  { return s_nodes[i].firstChild; }
int  NodeNextSibling(int i) { return s_nodes[i].nextSibling; }
void NodeSetOpen(int i, bool open) { if (i >= 0 && i < s_nodeCount) s_nodes[i].open = open; }
void SetPaused(bool p) { s_paused = p; }
bool IsPaused()        { return s_paused; }

const NodeView& NodeGet(int i) {
    static NodeView v;
    const Node& n = s_nodes[i];
    v.name = n.name; v.depth = n.depth; v.calls = n.dCalls;
    v.totalMs = n.dTotal; v.selfMs = n.dSelf; v.avgMs = n.avg;
    v.minMs = n.dispMin; v.maxMs = n.dispMax; v.open = n.open;
    return v;
}

int FrameHistory(const float** outMs) {
    static float lin[HISTORY];
    for (int i = 0; i < s_histN; i++)
        lin[i] = s_hist[(s_histHead - s_histN + i + 2 * HISTORY) % HISTORY];
    *outMs = lin;
    return s_histN;
}
float Fps()        { return s_fps; }
float FrameAvgMs() { return s_avgMs; }
float FrameMinMs() { float m = 1e30f; for (int i = 0; i < s_histN; i++) { float v = s_hist[i]; if (v < m) m = v; } return s_histN ? m : 0; }
float FrameMaxMs() { float m = 0;     for (int i = 0; i < s_histN; i++) { float v = s_hist[i]; if (v > m) m = v; } return m; }

// ========================================================== process metrics
static ProcStats s_ps;
static uint64_t  s_lastSampleQpc = 0;
// drift ring: one sample per SampleProcess tick (~2 Hz), ~2 min of history
static struct { float t, ram, vram; } s_drift[256];
static int s_driftN = 0, s_driftHead = 0;
static ULONGLONG s_lastKU = 0;
static uint64_t  s_lastPageFaults = 0;

// PDH loaded dynamically: no hard dependency, survives localized systems.
typedef PDH_STATUS (WINAPI* PdhOpenQueryW_t)(LPCWSTR, DWORD_PTR, PDH_HQUERY*);
typedef PDH_STATUS (WINAPI* PdhAddCounter_t)(PDH_HQUERY, LPCWSTR, DWORD_PTR, PDH_HCOUNTER*);
typedef PDH_STATUS (WINAPI* PdhCollect_t)(PDH_HQUERY);
typedef PDH_STATUS (WINAPI* PdhGetArrayW_t)(PDH_HCOUNTER, DWORD, LPDWORD, LPDWORD, PDH_FMT_COUNTERVALUE_ITEM_W*);
static PDH_HQUERY     s_pdhQuery;
static PDH_HCOUNTER   s_pdhGpu;
static PdhCollect_t   s_pdhCollect;
static PdhGetArrayW_t s_pdhGetArray;
static bool           s_pdhInit = false, s_pdhOk = false;
static char           s_otherEngine[24] = "?";
const char* OtherEngineName() { return s_otherEngine; }

static void PdhInit() {
    s_pdhInit = true;
    HMODULE m = LoadLibraryW(L"pdh.dll");
    if (!m) return;
    PdhOpenQueryW_t open  = (PdhOpenQueryW_t)(void*)GetProcAddress(m, "PdhOpenQueryW");
    PdhAddCounter_t addEn = (PdhAddCounter_t)(void*)GetProcAddress(m, "PdhAddEnglishCounterW");
    PdhAddCounter_t add   = (PdhAddCounter_t)(void*)GetProcAddress(m, "PdhAddCounterW");
    s_pdhCollect          = (PdhCollect_t)(void*)GetProcAddress(m, "PdhCollectQueryData");
    s_pdhGetArray         = (PdhGetArrayW_t)(void*)GetProcAddress(m, "PdhGetFormattedCounterArrayW");
    if (!open || !(addEn || add) || !s_pdhCollect || !s_pdhGetArray) return;
    if (open(nullptr, 0, &s_pdhQuery) != ERROR_SUCCESS) return;
    wchar_t path[128];
    swprintf(path, 128, L"\\GPU Engine(pid_%u*)\\Utilization Percentage", GetCurrentProcessId());
    PdhAddCounter_t use = addEn ? addEn : add;
    if (use(s_pdhQuery, path, 0, &s_pdhGpu) != ERROR_SUCCESS) return;
    s_pdhCollect(s_pdhQuery);            // prime (rate counter needs 2 samples)
    s_pdhOk = true;
}

static void PdhSample() {
    s_ps.gpuValid = false;
    if (!s_pdhInit) PdhInit();
    if (!s_pdhOk) return;
    if (s_pdhCollect(s_pdhQuery) != ERROR_SUCCESS) return;
    DWORD bytes = 0, count = 0;
    PDH_STATUS st = s_pdhGetArray(s_pdhGpu, PDH_FMT_DOUBLE, &bytes, &count, nullptr);
    if (st != (PDH_STATUS)PDH_MORE_DATA || !bytes) return;
    static DWORD cap = 0; static PDH_FMT_COUNTERVALUE_ITEM_W* items = nullptr;
    if (bytes > cap) { free(items); items = (PDH_FMT_COUNTERVALUE_ITEM_W*)malloc(bytes); cap = bytes; }
    if (!items) return;
    if (s_pdhGetArray(s_pdhGpu, PDH_FMT_DOUBLE, &bytes, &count, items) != ERROR_SUCCESS) return;
    float e3d = 0, ecp = 0, ecm = 0, evd = 0, eot = 0, otTop = 0;
    for (DWORD i = 0; i < count; i++) {
        const wchar_t* nm = items[i].szName;
        const wchar_t* et = wcsstr(nm, L"engtype_");
        double v = items[i].FmtValue.doubleValue;
        if (v <= 0) continue;
        if (et) et += 8;
        if      (et && (!_wcsnicmp(et, L"3D", 2) || !_wcsnicmp(et, L"Graphics", 8)))       e3d += (float)v;
        else if (et && !_wcsnicmp(et, L"Copy", 4))                                         ecp += (float)v;
        else if (et && !_wcsnicmp(et, L"Compute", 7))                                      ecm += (float)v;
        else if (et && (!_wcsnicmp(et, L"Video", 5) || !_wcsnicmp(et, L"VR", 2)))          evd += (float)v;
        else {
            eot += (float)v;
            if ((float)v > otTop) {   // remember the biggest unknown engine's label
                otTop = (float)v;
                const wchar_t* src = et ? et : nm;
                int k = 0;
                for (; k < 23 && src[k] && src[k] != L'_'; k++)
                    s_otherEngine[k] = src[k] < 128 ? (char)src[k] : '?';
                s_otherEngine[k] = 0;
            }
        }
    }
    s_ps.gpu3D = e3d; s_ps.gpuCopy = ecp; s_ps.gpuCompute = ecm;
    s_ps.gpuVideo = evd; s_ps.gpuOther = eot;
    s_ps.gpuValid = true;
}

void SampleProcess() {
    InitOnce();
    uint64_t now = Qpc();
    if (s_lastSampleQpc && (now - s_lastSampleQpc) / s_qpf < 0.5) return;
    double dt = s_lastSampleQpc ? (now - s_lastSampleQpc) / s_qpf : 0;
    s_lastSampleQpc = now;

    SYSTEM_INFO si; GetSystemInfo(&si);
    s_ps.cores = (int)si.dwNumberOfProcessors;

    FILETIME ct, et, kt, ut;
    if (GetProcessTimes(GetCurrentProcess(), &ct, &et, &kt, &ut)) {
        ULONGLONG k = ((ULONGLONG)kt.dwHighDateTime << 32) | kt.dwLowDateTime;
        ULONGLONG u = ((ULONGLONG)ut.dwHighDateTime << 32) | ut.dwLowDateTime;
        ULONGLONG ku = k + u;                              // 100ns units
        if (dt > 0 && s_lastKU) {
            double busySec = (ku - s_lastKU) * 1e-7;
            s_ps.cpuCorePct  = (float)(busySec / dt * 100.0);
            s_ps.cpuTotalPct = s_ps.cpuCorePct / (s_ps.cores > 0 ? s_ps.cores : 1);
        }
        s_lastKU = ku;
    }

    PROCESS_MEMORY_COUNTERS_EX pmc = {}; pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
        s_ps.wsMB     = pmc.WorkingSetSize     / (1024.0 * 1024.0);
        s_ps.wsPeakMB = pmc.PeakWorkingSetSize / (1024.0 * 1024.0);
        s_ps.privMB   = pmc.PrivateUsage       / (1024.0 * 1024.0);
        if (dt > 0 && s_lastPageFaults)
            s_ps.pageFaultsPerSec = (float)((pmc.PageFaultCount - s_lastPageFaults) / dt);
        s_lastPageFaults = pmc.PageFaultCount;
    }

    DWORD hc = 0; GetProcessHandleCount(GetCurrentProcess(), &hc);
    s_ps.handles = hc;

    uint32_t threads = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te; te.dwSize = sizeof(te);
        DWORD pid = GetCurrentProcessId();
        if (Thread32First(snap, &te)) do { if (te.th32OwnerProcessID == pid) threads++; } while (Thread32Next(snap, &te));
        CloseHandle(snap);
    }
    s_ps.threads = threads;

    PdhSample();

    // record drift sample
    float tSec = (float)((now - s_qpc0) / s_qpf);
    s_drift[s_driftHead] = { tSec, (float)s_ps.privMB, (float)s_ps.vramMB };
    s_driftHead = (s_driftHead + 1) % 256;
    if (s_driftN < 256) s_driftN++;
}

MemDrift GetMemDrift() {
    MemDrift d = {};
    if (s_driftN < 8) return d;                       // need ~4s of samples
    int n = s_driftN;
    auto at = [&](int i) -> const auto& { return s_drift[(s_driftHead - n + i + 512) % 256]; };
    const auto& last = at(n - 1);
    // a large single-sample jump is a one-time allocation (a STEP), not a
    // leak — regress the drift only over the samples after the latest step
    int start = 0;
    for (int i = 1; i < n; i++) {
        float dr = at(i).ram - at(i - 1).ram, dv = at(i).vram - at(i - 1).vram;
        if (dr > 16 || dr < -16 || dv > 16 || dv < -16) {
            start = i;
            d.hasStep = true;
            d.stepRamMB = dr; d.stepVramMB = dv;
            d.stepAgoSec = last.t - at(i).t;
        }
    }
    int m = n - start;
    if (m < 8) return d;                              // step too recent for a verdict
    double sx = 0, sy = 0, sz = 0, sxx = 0, sxy = 0, sxz = 0;
    for (int i = start; i < n; i++) {
        const auto& p = at(i);
        sx += p.t; sy += p.ram; sz += p.vram;
        sxx += (double)p.t * p.t; sxy += (double)p.t * p.ram; sxz += (double)p.t * p.vram;
    }
    double den = (double)m * sxx - sx * sx;
    if (den < 1e-6) return d;
    d.ramMBperMin  = (float)((m * sxy - sx * sy) / den * 60.0);
    d.vramMBperMin = (float)((m * sxz - sx * sz) / den * 60.0);
    d.spanSec = last.t - at(start).t;
    d.valid = d.spanSec > 4.0f;
    return d;
}
const ProcStats& GetProcStats() { return s_ps; }

// ========================================================= backend-pushed
static GpuZone s_gpuZones[64]; static int s_gpuZoneCount = 0;
static struct { const char* name; double sum; float mn, mx; float dAvg, dMin, dMax; } s_zoneAgg[64];
static int s_zoneAggCount = 0, s_zoneAggN = 0;
static char    s_adapter[128] = "unknown adapter";

void SetGpuZones(const GpuZone* z, int n) {
    if (s_paused) return;
    if (n > 64) n = 64;
    memcpy(s_gpuZones, z, n * sizeof(GpuZone));
    s_gpuZoneCount = n;
    // exact-window stats per zone name
    for (int i = 0; i < n; i++) {
        int k = -1;
        for (int j = 0; j < s_zoneAggCount; j++)
            if (s_zoneAgg[j].name == z[i].name) { k = j; break; }
        if (k < 0) {
            if (s_zoneAggCount >= 64) continue;
            k = s_zoneAggCount++;
            s_zoneAgg[k].name = z[i].name;
            s_zoneAgg[k].sum = 0; s_zoneAgg[k].mn = 1e30f; s_zoneAgg[k].mx = 0;
            s_zoneAgg[k].dAvg = s_zoneAgg[k].dMin = s_zoneAgg[k].dMax = 0;
        }
        s_zoneAgg[k].sum += z[i].ms;
        if (z[i].ms < s_zoneAgg[k].mn) s_zoneAgg[k].mn = z[i].ms;
        if (z[i].ms > s_zoneAgg[k].mx) s_zoneAgg[k].mx = z[i].ms;
    }
    if (++s_zoneAggN >= s_statWindow) {
        for (int j = 0; j < s_zoneAggCount; j++) {
            s_zoneAgg[j].dAvg = (float)(s_zoneAgg[j].sum / s_zoneAggN);
            s_zoneAgg[j].dMin = s_zoneAgg[j].mn;
            s_zoneAgg[j].dMax = s_zoneAgg[j].mx;
            s_zoneAgg[j].sum = 0; s_zoneAgg[j].mn = 1e30f; s_zoneAgg[j].mx = 0;
        }
        s_zoneAggN = 0;
    }
}
bool GetGpuZoneWindow(const char* name, ZoneStats* out) {
    for (int j = 0; j < s_zoneAggCount; j++)
        if (s_zoneAgg[j].name == name || !strcmp(s_zoneAgg[j].name, name)) {
            if (s_zoneAgg[j].dAvg <= 0 && s_zoneAggN > 0) {   // window not rolled yet: running mean
                out->avgMs = (float)(s_zoneAgg[j].sum / s_zoneAggN);
                out->minMs = s_zoneAgg[j].mn; out->maxMs = s_zoneAgg[j].mx;
            } else {
                out->avgMs = s_zoneAgg[j].dAvg; out->minMs = s_zoneAgg[j].dMin; out->maxMs = s_zoneAgg[j].dMax;
            }
            return true;
        }
    return false;
}
int GetGpuZones(const GpuZone** out) { *out = s_gpuZones; return s_gpuZoneCount; }
void SetVideoMemory(double v, double vb, double s, double sb) {
    s_ps.vramMB = v; s_ps.vramBudgetMB = vb; s_ps.sharedMB = s; s_ps.sharedBudgetMB = sb;
    s_ps.vramValid = true;
}
void SetAdapterName(const char* n) { strncpy(s_adapter, n, 127); s_adapter[127] = 0; }
const char* GetAdapterName() { return s_adapter; }

// ========================================================= memory inventory
static struct { const char* name; uint64_t bytes; } s_mem[64];
static int s_memCount = 0;

void TrackMem(const char* name, uint64_t bytes) {
    for (int i = 0; i < s_memCount; i++)
        if (s_mem[i].name == name || !strcmp(s_mem[i].name, name)) { s_mem[i].bytes = bytes; return; }
    if (s_memCount < 64) { s_mem[s_memCount].name = name; s_mem[s_memCount].bytes = bytes; s_memCount++; }
}
int  MemCount() { return s_memCount; }
void MemGet(int i, const char** name, uint64_t* bytes) { *name = s_mem[i].name; *bytes = s_mem[i].bytes; }
uint64_t MemTotal() { uint64_t t = 0; for (int i = 0; i < s_memCount; i++) t += s_mem[i].bytes; return t; }

// ==================================================================== dump
static int Put(char* buf, int cap, int at, const char* fmt, ...) {
    if (at >= cap - 1) return at;
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf + at, (size_t)(cap - at), fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    if (at + n > cap - 1) n = cap - 1 - at;
    return at + n;
}

static int DumpNode(char* buf, int cap, int at, int idx, float frameMs) {
    const Node& n = s_nodes[idx];
    float pct = frameMs > 0 ? n.dTotal / frameMs * 100.0f : 0;
    at = Put(buf, cap, at, "%*s%-*s %8.3f %8.3f %8.3f %8.3f %8.3f %6u %5.1f%%\n",
             n.depth * 2, "", 34 - n.depth * 2, n.name,
             n.dSelf, n.dTotal, n.avg, n.dispMin, n.dispMax, n.dCalls, pct);
    for (int c = n.firstChild; c != -1; c = s_nodes[c].nextSibling)
        at = DumpNode(buf, cap, at, c, frameMs);
    return at;
}

int Dump(char* buf, int cap, const char* extraInfo) {
    SYSTEMTIME st; GetLocalTime(&st);
    const ProcStats& p = s_ps;
    float frameMs = s_nodes[0].dTotal;
    int at = 0;
    at = Put(buf, cap, at, "==================== Prim32 PROFILER DUMP ====================\n");
    at = Put(buf, cap, at, "%04d-%02d-%02d %02d:%02d:%02d | %s\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, s_adapter);
    if (extraInfo && *extraInfo) at = Put(buf, cap, at, "%s\n", extraInfo);
    at = Put(buf, cap, at, "\n-- PROCESS ------------------------------------------------\n");
    at = Put(buf, cap, at, "CPU  : %5.1f%% of machine | %6.1f%% of one core | %d cores | %u threads | %u handles\n",
             p.cpuTotalPct, p.cpuCorePct, p.cores, p.threads, p.handles);
    at = Put(buf, cap, at, "RAM  : working set %.1f MB (peak %.1f) | private %.1f MB | %.0f page faults/s\n",
             p.wsMB, p.wsPeakMB, p.privMB, p.pageFaultsPerSec);
    if (p.gpuValid)
        at = Put(buf, cap, at, "GPU  : 3D %.1f%% | copy %.1f%% | compute %.1f%% | video %.1f%% | other(%s) %.1f%%  (per-process engine utilization)\n",
                 p.gpu3D, p.gpuCopy, p.gpuCompute, p.gpuVideo, s_otherEngine, p.gpuOther);
    else
        at = Put(buf, cap, at, "GPU  : engine utilization unavailable (PDH)\n");
    if (p.vramValid)
        at = Put(buf, cap, at, "VRAM : %.1f MB used / %.1f MB budget | shared %.1f / %.1f MB\n",
                 p.vramMB, p.vramBudgetMB, p.sharedMB, p.sharedBudgetMB);
    {
        MemDrift md = GetMemDrift();
        if (md.hasStep)
            at = Put(buf, cap, at, "STEP : one-time allocation %+.0f MB RAM / %+.0f MB VRAM, %.0f s ago (not a leak)\n",
                     md.stepRamMB, md.stepVramMB, md.stepAgoSec);
        if (md.valid)
            at = Put(buf, cap, at, "LEAK : RAM drift %+.2f MB/min | VRAM drift %+.2f MB/min  (over %.0f s)%s\n",
                     md.ramMBperMin, md.vramMBperMin, md.spanSec,
                     (md.ramMBperMin > 1.0f || md.vramMBperMin > 4.0f) ? "  <-- INVESTIGATE" : "  (steady)");
        else if (md.hasStep)
            at = Put(buf, cap, at, "LEAK : (waiting for post-step samples)\n");
    }
    at = Put(buf, cap, at, "\n-- FRAME --------------------------------------------------\n");
    at = Put(buf, cap, at, "%.0f fps | %.3f ms avg | min %.3f | max %.3f (last %d frames)%s\n",
             s_fps, s_avgMs, FrameMinMs(), FrameMaxMs(), s_histN, s_paused ? "  [PAUSED]" : "");
    at = Put(buf, cap, at, "\n-- CPU TREE (ms, avg/min/max over last %d frames) ---------\n", s_statWindow);
    at = Put(buf, cap, at, "%-34s %8s %8s %8s %8s %8s %6s %6s\n", "scope", "self", "total", "avg", "min", "max", "calls", "%frame");
    at = DumpNode(buf, cap, at, 0, frameMs);
    at = Put(buf, cap, at, "\n-- GPU ZONES (ms, frames-in-flight latent) -----------------\n");
    for (int i = 0; i < s_gpuZoneCount; i++) {
        ZoneStats zs = {};
        GetGpuZoneWindow(s_gpuZones[i].name, &zs);
        at = Put(buf, cap, at, "%*s%-*s %8.3f | avg %7.3f  min %7.3f  max %7.3f\n",
                 s_gpuZones[i].depth * 2, "", 34 - s_gpuZones[i].depth * 2,
                 s_gpuZones[i].name, s_gpuZones[i].ms, zs.avgMs, zs.minMs, zs.maxMs);
    }
    if (!s_gpuZoneCount) at = Put(buf, cap, at, "(none)\n");
    at = Put(buf, cap, at, "\n-- TRACKED MEMORY -----------------------------------------\n");
    for (int i = 0; i < s_memCount; i++)
        at = Put(buf, cap, at, "%-34s %10.2f MB\n", s_mem[i].name, s_mem[i].bytes / (1024.0 * 1024.0));
    at = Put(buf, cap, at, "%-34s %10.2f MB\n", "TOTAL tracked", MemTotal() / (1024.0 * 1024.0));
    at = Put(buf, cap, at, "%-34s %10.2f MB  (CRT heap, code, driver, ...)\n", "untracked (private - tracked)",
             p.privMB - MemTotal() / (1024.0 * 1024.0));
    at = Put(buf, cap, at, "=============================================================\n");
    return at;
}

bool CopyToClipboard(const char* extraInfo) {
    static char buf[128 * 1024];
    int len = Dump(buf, sizeof(buf), extraInfo);
    if (!OpenClipboard(nullptr)) return false;
    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)len + 1);
    bool ok = false;
    if (h) {
        void* dst = GlobalLock(h);
        if (dst) {
            memcpy(dst, buf, (size_t)len + 1);
            GlobalUnlock(h);
            ok = SetClipboardData(CF_TEXT, h) != nullptr;
        }
        if (!ok) GlobalFree(h);
    }
    CloseClipboard();
    return ok;
}

// ================================================================= CSV log
static bool  s_csv = false;
static FILE* s_csvFile = nullptr;
static double s_csvAcc = 0;

void SetCsvLog(bool e) {
    s_csv = e;
    if (!e && s_csvFile) { fclose(s_csvFile); s_csvFile = nullptr; }
}
bool GetCsvLog() { return s_csv; }

static void CsvTick(float frameMs) {
    if (!s_csv) return;
    s_csvAcc += frameMs / 1000.0;
    if (s_csvAcc < 1.0) return;
    s_csvAcc = 0;
    if (!s_csvFile) {
        s_csvFile = fopen("prim32_profile.csv", "a");
        if (!s_csvFile) { s_csv = false; return; }
        fseek(s_csvFile, 0, SEEK_END);
        if (ftell(s_csvFile) == 0)
            fprintf(s_csvFile, "time,fps,frame_ms,cpu_core_pct,ws_mb,private_mb,gpu3d_pct,gpu_copy_pct,vram_mb,threads,gpu_ui_ms\n");
    }
    SYSTEMTIME st; GetLocalTime(&st);
    float gpuUi = s_gpuZoneCount ? s_gpuZones[0].ms : 0;
    fprintf(s_csvFile, "%02d:%02d:%02d,%.0f,%.3f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%u,%.3f\n",
            st.wHour, st.wMinute, st.wSecond, s_fps, s_avgMs, s_ps.cpuCorePct,
            s_ps.wsMB, s_ps.privMB, s_ps.gpu3D, s_ps.gpuCopy, s_ps.vramMB, s_ps.threads, gpuUi);
    fflush(s_csvFile);
}

} // namespace p32prof
