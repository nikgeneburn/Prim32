// ============================================================================
// p32prof_ui — the profiler window, drawn with Prim32 itself.
//   frame-time graph | process CPU/RAM/GPU/VRAM | collapsible CPU scope tree
//   with self/total/avg/min/max/calls + bars | GPU zones | memory inventory
//   [Copy profiler] puts the full text dump on the clipboard.
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
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <prim32/prim32.h>
#include <prim32/p32prof.h>

namespace p32prof {

using namespace prim32;

static const float ROW_H = 17.0f;

static Col HeatCol(float frac) {   // green -> yellow -> red
    frac = frac < 0 ? 0 : frac > 1 ? 1 : frac;
    float r = frac < 0.5f ? frac * 2 : 1.0f;
    float g = frac < 0.5f ? 1.0f : 2.0f - frac * 2;
    return COL32((uint32_t)(r * 200 + 40), (uint32_t)(g * 180 + 40), 60, 255);
}

// one tree row: bar + toggle + name + numeric columns
static void NodeRow(Context* c, int idx, float frameMs, float x, float w) {
    const NodeView n = NodeGet(idx);   // copy: NodeGet's storage is reused in recursion
    bool hasKids = NodeFirstChild(idx) != -1;
    Vec2 p = GetCursorScreenPos();

    float frac = frameMs > 0 ? n.totalMs / frameMs : 0;
    DrawRect(c, x, p.y, x + w * (frac < 0 ? 0 : frac > 1 ? 1 : frac), p.y + ROW_H - 2,
             ColA(HeatCol(frac), 46));

    if (hasKids) {
        PushID(idx);
        SetCursorScreenPos({ x + n.depth * 12.0f, p.y });
        if (InvisibleButton("t", { 14, ROW_H - 2 })) NodeSetOpen(idx, !n.open);
        PopID();
    }
    char ind[2] = { hasKids ? (n.open ? '-' : '+') : ' ', 0 };
    DrawText(c, x + n.depth * 12.0f + 2, p.y, c->style.colors[ZC_TextDim], ind);
    DrawText(c, x + n.depth * 12.0f + 14, p.y, c->style.colors[ZC_Text], n.name);

    char buf[128];
    snprintf(buf, sizeof(buf), "%7.3f %7.3f %7.3f %7.3f %7.3f %5u %5.1f%%",
             n.selfMs, n.totalMs, n.avgMs, n.minMs, n.maxMs, n.calls, frac * 100.0f);
    Vec2 ts = TextSize(c, buf);
    DrawText(c, x + w - ts.x - 4, p.y, c->style.colors[ZC_Text], buf);

    SetCursorScreenPos({ p.x, p.y });
    Dummy({ w, ROW_H });
    if (n.open)
        for (int ch = NodeFirstChild(idx); ch != -1; ch = NodeNextSibling(ch))
            NodeRow(c, ch, frameMs, x, w);
}

static void SectionLabel(Context* c, const char* s) {
    Spacing();
    TextColored(COL32(255, 210, 120, 255), s);
}

void ShowProfiler(Context* c, bool* open, const char* extraInfo) {
    if (!open || !*open) return;

    const float winW = 660.0f, winH = 760.0f;
    SetNextWindowSize({ winW, winH }, true);
    if (!prim32::Begin("Profiler##p32prof")) { prim32::End(); return; }

    Window* win = c->curWindow;
    const float cx = win->pos.x + c->style.windowPadding.x;
    const float cw = winW - c->style.windowPadding.x * 2;

    // ------- scroll (mouse wheel over this window)
    static float scroll = 0, contentH = 0;
    bool hovered = c->hoveredWindow == win->id;
    if (hovered) scroll -= c->io.mouseWheel * 54.0f;
    float viewH = winH - c->style.titleBarHeight - c->style.windowPadding.y * 2;
    float maxScroll = contentH - viewH; if (maxScroll < 0) maxScroll = 0;
    if (scroll < 0) scroll = 0;
    if (scroll > maxScroll) scroll = maxScroll;
    float topY = GetCursorScreenPos().y;
    SetCursorScreenPos({ cx, topY - scroll });

    // ------- header: buttons
    static double copiedFlash = 0;
    if (Button("Copy profiler##cp", { 130, 0 })) {
        if (CopyToClipboard(extraInfo)) copiedFlash = GetTickCount64() / 1000.0;
    }
    SameLine();
    bool paused = IsPaused();
    if (Checkbox("pause##pp", &paused)) SetPaused(paused);
    SameLine();
    bool csv = GetCsvLog();
    if (Checkbox("csv log##cl", &csv)) SetCsvLog(csv);
    SameLine();
    if (Button("60##w60",  { 36, 0 })) SetStatWindow(60);
    SameLine();
    if (Button("120##w120",{ 42, 0 })) SetStatWindow(120);
    SameLine();
    if (Button("240##w240",{ 42, 0 })) SetStatWindow(240);
    SameLine();
    TextF("stats: last %d frames", GetStatWindow());
    if (GetTickCount64() / 1000.0 - copiedFlash < 1.5) {
        SameLine();
        TextColored(COL32(120, 230, 150, 255), "copied");
    }

    // ------- frame-time graph
    {
        const float* h; int n = FrameHistory(&h);
        Vec2 p = GetCursorScreenPos();
        float gw = cw, gh = 56;
        DrawRect(c, p.x, p.y, p.x + gw, p.y + gh, COL32(12, 13, 17, 255));
        float mx = 0.001f;
        for (int i = 0; i < n; i++) if (h[i] > mx) mx = h[i];
        float barW = gw / 240.0f;
        for (int i = 0; i < n; i++) {
            float v = h[i] / mx;
            DrawRect(c, p.x + i * barW, p.y + gh * (1 - v), p.x + i * barW + barW, p.y + gh,
                     HeatCol(h[i] / (mx > 33.3f ? mx : 33.3f) * 2.0f));
        }
        DrawRectStroke(c, p.x, p.y, p.x + gw, p.y + gh, c->style.colors[ZC_Border], 0, 1);
        char lbl[64]; snprintf(lbl, 64, "%.1f ms", mx);
        DrawText(c, p.x + 4, p.y + 2, c->style.colors[ZC_TextDim], lbl);
        Dummy({ gw, gh + 2 });
    }
    TextF("%.0f fps | %.3f ms avg | min %.3f | max %.3f%s",
          Fps(), FrameAvgMs(), FrameMinMs(), FrameMaxMs(), IsPaused() ? "  [PAUSED]" : "");

    // ------- process metrics
    const ProcStats& ps = GetProcStats();
    SectionLabel(c, "PROCESS");
    TextF("CPU  %5.1f%% of machine | %6.1f%% of one core | %d cores | %u threads | %u handles",
          ps.cpuTotalPct, ps.cpuCorePct, ps.cores, ps.threads, ps.handles);
    TextF("RAM  working %.1f MB (peak %.1f) | private %.1f MB | %.0f faults/s",
          ps.wsMB, ps.wsPeakMB, ps.privMB, ps.pageFaultsPerSec);
    if (ps.gpuValid)
        TextF("GPU  3D %.1f%% | copy %.1f%% | compute %.1f%% | video %.1f%% | other(%s) %.1f%%",
              ps.gpu3D, ps.gpuCopy, ps.gpuCompute, ps.gpuVideo, OtherEngineName(), ps.gpuOther);
    else
        TextColored(c->style.colors[ZC_TextDim], "GPU  engine utilization warming up / unavailable");
    if (ps.vramValid)
        TextF("VRAM %.0f MB / %.0f MB budget | shared %.0f / %.0f MB",
              ps.vramMB, ps.vramBudgetMB, ps.sharedMB, ps.sharedBudgetMB);
    {
        MemDrift md = GetMemDrift();
        if (md.hasStep)
            TextF("STEP %+.0f MB RAM / %+.0f MB VRAM, %.0fs ago (one-time alloc, not a leak)",
                  md.stepRamMB, md.stepVramMB, md.stepAgoSec);
        if (md.valid) {
            bool bad = md.ramMBperMin > 1.0f || md.vramMBperMin > 4.0f;
            TextColored(bad ? COL32(255, 110, 110, 255) : COL32(120, 210, 150, 255), "LEAK");
            SameLine();
            TextF("RAM %+.2f MB/min | VRAM %+.2f MB/min (over %.0fs)%s",
                  md.ramMBperMin, md.vramMBperMin, md.spanSec, bad ? "  <- investigate" : "  steady");
        } else if (md.hasStep) {
            TextColored(c->style.colors[ZC_TextDim], "LEAK verdict pending (collecting post-step samples)");
        }
    }
    TextColored(c->style.colors[ZC_TextDim], GetAdapterName());

    // ------- CPU tree
    SectionLabel(c, "CPU SCOPES (ms)");
    {
        char hdr[128];
        snprintf(hdr, sizeof(hdr), "%7s %7s %7s %7s %7s %5s %6s",
                 "self", "total", "avg", "min", "max", "calls", "%frame");
        Vec2 ts = TextSize(c, hdr);
        Vec2 p = GetCursorScreenPos();
        DrawText(c, cx + cw - ts.x - 4, p.y, c->style.colors[ZC_TextDim], hdr);
        DrawText(c, cx, p.y, c->style.colors[ZC_TextDim], "scope");
        Dummy({ cw, ROW_H });
    }
    float frameMs = NodeGet(0).totalMs;
    NodeRow(c, 0, frameMs, cx, cw);

    // ------- GPU zones
    SectionLabel(c, "GPU ZONES (ms)");
    {
        const GpuZone* z; int n = GetGpuZones(&z);
        float gmax = 0.001f;
        for (int i = 0; i < n; i++) if (z[i].depth == 0 && z[i].ms > gmax) gmax = z[i].ms;
        for (int i = 0; i < n; i++) {
            Vec2 p = GetCursorScreenPos();
            float frac = z[i].ms / gmax;
            DrawRect(c, cx, p.y, cx + cw * (frac > 1 ? 1 : frac), p.y + ROW_H - 2,
                     ColA(COL32(90, 140, 220, 255), 52));
            DrawText(c, cx + z[i].depth * 14.0f + 2, p.y, c->style.colors[ZC_Text], z[i].name);
            ZoneStats zs = {};
            GetGpuZoneWindow(z[i].name, &zs);
            char buf[96];
            snprintf(buf, 96, "%7.3f  avg %7.3f  min %7.3f  max %7.3f", z[i].ms, zs.avgMs, zs.minMs, zs.maxMs);
            Vec2 ts = TextSize(c, buf);
            DrawText(c, cx + cw - ts.x - 4, p.y, c->style.colors[ZC_Text], buf);
            Dummy({ cw, ROW_H });
        }
        if (!n) TextColored(c->style.colors[ZC_TextDim], "(waiting for GPU results)");
        TextColored(c->style.colors[ZC_TextDim], "frames-in-flight latent; low fps => GPU downclocks => ms rises");
    }

    // ------- memory inventory
    SectionLabel(c, "TRACKED MEMORY");
    {
        int n = MemCount();
        uint64_t largest = 1;
        for (int i = 0; i < n; i++) { const char* nm; uint64_t b; MemGet(i, &nm, &b); if (b > largest) largest = b; }
        for (int i = 0; i < n; i++) {
            const char* nm; uint64_t b; MemGet(i, &nm, &b);
            Vec2 p = GetCursorScreenPos();
            DrawRect(c, cx, p.y, cx + cw * ((float)b / (float)largest), p.y + ROW_H - 2,
                     ColA(COL32(120, 200, 160, 255), 42));
            DrawText(c, cx + 2, p.y, c->style.colors[ZC_Text], nm);
            char buf[32]; snprintf(buf, 32, "%10.2f MB", b / (1024.0 * 1024.0));
            Vec2 ts = TextSize(c, buf);
            DrawText(c, cx + cw - ts.x - 4, p.y, c->style.colors[ZC_Text], buf);
            Dummy({ cw, ROW_H });
        }
        TextF("tracked total %.2f MB | process private %.2f MB | untracked (heap/code/driver) %.2f MB",
              MemTotal() / 1048576.0, ps.privMB, ps.privMB - MemTotal() / 1048576.0);
        prim32::ResourceStats rs = prim32::GetResourceStats();
        TextF("resources: %u images | %u fonts | %.2f MB textures | %.2f MB font atlases",
              rs.images, rs.fonts, rs.textureBytes / 1048576.0, rs.atlasBytes / 1048576.0);
    }

    contentH = win->contentMax.y - (win->pos.y + c->style.titleBarHeight) + scroll + 8;
    prim32::End();
}

} // namespace p32prof
