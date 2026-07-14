// ============================================================================
// p32prof — hierarchical CPU/GPU/memory profiler for Prim32 apps.
//
//   CPU  : RDTSC scoped zones, unlimited nesting, path-based tree with
//          self/total/min/max/avg per node.  ~20-40 ns per scope pair.
//   GPU  : named timestamp zones pushed by the D3D12 backend (per draw range).
//   RAM  : process working set / private bytes / page faults + an inventory
//          of every tracked allocation ("what uses how much").
//   CPU% : process time vs wall time.   GPU%: per-process engine utilization
//          via PDH "GPU Engine" counters (same source Task Manager uses).
//   VRAM : local/shared usage + budget (DXGI).
//
//   Dump(): the whole thing as text.  CopyToClipboard(): one-click export.
//   Scope names MUST be string literals (pointers are stored, not copied).
// ============================================================================
#pragma once
#include <stdint.h>

namespace p32prof {

// ------------------------------------------------------------- CPU scopes
void Begin(const char* name);
void End();
void FrameMark();                        // call once per frame, at the very end

struct Scope { Scope(const char* n) { Begin(n); } ~Scope() { End(); } };
#define P32PROF_CAT2(a,b) a##b
#define P32PROF_CAT(a,b) P32PROF_CAT2(a,b)
#define P32PROF_SCOPE(name) p32prof::Scope P32PROF_CAT(p32prof_scope_, __LINE__)(name)

// --------------------------------------------------------------- CPU tree
// Display snapshot, updated by FrameMark (frozen while paused). Node 0 = frame.
struct NodeView {
    const char* name;
    int         depth;
    uint32_t    calls;                   // last frame
    float       totalMs, selfMs;         // last frame
    float       avgMs, minMs, maxMs;     // smoothed / rolling window
    bool        open;                    // UI collapse state
};
// Statistics window: avg/min/max are exact over the last N frames.
void            SetStatWindow(int frames);      // 60 / 120 / 240 (10..1000)
int             GetStatWindow();

int             NodeCount();
int             NodeFirstChild(int i);   // -1 = none
int             NodeNextSibling(int i);  // -1 = none
const NodeView& NodeGet(int i);
void            NodeSetOpen(int i, bool open);
void            SetPaused(bool p);
bool            IsPaused();

// ----------------------------------------------------------- frame history
int   FrameHistory(const float** outMs); // last N frame times, oldest first
float Fps();
float FrameAvgMs();
float FrameMinMs();
float FrameMaxMs();

// -------------------------------------------------------- process metrics
struct ProcStats {
    float    cpuTotalPct, cpuCorePct;    // % of all cores / % of one core
    int      cores;
    uint32_t threads, handles;
    double   wsMB, wsPeakMB, privMB;     // working set, peak, private commit
    float    pageFaultsPerSec;
    float    gpu3D, gpuCopy, gpuCompute, gpuVideo, gpuOther;  // % per engine
    bool     gpuValid;
    double   vramMB, vramBudgetMB, sharedMB, sharedBudgetMB;
    bool     vramValid;
};
void             SampleProcess();        // call every frame; sampled asynchronously at ~2 Hz
// Leak detector: linear drift of private bytes / VRAM over the sample ring.
struct MemDrift {
    float ramMBperMin, vramMBperMin, spanSec;
    bool  valid;
    // one-time allocation steps are NOT leaks; reported separately
    float stepRamMB, stepVramMB, stepAgoSec;
    bool  hasStep;
};
MemDrift         GetMemDrift();
const ProcStats& GetProcStats();

// ------------------------------------------------- pushed by the backend
struct GpuZone { const char* name; float ms; int depth; };
void        SetGpuZones(const GpuZone* zones, int count);
int         GetGpuZones(const GpuZone** out);
struct ZoneStats { float avgMs, minMs, maxMs; };
bool        GetGpuZoneWindow(const char* name, ZoneStats* out);  // window stats
void        SetVideoMemory(double vramMB, double vramBudgetMB,
                           double sharedMB, double sharedBudgetMB);
void        SetAdapterName(const char* utf8);
const char* GetAdapterName();
const char* OtherEngineName();   // label of the biggest unclassified GPU engine

// --------------------------------------------------------- memory inventory
void     TrackMem(const char* name, uint64_t bytes);   // upsert by name
int      MemCount();
void     MemGet(int i, const char** name, uint64_t* bytes);
uint64_t MemTotal();

// ------------------------------------------------------------------ export
int  Dump(char* buf, int cap, const char* extraInfo);  // returns length
bool CopyToClipboard(const char* extraInfo);           // full dump -> clipboard
void SetCsvLog(bool enabled);                          // prim32_profile.csv, 1 row/s
bool GetCsvLog();

} // namespace p32prof

// Profiler window (implemented in p32prof_ui.cpp, draws with Prim32 itself).
namespace prim32 { struct Context; }
namespace p32prof {
void ShowProfiler(prim32::Context* ctx, bool* open, const char* extraInfo);
}
