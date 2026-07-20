# Rendering

The renderer runs entirely on [bgfx](https://github.com/bkaradzic/bgfx): one codebase, backend
auto-selected at startup (Direct3D 11/12, Vulkan, Metal, OpenGL). The engine-facing classes
(`Shader`, `Texture2D`, `Framebuffer`, …) are **concrete wrappers over bgfx handles** — the old
virtual per-API layer is gone. The migration that produced this design, including every bug found
and how it was verified, is recorded in [`BGFX_MIGRATION.md`](../toDo&done/BGFX_MIGRATION.md);
read it before touching anything subtle here.

## The bgfx mental model (read this first)

Four things differ fundamentally from OpenGL and shape the whole renderer:

1. **Views replace bind/unbind, and view ID order is execution order.** A framebuffer is attached
   to a *view* (`Framebuffer::BindToView(viewId)`); every draw submitted to that view lands in it.
   bgfx sorts the frame by view ID, so the pass schedule is the table in
   [`RenderPassIDs.h`](../../GanymedEngine/source/GanymedE/Renderer/RenderPassIDs.h) — never a
   magic number at a call site. The current view ID is *sticky state*
   (`RenderCommand::SetViewId`); forgetting to restore it after a pass sends subsequent draws into
   the wrong target silently (this exact bug made all meshes invisible once — migration §8.6).
   Views that depend on submission order (scene, transparent) are set to `Sequential` mode.
   A view that receives no draws is skipped *including its clear* — hence the `bgfx::touch()`
   calls on scene/tonemap/FXAA views and the backbuffer.
2. **Uniforms are per-draw, not per-frame.** `bgfx::setUniform` contributes to the next submit and
   is consumed by it; setting the same uniform twice before a submit is a hard assert.
   [`FrameUniforms`](../../GanymedEngine/source/GanymedE/Renderer/FrameUniforms.h) therefore only
   *records* shared frame data (lights, ambient, camera position) and `RenderCommand` replays it
   via `Apply()` immediately before **every** submit. A skipped draw must `bgfx::discard()` or its
   pending uniforms/textures leak into the next draw. Camera matrices are the exception: they ride
   on `bgfx::setViewTransform` (genuine per-view state) feeding the predefined
   `u_view/u_proj/u_viewProj/u_modelViewProj`.
3. **Render-target origin and clip depth are backend-dependent.** D3D/Vulkan/Metal address RTs
   top-down, GL bottom-up (`caps->originBottomLeft` — consulted for the viewport image UVs, the
   pick coordinate flip, and the fullscreen-pass V flip). The workspace defines
   `GLM_FORCE_DEPTH_ZERO_TO_ONE` (see the comment in `premake5.lua`); `BgfxContext` logs an error
   if the live backend disagrees (`homogeneousDepth`).
4. **Readback is asynchronous.** There is no `glReadPixels`; entity picking is a blit +
   `readTexture` returning the frame number at which the result is valid (measured latency ≈3
   frames). See [Picking](#entity-picking-async).

## Frame state & draw submission

- [`RenderState`](../../GanymedEngine/source/GanymedE/Renderer/RenderState.h) packs depth
  test/write/func, culling, blending, color writes and topology into the `uint64_t` for
  `bgfx::setState`. Note bgfx culls by **winding** (engine meshes are CCW-front, so "cull back" =
  `CULL_CW`).
- [`RenderCommand`](../../GanymedEngine/source/GanymedE/Renderer/RenderCommand.h) is a thin
  namespace-like class keeping the old call-site shape: state setters mutate one `RenderState`;
  `DrawIndexed`/`DrawIndexedInstanced`/`DrawLines` bind buffers, apply `FrameUniforms`, fold the
  state in, and `bgfx::submit` with the program recorded by `Shader::Bind()`.
- [`Buffer.h`](../../GanymedEngine/source/GanymedE/Renderer/Buffer.h): `BufferLayout` keeps the
  `{ ShaderDataType::Float3, "a_Position" }` authoring syntax and translates to
  `bgfx::VertexLayout` via **`AttribFromName`** — bgfx attributes are semantic slots, so free-form
  data rides in spare TexCoords (`a_TexIndex`→TexCoord1, `a_TilingFactor`→TexCoord2,
  `a_EntityID`→TexCoord3). This table must stay in sync with `varying.def.sc`. There is no 32-bit
  int attribute: integers travel as floats (exact to 2^24 — fine for entity IDs), and the CPU-side
  data must be written as float. `VertexBuffer` is static (data ctor) or dynamic (size ctor +
  `SetData`); `IndexBuffer` is 32-bit; `Geometry` is the VB+IB pair that replaced `VertexArray`.
- Instancing: no attribute divisor. Per-instance data (`MeshInstanceData` = mat4 + vec4 entity ID;
  stride must be a multiple of 16) is copied into bgfx's transient instance buffer at each draw and
  arrives in the shader as `i_data0..4` — whose semantics are fixed by bgfx to TEXCOORD31..27 in
  `varying.def.sc`. Wrong semantics fail *silently* with garbage transforms (migration §8.6).

## Shaders

[`Shader`](../../GanymedEngine/source/GanymedE/Renderer/Shader.h) wraps a `bgfx::ProgramHandle`.
**Shaders are compiled offline**: sources are `.sc` pairs (`vs_Name.sc`/`fs_Name.sc` + shared
`varying.def.sc`, per-shader `varying.<Name>.def.sc` when the layout differs — ImGui has one) in
`assets/shaders/src/`, compiled by `scripts/compile_shaders.bat|.sh` into
`assets/shaders/compiled/<profile>/` for `dx11`, `spirv` and `glsl`. At runtime the constructor
picks the profile matching `bgfx::getRendererType()` (mapping in `Shader.cpp::ProfileDirectory`).
Call sites still say `Shader::Create("assets/shaders/Foo.glsl")` — the path is reduced to its stem.

**Edit a shader → re-run `compile_shaders`.** A missing/failed program logs an error and its draws
are skipped (the engine keeps running).

API notes:
- `Bind()` records the program for the next submit (no GPU work).
- All uniforms are vec4/mat4; scalars pad into vec4 (`u_Foo.x` in shaders). Arrays are sized at
  creation — always pass the full array (`SetFloat4Array`, `SetMat4Array`); `u_Name[i]` is not
  addressable by name.
- Samplers: `SetTexture(samplerName, slot, texture)` replaces `texture->Bind(slot)` — a binding
  belongs to the draw call and names the sampler uniform it feeds. `SetInt` on a sampler is a
  no-op kept for legacy call sites.
- `.sc` language gotchas (each cost a compile cycle — see migration §5): `vec3_splat` not
  `vec3(x)`, `mul(m, v)` not `m * v`, `mtxFromCols`, varyings only in `main`'s signature, `line`
  is reserved in HLSL.

Current programs (20): FlatColor, VertexPosColor, Texture, Line, Grid, Phong, ShadowDepth, Skybox,
SkyboxCube, Equirect, Irradiance, Prefilter, BRDFLut, BloomDownsample, BloomUpsample, Tonemap,
FXAA, Blit, ImGui, RmlUi.

Two of those ship a per-shader `varying.<name>.def.sc` because their vertex layout is fixed by a
third party and does not match the engine's: **ImGui** and **RmlUi** (whose colour and texcoord
attributes are in the opposite order to ImGui's). `compile_shaders` prefers such a file
automatically.

## Textures & framebuffers

- [`Texture2D`](../../GanymedEngine/source/GanymedE/Renderer/Texture.h) — stb_image-loaded (RGBA8)
  or size-allocated + `SetData`. Sampler state (wrap/filter) is per-bind flags, stored on the
  texture and passed by `Shader::SetTexture`. Loads in bgfx's **top-left origin** — default
  `(0,0)-(1,1)` UVs are correct; the old GL `{0,1}-{1,0}` flips are gone everywhere.
- [`Framebuffer`](../../GanymedEngine/source/GanymedE/Renderer/Framebuffer.h) — attachment
  formats RGBA8, RGBA16F (HDR), RED_INTEGER (entity IDs; R32I with an R32F fallback where not
  renderable), D24S8, D32F. `BindToView(viewId)` targets + sizes a view; `Resize` rebuilds.
  `RequestPixelRead` implements the async blit+read (used by SceneRenderer's picking).
  Clearing per-attachment values (entity ID = −1) uses bgfx's clear-*palette* form — the ordinary
  packed clear color cannot express it.

## Cameras

- [`Camera`](../../GanymedEngine/source/GanymedE/Renderer/Camera.h) — just a projection matrix.
- [`SceneCamera`](../../GanymedEngine/source/GanymedE/Scene/SceneCamera.h) — runtime camera
  component payload: perspective (FOV/near/far) or orthographic (size/near/far), aspect from the
  viewport.
- [`EditorCamera`](../../GanymedEngine/source/GanymedE/Renderer/EditorCamera.h) — the viewport
  camera: orbit (Alt+LMB rotate, MMB pan, scroll zoom) around a focal point; perspective.
- `OrthographicCamera(+Controller)` — legacy 2D-era pair, still used by Sandbox.

## Renderer2D

Batched quad renderer ([`Renderer2D`](../../GanymedEngine/source/GanymedE/Renderer/Renderer2D.h)):
up to 20k quads per batch in a CPU array, flushed to a dynamic vertex buffer; 16 texture slots
(slot 0 = white 1×1, so colored quads and textured quads share one shader — `Texture.glsl` with 16
discrete samplers, since bgfx has no sampler arrays); per-vertex color/tiling/entity-ID.
`DrawQuad`/`DrawRotatedQuad` overloads take positions or a full transform; the ECS `RenderSystem`
uses the transform+entityID form for sprites. Renders into **its own view**
(`RenderPass::SceneTransparent`) — a view transform is per-view state, so 2D sharing the 3D view
would retroactively re-project the 3D geometry (migration §8.6).

## Renderer3D

[`Renderer3D`](../../GanymedEngine/source/GanymedE/Renderer/Renderer3D.cpp) is a submission-based
forward renderer. `BeginScene` uploads the camera (view transform + CPU copy for culling) and
resets per-frame state; `Submit*` calls only record; `EndScene` executes:

1. **Frame uniforms** — directional light, ambient/IBL flags, and the `GPULight[32]` array
   (point/spot; four vec4s per light) recorded into `FrameUniforms`.
2. **Partition** the draw list: shadow casters (all non-transparent commands, *not* camera-culled —
   an off-screen mesh still casts a visible shadow) vs. opaque/transparent (frustum-culled against
   per-submesh world AABBs).
3. **Shadow pass** — directional cascaded shadow maps: 4 cascades, 2048² D32F depth-only targets,
   one view per cascade (`RenderPass::Shadow + n`). Cascade fitting is stable (bounding-sphere) and
   texel-snapped to kill edge shimmer; front-face culling reduces acne. Color writes are disabled
   (a depth-only FB rejects draws whose write mask targets missing attachments). Split scheme:
   log/linear blend (λ=0.7) capped at 200 units.
4. **View restore** — back to `RenderPass::SceneHDR` (the shadow pass left the sticky view ID on
   the last cascade).
5. **Opaque** — sorted material → mesh → submesh → front-to-back; contiguous runs of the same
   (mesh, submesh) draw as **instanced chunks** (≤1024 instances per draw, transient instance
   buffer). Material binds carry the per-pass extras: cascade matrices (one `mat4[4]`), splits
   (one vec4), shadow samplers (slots 5–8, clamped), IBL maps (slots 9–11) and flags.
6. **Transparent** — back-to-front, blending on, depth-write off; only neighbors that stayed
   adjacent after the depth sort are instanced together.
7. **Debug lines** — accumulated `DrawLine/DrawWireBox/DrawWireSphere/DrawWireCapsule` calls flush
   as one lines draw (20k-vertex dynamic buffer), depth-tested but not written. Used by collider
   gizmos and Jolt debug draw.

Also owned here: the procedural **skybox** (fullscreen quad, sky/ground gradient + sun) or the
**cubemap skybox** when an environment is active; the editor **grid** (fragment-shader infinite
grid on a scaled quad — its transform goes through `bgfx::setTransform`, and it must not set
`u_CameraPosition` because `FrameUniforms` already does, one-uniform-per-draw); an environment
cache for `LoadEnvironment`. `GetStats()` reports draws/meshes/culled/instanced/transparent counts
(shown in the editor Stats panel).

Slot budget (Phong): 0–2 material maps (albedo/normal/metallic-roughness), 5–8 shadow cascades,
9–11 IBL, 12 skybox cubemap.

## Materials & meshes

- [`Material`](../../GanymedEngine/source/GanymedE/Renderer/Material.h) — shader ref (in practice
  always the shared Phong program from
  [`MeshShader`](../../GanymedEngine/source/GanymedE/Renderer/MeshShader.h), which exists to give
  that cache explicit ownership released before bgfx dies) + albedo color/metallic/roughness
  scalars, albedo/normal/metallic-roughness maps (paths, or embedded compressed bytes for
  glb-embedded textures so `MeshCache` can persist them), two-sided and transparent flags.
  `Bind()` uploads the scalars and binds the maps to slots 0–2 (white fallback).
- [`Mesh`](../../GanymedEngine/source/GanymedE/Renderer/Mesh.h) — interleaved
  `MeshVertex{Position, Normal, Tangent, TexCoord}` + 32-bit indices + `Submesh` table
  (base vertex/index, count, material index, local transform, name, local AABB) + material list +
  the built `Geometry`. Bounds are computed on build and used for culling.

## Environment / IBL

[`Environment`](../../GanymedEngine/source/GanymedE/Renderer/Environment.h) bakes an
equirectangular HDR into: a 512² 5-mip environment cubemap (skybox), a 32² diffuse irradiance map,
a 128² 5-mip prefiltered specular map, and a 512² BRDF LUT. The bake runs **once, entirely within
one frame**, across the transient view block starting at `RenderPass::EnvironmentBake` (~67 views:
faces × mips, twice, + LUT) — valid only because views execute in ID order, so each stage samples
what a lower-numbered view wrote. bgfx cannot mipmap render targets, so every env mip is rendered
from the panorama directly. Binding is the caller's job (`Renderer3D` feeds the handles to
`Shader::SetTexture` per material — samplers belong to shaders, there is no global bind).

## SceneRenderer & the post stack

[`SceneRenderer`](../../GanymedEngine/source/GanymedE/Renderer/SceneRenderer.h) is the
render-graph-lite owning the frame's targets and pass order:

```
scene HDR (RGBA16F + entityID + D24S8)
  → bloom: threshold+downsample mip chain, then upsample-accumulate (half-res result)
  → tonemap (ACES-style, exposure; bloom composited additively in HDR before the curve)
  → FXAA (optional)
  → composite (LDR, shown in the editor viewport via GetFinalImageRendererID)
  → game UI (RmlUi, RenderPass::UI = 28) composited into that same LDR target
```

The UI pass sits after Composite purely by view ID, which is what keeps it in display space
instead of being tonemapped with the scene — see [ui.md](ui.md). Note that
`SetViewportSize` rebuilds the post-stack targets, so anything holding the composite framebuffer
(the UI does) has to re-fetch it on resize.

`BeginFrame` binds+clears the scene target (color to `ClearColor`, entity IDs to −1 via clear
palette); render between Begin and End; `EndFrame` runs the stack. Settings
(`SceneRendererSettings`) cover exposure, bloom threshold/knee/intensity/radius, FXAA — all
editable live in the editor Stats panel. Bloom mip views must ascend in execution order (the
upsample chain running backwards was a real bug); all fullscreen passes flip V on top-down
backends (`#if !BGFX_SHADER_LANGUAGE_GLSL` in the vertex shaders) — an odd number of unflipped
passes mirrors the output (migration §8.8).

### Entity picking (async)

`RequestEntityID(x, y)` blits 1×1 from the entity-ID attachment into a staging texture and queues a
`readTexture`; `PollEntityID(out)` reports the newest landed result (up to 4 picks in flight —
measured latency is 3 frames; requests are *dropped*, not queued, when all slots are busy, since
the next frame issues another). The editor requests on hover every frame, so the latency is
invisible. Pick storage is a fixed array because bgfx writes the result memory asynchronously.

## Renderer (the umbrella)

[`Renderer`](../../GanymedEngine/source/GanymedE/Renderer/Renderer.h) is init/shutdown plus
cross-cutting state:

- `Init/Shutdown` — RenderCommand, Renderer2D, Renderer3D, PostProcess, and releasing `MeshShader`
  while bgfx is alive.
- **`IsGpuAlive()`** — lowered by `BgfxContext` *before* `bgfx::shutdown()`; every resource
  destructor checks it. This is the systemic fix for the "static outlives bgfx" crash class
  (function-local `static Ref<Shader>` etc.) — the guard makes it safe, but resources should still
  be owned and released explicitly (the guard turns a crash into a leak, and bgfx reports leaks).
- `GetFrameNumber()` — fed by `BgfxContext` from `bgfx::frame()`; what async readback polls
  against.
- `SetDebugStatsEnabled` — the F1 stats overlay.
- `IsSceneRenderPathDormant()` — a leftover migration switch, now hard-false; slated for removal.
