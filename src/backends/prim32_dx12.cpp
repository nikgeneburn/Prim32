// ============================================================================
// Prim32 D3D12 backend implementation.
//
// GPU pipeline: no vertex/index buffers, no input assembler. One DrawInstanced
// of 6*N vertices; the vertex shader pulls 32-byte primitives from a
// StructuredBuffer (root SRV pointing directly at the mapped upload heap — the
// GPU reads exactly the bytes the CPU wrote, no copy in between), expands them
// to quads, clips them against a clip table, and the pixel shader evaluates
// analytic SDFs for AA rounded rects / circles / lines / borders / shadows.
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
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <stdio.h>
#include <string.h>
#include <prim32/prim32.h>
#include "prim32_internal.h"
#include <prim32/prim32_dx12.h>
#include <prim32/p32prof.h>

namespace prim32_dx12 {

template <class T> static void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

static const uint32_t CLIP_BYTES = 64 * 1024;          // 4096 clip rects
static const uint32_t MAX_FRAMES = 4;
static const uint32_t MAX_GPU_ZONES = 40;              // per frame
static const uint32_t TS_PER_FRAME  = MAX_GPU_ZONES * 2;

// ------------------------------------------------------------------- shaders
static const char* kShader = R"HLSL(
cbuffer CB : register(b0) { float2 uScale; float2 uOffset; uint uBase; uint uClipBase; uint2 uPad; }

struct Prim { float4 rect; uint uv0; uint uv1; uint color; uint meta; };
StructuredBuffer<Prim>   Prims : register(t0);
StructuredBuffer<float4> Clips : register(t1);
Texture2D    Tex[64] : register(t0, space1);
SamplerState SLin    : register(s0);

#define T_RECT      0
#define T_GLYPH     1
#define T_RRECT     2
#define T_RSTROKE   3
#define T_CIRCLE    4
#define T_CSTROKE   5
#define T_LINE      6
#define T_IMAGE     7
#define T_SHADOW    8

struct V2P {
    float4 pos  : SV_Position;
    float2 uv   : TEXCOORD0;
    nointerpolation float4 c0   : COLOR0;
    nointerpolation float4 c1   : COLOR1;
    nointerpolation float4 geo  : GEO;    // center.xy, half.xy (LINE: endpoints)
    nointerpolation float2 par  : PAR;    // pa=radius, pb=thickness/softness
    nointerpolation uint2  meta : META;   // x=type, y=texslot
};

float4 UnpackColor(uint c) {
    return float4(c & 0xff, (c >> 8) & 0xff, (c >> 16) & 0xff, c >> 24) * (1.0 / 255.0);
}

V2P VSMain(uint vid : SV_VertexID) {
    uint pi = vid / 6;
    uint corner = vid - pi * 6;
    Prim p = Prims[uBase + pi];

    uint type = p.meta & 15u;
    uint clipIdx = ((p.meta >> 4) & 4095u) + uClipBase;
    uint tex = (p.meta >> 16) & 255u;
    float pa = f16tof32(p.uv0);
    float pb = f16tof32(p.uv0 >> 16);

    float4 rect = p.rect + uOffset.xyxy;   // cached layers move for free

    // quad AABB (lines & shadows extend beyond their rect)
    float4 r = rect;
    if (type == T_LINE) {
        float pad = pb * 0.5 + 1.5;
        r = float4(min(rect.xy, rect.zw) - pad, max(rect.xy, rect.zw) + pad);
    } else if (type == T_SHADOW) {
        r += float4(-pb, -pb, pb, pb) * 3.0;
    }

    // vertex-shader clipping: clamp the quad, zero scissor changes ever
    float4 cl = Clips[clipIdx];
    float4 rc = float4(max(r.xy, cl.xy), min(r.zw, cl.zw));
    if (rc.z <= rc.x || rc.w <= rc.y) rc = 0;          // culled: zero-area

    float2 cw = float2((0x1A >> corner) & 1, (0x34 >> corner) & 1);
    float2 pos = lerp(rc.xy, rc.zw, cw);

    // uv remap through the ORIGINAL rect so clamping keeps texture registration
    float2 t = (pos - rect.xy) / max(rect.zw - rect.xy, 1e-6);
    float2 uvA = float2(p.uv0 & 0xffff, p.uv0 >> 16) * (1.0 / 65535.0);
    float2 uvB = float2(p.uv1 & 0xffff, p.uv1 >> 16) * (1.0 / 65535.0);

    V2P o;
    o.pos  = float4(pos.x * uScale.x - 1.0, 1.0 - pos.y * uScale.y, 0.5, 1.0);
    o.uv   = lerp(uvA, uvB, t);
    o.c0   = UnpackColor(p.color);
    o.c1   = UnpackColor(p.uv1);
    o.geo  = (type == T_LINE) ? rect
           : float4((rect.xy + rect.zw) * 0.5, (rect.zw - rect.xy) * 0.5);
    o.par  = float2(pa, pb);
    o.meta = uint2(type, tex);
    return o;
}

float SdRoundBox(float2 p, float2 h, float r) {
    float2 q = abs(p) - h + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

float4 PSMain(V2P i) : SV_Target {
    uint type = i.meta.x;
    float2 px = i.pos.xy;                 // pixel center, screen space
    float4 col = i.c0;
    float a = 1.0;

    [branch]
    if (type == T_RECT) {
        // solid: fastest possible path
    } else if (type == T_GLYPH) {
        a = Tex[NonUniformResourceIndex(i.meta.y)].Sample(SLin, i.uv).r;
    } else if (type == T_IMAGE) {
        col *= Tex[NonUniformResourceIndex(i.meta.y)].Sample(SLin, i.uv);
    } else if (type == T_LINE) {
        float2 pa2 = px - i.geo.xy, ba = i.geo.zw - i.geo.xy;
        float h = saturate(dot(pa2, ba) / max(dot(ba, ba), 1e-6));
        float d = length(pa2 - ba * h) - i.par.y * 0.5;
        a = saturate(0.5 - d);
    } else {
        // SDF family — distances in pixels, 1px analytic AA
        float d;
        if (type == T_CIRCLE || type == T_CSTROKE) {
            float r = min(i.geo.z, i.geo.w);
            d = length(px - i.geo.xy) - r;
        } else {
            d = SdRoundBox(px - i.geo.xy, i.geo.zw, min(i.par.x, min(i.geo.z, i.geo.w)));
        }
        if (i.c1.a > 0.0) {   // vertical gradient
            float t = saturate((px.y - (i.geo.y - i.geo.w)) / max(i.geo.w * 2.0, 1e-6));
            col = lerp(col, i.c1, t);
        }
        if (type == T_RSTROKE || type == T_CSTROKE)
            d = abs(d + i.par.y * 0.5) - i.par.y * 0.5;   // stroke kept inside bounds
        if (type == T_SHADOW) {
            float s = max(i.par.y, 0.5);
            float q = max(d, 0.0) / s;
            a = exp2(-2.885 * q * q);        // gaussian-ish falloff, a=1 inside
        } else {
            a = saturate(0.5 - d);
        }
    }
    float4 o = float4(col.rgb, col.a * a);
    if (o.a < 0.0035) discard;            // skip blend for fully transparent pixels
    return o;
}
)HLSL";

// --------------------------------------------------------------------- state
struct FrameCtx {
    ID3D12Resource* buf;       // [clips 64KB][prims]
    uint8_t*        mapped;
    uint64_t        fenceValue;
};

static struct {
    ID3D12Device*            device;
    ID3D12CommandQueue*      queue;
    ID3D12RootSignature*     rootSig;
    ID3D12PipelineState*     pso;
    ID3D12DescriptorHeap*    srvHeap;
    ID3D12Resource*          fontTex;
    ID3D12Fence*             fence;
    HANDLE                   fenceEvent;
    uint64_t                 fenceCounter;
    FrameCtx                 frames[MAX_FRAMES];
    uint32_t                 numFrames, frameIdx;
    uint32_t                 maxPrims, maxTextures, texCount;
    uint32_t                 srvStride;
    // gpu timing
    ID3D12QueryHeap*         queryHeap;
    ID3D12Resource*          queryReadback;
    uint64_t*                queryMapped;
    uint64_t                 tsFreq;
    GpuStats                 stats;
    uint64_t                 pendingBytes;
    prim32::Context*           ctx;
    // gpu zones
    struct ZoneRec { const char* name; int depth; uint32_t q0, q1; };
    ZoneRec                  zones[MAX_FRAMES][MAX_GPU_ZONES];
    uint32_t                 zoneCount[MAX_FRAMES];
    uint32_t                 uiZone[MAX_FRAMES];
    uint32_t                 tsUsed;
    int                      zoneStack[8]; int zoneSp;
    IDXGIAdapter3*           adapter3;
    uint64_t                 lastVramMs;
    // texture slots (font atlases + images); owned resources released here
    struct TexRes { ID3D12Resource* res; bool owned; uint64_t bytes; };
    TexRes                   texRes[64];
    uint16_t                 freeSlots[64];
    uint32_t                 freeCount;
    uint64_t                 texBytes;
    // cached-layer VRAM blocks (indexed by CachedLayer::id)
    struct CacheBlock { ID3D12Resource* res; D3D12_GPU_VIRTUAL_ADDRESS va; uint32_t cap; uint32_t version; };
    CacheBlock               cache[256];
    struct DeadRes { ID3D12Resource* res; uint64_t fence; };
    DeadRes                  dead[64];
    uint32_t                 deadCount;
    uint64_t                 cacheBytes;
} S;

// ------------------------------------------------------------------- helpers
static ID3D12Resource* CreateBuffer(uint64_t size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = heap;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* res = nullptr;
    S.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr,
                                      IID_PPV_ARGS(&res));
    return res;
}

static bool CompileShaders(ID3DBlob** vs, ID3DBlob** ps) {
    UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    ID3DBlob* err = nullptr;
    if (FAILED(D3DCompile(kShader, strlen(kShader), "prim32", nullptr, nullptr,
                          "VSMain", "vs_5_1", flags, 0, vs, &err))) {
        if (err) { OutputDebugStringA((char*)err->GetBufferPointer()); err->Release(); }
        return false;
    }
    if (err) { err->Release(); err = nullptr; }
    if (FAILED(D3DCompile(kShader, strlen(kShader), "prim32", nullptr, nullptr,
                          "PSMain", "ps_5_1", flags, 0, ps, &err))) {
        if (err) { OutputDebugStringA((char*)err->GetBufferPointer()); err->Release(); }
        return false;
    }
    if (err) err->Release();
    return true;
}

static bool CreatePipeline(DXGI_FORMAT rtvFormat) {
    // root signature: 8 root constants + 2 root SRVs + 1 texture table + static sampler
    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = S.maxTextures;
    range.BaseShaderRegister = 0; range.RegisterSpace = 1;
    range.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER params[4] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0; params[0].Constants.Num32BitValues = 8;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[1].Descriptor.ShaderRegister = 0;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[2].Descriptor.ShaderRegister = 1;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[3].DescriptorTable.NumDescriptorRanges = 1;
    params[3].DescriptorTable.pDescriptorRanges = &range;
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samp = {};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.MaxLOD = 0.0f;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rs = {};
    rs.NumParameters = 4; rs.pParameters = params;
    rs.NumStaticSamplers = 1; rs.pStaticSamplers = &samp;
    rs.Flags = (D3D12_ROOT_SIGNATURE_FLAGS)(
               D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
               D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
               D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS);

    ID3DBlob* sig = nullptr, *err = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) {
        if (err) { OutputDebugStringA((char*)err->GetBufferPointer()); err->Release(); }
        return false;
    }
    HRESULT hr = S.device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                               IID_PPV_ARGS(&S.rootSig));
    sig->Release();
    if (FAILED(hr)) return false;

    ID3DBlob* vs = nullptr, *ps = nullptr;
    if (!CompileShaders(&vs, &ps)) return false;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = S.rootSig;
    pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pd.BlendState.RenderTarget[0].BlendEnable = TRUE;
    pd.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    pd.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    pd.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    pd.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    pd.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    pd.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = rtvFormat;
    pd.SampleDesc.Count = 1;
    hr = S.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&S.pso));
    vs->Release(); ps->Release();
    return SUCCEEDED(hr);
}

// ------------------------------------------------- texture creation (shared)
// Load-time path for the built-in atlas, runtime font atlases and images.
// Uploads synchronously (transient list + fence wait): loading is an explicit
// resource-layer operation, never part of the frame hot path.
static void WriteNullSrv(uint32_t slot) {
    D3D12_CPU_DESCRIPTOR_HANDLE h = S.srvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (size_t)slot * S.srvStride;
    D3D12_SHADER_RESOURCE_VIEW_DESC nd = {};
    nd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    nd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    nd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    nd.Texture2D.MipLevels = 1;
    S.device->CreateShaderResourceView(nullptr, &nd, h);
}

static uint32_t AllocTexSlot() {
    if (S.freeCount) return S.freeSlots[--S.freeCount];
    if (S.texCount < S.maxTextures) return S.texCount++;
    return 0xFFFFFFFFu;
}

static uint32_t CreateTexture2D(const void* pixels, int w, int h, int fmt, const char* dbgName) {
    (void)dbgName;
    if (!pixels || w <= 0 || h <= 0 || !S.device) return 0xFFFFFFFFu;
    uint32_t slot = AllocTexSlot();
    if (slot == 0xFFFFFFFFu) return 0xFFFFFFFFu;
    const DXGI_FORMAT dxfmt = fmt == 0 ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    const uint32_t bpp = fmt == 0 ? 1u : 4u;

    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = (UINT64)w; td.Height = (UINT)h;
    td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = dxfmt;
    td.SampleDesc.Count = 1;
    ID3D12Resource* tex = nullptr;
    if (FAILED(S.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex)))) {
        if (S.freeCount < 64) S.freeSlots[S.freeCount++] = (uint16_t)slot;
        return 0xFFFFFFFFu;
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp; UINT64 total = 0; UINT rows; UINT64 rowBytes;
    S.device->GetCopyableFootprints(&td, 0, 1, 0, &fp, &rows, &rowBytes, &total);

    ID3D12Resource* up = CreateBuffer(total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!up) { tex->Release(); if (S.freeCount < 64) S.freeSlots[S.freeCount++] = (uint16_t)slot; return 0xFFFFFFFFu; }
    uint8_t* dst = nullptr; D3D12_RANGE rr = { 0, 0 };
    up->Map(0, &rr, (void**)&dst);
    for (UINT r = 0; r < rows; r++)
        memcpy(dst + fp.Offset + r * fp.Footprint.RowPitch,
               (const uint8_t*)pixels + (size_t)r * w * bpp, (size_t)w * bpp);
    up->Unmap(0, nullptr);

    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* cl = nullptr;
    S.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    S.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&cl));

    D3D12_TEXTURE_COPY_LOCATION dl = {}, slc = {};
    dl.pResource = tex; dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    slc.pResource = up; slc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; slc.PlacedFootprint = fp;
    cl->CopyTextureRegion(&dl, 0, 0, 0, &slc, nullptr);

    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = tex;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &b);
    cl->Close();

    ID3D12CommandList* lists[] = { cl };
    S.queue->ExecuteCommandLists(1, lists);
    S.queue->Signal(S.fence, ++S.fenceCounter);
    if (S.fence->GetCompletedValue() < S.fenceCounter) {
        S.fence->SetEventOnCompletion(S.fenceCounter, S.fenceEvent);
        WaitForSingleObject(S.fenceEvent, INFINITE);
    }
    cl->Release(); alloc->Release(); up->Release();

    D3D12_CPU_DESCRIPTOR_HANDLE hd = S.srvHeap->GetCPUDescriptorHandleForHeapStart();
    hd.ptr += (size_t)slot * S.srvStride;
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = dxfmt;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    S.device->CreateShaderResourceView(tex, &sd, hd);

    S.texRes[slot] = { tex, true, total };
    S.texBytes += total;
    p32prof::TrackMem("gpu: textures (fonts+images)", S.texBytes);
    return slot;
}

// prim32 resource-layer hooks
static uint32_t HookCreateTexture(const void* px, int w, int h, int fmt, const char* name) {
    return CreateTexture2D(px, w, h, fmt, name);
}
static void HookDestroyTexture(uint32_t slot) {
    if (slot >= 64 || !S.texRes[slot].res || !S.texRes[slot].owned) return;
    if (S.deadCount < 64) {
        S.dead[S.deadCount++] = { S.texRes[slot].res, S.fenceCounter + 1 };
    } else {                                   // dead list full (rare): drain
        S.queue->Signal(S.fence, ++S.fenceCounter);
        S.fence->SetEventOnCompletion(S.fenceCounter, S.fenceEvent);
        WaitForSingleObject(S.fenceEvent, INFINITE);
        S.texRes[slot].res->Release();
    }
    S.texBytes -= S.texRes[slot].bytes;
    p32prof::TrackMem("gpu: textures (fonts+images)", S.texBytes);
    S.texRes[slot] = {};
    WriteNullSrv(slot);
    if (S.freeCount < 64) S.freeSlots[S.freeCount++] = (uint16_t)slot;
}
static const prim32::Prim32BackendHooks s_prim32Hooks = { &HookCreateTexture, &HookDestroyTexture };

// Upload the built-in font atlas: slot 0, after null-filling the whole table
// so dynamic indexing of untouched slots is always defined.
static bool UploadFontAtlas() {
    const prim32::FontAtlas& fa = S.ctx->font;
    for (uint32_t i = 0; i < S.maxTextures; i++) WriteNullSrv(i);
    S.texCount = 0;
    S.freeCount = 0;
    return CreateTexture2D(fa.pixels, fa.width, fa.height, 0, "built-in font atlas") == 0;
}

// ----------------------------------------------------------------------- API
bool Init(prim32::Context* ctx, const InitDesc& desc) {
    memset(&S, 0, sizeof(S));
    S.ctx = ctx;
    S.device = desc.device;
    S.queue = desc.queue;
    S.numFrames = desc.framesInFlight ? desc.framesInFlight : 3;
    if (S.numFrames > MAX_FRAMES) S.numFrames = MAX_FRAMES;
    S.maxPrims = desc.maxPrims ? desc.maxPrims : (1u << 20);
    S.maxTextures = 64;   // fixed: matches shader-side Tex[64]
    S.srvStride = S.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    if (FAILED(S.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&S.fence)))) return false;
    S.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = S.maxTextures;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(S.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&S.srvHeap)))) return false;

    if (!CreatePipeline(desc.rtvFormat)) return false;
    if (!UploadFontAtlas()) return false;

    const uint64_t frameBytes = CLIP_BYTES + (uint64_t)S.maxPrims * sizeof(prim32::Prim);
    for (uint32_t i = 0; i < S.numFrames; i++) {
        FrameCtx& f = S.frames[i];
        f.buf = CreateBuffer(frameBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!f.buf) return false;
        D3D12_RANGE rr = { 0, 0 };                     // we never read
        f.buf->Map(0, &rr, (void**)&f.mapped);
        f.fenceValue = 0;
    }

    // GPU timestamps
    D3D12_QUERY_HEAP_DESC qh = {};
    qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qh.Count = S.numFrames * TS_PER_FRAME;
    S.device->CreateQueryHeap(&qh, IID_PPV_ARGS(&S.queryHeap));
    S.queryReadback = CreateBuffer((uint64_t)S.numFrames * TS_PER_FRAME * sizeof(uint64_t),
                                   D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    if (S.queryReadback) {
        D3D12_RANGE all = { 0, (SIZE_T)S.numFrames * TS_PER_FRAME * sizeof(uint64_t) };
        S.queryReadback->Map(0, &all, (void**)&S.queryMapped);
    }
    S.queue->GetTimestampFrequency(&S.tsFreq);
    if (!S.tsFreq) S.tsFreq = 1;

    prim32::Prim32SetBackendHooks(&s_prim32Hooks);   // resource layer can now create textures

    // adapter interface: VRAM usage/budget + name for the profiler
    IDXGIFactory4* fac = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&fac)))) {
        IDXGIAdapter3* ad = nullptr;
        if (SUCCEEDED(fac->EnumAdapterByLuid(S.device->GetAdapterLuid(), IID_PPV_ARGS(&ad)))) {
            S.adapter3 = ad;
            DXGI_ADAPTER_DESC d = {};
            ad->GetDesc(&d);
            char nm[128]; int k = 0;
            for (; k < 127 && d.Description[k]; k++) nm[k] = d.Description[k] < 128 ? (char)d.Description[k] : '?';
            nm[k] = 0;
            p32prof::SetAdapterName(nm);
        }
        fac->Release();
    }

    const prim32::FontAtlas& fat = ctx->font;
    p32prof::TrackMem("gpu: upload ring (mapped)", (uint64_t)frameBytes * S.numFrames);
    p32prof::TrackMem("gpu: font atlas", (uint64_t)fat.width * fat.height);
    p32prof::TrackMem("cpu: font atlas copy", (uint64_t)fat.width * fat.height);
    p32prof::TrackMem("gpu: query readback", (uint64_t)S.numFrames * TS_PER_FRAME * 8);
    return true;
}

void Shutdown() {
    prim32::Prim32SetBackendHooks(nullptr);        // no more loads after this
    // drain the queue
    if (S.queue && S.fence) {
        S.queue->Signal(S.fence, ++S.fenceCounter);
        if (S.fence->GetCompletedValue() < S.fenceCounter) {
            S.fence->SetEventOnCompletion(S.fenceCounter, S.fenceEvent);
            WaitForSingleObject(S.fenceEvent, INFINITE);
        }
    }
    for (uint32_t i = 0; i < 256; i++) SafeRelease(S.cache[i].res);
    for (uint32_t i = 0; i < S.deadCount; i++) SafeRelease(S.dead[i].res);
    S.deadCount = 0;
    for (uint32_t i = 0; i < S.numFrames; i++) SafeRelease(S.frames[i].buf);
    SafeRelease(S.queryReadback); SafeRelease(S.queryHeap);
    for (uint32_t i = 0; i < 64; i++) if (S.texRes[i].owned) SafeRelease(S.texRes[i].res);
    SafeRelease(S.fontTex); SafeRelease(S.srvHeap);
    SafeRelease(S.pso); SafeRelease(S.rootSig);
    SafeRelease(S.fence);
    SafeRelease(S.adapter3);
    if (S.fenceEvent) { CloseHandle(S.fenceEvent); S.fenceEvent = nullptr; }
}

prim32::FrameMem NewFrame() {
    FrameCtx& f = S.frames[S.frameIdx];
    // release retired cache buffers once the GPU is past them
    if (S.deadCount) {
        uint64_t done = S.fence->GetCompletedValue();
        uint32_t w = 0;
        for (uint32_t i = 0; i < S.deadCount; i++) {
            if (S.dead[i].fence <= done) S.dead[i].res->Release();
            else S.dead[w++] = S.dead[i];
        }
        S.deadCount = w;
    }
    if (f.fenceValue && S.fence->GetCompletedValue() < f.fenceValue) {
        S.fence->SetEventOnCompletion(f.fenceValue, S.fenceEvent);
        WaitForSingleObject(S.fenceEvent, INFINITE);
    }
    // this ring slot's timestamps are certainly resolved now: publish GPU zones
    if (S.queryMapped && f.fenceValue) {
        static p32prof::GpuZone out[MAX_GPU_ZONES];
        const uint64_t* ts = S.queryMapped + (size_t)S.frameIdx * TS_PER_FRAME;
        uint32_t n = S.zoneCount[S.frameIdx];
        int m = 0;
        for (uint32_t i = 0; i < n; i++) {
            const auto& r = S.zones[S.frameIdx][i];
            if (r.q1 == 0xFFFFFFFFu) continue;
            uint64_t a = ts[r.q0], b = ts[r.q1];
            float ms = b > a ? (float)((double)(b - a) * 1000.0 / (double)S.tsFreq) : 0.0f;
            if (i == S.uiZone[S.frameIdx]) S.stats.gpuMs = ms;
            out[m].name = r.name; out[m].ms = ms; out[m].depth = r.depth; m++;
        }
        if (m) p32prof::SetGpuZones(out, m);
    }
    S.zoneCount[S.frameIdx] = 0;
    S.uiZone[S.frameIdx] = 0;
    S.tsUsed = 0;
    S.zoneSp = 0;

    // VRAM usage/budget (cheap, but no need for more than ~2 Hz)
    if (S.adapter3) {
        uint64_t now = GetTickCount64();
        if (now - S.lastVramMs > 500) {
            S.lastVramMs = now;
            DXGI_QUERY_VIDEO_MEMORY_INFO l = {}, nl = {};
            S.adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &l);
            S.adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nl);
            p32prof::SetVideoMemory(l.CurrentUsage / 1048576.0, l.Budget / 1048576.0,
                                  nl.CurrentUsage / 1048576.0, nl.Budget / 1048576.0);
        }
    }
    prim32::FrameMem m;
    m.clips   = (prim32::ClipRect*)f.mapped;
    m.clipCap = CLIP_BYTES / sizeof(prim32::ClipRect);
    m.prims   = (prim32::Prim*)(f.mapped + CLIP_BYTES);
    m.primCap = S.maxPrims;
    return m;
}

void Render(prim32::DrawData* dd, ID3D12GraphicsCommandList* cmd) {
    FrameCtx& f = S.frames[S.frameIdx];
    if (dd->displaySize.x <= 0 || dd->displaySize.y <= 0) return;

    // ---- upload dirty cached layers via their own persistent upload buffer.
    // Steady state records nothing here; the GPU reads caches from VRAM.
    for (uint32_t i = 0; i < dd->rangeCount; i++) {
        const prim32::CachedLayer* cl = dd->ranges[i].cache;
        if (!cl || cl->id >= 256) continue;
        auto& b = S.cache[cl->id];
        if (b.res && b.version == cl->version) continue;         // clean
        uint64_t bytes = (uint64_t)cl->primCount * sizeof(prim32::Prim);
        if (!bytes) continue;
        if (!b.res || b.cap < cl->primCount) {
            if (b.res && S.deadCount < 64) {
                S.dead[S.deadCount++] = { b.res, S.fenceCounter + 1 };
                S.cacheBytes -= (uint64_t)b.cap * sizeof(prim32::Prim);
            }
            // right-size to the baked content (+25% slack, 1 MB granularity)
            uint32_t cap = cl->primCount + (cl->primCount >> 2);
            if (cap < 2048) cap = 2048;
            cap = (cap + 32767u) & ~32767u;
            if (cap > cl->primCap) cap = cl->primCap;
            uint64_t width = (uint64_t)cap * sizeof(prim32::Prim);
            D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC rd = {};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Width = width;
            rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
            rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ID3D12Resource* res = nullptr;
            if (FAILED(S.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&res)))) continue;
            b.res = res;
            b.va = res->GetGPUVirtualAddress(); b.cap = cap; b.version = cl->version - 1;
            S.cacheBytes += width;
            p32prof::TrackMem("gpu: cached layers (vram)", S.cacheBytes);
        } else {
            D3D12_RESOURCE_BARRIER tb = {};
            tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            tb.Transition.pResource = b.res;
            tb.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            tb.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            tb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cmd->ResourceBarrier(1, &tb);
        }
        // transient upload buffer: exists only for this copy, retired via fence
        ID3D12Resource* up = CreateBuffer(bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!up) continue;
        {
            uint8_t* m = nullptr; D3D12_RANGE rr = { 0, 0 };
            up->Map(0, &rr, (void**)&m);
            memcpy(m, cl->prims, (size_t)bytes);                  // sequential WC write
            up->Unmap(0, nullptr);
        }
        cmd->CopyBufferRegion(b.res, 0, up, 0, bytes);
        D3D12_RESOURCE_BARRIER tb = {};
        tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        tb.Transition.pResource = b.res;
        tb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        tb.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        tb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(1, &tb);
        if (S.deadCount < 64) {
            S.dead[S.deadCount++] = { up, S.fenceCounter + 1 };
        } else {   // dead list full (rare): drain synchronously
            S.queue->Signal(S.fence, ++S.fenceCounter);
            S.fence->SetEventOnCompletion(S.fenceCounter, S.fenceEvent);
            WaitForSingleObject(S.fenceEvent, INFINITE);
            up->Release();
        }
        b.version = cl->version;
    }

    S.uiZone[S.frameIdx] = S.zoneCount[S.frameIdx];
    GpuZoneBegin(cmd, "UI pass");

    D3D12_VIEWPORT vp = { 0, 0, dd->displaySize.x, dd->displaySize.y, 0, 1 };
    D3D12_RECT sc = { 0, 0, (LONG)dd->displaySize.x, (LONG)dd->displaySize.y };
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);

    cmd->SetPipelineState(S.pso);
    cmd->SetGraphicsRootSignature(S.rootSig);
    cmd->SetDescriptorHeaps(1, &S.srvHeap);
    cmd->SetGraphicsRootDescriptorTable(3, S.srvHeap->GetGPUDescriptorHandleForHeapStart());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    D3D12_GPU_VIRTUAL_ADDRESS base = f.buf->GetGPUVirtualAddress();
    const D3D12_GPU_VIRTUAL_ADDRESS ringVA = base + CLIP_BYTES;
    D3D12_GPU_VIRTUAL_ADDRESS boundVA = 0;
    cmd->SetGraphicsRootShaderResourceView(2, base);               // clips

    struct { float sx, sy, ox, oy; uint32_t basePrim, clipBase, pad0, pad1; } cb = {
        2.0f / dd->displaySize.x, 2.0f / dd->displaySize.y, 0, 0, 0, 0, 0, 0 };

    // ~one draw per range; dynamic ranges read the ring, cached ranges read VRAM
    for (uint32_t i = 0; i < dd->rangeCount; i++) {
        const prim32::DrawRange& r = dd->ranges[i];
        D3D12_GPU_VIRTUAL_ADDRESS va = ringVA;
        if (r.cache) {
            const auto& b = S.cache[r.cache->id < 256 ? r.cache->id : 0];
            if (!b.res || b.version != r.cache->version) continue;   // not uploaded yet
            va = b.va;
        }
        if (va != boundVA) { cmd->SetGraphicsRootShaderResourceView(1, va); boundVA = va; }
        GpuZoneBegin(cmd, r.label ? r.label : "(range)");
        cb.ox = r.ox; cb.oy = r.oy;
        cb.basePrim = r.start;
        cb.clipBase = r.clipBase;
        cmd->SetGraphicsRoot32BitConstants(0, 8, &cb, 0);
        cmd->DrawInstanced(r.count * 6, 1, 0, 0);
        GpuZoneEnd(cmd);
    }

    GpuZoneEnd(cmd);   // UI pass
    if (S.queryHeap && S.tsUsed)
        cmd->ResolveQueryData(S.queryHeap, D3D12_QUERY_TYPE_TIMESTAMP,
                              S.frameIdx * TS_PER_FRAME, S.tsUsed,
                              S.queryReadback, (uint64_t)S.frameIdx * TS_PER_FRAME * sizeof(uint64_t));
    S.pendingBytes = (uint64_t)dd->totalPrims * sizeof(prim32::Prim)
                   + (uint64_t)dd->clipCount * sizeof(prim32::ClipRect);
}

void NotifyExecuted() {
    FrameCtx& f = S.frames[S.frameIdx];
    S.queue->Signal(S.fence, ++S.fenceCounter);
    f.fenceValue = S.fenceCounter;
    S.frameIdx = (S.frameIdx + 1) % S.numFrames;
    S.stats.bytesStreamed = S.pendingBytes;
}

void GpuZoneBegin(ID3D12GraphicsCommandList* cmd, const char* name) {
    if (!S.queryHeap || S.zoneSp >= 8) return;
    uint32_t fi = S.frameIdx;
    if (S.zoneCount[fi] >= MAX_GPU_ZONES || S.tsUsed + 2 > TS_PER_FRAME) return;
    uint32_t z = S.zoneCount[fi]++;
    auto& rec = S.zones[fi][z];
    rec.name = name; rec.depth = S.zoneSp; rec.q0 = S.tsUsed++; rec.q1 = 0xFFFFFFFFu;
    S.zoneStack[S.zoneSp++] = (int)z;
    cmd->EndQuery(S.queryHeap, D3D12_QUERY_TYPE_TIMESTAMP, fi * TS_PER_FRAME + rec.q0);
}

void GpuZoneEnd(ID3D12GraphicsCommandList* cmd) {
    if (!S.queryHeap || S.zoneSp <= 0) return;
    uint32_t fi = S.frameIdx;
    auto& rec = S.zones[fi][S.zoneStack[--S.zoneSp]];
    if (S.tsUsed >= TS_PER_FRAME) return;
    rec.q1 = S.tsUsed++;
    cmd->EndQuery(S.queryHeap, D3D12_QUERY_TYPE_TIMESTAMP, fi * TS_PER_FRAME + rec.q1);
}

uint32_t RegisterTexture(ID3D12Resource* tex, DXGI_FORMAT srvFormat) {
    uint32_t slot = AllocTexSlot();
    if (slot == 0xFFFFFFFFu) return 0;
    S.texRes[slot] = { nullptr, false, 0 };    // caller keeps ownership
    D3D12_CPU_DESCRIPTOR_HANDLE h = S.srvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (size_t)slot * S.srvStride;
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = srvFormat;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    S.device->CreateShaderResourceView(tex, &sd, h);
    return slot;
}

GpuStats GetGpuStats() { return S.stats; }
} // namespace prim32_dx12
