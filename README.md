# Prim32

**One primitive. Thirty-two bytes. Straight to the GPU.**

**Documentation: [prim32.mintlify.site](https://prim32.mintlify.site/)**

Prim32 is a raw, high-throughput immediate-mode GUI and 2D rendering framework built specifically for Direct3D 12.

It keeps the familiar programming model of libraries such as Dear ImGui while replacing the traditional CPU-generated vertex and index pipeline with a compact GPU-driven primitive stream.

Every drawable is represented by one fixed 32-byte primitive:

- Solid rectangles
- Rounded rectangles
- Rectangle borders
- Circles and rings
- Lines
- Glyphs
- Images
- Drop shadows

Widgets and raw drawing commands write these primitives directly into persistently mapped D3D12 upload memory. The vertex shader reads the primitive stream with `SV_VertexID`, expands each primitive into a quad, applies clipping, and passes analytic shape data to the pixel shader.

There is no CPU tessellation step, no intermediate vertex buffer build, no index buffer, and no per-shape draw call.

## Goals

Prim32 is designed around a small set of priorities:

1. Extremely low CPU overhead per primitive
2. Predictable memory usage
3. Minimal draw calls and state changes
4. No steady-state allocations
5. Raw D3D12 integration without owning the application
6. Immediate-mode ergonomics for widgets
7. Direct draw-list access for custom rendering
8. Built-in profiling for CPU, GPU, RAM, and VRAM behavior

Prim32 is not intended to hide Direct3D 12. It is intended to fit cleanly inside an existing D3D12 renderer.

## Why Prim32 is fast

### One drawable equals one 32-byte primitive

Traditional immediate-mode GUI renderers usually convert shapes into CPU-generated vertices and indices.

A rounded rectangle may require dozens of vertices. A glyph normally requires multiple vertices plus index data. Prim32 stores either case as one 32-byte structure.

```cpp
struct Prim
{
    float x0;
    float y0;
    float x1;
    float y1;

    uint32_t uv0;
    uint32_t uv1;
    uint32_t color;
    uint32_t meta;
};

static_assert(sizeof(Prim) == 32);
```

The primitive type, clip rectangle, texture slot, packed parameters, color, and destination bounds all fit inside this structure.

### Direct writes into mapped GPU memory

The CPU does not build an intermediate draw list that later gets converted or copied.

The active frame owns a persistently mapped upload buffer. Primitive emitters reserve space and write directly into that mapped region.

```cpp
prim32::Prim* prim = prim32::AddPrims(context, 1);

prim->x0 = x0;
prim->y0 = y0;
prim->x1 = x1;
prim->y1 = y1;
prim->color = color;
prim->meta = prim32::PackMeta(...);
```

The memory is written sequentially and is never read back by the CPU during frame construction.

### No vertex or index buffers

The backend does not bind a conventional GUI vertex buffer or index buffer.

The vertex shader uses `SV_VertexID` to determine:

- Which primitive is being drawn
- Which of the six quad vertices is being generated
- Which rectangle, UV range, clip index, texture, and shape parameters belong to that primitive

The GPU expands every primitive into two triangles.

```hlsl
uint primitiveIndex = vertexId / 6;
uint cornerIndex = vertexId - primitiveIndex * 6;
Prim primitive = Prims[basePrimitive + primitiveIndex];
```

### Analytic SDF shapes

Rounded rectangles, circles, rings, borders, lines, and shadows are evaluated analytically in the pixel shader.

This removes CPU-side geometry generation and keeps shape cost fixed regardless of roundness or resolution.

Benefits include:

- Smooth one-pixel antialiasing
- No MSAA requirement
- No per-corner tessellation
- Resolution-independent rounded geometry
- Constant primitive storage size

### Vertex-shader clipping

Each primitive stores a compact clip-table index.

The vertex shader clamps the generated quad against the corresponding clip rectangle. UV coordinates are reconstructed using the original unclipped rectangle, so text and images remain correctly registered.

This avoids changing the hardware scissor rectangle for every clipped region.

### One pipeline

The standard backend uses:

- One graphics pipeline state
- One root signature
- One shader-visible texture descriptor heap
- One structured primitive stream
- One structured clip table
- One draw per ordered range

A simple scene can render in one draw. Windowed scenes normally use one draw per overlapping ordered range, with no pipeline changes between ranges.

## Architecture

Prim32 is divided into a small core, a D3D12 backend, a resource layer, and a profiler.

```text
include/
    prim32/
        prim32.h
        prim32_dx12.h
        p32prof.h

src/
    prim32.cpp
    prim32_internal.h
    prim32_resources.cpp
    backends/
        prim32_dx12.cpp
    profiler/
        p32prof.cpp
        p32prof_ui.cpp

examples/
    d3d12_demo/
        main.cpp
        embedded_png.h

scripts/
    build_demo.bat
```

### Core

The core owns:

- Context state
- Input state
- Window state
- Widget behavior
- Layout
- IDs
- Clip stacks
- Font measurement
- Primitive emission
- Layer ordering
- Cached layers

### D3D12 backend

The backend owns:

- Root signature
- Pipeline state
- Shader-visible SRV heap
- Frame upload ring
- Clip buffers
- Texture registration
- Font atlas textures
- GPU timestamp queries
- Fence synchronization
- Cached layer GPU buffers

### Resource layer

The resource layer provides:

- Images from files
- Images from encoded memory
- Fonts from files
- Fonts from memory
- Generational image handles
- Generational font handles
- Default font selection
- Font stack selection
- Fence-safe texture destruction

### Profiler

The included profiler provides:

- Hierarchical CPU scopes
- GPU timestamp zones
- Frame history
- Process CPU usage
- Process RAM usage
- Page-fault rate
- Thread and handle counts
- Per-process GPU engine usage
- VRAM usage and budget
- Shared GPU memory usage and budget
- Allocation inventory
- RAM and VRAM drift estimates
- Clipboard dumps
- CSV logging

## Documentation

Full documentation lives at **[prim32.mintlify.site](https://prim32.mintlify.site/)**:

- [Quickstart](https://prim32.mintlify.site/quickstart) — build and run the benchmark in two minutes
- [Integration](https://prim32.mintlify.site/integration) — wire Prim32 into an existing D3D12 frame loop
- [Architecture](https://prim32.mintlify.site/concepts/architecture) — the 32-byte primitive model, zero-copy submission, vertex pulling
- [Layers & caching](https://prim32.mintlify.site/concepts/layers-and-caching) — background/foreground layers and retained layers that move for free
- [Performance](https://prim32.mintlify.site/concepts/performance) — measured numbers, the bottleneck walls, the SIMD bulk-emit pattern
- [API reference](https://prim32.mintlify.site/api/drawlist) — [DrawList](https://prim32.mintlify.site/api/drawlist), [images & fonts](https://prim32.mintlify.site/api/images-and-fonts), [widgets & windows](https://prim32.mintlify.site/api/widgets-and-windows), [D3D12 backend](https://prim32.mintlify.site/api/backend-d3d12), [profiler](https://prim32.mintlify.site/api/profiler)

## Requirements

- Windows
- Direct3D 12
- C++17 or newer
- A D3D12-capable GPU
- Windows SDK
- MSVC, Clang-cl, or MinGW-w64

The current Windows implementation uses:

- D3D12
- DXGI
- D3DCompiler
- GDI for font rasterization (FreeType optional)
- WIC for image decoding
- PDH for per-process GPU utilization
- PSAPI for process memory statistics

No external runtime library is required by the default implementation.

## Build

### MSVC

Open an x64 Native Tools Command Prompt and build the project:

```bat
build.bat
```

The executable is written to `build/msvc-release/Prim32Demo.exe`.

### CMake

```bash
cmake -S . -B build
cmake --build build --config Release
```

The demo is built by default only when Prim32 is the top-level CMake project.

### Add to an existing CMake project

Copy the Prim32 repository into your project, then link its namespaced target:

```cmake
add_subdirectory(third_party/Prim32)
target_link_libraries(your_app PRIVATE Prim32::Prim32)
```

Public headers are available from the target automatically:

```cpp
#include <prim32/prim32.h>
#include <prim32/prim32_dx12.h>
#include <prim32/p32prof.h>
```

No source paths or Windows libraries need to be repeated in the parent project.

### Typical Windows libraries

```text
d3d12.lib
dxgi.lib
d3dcompiler.lib
gdi32.lib
user32.lib
ole32.lib
windowscodecs.lib
psapi.lib
```

PDH is loaded dynamically by the profiler.

## Basic integration

Prim32 does not create your device, command queue, swap chain, render target, or command list.

Your application owns the D3D12 frame.

### Initialization

```cpp
#include <prim32/prim32.h>
#include <prim32/prim32_dx12.h>

prim32::FontDesc fontDesc = {
    L"Segoe UI",
    17.0f,
    true
};

prim32::Context* context = prim32::CreateContext(&fontDesc);

prim32_dx12::InitDesc backendDesc = {};
backendDesc.device = device;
backendDesc.queue = commandQueue;
backendDesc.rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
backendDesc.framesInFlight = 3;
backendDesc.maxPrims = 1u << 20;

if (!prim32_dx12::Init(context, backendDesc))
{
    return false;
}
```

### Per-frame flow

```cpp
prim32::FrameMem frameMem = prim32_dx12::NewFrame();

prim32::IO io = {};
io.displaySize = {
    static_cast<float>(width),
    static_cast<float>(height)
};
io.mousePos = mousePosition;
io.mouseDown[0] = leftMouseDown;
io.mouseWheel = mouseWheel;
io.deltaTime = deltaTime;

prim32::NewFrame(frameMem, io);

if (prim32::Begin("Control Panel"))
{
    prim32::Text("Prim32 is running");

    static bool enabled = true;
    prim32::Checkbox("Enabled", &enabled);

    static float strength = 0.5f;
    prim32::SliderFloat("Strength", &strength, 0.0f, 1.0f);

    if (prim32::Button("Apply"))
    {
        ApplySettings();
    }
}

prim32::End();

prim32::DrawData* drawData = prim32::EndFrame();
prim32_dx12::Render(drawData, commandList);
```

After submitting the command list:

```cpp
ID3D12CommandList* lists[] = {
    commandList
};

commandQueue->ExecuteCommandLists(1, lists);
prim32_dx12::NotifyExecuted();
```

`NotifyExecuted()` fences the active backend frame slot and advances the upload ring.

## DrawList API

The draw-list interface provides direct 2D rendering without requiring a widget or window.

```cpp
prim32::DrawList* draw = prim32::GetDrawList();
```

### Rectangles

```cpp
draw->FilledRect(
    prim32::Rect{ 20.0f, 20.0f, 240.0f, 80.0f },
    prim32::COL32(35, 38, 48, 255),
    8.0f
);

draw->Rect(
    prim32::Rect{ 20.0f, 20.0f, 240.0f, 80.0f },
    prim32::COL32(90, 110, 150, 255),
    1.0f,
    8.0f
);
```

### Lines and circles

```cpp
draw->Line(
    prim32::Vec2{ 40.0f, 140.0f },
    prim32::Vec2{ 280.0f, 140.0f },
    prim32::COL32(120, 180, 255, 255),
    2.0f
);

draw->FilledCircle(
    prim32::Vec2{ 160.0f, 220.0f },
    24.0f,
    prim32::COL32(120, 220, 160, 255)
);

draw->Circle(
    prim32::Vec2{ 160.0f, 220.0f },
    30.0f,
    prim32::COL32(255, 255, 255, 180),
    2.0f
);
```

### Shadows

```cpp
draw->Shadow(
    prim32::Rect{ 20.0f, 300.0f, 280.0f, 120.0f },
    prim32::COL32(0, 0, 0, 150),
    12.0f,
    20.0f
);
```

## Images

Images can be loaded from disk or encoded memory.

Supported formats are handled through Windows Imaging Component.

Typical supported formats include:

- PNG
- JPEG
- BMP
- GIF
- TIFF

### Load from disk

```cpp
prim32::ImageHandle logo =
    prim32::LoadImageFromFile("assets/logo.png");
```

### Load from memory

```cpp
prim32::ImageHandle icon =
    prim32::LoadImageFromMemory(iconBytes, iconByteCount);
```

The input memory is only needed for the duration of the load call. Prim32 decodes and uploads the image before returning.

### Draw an image

```cpp
prim32::DrawList* draw = prim32::GetDrawList();

draw->Image(
    logo,
    prim32::Rect{ 20.0f, 20.0f, 128.0f, 128.0f }
);
```

### Tint and alpha

```cpp
draw->Image(
    logo,
    prim32::Rect{ 160.0f, 20.0f, 128.0f, 128.0f },
    prim32::COL32(255, 255, 255, 128)
);
```

### UV cropping

```cpp
draw->Image(
    logo,
    prim32::Rect{ 300.0f, 20.0f, 128.0f, 128.0f },
    prim32::Vec2{ 0.0f, 0.0f },
    prim32::Vec2{ 0.5f, 0.5f }
);
```

### Natural image size

```cpp
draw->Image(
    logo,
    prim32::Vec2{ 20.0f, 180.0f }
);
```

### Destroy an image

```cpp
prim32::DestroyImage(logo);
```

Image handles are generational. A stale handle will not resolve to a newly allocated image that happens to reuse the same registry slot.

## Fonts

Fonts can be loaded from disk or memory.

The current implementation supports TTF, OTF, and TTC font data.

Text is UTF-8 with a dynamic glyph cache: glyphs of any script (CJK, Cyrillic, icon fonts in the Private Use Area, and so on) rasterize on first use, pack into atlas pages, and upload incrementally. Nothing is pre-baked.

Two rasterizers are available:

- GDI (default, zero dependencies) covers the entire Basic Multilingual Plane
- FreeType (optional: `PRIM32_WITH_FREETYPE` in CMake, or define `PRIM32_HAS_FREETYPE` and link `freetype`) covers every Unicode plane

Icon fonts are ordinary fonts: load one (for example Segoe MDL2 Assets) and draw PUA codepoints, using `prim32::EncodeUtf8(0xE713, buf)` to build the UTF-8 string.

### Load from disk

```cpp
prim32::FontHandle bodyFont =
    prim32::LoadFontFromFile(
        "assets/Inter-Regular.ttf",
        16.0f
    );
```

### Load from memory

```cpp
prim32::FontHandle titleFont =
    prim32::LoadFontFromMemory(
        titleFontBytes,
        titleFontByteCount,
        24.0f
    );
```

### Set the default font

```cpp
prim32::SetDefaultFont(bodyFont);
```

### Draw with the default font

```cpp
draw->Text(
    "Default font",
    prim32::Vec2{ 20.0f, 20.0f }
);
```

### Draw with an explicit font

```cpp
draw->Text(
    titleFont,
    "Explicit title font",
    prim32::Vec2{ 20.0f, 50.0f },
    prim32::COL32(255, 220, 120, 255)
);
```

### Font stack

```cpp
prim32::PushFont(titleFont);

prim32::Text("This widget text uses the title font");
prim32::Button("Title font button");

prim32::PopFont();
```

Font resolution order is:

1. Explicit font argument
2. Font stack top
3. Default font
4. Built-in context font

Invalid or destroyed fonts fall back safely instead of leaving the renderer in an invalid state.

## Text layout

Prim32 supports measurement, alignment, wrapping, line spacing, and clipping.

### Measure text

```cpp
prim32::Vec2 size =
    prim32::MeasureText("Measure me");
```

### Bounded text

```cpp
prim32::TextOptions options = {};
options.align = prim32::ALIGN_CENTER;
options.valign = prim32::VALIGN_MIDDLE;
options.wrapWidth = 280.0f;
options.lineSpacing = 1.1f;
options.clip = true;

draw->Text(
    "Wrapped and centered text inside a fixed rectangle.",
    prim32::Rect{ 20.0f, 20.0f, 300.0f, 100.0f },
    options
);
```

Text measurement and rendering share the same wrapping and font-resolution path.

## Widgets

The current widget set includes:

- Windows
- Text
- Formatted text
- Colored text
- Buttons
- Invisible buttons
- Checkboxes
- Float sliders
- Integer sliders
- Progress bars
- Separators
- Same-line layout
- Spacing
- Dummy layout items
- Item hover state
- Item active state
- ID stacks
- Font stacks

Example:

```cpp
if (prim32::Begin("Settings"))
{
    static bool enabled = true;
    static float scale = 1.0f;
    static int quality = 3;

    prim32::Checkbox("Enabled", &enabled);
    prim32::SliderFloat("Scale", &scale, 0.5f, 2.0f);
    prim32::SliderInt("Quality", &quality, 1, 5);

    if (prim32::Button("Reset"))
    {
        enabled = true;
        scale = 1.0f;
        quality = 3;
    }
}

prim32::End();
```

## Windows and ordering

Windows support:

- Dragging
- Z ordering
- Automatic sizing
- Title bars
- Background suppression
- Topmost overlays
- Optional bring-to-front behavior
- Per-window clipping

```cpp
prim32::SetNextWindowPos(
    prim32::Vec2{ 24.0f, 24.0f }
);

prim32::SetNextWindowSize(
    prim32::Vec2{ 420.0f, 280.0f }
);

if (prim32::Begin(
    "Inspector",
    prim32::WF_None
))
{
    prim32::Text("Window contents");
}

prim32::End();
```

### Window flags

```cpp
prim32::WF_NoTitleBar
prim32::WF_NoMove
prim32::WF_NoBackground
prim32::WF_AutoSize
prim32::WF_Topmost
prim32::WF_NoBringToFront
```

## Layers

Draw order is divided into three sections:

1. Background layer
2. Windows sorted by Z order
3. Foreground layer

### Foreground overlay

```cpp
prim32::BeginLayer(prim32::LAYER_FOREGROUND);

prim32::DrawList* draw = prim32::GetDrawList();

draw->Circle(
    mousePosition,
    12.0f,
    prim32::COL32(255, 220, 120, 180),
    2.0f
);

prim32::EndLayer();
```

Foreground primitives are rendered above every normal window.

## Cached layers

Cached layers provide a retained path for expensive static content.

A cached layer is recorded once into CPU staging memory, uploaded into a default-heap GPU buffer, and reused until its version changes.

```cpp
prim32::CachedLayer* minimapLayer =
    prim32::CreateCachedLayer(
        100000,
        "minimap"
    );

prim32::BeginCache(minimapLayer);

draw->FilledRect(
    prim32::Rect{ 0.0f, 0.0f, 512.0f, 512.0f },
    prim32::COL32(20, 24, 30, 255)
);

for (const MapMarker& marker : markers)
{
    draw->FilledCircle(
        marker.position,
        marker.radius,
        marker.color
    );
}

prim32::EndCache();
```

Draw it every frame:

```cpp
prim32::DrawCached(
    minimapLayer,
    prim32::Vec2{ 40.0f, 40.0f }
);
```

Moving a cached layer only changes a root constant. The cached primitives are not rebuilt or re-streamed.

Re-record the layer when its contents change:

```cpp
prim32::BeginCache(minimapLayer);

// Rebuild content

prim32::EndCache();
```

Cached layers reduce:

- CPU primitive emission
- Upload bandwidth
- Per-frame mapped-memory writes

Cached layers do not reduce:

- Pixel shading
- Overdraw
- Rasterization cost
- Blend cost

Caching helps most when the application is CPU emit-bound or upload-bandwidth-bound.

## Texture table

The default backend uses a shader-visible SRV heap and dynamically indexes textures from the pixel shader.

Images and font atlases can share the same render pass.

External textures can also be registered:

```cpp
uint32_t textureSlot =
    prim32_dx12::RegisterTexture(
        textureResource,
        DXGI_FORMAT_R8G8B8A8_UNORM
    );
```

The returned slot can be used with the lower-level image primitive API.

The default shader table currently supports 64 texture slots.

## Frame memory

Each frame-in-flight slot contains:

- A clip table region
- A primitive stream region
- A fence value

The backend waits only when it is about to reuse a frame slot that the GPU has not finished reading.

```cpp
prim32::FrameMem frameMem =
    prim32_dx12::NewFrame();
```

The default primitive capacity is one million primitives per frame.

At 32 bytes per primitive, one million primitives require approximately 32 MB per frame slot.

Increase `maxPrims` when needed:

```cpp
backendDesc.maxPrims =
    2u * 1024u * 1024u;
```

Check for overflow:

```cpp
prim32::Stats stats =
    prim32::GetStats();

if (stats.overflow)
{
    // Raise maxPrims.
}
```

## Profiling

Prim32 includes `p32prof`, a profiler designed to inspect the entire UI path.

### CPU scopes

```cpp
P32PROF_SCOPE("Build inspector");
```

Scopes form a hierarchical tree based on nesting.

The profiler tracks:

- Total milliseconds
- Self milliseconds
- Calls
- Percentage of frame
- Rolling average
- Rolling minimum
- Rolling maximum

Call the frame marker once at the end of each frame:

```cpp
p32prof::FrameMark();
```

### GPU zones

The D3D12 backend automatically records the main UI pass and draw ranges.

Custom zones can be inserted into the same query stream:

```cpp
prim32_dx12::GpuZoneBegin(
    commandList,
    "Custom overlay pass"
);

// Record commands

prim32_dx12::GpuZoneEnd(commandList);
```

GPU timing results are delayed by the number of frames in flight.

### Process metrics

Call:

```cpp
p32prof::SampleProcess();
```

The sampler internally limits expensive process queries to approximately two samples per second.

Tracked data includes:

- Process CPU usage
- CPU usage relative to one core
- Thread count
- Handle count
- Working set
- Peak working set
- Private committed memory
- Page faults per second
- GPU 3D usage
- GPU copy usage
- GPU compute usage
- GPU video usage
- Local VRAM usage
- Local VRAM budget
- Shared GPU memory usage
- Shared GPU memory budget

### Memory inventory

Register known allocations:

```cpp
p32prof::TrackMem(
    "gpu: upload ring",
    uploadRingBytes
);
```

The profiler displays tracked totals and the difference between tracked memory and process private memory.

### Profiler UI

```cpp
static bool profilerOpen = true;

p32prof::ShowProfiler(
    context,
    &profilerOpen,
    "optional build or scene information"
);
```

### Clipboard dump

```cpp
p32prof::CopyToClipboard(
    "build: release"
);
```

### CSV logging

```cpp
p32prof::SetCsvLog(true);
```

The profiler writes one row per second to:

```text
prim32_profile.csv
```

## Performance notes

The included benchmark supports stress tests for:

- Solid rectangles
- Rounded rectangles
- Circles
- Glyphs
- Widget-like primitive clusters
- Large blended quads
- Shadows
- Cached primitive layers

The current implementation has demonstrated approximately:

- 275 million scalar primitive emissions per second per CPU core
- 850 million primitive emissions per second in the specialized SSE2 grid path

These numbers are benchmark-specific and depend on:

- CPU
- Compiler
- Optimization settings
- Memory behavior
- Primitive pattern
- Operating system
- GPU
- Resolution
- Windowed or fullscreen presentation
- VSync
- Overdraw

Use the built-in profiler to identify the active bottleneck.

### Common bottlenecks

#### CPU emission bound

Symptoms:

- `BuildUI` or a custom emit scope dominates CPU time
- GPU time is low
- Frame-fence wait time is low

Possible fixes:

- Cache static content
- Batch repetitive primitive generation
- Use SIMD bulk emitters
- Reduce formatted text work
- Avoid rebuilding unchanged content

#### Upload-bandwidth bound

Symptoms:

- Very large dynamic primitive counts
- High streamed MB per frame
- Low pixel cost
- CPU writes and PCIe traffic dominate

Possible fixes:

- Cache retained content
- Reduce dynamic primitive size
- Lower update frequency for static panels
- Split static and dynamic layers

#### GPU raster or fill bound

Symptoms:

- UI GPU zone approaches total frame time
- Large alpha-blended primitives
- Heavy overlap
- High resolution
- Shadows or overdraw modes dominate

Possible fixes:

- Reduce overlapping translucent geometry
- Reduce shadow softness and screen area
- Reduce large rounded or blended surfaces
- Lower render resolution
- Avoid drawing hidden content

Caching cannot remove rasterization or blend cost.

#### Presentation bound

Symptoms:

- Low CPU and GPU cost
- Present or compositor pacing dominates
- Windowed uncapped frame rate stops increasing

Possible fixes:

- Test borderless fullscreen
- Test tearing support
- Disable VSync for benchmarking
- Measure actual application workloads instead of empty frames

## Threading model

Prim32 follows a one-context-per-thread model.

The current default implementation assumes:

- One active context
- One thread building a context at a time
- Resources loaded outside the frame hot path
- D3D12 command recording controlled by the host application

Multiple independent contexts can be added later, but global resource registry behavior must be updated before treating the current implementation as fully multi-context.

## Error handling

Resource functions return invalid handles on failure.

```cpp
prim32::ImageHandle image =
    prim32::LoadImageFromFile(
        "assets/missing.png"
    );

if (!prim32::IsValid(image))
{
    OutputDebugStringA(
        prim32::GetLastResourceError()
    );
}
```

Invalid handles are safe:

- Debug builds may draw a visible placeholder and log an error
- Release builds may skip the draw
- Stale generations do not resolve to new resources

## Current limitations

The current implementation is intentionally focused.

Known limitations include:

- Windows-only backend
- D3D12-only renderer
- GDI rasterizer limited to the Basic Multilingual Plane (FreeType covers all planes)
- No font-fallback chains (missing glyphs render a notdef box)
- No HarfBuzz shaping
- No bidirectional text
- No complex-script shaping
- No text input widget
- No docking
- No table widget
- No built-in menu system
- No built-in tree widget
- No built-in combo box
- No built-in color picker
- No arbitrary triangle primitive
- Fixed texture-table size
- Fixed primitive capacity per frame
- Single-context resource registry assumptions
- Runtime shader compilation through FXC
- Windows GDI font baking
- Windows WIC image decoding

The absence of arbitrary triangles is deliberate. Prim32 is optimized around fixed quad-expanded primitives. General triangle rendering should normally remain in the host renderer.

## Planned work

Potential future improvements include:

- HarfBuzz text shaping
- Font-fallback chains
- Ellipsis and truncation
- Text input
- Multi-line editing
- Tables
- Docking
- Menus
- Tree controls
- Combo boxes
- Color editors
- Resizable windows
- Multi-viewport support
- Multiple independent contexts
- Descriptor indexing with larger tables
- Precompiled DXIL shaders
- Optional DirectWrite font backend
- Optional Vulkan backend
- Optional D3D11 backend
- Automated benchmark result export
- More cached-layer invalidation strategies

## Design principles

### Keep the hot path obvious

Primitive emission should remain:

- Allocation-free
- Branch-light
- Sequential
- Easy to inspect in assembly
- Easy to profile
- Independent of resource decoding

### Keep loading outside drawing

Drawing should never:

- Decode an image
- Open a file
- Rasterize a font
- Create a texture
- Allocate a container
- Wait for an upload

Resources are created once and represented by small handles during rendering.

### Keep D3D12 ownership in the host

Prim32 should not force an application architecture.

The host remains responsible for:

- Device creation
- Queue creation
- Swap chain creation
- Render targets
- Command allocators
- Command lists
- Back-buffer transitions
- Presentation
- Main frame synchronization

### Measure everything

Optimizations should be based on:

- CPU scope time
- GPU timestamp time
- Primitive count
- Draw-range count
- Bytes streamed
- VRAM behavior
- Allocation inventory
- Actual bottleneck classification

## Example project layout

```text
project/
    CMakeLists.txt

    assets/
        Inter-Regular.ttf
        Inter-Bold.ttf
        logo.png

    third_party/
        Prim32/
            CMakeLists.txt
            include/
                prim32/
                    prim32.h
                    prim32_dx12.h
                    p32prof.h
            src/
                prim32.cpp
                prim32_internal.h
                prim32_resources.cpp
                backends/
                profiler/

    src/
        main.cpp
        renderer.cpp
        renderer.h
        ui.cpp
        ui.h
```

## Naming

The name Prim32 describes the core rendering unit:

- `Prim` refers to the GPU primitive stream
- `32` refers to the fixed 32-byte primitive format

Public namespaces:

```cpp
namespace prim32 {}
namespace prim32_dx12 {}
namespace p32prof {}
```

Public headers:

```cpp
#include <prim32/prim32.h>
#include <prim32/prim32_dx12.h>
#include <prim32/p32prof.h>
```

## Status

Prim32 is currently an experimental performance-focused framework.

Its architecture is functional and already includes:

- Immediate-mode windows and widgets
- A raw draw-list API
- D3D12 rendering
- Images and multiple fonts
- Cached retained layers
- GPU timestamp profiling
- CPU profiling
- Process and memory diagnostics
- A stress-test application

Public API stability is not yet guaranteed.

## Contributing

Contributions should preserve the core performance model.

Before submitting a major feature, consider:

1. Does it add work to every primitive?
2. Does it add allocations to the steady-state frame?
3. Does it force a pipeline change?
4. Does it break direct mapped-memory emission?
5. Can it be implemented as a new fixed primitive type?
6. Can it remain outside the hot path?
7. Can its cost be measured in the profiler?

Performance changes should include before-and-after measurements with:

- Compiler and flags
- CPU
- GPU
- Resolution
- Primitive count
- Primitive type
- CPU build time
- GPU pass time
- Streamed bytes per frame

## License

Prim32 is licensed under the Apache License 2.0.

You may use Prim32 in open-source, commercial, and closed-source projects, subject to the terms of the license. See [LICENSE](LICENSE) for the complete license text.
