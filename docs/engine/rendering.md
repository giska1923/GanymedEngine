# Rendering

The renderer runs entirely on [bgfx](https://github.com/bkaradzic/bgfx): one codebase, backend
auto-selected at startup (Direct3D 11/12, Vulkan, Metal, OpenGL). The engine-facing classes
(`Shader`, `Texture2D`, `Framebuffer`, …) are **concrete wrappers over bgfx handles** — the old
virtual per-API layer is gone. The migration that produced this design, including every bug found
and how it was verified, is recorded in [`BGFX_MIGRATION.md`](../history/BGFX_MIGRATION.md);
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
   pick coordinate flip, and the fullscreen-pass V flip), and clip depth is `[0,1]` on the former
   and `[-1,1]` on GL (`caps->homogeneousDepth`). Both are answered from caps rather than assumed —
   see [Projection matrices](#projection-matrices).
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
  `DrawIndexedInstancedSkinned` is the same call plus a second vertex stream, bound at the *same*
  `baseVertex` as stream 0 — the two must be parallel per-vertex or the mesh skins against the
  wrong joints, silently.
- [`Buffer.h`](../../GanymedEngine/source/GanymedE/Renderer/Buffer.h): `BufferLayout` keeps the
  `{ ShaderDataType::Float3, "a_Position" }` authoring syntax and translates to
  `bgfx::VertexLayout` via **`AttribFromName`** — bgfx attributes are semantic slots, so free-form
  data rides in spare TexCoords (`a_TexIndex`→TexCoord1, `a_TilingFactor`→TexCoord2,
  `a_EntityID`→TexCoord3). Skinned meshes add `a_JointIndices`→Indices and `a_JointWeights`→Weight
  on a second vertex stream; note the shader-side names for those two are **not** ours to pick —
  shaderc rejects any vertex input outside its fixed list, so they are declared `a_indices` and
  `a_weight`. This table must stay in sync with `varying.def.sc`. There is no 32-bit
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

The `vs_<name>`/`fs_<name>` pairing is a convention of the loader, not a requirement of the format,
and `Shader::CreateFromStages(name, vertexStage, fragmentStage)` names the two independently. Both
skinned programs use it: they differ from their static counterparts only in the vertex shader, and
without it the naming rule alone would force a duplicate copy of the 300-line `fs_Phong` to sit
next to it. (Not to be confused with the three-argument `Shader::Create`, which takes source.)

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

Current programs (23): FlatColor, VertexPosColor, Texture, Line, Grid, Phong, PhongSkinned,
ShadowDepth, ShadowDepthSkinned, Skybox, SkyboxCube, Equirect, Irradiance, Prefilter, BRDFLut,
BloomDownsample, BloomUpsample, Tonemap, FXAA, Blit, ImGui, RmlUi, Particle.

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

### Backend selection

`--renderer=<backend>` on the command line picks the bgfx backend; without it bgfx chooses, which
is the right default for shipping. Accepted: `auto`, `d3d11`, `d3d12`, `vulkan`, `metal`, `gl`,
`gles`, plus bgfx's own longer spellings (`direct3d11`) and the obvious short ones (`dx11`, `vk`).
Case-insensitive, last one wins. Parsed in
[`BgfxContext.cpp`](../../GanymedEngine/source/Platform/Bgfx/BgfxContext.cpp).

The engine requests a **GL 3.3 core context** (`BGFX_CONFIG_RENDERER_OPENGL_MIN_VERSION=33` in
`extern/bgfx.lua`); bgfx otherwise asks for its minimum and gets a GL 2.1 compatibility context.
Set the *MIN_VERSION*, never `BGFX_CONFIG_RENDERER_OPENGL` itself — bgfx applies its per-platform
renderer defaults inside one `#if !defined(...)` block that tests every `BGFX_CONFIG_RENDERER_*`
macro, so defining that one silently disables Direct3D 11/12 and Vulkan.

It is a launch-time switch rather than a config key on purpose: which backend to debug on belongs to
the run, not to the build — which is also why it is absent from `runtime.yaml`, a file whose own
header reserves it for what "belongs to the build rather than to the launch".

Every failure path falls back to letting bgfx choose, because an unusable `--renderer` should cost a
warning and a working window rather than a black screen: an unknown name warns and lists the valid
ones; a name this build of bgfx does not support warns and lists what it does support
(`getSupportedRenderers`); and a backend that is supported but cannot start — no driver, no device —
is retried on auto.

**The boot log reports what bgfx actually selected, not what was asked.** bgfx substitutes a working
backend silently when the requested one fails to start, so asking for Vulkan on a machine with no
Vulkan driver returns success and a D3D11 context. Labelling that line from the request would print
`Vulkan (requested)` over a D3D11 frame, so the line compares `getRendererType()` against the request
and says plainly when they differ.

Note for anything reading positional arguments: use `ApplicationCommandLineArgs::FirstPositional()`,
not `Args[1]`. Both apps take a path positionally and both used to index slot 1 directly, which broke
the moment the engine grew its first flag.

### bgfx diagnostics

**`BgfxContext` installs a `bgfx::CallbackI` that forwards into the engine logger.** Without one,
bgfx uses its own stub, which on Windows writes traces and fatals to the *debugger* via
`OutputDebugString` and nowhere else — so every bgfx warning the engine ever produced was absent
from `GanymedE.log`.

That blindness was expensive: a backend could fail to compile a shader and the editor would appear
to hang during startup with a clean log. Both OpenGL bugs below were found within minutes of the
callback existing, having previously been misdiagnosed from the outside.

- `fatal` logs, and asserts for everything except `DeviceLost` (which bgfx survives).
- `traceVargs` logs at TRACE, because bgfx is chatty in Debug (`BX_CONFIG_DEBUG=1`).
- `screenShot` writes an uncompressed 32-bit TGA. Taking over the callback means taking over
  `bgfx::requestScreenShot`, including the stub's convention of appending `.tga` to the name.
- The shader/texture cache hooks decline, and the profiler hooks are deliberately empty — routing
  bgfx's per-draw scopes into the mutex-guarded Instrumentor would measure the tracer
  ([core.md](core.md#thread-naming-and-profiler-callbacks) makes the same point about the job
  system's wait callbacks).

### Uniform lifetime: create before the program links

**bgfx binds a program's uniforms by name when the program is linked.** A uniform handle that does
not exist at that moment is never wired to the program — and on OpenGL it then reads as **zero** in
the shader for the life of that program, announced only as
`WARN User defined uniform 'u_Exposure' is not found, it won't be set`. Direct3D resolves uniforms
per draw instead and tolerates late creation, so this asymmetry hides completely on D3D.

`Shader` creates its uniforms lazily, on the first `SetFloat`/`SetTexture`/… for a given name, which
is always *after* the constructor linked the program. `Shader::GetUniform` therefore **relinks the
program whenever a new name appears**. It settles immediately — a uniform is only ever new once —
and costs a few relinks during the first frames that exercise a shader.

Two details the implementation depends on:

- **The old program must be destroyed first.** bgfx caches programs by their (vertex, fragment)
  stage pair and returns the existing handle for a repeat request, so calling `createProgram` again
  on the same stages relinks nothing. That is why `Shader` keeps its stage handles alive
  (`destroyShaders = false`) and owns their destruction.
- **The bound program is re-set after a relink**, because `Bind()` runs before the `Set*` calls that
  trigger one, and `RenderCommand` would otherwise submit the handle that was just replaced.

`FrameUniforms` avoids all of this by creating its uniforms eagerly in `Init()`, before any shader
exists — which is why the scene itself always rendered on OpenGL while the post-process chain, whose
exposure arrived as 0, produced a black viewport.

### Writing shaders that compile on every profile

Shader bytecode is built per profile, and **HLSL is permissive where GLSL is strict**, so a shader
that compiles for `dx11` proves nothing about `glsl`. Two rules, both learned from shaders that had
never once compiled on OpenGL:

- **`BgfxSampler2D` is not a portable type.** It is a struct bundling a `SamplerState` with a
  `Texture2D`, declared only for HLSL, SPIR-V and Metal; GLSL has a real `sampler2D` and gets no
  such struct. A function taking a sampler needs a per-language alias (see `Sampler2DParam` in
  `fs_Phong.sc`). Sampling inside the body is fine — `texture2D` is bgfx's portable macro.
- **Truncate vec4 uniforms explicitly.** bgfx uniforms are always `vec4`. HLSL silently truncates
  `u_CameraPosition - v_worldpos`; GLSL rejects it. Write `.xyz`.

### Projection matrices

**Every projection on the render path is built through `Projection::` in
[`Renderer.h`](../../GanymedEngine/source/GanymedE/Renderer/Renderer.h), never `glm::perspective`
directly.** `Projection::Perspective` and `Projection::Orthographic` read
`caps->homogeneousDepth` and pick glm's convention-explicit form — `perspectiveRH_ZO` for `[0,1]`
clip depth, `perspectiveRH_NO` for `[-1,1]`.

The problem this solves (BGFX_MIGRATION.md §9.3): the workspace compiles with
`GLM_FORCE_DEPTH_ZERO_TO_ONE`, which is what makes `glm::perspective` emit a `[0,1]` matrix. Being
a compile-time define it could not adapt, so an OpenGL backend would have rendered with incorrect
near-plane clipping and half its depth precision — and silently, since a define cannot fail at
runtime. `BgfxContext` used to log an error on the mismatch; that error is gone because the
mismatch is now handled.

Call sites: `SceneCamera`, `EditorCamera`, the shadow cascades and light projection in
`Renderer3D`, and the IBL capture projection in `Environment` (a bake that clipped wrongly would
poison every scene lit by the result).

Two details worth knowing:

- **`HomogeneousDepth()` is `false` before the GPU is up**, which is exactly what
  `GLM_FORCE_DEPTH_ZERO_TO_ONE` meant, so a camera constructed before `Renderer::Init` gets the
  matrix it always got. Every camera recomputes on resize and on any setter, so no pre-init matrix
  survives into a rendered frame.
- **Shaders answer the same question per profile, not per frame.** Bytecode is compiled once per
  backend, so the shading language *is* the backend: `fs_Phong.sc`'s cascade lookup remaps `proj.z`
  only under `BGFX_SHADER_LANGUAGE_GLSL` (where clip depth is `[-1,1]`) and flips `proj.y` only
  where it is not (where render targets are top-down). That is the same idiom the fullscreen passes
  use — see `vs_Blit.sc`.

Verified on D3D11 by asserting the new matrices are **bit-identical** to the `glm` calls they
replaced — which they are by construction, since `glm::perspective` under the define *is*
`perspectiveRH_ZO` — and by capturing the 3D scene before and after: pixel-identical. The `[-1,1]`
branch differs from `[0,1]` in exactly the two depth-row elements, so the caps check is not a
no-op. **No `[-1,1]` backend has actually been run**; that needs backend selection (§9.2) and is
tracked in [ToDo/rendering.md](../ToDo/rendering.md).

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
   log/linear blend (λ=0.7) capped at 200 units. Skinned casters are split out of the instanced
   caster list and redrawn through `ShadowDepthSkinned` in **every** cascade — a character standing
   in cascade 0 casting into cascade 2 is ordinary, and a bind-pose shadow under a moving character
   reads as a bug even though nothing errored.
4. **View restore** — back to `RenderPass::SceneHDR` (the shadow pass left the sticky view ID on
   the last cascade).
5. **Opaque** — sorted material → mesh → submesh → front-to-back; contiguous runs of the same
   (mesh, submesh) draw as **instanced chunks** (≤1024 instances per draw, transient instance
   buffer). Material binds carry the per-pass extras: cascade matrices (one `mat4[4]`), splits
   (one vec4), shadow samplers (slots 5–8, clamped), IBL maps (slots 9–11) and flags. A skinned
   command never joins a run (see below).
6. **Transparent** — back-to-front, blending on, depth-write off; only neighbors that stayed
   adjacent after the depth sort are instanced together.
7. **Particles** — [`ParticleRenderer`](../../GanymedEngine/source/GanymedE/Renderer/ParticleRenderer.h)
   flushes here, after the transparent loop and before depth-write is restored. Billboard emitters
   are one `bgfx::submit` each into view 5's Sequential stream (CPU corner construction, camera
   right/up basis, per-emitter Alpha/Additive via the sticky `RenderState` singleton — restored
   unconditionally, including on transient-buffer exhaustion). Within an Alpha emitter, particles
   are sorted back-to-front on a **scratch index array**; the pool itself is never reordered (it is
   the sim's determinism instrument). Additive emitters skip the sort. Mesh particles never reach
   this flush: `RenderSystem` submitted them earlier as ordinary Phong opaques, so they batch,
   light, and cast shadows. **Hard authoring rule: mesh particles must use opaque materials** — the
   transparent path merges only sort-adjacent runs, so a transparent debris material is one draw
   *per particle*. Particles always composite *after* transparent meshes (a smoke plume behind glass
   draws over it) — accepted v1 artifact; Unity interleaves the queues, Ganymed does not because
   that means injecting into the transparent sort.
8. **Debug lines** — accumulated `DrawLine/DrawWireBox/DrawWireSphere/DrawWireCapsule` calls flush
   as one lines draw (20k-vertex dynamic buffer), depth-tested but not written. Used by collider
   gizmos and Jolt debug draw.

Also owned here: the procedural **skybox** (fullscreen quad, sky/ground gradient + sun) or the
**cubemap skybox** when an environment is active; the editor **grid** (fragment-shader infinite
grid on a scaled quad — its transform goes through `bgfx::setTransform`, and it must not set
`u_CameraPosition` because `FrameUniforms` already does, one-uniform-per-draw). The active
environment is whatever `SubmitEnvironment` set this frame — caching environments by path is
`AssetManager`'s job, not the renderer's. `GetStats()` reports
draws/meshes/culled/instanced/transparent/skinned plus particle emitters/billboards/draws/culled
(shown in the editor Stats panel). Per-emitter frustum cull uses the CPU AABB `ParticleSystem`
wrote; there is no per-billboard cull. Budget: billboards carry the high counts; mesh debris is
hundreds, not tens of thousands (one `DrawCommand` + frustum test per particle, and opaque casters
are pushed unculled ×4 shadow cascades).

Slot budget (Phong): 0–2 material maps (albedo/normal/metallic-roughness), 5–8 shadow cascades,
9–11 IBL, 12 skybox cubemap.

### Skinned meshes

`Renderer3D::SubmitSkinnedMesh(mesh, transform, palette, jointCount, entityID)` is the skinned
entry point; `RenderSystem` calls it for entities whose mesh has a skeleton and whose animator has
built a palette. Everything else about the command — sorting, culling, material binding, entity-ID
picking — goes through the same path as a static draw. Only three things differ:

- **The palette is copied at submit** into a frame-lifetime `PaletteStorage`, one `MaxBones`-sized
  identity-padded block per skinned entity (shared by all of its skinned submeshes), and the
  command holds an offset into it. This is how instance data is already staged, and it means the
  flush does not depend on `AnimatorComponent::Palette` still being alive or unchanged.
- **Skinned commands do not batch.** The palette is uniform state, not per-instance data, so two
  characters in different poses cannot share a draw. Each skinned submesh is one submit with
  `SetMat4Array("u_Bones", …)` in front of it. Instance count stays **1** rather than dropping to a
  non-instanced draw, so there is one vertex-input convention (`i_data0..4`) everywhere and the
  entity ID still reaches the picking attachment. The merge loop's guards are written so that a
  scene with no skinned commands batches byte-for-byte as it did before.
- **The program is replaced per skinned draw**, which forces a material rebind on it and on the
  next static draw after it — bgfx discards texture bindings at submit, so a skinned draw cannot
  ride the material cache, and a static draw must not inherit the skinned program (it would read a
  stream 1 that is not bound).

`Skeleton::MaxBones` is **128**: one `mat4[128]` uniform, which is a *single* handle against
`BGFX_CONFIG_MAX_UNIFORMS = 512` (the limit counts handles, not vec4s) and 512 vec4s inside D3D11's
4096-vec4 constant buffer. Humanoid rigs run 60–90 joints. The caveat is GL: its guaranteed minimum
`MAX_VERTEX_UNIFORM_VECTORS` is 256, so a minimal GL implementation may fail to link the skinned
programs. D3D/Vulkan/Metal are the primary backends and this is documented rather than engineered
around. Import warns and clamps above 128 joints — drawing wrong, loudly.

Bounds are the one deliberate approximation. A skinned submesh's vertices are the bind pose, so the
measured AABB is not the box that gets drawn; `Mesh::ComputeBounds` pads it by
`SkinnedBoundsPadding` (25%) of the box's **largest** extent — not per axis, because a limb can
swing about as far as the rig is long, so a narrow axis needs the same absolute slack as a wide one
(CesiumMan stands arms-down with an X extent of 0.31 against a height of 1.51, and its walk cycle
overruns a per-axis 50% pad). Exact posed bounds mean skinning every vertex on the CPU each frame to
decide one culling test. The failure mode is a character popping at the screen edge if a clip swings
wider than the pad.

Order of operations in `vs_PhongSkinned`: blend the palette in mesh space **first**, apply the
per-instance model matrix after, exactly where `vs_Phong` applies it. The palette is
`Global * InverseBind`, which is identity at the bind pose, so an unposed rig lands precisely where
`vs_Phong` would have put it.

One last trap. `Submesh::LocalTransform` is *kept* for skinned submeshes, where static ones have it
reset to identity by the world-space bake, and re-applying it here is what cancels the
`inverse(skinnedMeshNodeWorld)` folded into `Skeleton::RootTransform`. Both halves have to be
present: dropping either alone leaves a Y-up-corrected character rendering on its side. See
[assets.md](assets.md#skinning-data).

## Materials & meshes

- [`Material`](../../GanymedEngine/source/GanymedE/Renderer/Material.h) — shader ref (in practice
  always the shared Phong program from
  [`MeshShader`](../../GanymedEngine/source/GanymedE/Renderer/MeshShader.h), which exists to give
  that cache explicit ownership released before bgfx dies) + albedo color/metallic/roughness
  scalars, albedo/normal/metallic-roughness maps (paths, or embedded compressed bytes for
  glb-embedded textures so the mesh blob can persist them), two-sided and transparent flags.
  `Bind()` uploads the scalars and binds the maps to slots 0–2 (white fallback). A material can
  come from a mesh's own import *or* from a `.gmat` asset — see
  [assets.md](assets.md#materials-gmat); the renderer does not care which.
- [`Mesh`](../../GanymedEngine/source/GanymedE/Renderer/Mesh.h) — interleaved
  `MeshVertex{Position, Normal, Tangent, TexCoord}` + 32-bit indices + `Submesh` table
  (base vertex/index, count, material index, local transform, name, local AABB, `IsSkinned`) +
  material list + the built `Geometry`, plus the skeleton and clips on a rigged asset. Bounds are
  computed on build and used for culling. `Build` also creates the optional stream-1 buffer
  (`GetSkinVertexBuffer()`, null when there is no skin data) from `SkinVertex{JointIndices,
  JointWeights}`; the skin attributes ride a second stream rather than widening `MeshVertex`, which
  would cost every static vertex in the engine 32 bytes to serve the few that are rigged.

### Per-entity material overrides

`SubmitMesh` and `SubmitSkinnedMesh` both take an optional `(const Ref<Material>* overrides,
uint32_t count)` pair, indexed by `Submesh::MaterialIndex` — a null entry, or an index past the
end, falls back to the mesh's own material. `RenderSystem` copies
`StaticMeshComponent::MaterialOverrides` into a reused scratch vector and passes the array down.
The copy is not a lookup any more — each slot is an `AssetRef<Material>` holding its object — but a
contiguous `const Ref<Material>*` is still needed, and `AssetRef` is not layout-compatible with
`Ref`.

Passing the array down rather than looping submeshes at the call site is what lets the **skinned**
path honour overrides too: its palette staging is internal to `Renderer3D`, so a caller cannot
reproduce the loop. One `ResolveMaterial` helper serves both paths, so they cannot drift.

Three consequences worth encoding rather than discovering:

- **Batching is by `Ref` identity.** N entities sharing one `.gmat` handle receive one `Ref` from
  the asset cache and merge into the same instanced run; assigning a different `.gmat` to one entity
  splits exactly that entity out. Measured, not assumed: four boxes on one mesh draw the same
  whether they use the mesh default or one shared `.gmat`, and moving one onto a second `.gmat`
  costs exactly one more draw.
- **An override's `Transparent` flag repartitions that submesh.** It moves to the transparent pass
  *and* stops being a shadow caster — the partition reads `cmd.Material->IsTransparent()`, and the
  material it reads is now the override. That is the feature, not a bug, but it means a material
  swap can change a scene's shadows.
- **The `boundMaterial` raw-pointer cache now sees interleaved override and default materials.**
  Its invalidation rules are unchanged (a skinned draw always rebinds, and it invalidates the cache
  behind it), and the mixed case is verified rather than assumed: a scene of default, overridden and
  skinned draws renders a stable draw count across 100 frames.

## Colour space

**The engine has no sRGB pipeline, and this section exists so that is a recorded decision rather
than a thing you rediscover.** Textures are sampled as raw `RGBA8` with no `BGFX_TEXTURE_SRGB` flag
and no shader-side decode; lighting runs in whatever space the texels are already in; and
`Tonemap.glsl` applies `pow(mapped, 1.0/2.2)` once at the end of the post stack, on the way to an
8-bit target ImGui shows.

That is not correct, and it is consistent. Albedo authored in sRGB is being lit as though it were
linear, which makes mid-tones brighter than they should be — and the single gamma at the end hides
enough of it that everything looks plausible. Fixing it means flagging colour textures sRGB at
creation, leaving normal/roughness/metallic linear, and re-checking every lighting constant that
was tuned against the current look. **That is a rendering change with a visible before/after, not a
side effect of anything else**, which is why the asset compiler deliberately has no `sRGB` config
key ([assets.md](assets.md#texture-compilation)): a key that changed the look of every scene would
be smuggling this change in through the back door.

The convention to adopt when it is done is the standard one — albedo and emissive sRGB, everything
else linear — and the check is a known reference gradient rendering identically before and after
the *compiler*, then deliberately differently when the sRGB flag lands.

Block compression does not interact with this. BCn stores bits; colour space is a property of how
the texture is created and sampled, not of the encoded payload.

## Environment / IBL

[`Environment`](../../GanymedEngine/source/GanymedE/Renderer/Environment.h) bakes an
equirectangular HDR into: a 512² 5-mip environment cubemap (skybox), a 32² diffuse irradiance map,
a 128² 5-mip prefiltered specular map, and — once per process, not once per environment — a 512²
BRDF LUT. The bake runs **once, entirely within one frame**, across the transient view block starting at `RenderPass::EnvironmentBake` (67 views:
faces × mips, twice, + LUT) — valid only because views execute in ID order, so each stage samples
what a lower-numbered view wrote. bgfx cannot mipmap render targets, so every env mip is rendered
from the panorama directly. Binding is the caller's job (`Renderer3D` feeds the handles to
`Shader::SetTexture` per material — samplers belong to shaders, there is no global bind).

Only the upload and the submission are on the submit thread; the panorama is `stbi_loadf`-decoded on
a worker first (`Environment::Load`, the asset layer's Parse stage — see
[assets.md](assets.md#what-is-still-synchronous)).

### What every environment shares

Two things are created once and reused, both released by `Renderer::Shutdown` while bgfx is still
alive — static destruction runs *after* `bgfx::shutdown`, which is how handles left to it leak or
crash (the reasoning is spelled out in [`MeshShader.h`](../../GanymedEngine/source/GanymedE/Renderer/MeshShader.h)):

- **The four bake programs** (`Equirect`, `Irradiance`, `Prefilter`, `BRDFLut`). They were recreated
  per environment, at 1.6–1.8 ms of file IO and shader creation for an identical result.
- **The BRDF LUT.** It is the split-sum approximation's second term — a pure function of the BRDF
  over (NdotV, roughness), with **no dependence on the HDR**. Baking it per environment produced a
  byte-identical 512² texture every time and cost a view, a framebuffer and a draw per load.
  `Environment::GetBRDFLut()` returns the shared handle; no environment owns it, and no
  environment's destructor frees it.

Together these take a second and subsequent environment load from ~4–5 ms of submit-thread work to
**2.2–3.3 ms**. The first load in a process still pays for both (~6.3 ms).

### The IBL bake is a prepass

`RenderPass::EnvironmentBake = 1` — **before the shadow and scene passes**, not after them. That
placement is the whole reason the bake is cheap, and it was not always so.

The block used to sit at 32, after `SceneHDR`. An environment applied at the top of a frame was
therefore unreadable by that same frame's scene pass, and the bake worked around it by calling
`bgfx::frame()` **twice, inline**, to force its own frames through. That cost 24–26 ms of blocked
main thread (measured, Release) — it was the larger half of the load hitch — and on the way past it
presented two half-built frames.

Ordering the bake first makes it correct within the frame it is submitted in, so the forced frames
are gone. Verified frame by frame with backbuffer screenshots: the frame an environment applies in
already renders the baked skybox, and the following frames are pixel-identical to it.

Two consequences worth knowing if you add a pass:

- **Everything that samples the bake must sort above 67.** The pass table leaves views 1–68 to the
  bake and starts the frame proper at `Shadow = 69`; `ViewAllocator` asserts against `Shadow` rather
  than counting. A bake takes 67 views the first time and **66 after that** — see the BRDF LUT below.
- Destroying the bake's 67 transient framebuffers immediately after submission is safe: bgfx defers
  handle destruction until the frame that used them has been rendered. The cube textures they wrote
  into are owned by the `Environment`.

## SceneRenderer & the post stack

[`SceneRenderer`](../../GanymedEngine/source/GanymedE/Renderer/SceneRenderer.h) is the
render-graph-lite owning the frame's targets and pass order:

```
scene HDR (RGBA16F + entityID + D24S8)
  → bloom: threshold+downsample mip chain, then upsample-accumulate (half-res result)
  → tonemap (ACES-style, exposure; bloom composited additively in HDR before the curve)
  → FXAA (optional)
  → composite (LDR, shown in the editor viewport via GetFinalImageRendererID)
  → game UI (RmlUi, RenderPass::UI = 96) composited into that same LDR target
```

(Or, with `SetOutputToBackbuffer(true)`, the final post pass and the UI both land on the backbuffer
instead — see [Backbuffer output mode](#backbuffer-output-mode).)

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

Which pass is *final* is decided once, from `FXAAEnabled && the FXAA shader loaded`. Both the
"where does tonemap write" and "does FXAA run" decisions read that single predicate: splitting them
left a hole where FXAA enabled with a missing shader sent tonemap to the FXAA input and left the
composite target unwritten, i.e. a black image.

### Backbuffer output mode

`SetOutputToBackbuffer(true)` sends the **final** pass to the backbuffer instead of the composite
framebuffer — FXAA at view 93 when active, tonemap at view 92 when not. It replaces
`Framebuffer::BindToView` with `setViewFrameBuffer(view, BGFX_INVALID_HANDLE)` +
`setViewRect(view, 0, 0, w, h)`. A host in this mode also passes `nullptr` to `UIEngine::SetTarget`,
which routes RmlUi's view 96 at the backbuffer too. View order stays monotonic: 0 (context touch) <
92/93 (final post) < 96 (UI). This is the mode `GanymedRuntime` runs in; the editor never touches it.

Consequences:

- The intermediate targets (HDR, bloom chain, tonemap) are still allocated at `SetViewportSize`'s
  dimensions, so **the host must feed it the window size** — the final view rect comes from there.
  There is no resolution-scaling knob hiding in this switch.
- `GetFinalImageRendererID()` asserts: nothing writes the composite target in this mode, so the
  handle names a texture holding whatever was in it before the switch.
- The editor's "composite framebuffer is a different object after resize, re-point the UI at it"
  hazard disappears, because the UI target is null.
- The picking `RED_INTEGER` attachment is still allocated even with no picking consumer. Dead cost,
  accepted for v1.

**Retarget rather than a dedicated present pass** — the divergence from the production norm.
Unity/Unreal both end on a present/upscale pass because it carries resolution scaling, HDR-display
output, and platform present semantics. None of those exist here yet, and a present pass would cost a
new view ID *above* `RenderPass::UI = 96` (since it must composite a UI'd image) plus a new shader:
`vs_Blit.sc` expects `a_texcoord0` while the PostProcess fullscreen quad supplies only
`a_Position` as Float2, so it would have to derive UV from position like `vs_FXAA`/`vs_Tonemap` — a
new program and a new flip-parity surface. The escape hatch is named and deferred to whenever
resolution scaling arrives.

**Known GL caveat, not engineered around.** `vs_Tonemap`/`vs_FXAA` carry the V-flip branch written
for offscreen targets, and bgfx flips offscreen versus backbuffer on GL backends. So the same pass
retargeted at the backbuffer may render **upside-down on GL** while being correct on
D3D/Vulkan/Metal. The primary platform is D3D and the probe verified it there; if you hit a mirrored
image on GL, this is why — the fix is a caps-driven flip in those two vertex shaders, not a redesign.

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
  and `Environment`'s shared bake resources while bgfx is alive.
- **`IsGpuAlive()`** — lowered by `BgfxContext` *before* `bgfx::shutdown()`; every resource
  destructor checks it. This is the systemic fix for the "static outlives bgfx" crash class
  (function-local `static Ref<Shader>` etc.) — the guard makes it safe, but resources should still
  be owned and released explicitly (the guard turns a crash into a leak, and bgfx reports leaks).

  **The rule that follows, and it is easy to miss:** every `Ref<>` a renderer's `static` data holds
  must be cleared in that renderer's `Shutdown()`. `Renderer3D::Shutdown` clears twelve such members
  and for a long time missed the thirteenth — `s_Data.ActiveEnvironment`, the environment the last
  frame drew with. A static's destructor runs after `main()`, by which point `IsGpuAlive()` is false,
  so `~Environment` took its early-out and bgfx reported `LEAK: TextureHandle 3` at shutdown (the env
  cubemap, the irradiance map and the prefiltered map). Clearing it does not destroy anything early —
  the scene still owns the environment through an `AssetRef` and dies during the LayerStack unwind,
  which is inside the window's lifetime. It only stops a static from being the last owner.

  Checking this is cheap: run a **Debug** build (Release emits no bgfx diagnostics at all), close it
  normally, and look for `BGFX LEAK` on stdout. A killed process proves nothing, because shutdown
  never runs.
- `GetFrameNumber()` — fed by `BgfxContext` from `bgfx::frame()`; what async readback polls
  against.
- `SetDebugStatsEnabled` — the F1 stats overlay.
- `IsSceneRenderPathDormant()` — a leftover migration switch, now hard-false; slated for removal.
