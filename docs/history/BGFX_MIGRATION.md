# bgfx Migration Plan

Migrating GanymedEngine's renderer from the current OpenGL-only backend to
[bgfx](https://github.com/bkaradzic/bgfx), gaining Direct3D 11/12, Vulkan, Metal,
and OpenGL backends behind a single codebase.

**Strategy:** bgfx replaces the `Platform/OpenGL` backend *and* most of the virtual
abstraction layer in `GanymedE/Renderer`. The public class names (`Texture2D`,
`Framebuffer`, `Shader`, …) stay so that `SceneRenderer`, `Renderer2D/3D`,
`Material`, `Mesh`, and the editor barely change — but they become thin concrete
wrappers over bgfx handles instead of virtual interfaces with per-API subclasses.

**Order of work is chosen so the engine compiles and runs at the end of every
phase.** The riskiest items (shader port, uniform model, picking readback) are
isolated into their own phases.

---

## 1. Current state inventory

| Area | Today | Fate under bgfx |
|---|---|---|
| Context | `OpenGLContext` (glad + `glfwMakeContextCurrent`/`SwapBuffers`) | Deleted → `bgfx::init` / `bgfx::frame` |
| Loader | Glad (vendored project) | Deleted |
| Window | GLFW (kept) | Kept; window hint changes to `GLFW_NO_API` |
| Abstraction | `RendererAPI` + `RenderCommand` virtual dispatch | Collapses into direct bgfx calls / state flags |
| Geometry | `VertexBuffer`/`IndexBuffer`/`VertexArray` + `BufferLayout` | VAO concept removed; layout → `bgfx::VertexLayout` |
| Shaders | 17 single-file `.glsl` (Hazel `#type` sections), compiled at runtime | Rewritten as `.sc` pairs, compiled offline by `shaderc` |
| Uniforms | UBOs (`UniformBuffer`, std140 binding points) + `glUniform*` | bgfx uniforms (vec4/mat4 only) — **no UBO equivalent**, see §5 |
| Render targets | `Framebuffer` (RGBA16F HDR, R32I entity IDs, depth) | `bgfx::FrameBufferHandle` + views |
| Picking | Synchronous `glReadPixels` (`Framebuffer::ReadPixel`) | Async `bgfx::blit` + `readTexture` (1–2 frame latency), see §7 |
| Post stack | `SceneRenderer`: HDR → bloom chain → tonemap → FXAA → composite | Same passes, each pass = a bgfx view ID |
| IBL bake | `OpenGLEnvironment` (equirect → cubemap, irradiance, prefilter, BRDF LUT) | Same fragment-shader bakes into cubemap faces via views |
| Editor UI | `ImGui_ImplOpenGL3` + `ImGui_ImplGlfw` | bgfx ImGui renderer backend, see §8 |
| Build | premake5, static libs | + bx/bimg/bgfx libs, + shaderc build step |

Shaders to port (GanymedEditor/assets/shaders):
`BRDFLut, BloomDownsample, BloomUpsample, Equirect, FXAA, FlatColor, Grid,
Irradiance, Line, Phong, Prefilter, ShadowDepth, Skybox, SkyboxCube, Texture,
Tonemap, VertexPosColor` — 17 programs.

---

## 2. Phase 0 — Vendor & build bgfx ✅ DONE

**Goal:** bgfx/bx/bimg compile and link into the engine; tools (`shaderc`) built.

> **Status: complete** (branch `bgfx`). Debug/Release/Dist all build; Sandbox logs
> `bgfx sanity check: initialised, renderer = Noop` then shuts down clean.
>
> Deviations from the plan below, and why:
>
> - **Option (A) taken** — `GanymedEngine/extern/bgfx.lua` defines `bx`, `bimg`,
>   `bgfx` as premake5 static libs off each project's amalgamated TU.
> - **`bimg_decode` is not built.** Upstream now drags in dav1d + libavif (AVIF)
>   for it. The engine loads images through stb_image, and bgfx core only needs
>   `bimg` proper, so the whole 3rdparty decode tree is skipped. Revisit only if
>   we adopt KTX/DDS via bimg (§6.1). This retires the "bimg 3rdparty build
>   friction" risk.
> - **`image_gnf.cpp` no longer exists** in current bimg; `bimg` is `image.cpp`
>   plus astc-encoder.
> - **bx requires C++20.** The three vendored projects build at C++20 while the
>   engine stays C++17 — safe because `<bgfx/bgfx.h>` includes no bx headers.
> - **MSVC needs `/Zc:preprocessor`** (bx `platform.h` hard-errors without it)
>   and `exceptionhandling "Off"` (bx `thread.cpp` uses `__try`, which MSVC
>   rejects under object unwinding). We deliberately do *not* define
>   `_HAS_EXCEPTIONS=0`, which would change STL layout across the lib boundary.
> - **shaderc is built by script, not checked in.** `*.exe` is gitignored
>   repo-wide, and the tool pulls in glslang/spirv-tools/spirv-cross/tint —
>   too large a graph to re-describe in premake. Run
>   `scripts/build_shader_tools.bat` (or `.sh`) once per machine; it drives
>   bgfx's own GENie build and stages `scripts/tools/<os>/shaderc`.
>   Verified producing valid bytecode for `s_5_0`, `spirv`, and `410`.
> - **Sandbox's `ExampleLayer` was deleted.** It was already dead (only
>   `Sandbox2D` is pushed) and had not compiled since the ECS refactors removed
>   `Renderer::BeginScene/Submit/EndScene` — unrelated to bgfx, but it blocked
>   the "solution builds" exit criterion.
>
> Temporary code to remove in Phase 1: `BgfxSanityCheck()` in
> `Sandbox/source/SandboxApp.cpp`.

1. Add submodules/vendored copies under `GanymedEngine/extern/`:
   - `bx` (base library — required)
   - `bimg` (image library — required)
   - `bgfx`
2. Build integration. Two options:
   - **(A) Recommended:** write `bgfx.lua` premake scripts (like the existing
     `GLFW.lua`/`Jolt.lua`) that compile the sources directly:
     - `bx`: `bx/src/amalgamated.cpp`
     - `bimg`: `bimg/src/image.cpp`, `image_gnf.cpp` + `bimg_decode`
       (`image_decode.cpp`) — needs `bimg/3rdparty` (astc-encoder, tinyexr, …)
     - `bgfx`: `bgfx/src/amalgamated.cpp` (use `amalgamated.mm` on macOS)
     - Defines: `BX_CONFIG_DEBUG=1` in Debug; `__STDC_FORMAT_MACROS`;
       include `bx/include/compat/msvc` on Windows.
   - (B) Build once with bgfx's own GENie makefiles and link the produced
     static libs. Less premake work, worse for CI/other machines.
3. Build the **tools** with GENie (`shaderc`, `texturec`, `geometryc`) and check
   the binaries into `scripts/tools/` (or document how to build them). Only
   `shaderc` is strictly required.
4. Sanity check: temporary code in `Sandbox` that calls
   `bgfx::init()` headless (`bgfx::RendererType::Noop`) and `bgfx::shutdown()`.

**Exit criteria:** solution builds with bgfx linked; shaderc.exe runs.

---

## 3. Phase 1 — Init, frame loop, and window plumbing ✅ DONE

**Goal:** bgfx owns the swapchain; the app runs and clears the backbuffer.

> **Status: complete** (branch `bgfx`). Auto-selected backend on this machine is
> **Direct3D 11**. Verified by capturing the backbuffer with
> `bgfx::requestScreenShot` and inspecting pixels, not by eyeballing:
>
> | Check | Result |
> |---|---|
> | Clear | 1600×900, **100%** of 1,440,000 px `#1a1a2e` |
> | Resize | capture returns **1024×768**, still 100% cleared → `bgfx::reset` works |
> | `F1` stats | same frame gains `#555753` panel (16.3%) + `#34e2e2` text (1.7%) |
> | Shutdown | window close → exit code **0** |
>
> Logged caps worth remembering for later phases:
> `homogeneousDepth = false`, `originBottomLeft = false` (D3D conventions) —
> these are exactly the two knobs §9.3 says to check per backend.
>
> Notes on the implementation:
>
> - **`<bgfx/platform.h>` no longer exists.** `PlatformData` and `renderFrame()`
>   were folded into `bgfx.h` upstream; §3.2's snippet is otherwise accurate.
> - **New `Platform/Bgfx/BgfxContext`** owns init/frame/reset/vsync. It is
>   deliberately *not* a `GraphicsContext` subclass — with one backend the
>   virtual indirection buys nothing, so `GraphicsContext` and `OpenGLContext`
>   now both die together in Phase 7.
> - **`RenderPassIDs.h` was pulled forward from Phase 4** (currently just
>   `RenderPass::Backbuffer = 0`) to avoid seeding magic view numbers.
> - **Resize is applied in `Window::OnUpdate`, not the GLFW callback**, so
>   `bgfx::reset` lands on a frame boundary.
> - **Shutdown order matters**: `m_Context.reset()` must precede
>   `glfwDestroyWindow`, since bgfx holds the native window handle.
>
> **Dormancy scaffolding** (all removed across Phases 2-6, tracked by
> `Renderer::IsLegacyGLPathDormant()`):
>
> - `Renderer::Init/Shutdown/OnWindowResize` skip the GL subsystems.
> - `Application` skips layer `OnAttach`, `OnUpdate`, `OnImGuiRender`, and event
>   propagation to unattached layers.
> - Every `Create` factory gained a `case API::Bgfx: return nullptr;`. This is
>   not just belt-and-braces: `ContentBrowserPanel`'s *constructor* builds
>   `Texture2D`s and runs before any gate could fire. Those switches are also
>   the exact shape Phases 2-4 fill in with real bgfx implementations.
> - New `Layer::IsAttached()`: `~LayerStack` used to call `OnDetach()`
>   unconditionally, which tore down an ImGui context that was never created.
>   Worth keeping past the migration.
>
> **The editor and Sandbox render nothing but the clear colour right now.** That
> is the expected Phase 1 state; the picture comes back over Phases 3-6.

Files: `WindowsWindow.cpp` (and Linux/macOS twins), `GraphicsContext.*`,
`Renderer.cpp`, `Application` run loop.

1. `glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API)` — bgfx manages the API context
   itself (including its GL context when the GL backend is selected).
2. Replace `OpenGLContext` with a `BgfxContext` (or fold into `Window`):
   ```cpp
   bgfx::renderFrame();               // call BEFORE init → single-threaded mode
   bgfx::Init init;
   init.platformData.nwh  = glfwGetWin32Window(window);   // per-OS
   init.platformData.ndt  = nullptr;                      // X11 display on Linux
   init.resolution.width  = w;
   init.resolution.height = h;
   init.resolution.reset  = BGFX_RESET_VSYNC;
   init.type = bgfx::RendererType::Count; // auto-pick; make configurable later
   bgfx::init(init);
   ```
   Start **single-threaded** (the `renderFrame()`-before-`init` trick). Moving to
   the multithreaded model is a later, optional optimization.
3. `WindowsWindow::OnUpdate`: replace `m_Context->SwapBuffers()` with
   `bgfx::frame()`. Window resize → `bgfx::reset(w, h, flags)`.
4. `Renderer::Init/Shutdown` → `bgfx` capability logging, `bgfx::shutdown()`.
5. Keep `RendererAPI::API` enum but add `Bgfx` and make it the only path; the
   enum becomes a config knob for `init.type` (Vulkan/D3D11/D3D12/OpenGL/Metal)
   rather than a dispatch mechanism.
6. VSync toggle → `bgfx::reset` flags instead of `glfwSwapInterval`.

**Exit criteria:** app opens, clears to a color via
`bgfx::setViewClear(0, ...)` + `bgfx::touch(0)`, resizes cleanly, `F1` debug
overlay (`bgfx::setDebug(BGFX_DEBUG_STATS)`) works. Rendering and ImGui are
expected to be broken/disabled during this phase — do it on a branch.

---

## 4. Phase 2 — Geometry resources ✅ DONE

**Goal:** `Mesh`, `Renderer2D`, `Renderer3D` submit geometry through bgfx.

> **Status: complete** (branch `bgfx`). All three configs build; both apps run
> and exit 0.
>
> Draw *submission* cannot execute yet — `bgfx::submit` needs a program, and
> programs arrive in Phase 3 — so `RenderCommand` skips draws while its program
> handle is invalid. What *is* testable without shaders was tested: a temporary
> harness ran **29 assertions** against real bgfx on D3D11, all passing.
>
> | Verified | Result |
> |---|---|
> | Renderer2D quad layout stride | 48, matching the CPU `QuadVertex` |
> | Mesh layout stride | 44 == `sizeof(MeshVertex)` — no padding drift |
> | Attribute offsets | Position@0, Color0@12, TexCoord0@28/1@36/2@40/3@44 |
> | Semantic mapping | TexIndex/TilingFactor/EntityID land in TexCoord1/2/3 |
> | Instanced elements | excluded from the vertex layout (stride 12, not 12+68) |
> | Buffers | static VB, dynamic VB, `SetData`, 32-bit IB all valid on D3D11 |
> | `RenderState` packing | depth off, LESS default, cull Back→CW, BLEND_ADD, PT_LINES |
>
> Shape of the result:
>
> - **`VertexArray` is gone.** Replaced by a `Geometry { Ref<VertexBuffer>,
>   Ref<IndexBuffer> }` struct — the pairing was the only part worth keeping.
> - **`BufferLayout::ToBgfx()`** does the translation, driven by `AttribFromName`
>   — one table, asserting on unmapped names, per the risk register.
> - **`RenderState`** packs depth/cull/blend/write/topology into the `uint64_t`
>   for `bgfx::setState`. `RenderCommand` keeps its old setter names but now
>   mutates one of these and folds it into each submit, so call sites barely moved.
> - **`RendererAPI` is now just the backend enum** — the virtual dispatch layer is
>   deleted.
> - **Instancing** no longer uses a divisor-1 vertex buffer. `Mesh` holds instance
>   data CPU-side and each draw copies it into a bgfx transient instance buffer.
>
> **Files deleted** (Phase 7 work that removing `VertexArray` forced early —
> each took `Ref<VertexArray>` and could no longer compile): `VertexArray.*`,
> `OpenGLVertexArray.*`, `OpenGLBuffer.*`, `OpenGLRendererAPI.*`,
> `OpenGLEnvironment.*`. Only `OpenGLContext`, `OpenGLFramebuffer`,
> `OpenGLShader`, `OpenGLTexture`, `OpenGLUniformBuffer` remain.
>
> ### ⚠ Carried into Phase 3
>
> **bgfx has no 32-bit integer vertex attribute** (only Uint8, Uint10, Int16,
> Half, Float). `ShaderDataType::Int` therefore maps to `AttribType::Float`.
> Exact up to 2^24, which is fine for entity IDs — **but the CPU-side data must
> be written as float, and it currently is not**: `QuadVertex::EntityID` is still
> `int`, so its bits would be read as garbage floats. Fix it together with the
> `Texture`/`FlatColor` shaders in Phase 3 step 1, and check
> `Renderer2D::DrawQuad`'s entity-ID writes at the same time.

1. **`BufferLayout` → `bgfx::VertexLayout`.** bgfx attributes are *semantic*
   (`Position, Normal, Tangent, Bitangent, Color0..3, Indices, Weight,
   TexCoord0..7`), not arbitrary names. Map each `BufferElement::Name` used in
   the engine ("a_Position", "a_Normal", "a_TexCoord", "a_EntityID"…) to a
   semantic; free-form data (entity IDs, material indices) rides in spare
   `TexCoordN` slots. Add the translation in `Buffer.h` so call sites keep
   their `{ ShaderDataType::Float3, "a_Position" }` syntax.
2. **`VertexBuffer`/`IndexBuffer`** become concrete classes over
   `bgfx::VertexBufferHandle` / `DynamicVertexBufferHandle` (the
   `Create(uint32_t size)` + `SetData` path maps to a dynamic buffer;
   the `Create(data, size)` path to a static one). 32-bit indices need
   `BGFX_BUFFER_INDEX32`. `Bind()/Unbind()` disappear from the interface —
   binding happens at submit time.
3. **Delete `VertexArray`** (and `OpenGLVertexArray`). Replace its role — the
   VB+IB pairing — with a small `GeometryHandle` struct or direct members in
   `Mesh`. This touches every draw call site; do it mechanically.
4. **`RenderCommand::DrawIndexed*` → submit calls:**
   ```cpp
   bgfx::setVertexBuffer(0, vb);
   bgfx::setIndexBuffer(ib, firstIndex, indexCount);
   bgfx::setState(state);            // depth/cull/blend flags, see below
   bgfx::submit(viewId, program);
   ```
   The `SetDepthTest/SetCullMode/SetBlendMode` calls on `RendererAPI` become a
   `uint64_t` state builder (`BGFX_STATE_WRITE_RGB | BGFX_STATE_DEPTH_TEST_LESS
   | BGFX_STATE_CULL_CW | …`). Note: bgfx uses **CW/CCW** culling, not
   front/back — winding depends on the backend-agnostic convention, verify
   against the meshes once visible.
5. **Renderer2D batching** → `bgfx::TransientVertexBuffer` /
   `TransientIndexBuffer` (allocated per frame, no orphaning logic needed).
6. **Instancing** (`DrawIndexedInstanced`, the `Instanced` flag in
   `BufferElement`) → `bgfx::InstanceDataBuffer`; there is no attribute-divisor
   concept. Instance data is fetched in the shader via `i_data0..4`.

**Exit criteria:** compiles; geometry submits (nothing visible yet without
shaders — combine with Phase 3 for the first pixels).

---

## 5. Phase 3 — Shader port (the big one) ✅ 12 of 17 DONE

**Goal:** all 17 programs rewritten in bgfx `.sc`, compiled offline, loaded at
runtime.

> **Status: 12 of 17 programs ported; the scene renders again.** All configs
> build, both apps exit 0, **zero bgfx asserts**.
>
> ### ✅ The viewport is live
>
> | Measure | Before the port | After |
> |---|---|---|
> | `numDraw` | 24 (ImGui only) | **42** |
> | Distinct colours | 579 | **733** |
> | ImGui chrome share | 85% | **48%** — the viewport now has content |
>
> A vertical strip through the viewport is the procedural sky gradient:
> `#c4d3e0` at the top descending smoothly to `#aba69a` at the horizon (~y=50%),
> blue falling 0xe0→0x9a while red barely moves — exactly
> `mix(groundColor, skyColor, smoothstep(dir.y))`.
>
> **Ported:** FlatColor, VertexPosColor, Blit, ImGui, Texture, Line, Grid,
> Skybox, ShadowDepth, Phong, BloomDownsample, BloomUpsample, Tonemap, FXAA.
>
> **Not ported (5):** `Equirect`, `Irradiance`, `Prefilter`, `BRDFLut`,
> `SkyboxCube` — all IBL, and all blocked on `Environment` still being
> unported, so there would be nothing to drive them. `SkyboxCube` is the one
> that logs a load failure at startup; the procedural `Skybox` covers for it.
>
> ### bgfx/HLSL constraints worth knowing (each cost a compile cycle)
>
> - **Varyings are `main()`'s parameters.** A helper function cannot reference
>   `v_worldpos`; `ShadowFactor` had to take it as an argument.
> - **`vecN(x)` does not broadcast in HLSL.** GLSL's `vec3(0.0)` must become
>   `vec3_splat(0.0)`. `vec2_splat` does not exist — use `vec2(x, x)`.
> - **No `matrix * vector` operator** — `TBN * n` becomes `mul(TBN, n)`, and
>   `mat3(a,b,c)` becomes `mtxFromCols` so column order matches on all backends.
> - **`line` is a reserved word** in HLSL.
> - **Instance data stride must be a multiple of 16.** `MeshInstanceData` was
>   `mat4 + int` (68 bytes); the entity ID now occupies a full `vec4`, which
>   also matches how bgfx delivers instance data (`i_data4`) as vec4s.
> - **A sampler is its own uniform type.** Leftover `SetInt("u_IrradianceMap",
>   slot)` calls — correct under GL — are a hard type-mismatch assert here.
> - **Scalars and ints arrive as vec4**, so every `u_Foo` scalar is read as
>   `u_Foo.x` in the ported shaders.
>
> ### Entity IDs are R32F, not R32I
>
> Changed from the Phase 4 decision. bgfx fragment shaders emit `float4` only,
> so writing IDs into an *integer* render target has no well-defined conversion
> even where R32I is renderable. Float32 is exact to 2^24 and keeps the whole
> chain consistent: the `a_EntityID` vertex attribute, the shader output, and
> the readback.
>
> ### ✅ Done — and verified with actual pixels
>
> A temporary harness drew two quads and the captured frame was exactly right:
>
> | Sample | Colour | Proves |
> |---|---|---|
> | Left quad | `#00ff00` | colour from the **vertex stream** (VertexPosColor) |
> | Right quad | `#ff0000` | colour from a **vec4 uniform** (FlatColor `u_Color`) |
> | Each quad's area | 20.00% | matches its NDC footprint exactly — no transform drift |
> | Background | `#1a1a2e` | untouched clear |
>
> That one frame exercises the entire new path: shaderc → `.bin` → profile
> selection → `createProgram` → `BufferLayout`→`VertexLayout` semantics →
> `setViewTransform`/`setTransform` → `u_modelViewProj` → uniform → submit.
>
> - **`assets/shaders/src/`** is now the single shader source tree (shared by
>   both apps, replacing the duplicated `.glsl` copies), with one
>   `varying.def.sc` whose semantics mirror `AttribFromName` in `Buffer.h`.
> - **`scripts/compile_shaders.bat`** compiles every `.sc` for `dx11`, `spirv`
>   and `glsl` into each app's `assets/shaders/compiled/<profile>/`. 24 binaries
>   currently. **Shaders no longer rebuild themselves — run this after edits.**
>
>   The compiled `.bin` output is **gitignored** (it is derived from the `.sc`
>   sources), so a fresh clone needs two one-time steps before the app has any
>   shaders:
>
>   ```
>   scripts\build_shader_tools.bat   # builds shaderc (~minutes, once per machine)
>   scripts\compile_shaders.bat      # compiles assets/shaders/src -> compiled/
>   ```
>
>   Wiring `compile_shaders` into premake `prebuildcommands` would automate the
>   second step; not done yet, since it would fail the build outright on a
>   machine without shaderc.
> - **`Shader` is concrete over `bgfx::ProgramHandle`**, picking the blob for the
>   live backend. `Bind()` no longer touches the GPU; it records the program for
>   the next submit. Scalars pad into vec4s. `ShaderLibrary` and every
>   `Shader::Create("assets/shaders/Foo.glsl")` call site are unchanged — the path
>   is reduced to its stem.
> - `OpenGLShader` deleted. Remaining GL files: `OpenGLContext`,
>   `OpenGLFramebuffer`, `OpenGLTexture`, `OpenGLUniformBuffer`.
>
> ### ⚠ Phases 3 and 4 are entangled — the plan's split does not hold
>
> §5.3 step 1 lists `Texture` alongside `FlatColor`/`VertexPosColor`, but it
> cannot land here: binding a sampler is `bgfx::setTexture(slot, uniform,
> handle)`, which needs `Texture2D` to already own a `bgfx::TextureHandle` —
> that is **Phase 4** work. The same is true further down: `Grid`/`Line` need
> `Renderer3D` awake, and the post stack needs `Framebuffer` ported.
>
> Recommended resequencing: **do Phase 4's `Texture2D` and `Framebuffer` next**,
> then return and sweep §5.3 steps 1–5 in order with each one verifiable on
> screen, as originally intended.
>
> ### Remaining in Phase 3
>
> - 15 programs: `Texture`, `Line`, `Grid`, `Phong`, `ShadowDepth`, `Equirect`,
>   `Irradiance`, `Prefilter`, `BRDFLut`, `Skybox`, `SkyboxCube`,
>   `BloomDownsample`, `BloomUpsample`, `Tonemap`, `FXAA`.
> - ~~**The UBO rework (§5.2) is untouched.**~~ ✅ **DONE** — see below.
>
> ### ✅ §5.2 UBO rework — done, and the scene path is live again
>
> `UniformBuffer` and `OpenGLUniformBuffer` are **deleted**. `Platform/OpenGL` is
> now down to `OpenGLContext` alone. `Renderer::IsSceneRenderPathDormant()`
> returns **false**: layers attach, update, and submit. Verified with
> `numDraw = 24` and **zero bgfx asserts**, closing cleanly with exit 0.
>
> Replaced by:
> - **`FrameUniforms`** — a small central owner, because *bgfx uniforms are
>   global by name*; frame data does not need to route through a `Shader`.
> - **Camera matrices need no uniforms at all.** `bgfx::setViewTransform` feeds
>   the predefined `u_view`/`u_proj`/`u_viewProj`/`u_modelViewProj`, so only the
>   camera *position* is uploaded by hand.
> - **The light block maps one-to-one.** `LightsUBO` was already vec4-shaped, so
>   `GPULight[32]` uploads as a flat `vec4[128]` with no repacking.
> - `Shader::SetFloat4Array` / `SetMat4Array` added. Note bgfx cannot address
>   `u_Name[i]` as a name — the cascade matrices go up as one `mat4[4]` and the
>   four cascade splits as a single `vec4`.
>
> ### ⚠ §5.2's "set once per frame/view" is wrong — uniforms are per-DRAW
>
> The plan says to set these blocks "once per frame/view". bgfx does not work
> that way: `setUniform` contributes to the **next submit** and is cleared by it.
> Setting the same uniform twice before a submit is a hard assert
> (*"Uniform N was already set for this draw call"*), which is precisely what
> happened — twice, on `u_CameraPosition` and then `u_Texture`.
>
> Two consequences, both now handled:
> 1. `FrameUniforms` only *records* values; `Apply()` replays them, and
>    `RenderCommand` calls it immediately before **every** submit.
> 2. **A skipped draw must `bgfx::discard()`.** Pending uniforms, textures and
>    buffers are draw-call state that only a submit consumes; bailing out without
>    discarding leaks them into the next draw and trips the same assert. This
>    matters permanently, not just while shaders are missing.
>
> ### ⚠ Bug found: static renderer data outliving bgfx
>
> Closing the app died with an access violation (0xC0000005).
> `Renderer2D::Shutdown` only freed a CPU array — it never released
> `QuadGeometry`, `TextureShader`, `WhiteTexture` or the 16 texture slots. Those
> live in a file-scope `static`, so they destructed *after* `main`, long after
> `bgfx::shutdown()`. Invisible while the renderer was dormant (everything was
> null); fatal the moment it initialised. `Renderer3D` and `PostProcess` already
> released theirs correctly.
> - **Entity ID as float** (carried from Phase 2): `QuadVertex::EntityID` is
>   still `int` while the attribute is declared float. Fix with the `Texture`
>   shader.
>
> ### ✅ Decided: `Texture`'s 16 samplers → 16 discrete samplers
>
> bgfx has no `sampler2D[16]`. Three options were weighed; **16 individually
> declared samplers** (`SAMPLER2D(s_tex0, 0)` … `s_tex15`) keeping the existing
> switch won, because the alternatives change what `Renderer2D` can do:
>
> - `BGFX_CONFIG_MAX_TEXTURE_SAMPLERS` is **16** — exactly the count the engine
>   already uses (both derive from GL 4.1's guaranteed minimum), so the budget
>   fits precisely.
> - **A texture atlas is ruled out by tiling.** `Renderer2D::DrawQuad` takes a
>   `tilingFactor` and `OpenGLTexture` sets `GL_REPEAT`; an atlas sub-region
>   cannot repeat without per-sprite `fract()` math and padding, and it still
>   breaks under mipmapping.
> - **A texture array is ruled out by dimensions** — array layers must share size
>   and format, but Renderer2D accepts arbitrary user textures.
>
> An atlas remains the right *optimisation* for larger 2D batches later, as its
> own change, with tiling handled deliberately.
>
> ### ✅ Verified: winding is correct, and a latent depth bug was found and fixed
>
> One capture through a real `glm::perspective`, run twice with only the glm
> depth convention changed:
>
> | Marker | GL default `[-1,1]` | `GLM_FORCE_DEPTH_ZERO_TO_ONE` |
> |---|---|---|
> | RED — `CullMode::Back` → `CULL_CW` | VISIBLE 2.569% | VISIBLE 2.569% |
> | BLUE — `CullMode::Front` → `CULL_CCW` | absent | absent |
> | GREEN — probe 0.15 units from camera | **absent** | **VISIBLE 4.550%** |
>
> - **Winding: `RenderState`'s mapping is right.** The engine's CCW-front
>   geometry survives back-face culling under `BGFX_STATE_CULL_CW`, and the
>   result is independent of the depth convention.
> - **Depth: glm was mis-configured for this backend.** Caps report
>   `homogeneousDepth = false`, but nothing defined
>   `GLM_FORCE_DEPTH_ZERO_TO_ONE`, so everything nearer than
>   ~`2*near*far/(far+near)` (≈0.2 with near=0.1/far=100) was being clipped away
>   — plus half the depth buffer's precision wasted. Now defined at **workspace
>   scope** in `premake5.lua`: glm is header-only, so a project disagreeing would
>   silently change shared glm type layout across the static-lib boundary.
> - Because that fix is compile-time, `BgfxContext::Init` now **logs an error if
>   the live backend reports `homogeneousDepth`** — otherwise a GL backend would
>   render subtly wrong rather than fail. A caps-driven projection helper (§9.3)
>   is still the proper multi-backend answer.

### 5.1 Mechanics

- Each `.glsl` splits into `vs_<name>.sc`, `fs_<name>.sc`, plus a shared
  `varying.def.sc` describing all varyings/attributes with semantics.
- Language deltas from the current GLSL 410:
  - `$input` / `$output` declarations instead of `in`/`out`;
    `#include <bgfx_shader.sh>` first.
  - `SAMPLER2D(s_tex, 0);` / `SAMPLERCUBE(...)` macros — the number is the
    texture unit, fixed at author time.
  - `mul(mat, vec)` instead of `mat * vec`; `mtxFromCols` etc. for construction.
  - **All uniforms are `vec4`/`mat4` (or arrays thereof).** Scalars/ints/bools
    must be packed into vec4 components. No UBOs / interface blocks.
  - Predefined uniforms exist: `u_model`, `u_view`, `u_proj`, `u_viewProj`,
    `u_modelViewProj`, `u_viewRect`, … — set via `bgfx::setTransform` and view
    transforms, which removes most of the current per-draw matrix uniforms.
- Compile with shaderc per backend profile:
  `s_5_0` (D3D11/12), `spirv` (Vulkan), `440`/`410` (GL), `metal`.
  Output layout: `assets/shaders/compiled/<profile>/<name>.bin`.
- Add a build step: a `scripts/compile_shaders.(bat|sh)` invoked as a premake
  `prebuildcommands` (or a small custom rule) so shaders rebuild with the
  project. Keep the `.sc` sources in `assets/shaders/src/`.

### 5.2 Engine-side changes

- `Shader::Create(filepath)` → loads the two `.bin` blobs for
  `bgfx::getRendererType()`, `bgfx::createShader` ×2 → `bgfx::createProgram`.
  `ShaderLibrary` unchanged.
- `Shader::SetInt/SetFloat/SetMat4/...` — bgfx uniforms are created by name and
  set globally before submit (`bgfx::setUniform`). Keep the current `Shader`
  setter API but back it with a name → `bgfx::UniformHandle` cache, packing
  scalars into vec4s. `SetInt` for samplers becomes `bgfx::setTexture(slot,
  samplerUniform, handle)` — this replaces every `texture->Bind(slot)` call.
- **UBO replacement.** The `UniformBuffer` class (camera matrices, lights, etc.
  at std140 binding points) has no bgfx equivalent. Replace each UBO with:
  - view/proj matrices → bgfx view transform (`bgfx::setViewTransform`), used
    via predefined uniforms; and
  - remaining blocks (light arrays, material params) → `vec4[]` array uniforms
    set once per frame/view. Mirror the current struct layout in a
    C++-side `glm::vec4` array so the data flow stays centralized.
  Delete `UniformBuffer`/`OpenGLUniformBuffer` when done.

### 5.3 Porting order (each is testable on screen)

| Step | Shaders | Unlocks |
|---|---|---|
| 1 | `FlatColor`, `VertexPosColor`, `Texture` | Renderer2D quads/sprites, first visible pixels |
| 2 | `Line`, `Grid` | Editor grid + debug lines |
| 3 | `Phong`, `ShadowDepth` | Lit 3D scene + shadow pass |
| 4 | `Equirect`, `Irradiance`, `Prefilter`, `BRDFLut`, `Skybox`, `SkyboxCube` | Environment/IBL pipeline |
| 5 | `BloomDownsample`, `BloomUpsample`, `Tonemap`, `FXAA` | Full post stack |

Note: shader **hot-reload** changes character — editing a shader now requires a
shaderc run. Optional later: file-watch + spawn shaderc + recreate program.

---

## 6. Phase 4 — Textures & framebuffers 🚧 MOSTLY DONE

**Goal:** `Texture2D`, `Environment`, `Framebuffer` on bgfx handles.

> **Status: `Texture2D`, `Framebuffer` and the view model done; `Environment`
> (IBL) not started.** All configs build.
>
> ### ✅ Verified — a real render-to-texture round trip
>
> One frame: fill an offscreen framebuffer, then sample it back as a texture
> alongside a PNG loaded from disk. Coverage matches geometry exactly:
>
> | Region | Measured | Expected |
> |---|---|---|
> | Left (framebuffer sampled back) | red 6.68% + green 6.68% + blue 13.40% = **26.76%** | 27% NDC area ✓ |
> | Right (PNG from disk) | 13.53% + 13.47% = **27.00%** | 27% ✓ |
> | Background | 46.00% | 46.24% ✓ |
>
> Red and green counts are *identical* (96 212 px each), so there is no sampling
> drift. This exercises RT creation with the real SceneRenderer attachment set
> (RGBA16F + R32I + D24S8), view targeting, attachment-as-sampler,
> `Shader::SetTexture`, and the stb→bgfx upload.
>
> - **R32I is a valid render target on D3D11** — the fallback probe stayed quiet,
>   so the entity-ID attachment keeps full integer precision here. The
>   `R32F` fallback in `ResolveFormat` is still there for backends that refuse it,
>   which retires that risk-register row for this backend only.
> - **`Texture2D` is concrete**; the abstract `Texture` base is gone (nothing used
>   it). Sampler state moved off the texture object into `BGFX_SAMPLER_*` flags
>   passed at bind time.
> - **`Shader::SetTexture(sampler, slot, texture)` replaces `texture->Bind(slot)`**
>   everywhere. A texture can no longer bind itself — the binding names the
>   sampler uniform it feeds, which only the shader knows.
> - **Views replaced bind/unbind.** `Framebuffer::BindToView(viewId)`;
>   `Unbind()` has no equivalent and is gone. `RenderPassIDs.h` now carries the
>   full §6.3 map — shadow cascades, scene, bloom chain (one view per mip in each
>   direction), tonemap, FXAA, composite, IBL bake, ImGui.
> - **`ClearAttachment` is gone**: bgfx clears every attachment of a view at once,
>   so clearing entity IDs to −1 rides along with the view clear.
> - `OpenGLTexture` and `OpenGLFramebuffer` deleted. Remaining GL files:
>   `OpenGLContext`, `OpenGLUniformBuffer`, `OpenGLEnvironment`.
>
> ### Remaining in Phase 4
>
> - **`Environment` / IBL is untouched** — `OpenGLEnvironment` still does the
>   equirect→cubemap, irradiance, prefilter and BRDF-LUT bakes in raw GL. It
>   needs cubemap render targets via `bgfx::Attachment` and the
>   `RenderPass::EnvironmentBake` views.
> - **Picking is disabled, not ported.** `SceneRenderer::ReadEntityID` returns −1
>   rather than a stale value; `Framebuffer::RequestPixelRead` already implements
>   the blit + `readTexture` half, so Phase 5 only needs the pending-pick queue.
> - ~~**Texture orientation is unverified.**~~ ✅ **Verified.** A 1×2 texture with
>   row 0 red / row 1 blue, drawn with v=0 along the top edge, put **red on top**
>   (rows 225-268 vs blue at 547-674, the quad spanning exactly NDC −0.5…+0.5).
>   `Texture2D` genuinely loads in bgfx's top-left origin, so default
>   `(0,0)-(1,1)` UVs are correct. This surfaced as a real bug — see below.
> - The exit criterion ("editor viewport shows the full lit scene") still depends
>   on the 15 remaining shaders.

1. **`Texture2D`** → `bgfx::createTexture2D` (+ `updateTexture2D` for
   `SetData`). stb_image loading stays; alternatively adopt `bimg` for KTX/DDS
   and drop stb later. `GetRendererID()` (used for ImGui images) → return the
   `bgfx::TextureHandle` (see §8). Sampler state (filter/wrap) moves from
   texture object state to `BGFX_SAMPLER_*` flags passed at `setTexture` time.
2. **`Framebuffer`** → create attachment textures with `BGFX_TEXTURE_RT`
   (`RGBA16F`, `R32I` for entity IDs — check `bgfx::isTextureValid` for R32I as
   a render target on each backend; fall back to `R32F` if needed), then
   `bgfx::createFrameBuffer(count, handles)`. `Resize` = destroy + recreate
   (same as today's GL path). MSAA via `BGFX_TEXTURE_RT_MSAA_X{2,4,8,16}`.
3. **Views replace Bind/Unbind.** `Framebuffer::Bind()` becomes
   `bgfx::setViewFrameBuffer(viewId, fbh)` + `setViewRect` + `setViewClear`.
   Assign a **static view ID map** for the frame (bgfx sorts by view ID):
   ```
   0  shadow map
   1  scene HDR (opaque + skybox)
   2  scene transparent/2D overlay
   3..3+N  bloom mip chain (downsample+upsample per mip)
   ~14 tonemap
   15 fxaa/composite
   16 IBL bake views (transient, on environment load)
   200 ImGui (editor, renders to backbuffer)
   ```
   Encode this in one header (`RenderPassIDs.h`) instead of magic numbers.
   `SceneRenderer` owns the assignment — its Begin/End structure already
   matches the view model well.
4. **`Environment` (IBL bake)**: the equirect→cubemap / irradiance / prefilter
   passes render into cubemap faces — `bgfx::createFrameBuffer` accepts a
   cubemap layer+mip via `bgfx::Attachment`. The bakes run once on load across
   a handful of transient views, then the views are released. `BindIBL` becomes
   three `setTexture` calls.

**Exit criteria:** editor viewport shows the full lit scene with post stack,
on the GL backend *and* at least one modern backend (D3D11 is usually the
easiest second target on Windows).

---

## 7. Phase 5 — Entity picking (async readback) ✅ DONE

**Goal:** mouse picking works despite bgfx's asynchronous readback model.

> **Status: complete.** All configs build.
>
> ### ✅ Verified
>
> A harness cleared an R32I attachment to entity ID 12345, blitted, and polled:
>
> ```
> attachment 1 is integer format: true
> requested at frame 5, bgfx says ready at frame 8 (latency 3)
> landed at frame 8: read 12345, expected 12345 -> PASS
> ```
>
> - **Measured latency is 3 frames, not the 2 assumed in §7.2.** Harmless for
>   hover highlighting, but it is why the pending queue holds 4 slots rather
>   than 2 — at 2 it would have stalled every frame.
> - **The clear had to change form.** §7 does not mention this: the ordinary
>   `setViewClear(viewId, flags, rgba, ...)` applies one packed 8-bit-per-channel
>   word to *every* attachment, which cannot express "entity ID = -1". The
>   **clear-palette** form gives each attachment its own float4 that bgfx
>   converts per format, so R32I genuinely receives -1. This corrects the Phase 4
>   note claiming the ID clear simply rides along with the colour clear.
>
> ### Shape of the change
>
> - `SceneRenderer::ReadEntityID(x, y)` → `RequestEntityID(x, y)` +
>   `PollEntityID(int&)`. Only the newest landed result is reported and older
>   in-flight slots are retired, so a stale pixel can never overwrite a newer one.
> - Requests are **dropped, not queued**, when all four slots are busy: the next
>   frame issues another, whereas a growing queue would only add latency.
> - `Renderer::GetFrameNumber()` (fed by `BgfxContext` from `bgfx::frame()`)
>   is what the poll gates on.
> - The editor picks on *hover*, every frame, so the request/poll pair costs
>   nothing extra there.
>
> ### ⚠ Bug found and fixed while porting
>
> `EditorLayer` unconditionally flipped the pick Y coordinate with
> `my = viewportSize.y - my; // framebuffer origin is bottom-left`. That is an
> OpenGL assumption — D3D/Vulkan/Metal address render targets from the **top**
> left, and caps report `originBottomLeft = false`. The flip is now conditional
> on `bgfx::getCaps()->originBottomLeft`. Left alone this would have made
> picking vertically mirrored on every modern backend, and it is the same
> origin question §8.3 raises for the viewport image.
>
> **Not yet exercised against a real scene**: nothing writes entity IDs into
> attachment 1 until the `Texture`/`Phong` shaders land, so the pipeline is
> verified but the IDs themselves are still synthetic.

Today `SceneRenderer::ReadEntityID` does a synchronous `glReadPixels`. bgfx has
no synchronous readback:

1. Create a 1×1 (or small) `BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK`
   staging texture.
2. On click: `bgfx::blit(viewId, staging, 0,0, entityIdTex, mouseX, mouseY, 1,1)`
   then `bgfx::readTexture(staging, &result)` → returns the **frame number** at
   which `result` is valid (typically current+2).
3. `SceneRenderer` keeps a tiny pending-pick queue; `EditorLayer` consumes the
   result 1–2 frames later. For editor clicking this latency is imperceptible,
   but the call site changes from `int id = ReadEntityID(x,y)` to
   request/poll — this is the one **API-shape change that leaks above the
   renderer**, so adjust `EditorLayer` accordingly.

Alternative if preferred: CPU-side ray-casting against Jolt/AABBs for picking,
dropping the ID attachment entirely. Decide when we get here.

---

## 8. Phase 6 — ImGui editor backend 🚧 BACKEND DONE

**Goal:** editor UI renders through bgfx.

> **Status: the ImGui backend is written and rendering. The exit criterion
> ("full editor usable") is blocked on §5.2, not on anything in this phase.**
>
> ### ✅ Verified — the editor chrome draws through bgfx
>
> Captured frame of the running editor:
>
> | Measure | Value | Meaning |
> |---|---|---|
> | Distinct colours | **579** | a real UI (earlier phases' tests had 2-6) |
> | Dominant colour | `#1a1b1c` at 85% | exactly `ImGuiCol_WindowBg` from `SetDarkThemeColors` (0.1, 0.105, 0.11) |
> | Clear colour remaining | **0.00%** | the dockspace covers the window |
>
> - **`Platform/Bgfx/ImGuiRendererBgfx`** written rather than vendored: draw
>   lists go into transient vertex/index buffers, one submit per `ImDrawCmd`
>   with scissor, all on `RenderPass::ImGui` (view 200) in `Sequential` mode so
>   ImGui's own ordering is preserved.
> - **`ImGui_ImplGlfw` is kept**, but switched from `InitForOpenGL` to
>   **`InitForOther`** — the window is `GLFW_NO_API`, so there is no GL context
>   for it to assume.
> - `vs_ImGui`/`fs_ImGui` needed **their own `varying.ImGui.def.sc`**: `ImDrawVert`
>   is fixed at vec2 position + vec2 uv + packed uint8 colour, which does not
>   match the engine's vec3-position layout. `compile_shaders` now prefers
>   `varying.<name>.def.sc` when present.
> - The ortho matrix is built by hand from `getCaps()->homogeneousDepth` rather
>   than glm, so the UI stays correct on a backend that disagrees about clip
>   depth — the compile-time `GLM_FORCE_DEPTH_ZERO_TO_ONE` cannot adapt.
> - **Multi-viewport is disabled** (§8.4 defers it). It needs one bgfx
>   framebuffer per OS window; leaving the flag on would spawn platform windows
>   that never draw.
>
> ### Lifting dormancy revealed the real blocker (since resolved)
>
> Turning the gate off segfaulted on the first frame: `Renderer3D` called
> `SetData` on a **null** `CameraUniformBuffer`. **The UBO rework of §5.2 was a
> hard prerequisite for the editor working**, which the plan does not call out —
> §8 reads as though the ImGui backend alone restores the editor.
>
> That rework is now done (see §5.2 above) and
> `Renderer::IsSceneRenderPathDormant()` returns false, so the layer stack is
> fully live.
>
> ### Bug found and fixed
>
> `ImGuiLayer` called `AddFontFromFileTTF` unchecked, which asserts and kills the
> process on a missing file. Sandbox ships no `assets/fonts`, so it aborted at
> startup the moment ImGui was attached. Font loading now checks first and falls
> back to ImGui's built-in font.
>
> ### Remaining in Phase 6
>
> - **Viewport image** (§8.3) is still `GetFinalImageRendererID()` returning a
>   raw index. It cannot be exercised until the scene renders, and the UV flip
>   against `originBottomLeft` is exactly the bug already found in picking.
> - Multi-viewport support.

1. Replace `ImGui_ImplOpenGL3` with a bgfx renderer backend. Options:
   - Port `bgfx/examples/common/imgui/` (upstream, maintained, includes the
     `.sc` shaders precompiled for all backends), or
   - a community `imgui_impl_bgfx.cpp` (small, drop-in alongside
     `ImGui_ImplGlfw`).
   Keep `ImGui_ImplGlfw` for input/platform — only the render half changes.
2. `ImGuiLayer::End()` → build draw data into a dedicated high view ID (e.g.
   200) targeting the backbuffer.
3. **Viewport image**: `ImGui::Image((ImTextureID)handle.idx, ...)` with the
   composite framebuffer's texture handle
   (`bgfx::getTexture(compositeFB, 0)`). `GetFinalImageRendererID()` changes
   type from GL uint to `bgfx::TextureHandle` — wrap in a typedef.
   Watch UV flip: **GL renders render-targets upside-down vs D3D/Vulkan**;
   use `bgfx::getCaps()->originBottomLeft` to pick UVs so the viewport is
   correct on every backend.
4. Multi-viewport ImGui (if enabled later) needs one bgfx framebuffer per
   OS window — defer.

**Exit criteria:** full editor usable on bgfx GL + D3D11.

---

## 8.5 Post-integration fixes

Three defects found by running the editor after the §5.2 rework. All fixed.

### Icons rendered upside down

Every `ImGui::Image`/`ImageButton` call site hard-coded `{0,1}-{1,0}` UVs — a
compensation for the **old GL loader's vertical flip**, which no longer happens.
Two different fixes, because the two cases are not the same thing:

- **Plain textures** (toolbar icons, content-browser thumbnails) → default
  `(0,0)-(1,1)`. `Texture2D` already loads top-left origin (verified above).
- **The viewport image** is a *render target*, whose orientation follows the
  backend → UVs now chosen from `getCaps()->originBottomLeft`, as §8.3 requires.
  Hard-coding either way is wrong on half the backends.

### Crash on exit — statics outliving bgfx

```
bgfx::Context::destroyUniform  <-  Shader::~Shader
  <-  'dynamic atexit destructor for GetMeshShader::s_Shader'
```

Two function-local `static Ref<Shader>` caches (`MeshCache.cpp`,
`MeshImporter.cpp`) destruct at `atexit`, long after `bgfx::shutdown()`.

This was the **third** instance of the same class of bug (after `Renderer2D`'s
static block and the layer-teardown ordering), so it is fixed systemically
rather than site by site: **`Renderer::IsGpuAlive()`**, lowered by `BgfxContext`
*before* `bgfx::shutdown()`, and checked by every resource destructor
(`Shader`, `Texture2D`, `Framebuffer`, `VertexBuffer`, `IndexBuffer`).
After shutdown bgfx has already freed its own memory, so skipping `destroy` is
correct as well as safe. C++ gives no ordering guarantee between statics and
`main`, so no amount of call-site discipline would have closed this off.

### Empty viewport

**Not a bug in the plumbing — the scene has no shaders to draw with.** The
composite target is filled by the tonemap/FXAA passes, whose programs are among
the 15 still unported, so nothing is submitted to it.

One real defect did hide underneath: **a view that receives no draw calls is
skipped entirely, and its clear with it**, so the scene and post targets kept
stale contents rather than clearing. `SceneRenderer` now `bgfx::touch()`es the
scene, tonemap and FXAA views, the same way `BgfxContext` does the backbuffer.
The viewport is now a defined black instead of undefined, but **the picture only
returns when the shaders land**.

---

## 8.6 Post-shader-port fixes

Three defects found by loading a real scene and closing the editor.

### Meshes invisible — the view id was left on the shadow cascade

`RenderShadowPass` sets the current view per cascade and never restored it, so
every opaque/transparent/grid draw after it was submitted into the **shadow
framebuffer** instead of the scene target. The draws were happening (`numDraw`
rose correctly) and nothing asserted — the geometry simply went somewhere else.

Under GL this class of bug could not exist: "where does this draw go" was
whatever framebuffer was bound, and the shadow pass unbound it. Under bgfx the
target is the view id, which is *sticky state that survives the pass*.

### Draw order — views must be Sequential

bgfx reorders draws within a view by default to minimise state changes. This
renderer depends on submission order in two places: the skybox draws with depth
test off (it would paint over opaque geometry if reordered after it), and
transparents are sorted back-to-front on the CPU. The scene views are now
`bgfx::ViewMode::Sequential`.

### 2D overlay needs its own view

`Renderer2D::BeginScene` runs *after* `Renderer3D::EndScene` and both set the
camera. **A view transform is per-view-per-frame, not per-draw** — the last
`setViewTransform` before `bgfx::frame()` applies to every draw in that view, so
the 2D camera retroactively re-projected all the 3D geometry. Renderer2D now
owns `RenderPass::SceneTransparent`, which the view map already reserved.

This is the sharpest edge of the §5.2 rework: moving matrices out of per-draw
UBOs and into view transforms changes their *lifetime*, not just their location.

### Instanced geometry drawn with a garbage transform

**bgfx does not let you choose the instance-data semantics.** It binds the
instance data buffer to `TEXCOORD31` counting *down*, so `i_data0..4` must be
declared exactly `TEXCOORD31, 30, 29, 28, 27`
(see `bgfx/examples/05-instancing/varying.def.sc`).

`varying.def.sc` had them on `TEXCOORD4..7` + `COLOR1`, which additionally
collided with `v_worldpos : TEXCOORD7` and `v_shadowpos : TEXCOORD4`. The
instance matrix therefore read undefined data and every mesh was drawn with a
garbage transform - one cube filled the viewport as a huge distorted plane.

Nothing errored: shaderc compiled it, bgfx asserted nothing, and `numDraw` was
correct. Only the picture was wrong. Worth remembering that **wrong attribute
semantics fail silently** — the CPU-side data was verified correct
(unit cube, 24 verts, bounds ±0.5, clean rotation + translation) before the
shader was suspected, which is what localised it.

### Memory leak on exit — 32 blocks

`MeshCache.cpp` and `MeshImporter.cpp` each held a function-local
`static Ref<Shader>` of the same Phong program. Statics are destroyed after
`main()` returns, so `Renderer::IsGpuAlive` correctly stopped them touching a
dead bgfx — but that turned the crash into a leak: the handles were never
released at all.

Both are now one explicitly-owned `MeshShader` cache, released by
`Renderer::Shutdown` while bgfx is still alive. The guard remains as the safety
net; it is not a substitute for owning resources properly.

---

## 8.7 Environment / IBL — ported, with open visual questions

`Environment` is concrete over bgfx handles and all 17 shaders now compile
(228 binaries). The bake runs on load: equirect -> cubemap (all mips) ->
irradiance -> prefiltered specular -> BRDF LUT, targeting cubemap faces and mips
through `bgfx::Attachment`.

- **The whole bake runs in one frame.** That is only valid because bgfx executes
  views in ID order, so each stage samples what a lower-numbered view wrote. The
  bake view range widened from 8 to ~67 (6 faces x 5 mips, twice, plus the LUT).
- **bgfx cannot generate mipmaps for a render target**, so every env-cubemap mip
  is rendered from the panorama rather than downsampled. Prefilter samples these
  by roughness.
- **IBL binds per material.** There is no global "bind to texture unit 9" under
  bgfx - a sampler uniform belongs to a shader - so `BindIBL`/`BindSkybox` became
  handle accessors that `Renderer3D` feeds to `Shader::SetTexture`.
- **Equirect V flip.** `asin(v.y)` maps up to v=1.0, which was the image top
  under the old GL loader's vertical flip but is the *bottom* under bgfx's
  top-left origin. Same root cause as the flipped editor icons.

### Verified against the source HDR

Decoding `studio_small_08_1k.hdr` directly gives, per equirect band:

| Band | p50 | character |
|---|---|---|
| top third (up / ceiling) | 0.048 | dark |
| middle (walls) | 0.269 | contains the light sources (max 78) |
| bottom third (floor) | **0.705** | brightest, and nearly uniform (max 0.94) |

This retracts an earlier concern in this document. The rendered skybox reading as
a bright, flat wash is **what this HDR actually looks like** when the camera is
angled down at the floor - not evidence of a broken bake.

### Open: the ghost images on the "floor"

The test scene contains **only 40 unit boxes plus Sun and Sky Light - there is no
ground plane**, so the "floor" is the skybox and nothing there can receive a
shadow or a reflection. Ruled out so far:

- **Bloom** - an A/B capture differs by at most 10/255 in that region.
- **HDR content** - the source floor band is uniform (max 0.94, no bright blobs).

Still unexplained. Next thing to check would be dumping a baked cubemap face to
disk, which isolates the bake from everything downstream.

### Shadows cannot be judged from this scene

With no ground plane, the only possible shadowing is box-on-box (visible on the
stacked column). Assessing the cascades properly needs a scene with a receiver.

---

## 8.8 Known regressions vs `master`

Established by comparing the same scene on both branches.

### Shadows are inert (not wrong - absent)

Measured: toggling `u_UseShadows`, and removing `RenderShadowPass` entirely,
both produce a **byte-identical frame** (max diff 0). The face darkening on the
boxes is directional lighting, not shadowing.

The CPU side is correct - `HasShadowLight = true`, 40 casters, cascade splits
`(15.3, 32.2, 62.6, 200.0)` and a populated light-space matrix. The signature
(every lookup reading "unshadowed") is what an **empty shadow map** produces: a
depth target cleared to 1.0 with nothing rendered into it, so
`proj.z - bias > pcfDepth` is never true.

**Next place to look:** why `RenderShadowPass` submits nothing. Candidates worth
checking first - `ShadowDepth` draws being discarded, and the depth-only
framebuffer being written with `BGFX_STATE_WRITE_RGB|WRITE_A` set while it has
no colour attachment.

**Fixed along the way** (correct regardless, but not the blocker): `SampleCascade`
remapped all three components with `proj * 0.5 + 0.5`, the GL convention. Under
`GLM_FORCE_DEPTH_ZERO_TO_ONE` the light-space matrix already emits z in [0,1],
so z was being pushed into [0.5,1] and every depth comparison was wrong. Only
XY are remapped now, plus a Y flip for the top-down render-target origin (which
should become caps-driven with §9.3's helper).

### The mirrored-box artifact — fullscreen passes were flipping V

**Root cause: every fullscreen pass sampled its source vertically flipped.**

The fullscreen vertex shaders derived their UV straight from clip position
(`a_position.xy * 0.5 + 0.5`). bgfx addresses render targets **top-down** on
D3D/Vulkan/Metal but bottom-up on OpenGL, so on the top-down backends the
destination's top row sampled the source's bottom row.

It hid because **tonemap and FXAA are two such passes and their flips
cancelled**. Bloom has an *odd* pass count (N downsamples + N-1 upsamples), so
its result came out mirrored about the screen centre - the "reflections" under
the geometry.

Proof: disabling FXAA (properly - the tonemap target selection also has to be
redirected) rendered the whole scene **upside down**, geometry centroid moving
from ~40% to **62.1%**. After the fix, FXAA on and off produce identical
geometry placement, which is what "no pass flips" predicts.

Fixed in all six fullscreen vertex shaders (`Tonemap`, `FXAA`,
`BloomDownsample`, `BloomUpsample`, `BRDFLut`, `Blit`):

```
v_texcoord0 = a_position.xy * 0.5 + 0.5;
#if !BGFX_SHADER_LANGUAGE_GLSL
	v_texcoord0.y = 1.0 - v_texcoord0.y;
#endif
```

This also silently fixed the **BRDF LUT**, which was being written with its
roughness axis inverted relative to how Phong samples it.

**Also fixed on the way** (a real bug, but not this one): the bloom upsample loop
walked `i` downwards while assigning `view = BloomUpsample + i`, so the view IDs
*descended*. bgfx executes a frame's views in ascending ID order, so the chain
ran backwards and every pass sampled a mip not yet written that frame. The ID is
now derived from the step index.

That makes **three** view/origin-convention bugs in this migration (scene view
needing `Sequential`, the upsample ordering, and this). Under GL, submission
order was execution order and the framebuffer origin was uniform; under bgfx the
view ID *is* the schedule and the RT origin is backend-dependent. Both deserve
scrutiny at every new pass.

**Needs visual confirmation** - the artifact is invisible from the default
editor camera.

---

## 9. Phase 7 — Cleanup & multi-backend hardening 🚧 STEP 1 DONE

> **GL is fully removed from the build.** `Platform/OpenGL/` no longer exists,
> along with `GraphicsContext` (its only remaining subclass was `OpenGLContext`,
> and the two referenced only each other - a closed dead loop). The `Glad`
> vendored project is deleted and every premake reference with it, as is the
> `opengl32.lib` link: bgfx resolves the GL entry points itself when the GL
> backend is selected, so nothing links OpenGL statically.
>
> Verified: there are **zero `gl*` calls anywhere in the engine**, all three
> configs build, and both apps run on Direct3D 11 with 0 asserts, no leak and no
> log errors.
>
> **GLFW stays.** It is a windowing and input library, not a graphics one - bgfx
> does no windowing at all and needs the native handle GLFW provides. §1 already
> says as much ("Window | GLFW (kept)"); the only change was the `GLFW_NO_API`
> hint in Phase 1. Note the Linux/macOS link lists still name `GL` /
> `OpenGL.framework`: those are for **bgfx's** GL backend, not for engine code.
>
> Still open in this phase: backend selection on the command line (§9.2), the
> caps-driven projection helper for backends wanting `[-1,1]` depth (§9.3), and
> validation on D3D12 / Vulkan / OpenGL.



1. Delete: `Platform/OpenGL/*` (10 files), Glad project + premake references,
   `opengl32.lib` link, `RenderCommand`/`RendererAPI` virtual layer (or keep
   `RenderCommand` as a thin namespace over bgfx for call-site stability).
2. Backend selection: config/command line (`--renderer=vulkan|d3d12|d3d11|gl`)
   → `init.type`. Log `bgfx::getRendererName()`.
3. Test matrix on Windows: D3D11, D3D12, Vulkan, OpenGL. Known per-backend
   gotchas to verify:
   - Depth range / clip space (bgfx normalizes, but check
     `getCaps()->homogeneousDepth` interaction with glm projection matrices —
     currently glm defaults to GL's [-1,1]; use
     `bx::mtxProj`-style handling or `GLM_FORCE_DEPTH_ZERO_TO_ONE` per caps).
   - Render-target origin flip (see §8.3) in every fullscreen pass.
   - sRGB: current pipeline tonemaps manually — keep backbuffer linear
     (`BGFX_RESET_SRGB_BACKBUFFER` off) to match current look, verify.
   - R32I attachment support (picking) per backend.
4. Later/optional: multithreaded render (drop the `renderFrame()` trick),
   compute-shader IBL bakes, `texturec`-preprocessed KTX textures with mips,
   occlusion queries, bgfx debug text overlay in dev builds.

---

## 10. Risk register

| Risk | Impact | Mitigation |
|---|---|---|
| Uniform model mismatch (no UBOs, vec4-only) | Touches every shader + material path | Phase 5.2 plan: predefined uniforms for matrices, vec4[] mirrors for blocks; do Phong last in step 3 |
| Async picking readback | Editor UX + API shape change | Pending-pick queue; fallback to physics raycast |
| Semantic-only vertex attributes | Entity ID / custom attributes need TexCoordN slots | Central mapping table in `Buffer.h`, assert on unmapped names |
| Shader port regressions (17 programs) | Visual bugs | Port in the §5.3 order, compare screenshots against GL master per step |
| bimg 3rdparty build friction (MSVC) | Phase 0 stalls | Only `bimg_decode` needed initially; can stub encode |
| bgfx hides low-level features | Future ray tracing/mesh shaders unavailable | Accepted trade-off of choosing bgfx (revisit only if requirements change) |
| RT origin & depth-range differences across backends | Flipped/misclipped fullscreen passes | `getCaps()` checks centralized in one helper used by all post passes |

## 11. Suggested sequencing & effort

Rough effort at a comfortable pace, assuming familiarity with the codebase:

| Phase | Scope | Estimate |
|---|---|---|
| 0 | Vendor + build bgfx & tools | 0.5–1 day |
| 1 | Init/frame/window plumbing | 0.5 day |
| 2 | Geometry resources | 1–2 days |
| 3 | Shader port (17 programs + uniform rework) | 3–5 days |
| 4 | Textures, framebuffers, views, IBL | 2–3 days |
| 5 | Async picking | 0.5–1 day |
| 6 | ImGui backend | 0.5–1 day |
| 7 | Cleanup + D3D11/D3D12/Vulkan validation | 1–2 days |

**Total: roughly 2–3 weeks part-time.** Phases 1–3 leave the app partially
broken; do the whole migration on a `bgfx` branch and keep `master` on GL until
Phase 6 lands.
