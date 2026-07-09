// ============================================================================
// Prim32 D3D12 backend.
//
//   InitDesc: your device/queue + limits. The backend owns:
//     - one PSO + one root signature (set once per frame)
//     - a ring of persistently-mapped upload buffers (frames-in-flight deep)
//     - the font atlas texture + a 64-slot bindless-style texture table
//     - GPU timestamp queries
//
//   Frame flow:
//     prim32::FrameMem mem = prim32_dx12::NewFrame();     // waits ring fence
//     prim32::NewFrame(mem, io);
//     ... widgets / draws ...                          // write straight into GPU memory
//     prim32::DrawData* dd = prim32::EndFrame();
//     prim32_dx12::Render(dd, cmdList);                  // records ~1 draw per window
//     queue->ExecuteCommandLists(...);
//     prim32_dx12::NotifyExecuted();                     // fences the ring slot
// ============================================================================
#pragma once
#include <d3d12.h>
#include <prim32/prim32.h>

namespace prim32_dx12 {

struct InitDesc {
    ID3D12Device*       device;
    ID3D12CommandQueue* queue;          // used for atlas upload + ring fencing
    DXGI_FORMAT         rtvFormat;      // e.g. DXGI_FORMAT_R8G8B8A8_UNORM
    uint32_t            framesInFlight; // typically 3 (match swapchain)
    uint32_t            maxPrims;       // per frame; 0 -> 1,048,576 (32 MB/frame)
    uint32_t            maxTextures;    // 0 -> 64 (slot 0 = font atlas)
};

struct GpuStats {
    float    gpuMs;          // UI pass GPU time of the last completed frame
    uint64_t bytesStreamed;  // prim+clip bytes written last frame
};

PRIM32_API bool           Init(prim32::Context* ctx, const InitDesc& desc);
PRIM32_API void           Shutdown();
PRIM32_API prim32::FrameMem NewFrame();
PRIM32_API void           Render(prim32::DrawData* dd, ID3D12GraphicsCommandList* cmd);
PRIM32_API void           NotifyExecuted();
PRIM32_API uint32_t       RegisterTexture(ID3D12Resource* tex, DXGI_FORMAT srvFormat);
// GPU timestamp zones (nest up to 8 deep). Record only BETWEEN NewFrame() and
// the end of Render() — Render resolves the queries. Results appear in p32prof
// (framesInFlight frames latent).
PRIM32_API void           GpuZoneBegin(ID3D12GraphicsCommandList* cmd, const char* name);
PRIM32_API void           GpuZoneEnd(ID3D12GraphicsCommandList* cmd);
PRIM32_API GpuStats       GetGpuStats();

} // namespace prim32_dx12
