# GanymedEngine — Roadmap to a 3D Engine

A concrete, ordered plan for taking the engine from its current state (2D batched quad renderer with a scene/ECS editor) to a usable 3D engine: 3D graphics, 3D physics, an upgraded ECS, and the asset plumbing all of it needs.

Phases are ordered by dependency: each one unblocks the next, and the engine stays runnable after every phase.

---

## Where the engine is today

What we already have that carries over to 3D unchanged or nearly unchanged:

| Area                   | State                                                                                                            |
| ---------------------- | ---------------------------------------------------------------------------------------------------------------- |
| Platform layer         | GLFW window, input, events — Windows/Linux/macOS                                                                 |
| Render API abstraction | `RendererAPI` / `RenderCommand` / `Shader` / `Buffer` / `VertexArray` / `Framebuffer` (GL backend only)          |
| Framebuffers           | Multi-attachment (RGBA8 + R32I entity ID + depth) — the entity-ID picking pipeline works                         |
| Cameras                | `SceneCamera` already supports **perspective and orthographic**; `EditorCamera` is a perspective orbit camera    |
| Transforms             | `TransformComponent` is already full 3D (vec3 translation/rotation/scale → mat4)                                 |
| ECS                    | Vendored single-header **entt (old, pre-3.5 API**: `registry.has`, `registry.each`)                              |
| Editor                 | Dockspace, hierarchy/properties/content browser panels, ImGuizmo (3D-capable already), play/stop state           |
| Serialization          | yaml-cpp scene (de)serializer — **entity IDs are a hardcoded TODO** (`SceneSerializer.cpp:88`)                   |
| 2D renderer            | `Renderer2D` batched quads; `Renderer` (3D "generic" path) still only accepts `OrthographicCamera` and a raw VAO |

**Hard constraint to keep in mind:** the GL baseline is **4.1 core** (macOS ceiling). That means **no** compute shaders, SSBOs, `glClearTexImage`, DSA (`glCreateTextures`, `glTextureStorage`), or bindless textures on the mac path. Everything below is written to fit 4.1; the escape hatch is Phase 8 (RHI/Vulkan).

---

## Phase 0 — Foundations (do these first, everything depends on them)

Small, unglamorous, and they get exponentially more painful the longer they wait.

### 0.1 Real entity identity: `IDComponent` (UUID)

- Add a `UUID` class (`uint64_t`, random via `std::mt19937_64`) in `GanymedE/Core/UUID.h`.
- Every entity gets an `IDComponent` in `Scene::CreateEntity` (add `CreateEntityWithUUID` for deserialization).
- Fix `SceneSerializer.cpp:88` — serialize the real UUID instead of the hardcoded `12837192831273`.
- Why now: asset references, physics body ↔ entity maps, prefabs, and undo all key off stable IDs. `entt::entity` handles are not stable across runs.

### 0.2 Upgrade entt

- Replace the vendored old single header with a current entt release (pin it, same policy as the other submodules).
- API migration is mechanical: `registry.has<T>()` → `all_of<T>()`, `registry.each(...)` → iterate `registry.storage<entt::entity>()`, group/view API is mostly unchanged.
- Touchpoints: `Entity.h`, `SceneHierarchyPanel.cpp`, `Scene.cpp`.
- Why now: modern entt has `entt::organizer`, better views, and the old API is removed — every new component/system written before the upgrade adds migration surface.

### 0.3 Scene copy on Play

- `Scene::Copy(Ref<Scene> other)` — copy all components entity-by-entity, keyed by UUID (this is why 0.1 comes first).
- `EditorLayer::OnScenePlay` runs the copy; `OnSceneStop` throws the runtime scene away and restores the editor scene.
- Currently the play button mutates the editing scene — with physics this becomes unusable (bodies fall, transforms are destroyed).

### 0.4 Scene hierarchy (parent/child)

- `RelationshipComponent { UUID Parent; std::vector<UUID> Children; }` (or entt-handle based with UUID fallback for serialization).
- `Scene::GetWorldSpaceTransform(Entity)` walking the parent chain; hierarchy panel gains drag-to-reparent.
- 3D content is inherently hierarchical (a model = a tree of mesh nodes). Doing this before mesh import means imported node trees map 1:1 onto entities.

### 0.5 Uniform buffer objects

- Add `UniformBuffer` to the render API (`glGenBuffers` + `GL_UNIFORM_BUFFER` + `glBindBufferBase` — core in 4.1).
- Move the camera `u_ViewProjection` into a `CameraData` UBO at binding 0.
- With one shader (`Texture.glsl`) per-shader uniforms were fine; with N mesh/material shaders, per-shader `SetMat4` calls for shared data don't scale.

---

## Phase 1 — 3D renderer core (meshes on screen)

### 1.1 Mesh + Model import

- `Mesh`: interleaved vertex buffer (`Position, Normal, Tangent, TexCoord`), index buffer, list of `Submesh { baseVertex, baseIndex, indexCount, materialIndex, localTransform }`.
- Importer options:
  - **assimp** (submodule) — imports everything (FBX/OBJ/glTF/DAE), heavyweight, slow debug builds.
  - **tinygltf / cgltf** (header-only) — glTF 2.0 only, tiny, modern PBR material model built in.
  - Recommendation: **start with cgltf/tinygltf** (glTF covers the modern pipeline and maps directly onto PBR materials), add assimp later behind the same `MeshImporter` interface if FBX/OBJ support is needed.
- Import produces an entity subtree (via 0.4) with `StaticMeshComponent { AssetHandle mesh; uint32_t submeshIndex; }` per node — or a single entity + submesh table for simple models.

### 1.2 Renderer3D (forward)

- New `Renderer3D` alongside `Renderer2D` (don't retrofit the quad batcher):
  - `BeginScene(camera, lights)` → uploads Camera UBO + light data.
  - `SubmitMesh(mesh, submeshIndex, material, transform, entityID)` → **records into a draw list, does not draw immediately**.
  - `EndScene()` → sort draw list (by shader → material → mesh, front-to-back for opaque), then flush.
- Depth: `RenderCommand::SetDepthTest/SetDepthWrite`, enable `GL_DEPTH_TEST`; face culling `GL_BACK` with a two-sided material flag.
- Entity ID goes to the existing R32I attachment from the mesh shader — **3D mouse picking then works with zero editor changes** (the readback path is already there).
- Retire the `Renderer::BeginScene(OrthographicCamera&)` stub in `Renderer.h` — `Renderer` becomes init/shutdown + shared state owner (UBO ring, shader library), `Renderer2D`/`Renderer3D` are the submission APIs.

### 1.3 Materials

- `Material`: shader ref + uniform value table + texture slots; `MaterialInstance` for per-object overrides.
- Start with a single `PBR_Static.glsl` (or `Phong.glsl` first, see 2.1) — albedo/normal/metallic-roughness maps with scalar fallbacks (glTF convention).
- Serialize materials as their own asset files (`.gmat`, YAML) — needs Phase 4 handles; until then, embed material data in the scene file.

### 1.4 Editor integration for meshes

- `StaticMeshComponent` UI in `SceneHierarchyPanel::DrawComponents` (mesh path, material list).
- Content browser: recognize `.gltf/.glb`, drag-drop onto viewport → import + instantiate at origin.
- Viewport grid (a shader-drawn infinite grid quad) — orientation is impossible in empty 3D space without one.
- `EditorCamera`: add WASD fly mode (RMB-hold) alongside the orbit controls; orbit-only gets painful inside larger scenes.

**Milestone: glTF model dropped into the viewport renders textured, is clickable (picking), and gizmo-movable.**

---

## Phase 2 — Lighting & shadows

### 2.1 Analytic lights

- Components: `DirectionalLightComponent { color, intensity }`, `PointLightComponent { color, intensity, radius, falloff }`, `SpotLightComponent { …, innerCone, outerCone }`, plus `SkyLightComponent` later (2.4).
- Forward path, light data in a `LightData` UBO (fixed max, e.g. 1 directional + 32 point/spot — fine without SSBOs; going beyond that on GL 4.1 means forward+ via texture buffers, don't bother early).
- Start Blinn-Phong to validate the pipeline in a day, then swap the BRDF to **Cook-Torrance PBR** (metallic/roughness, matching the glTF material inputs) — the surrounding architecture is identical.

### 2.2 Shadow mapping

- Directional: single shadow map first (depth-only framebuffer — the `FramebufferTextureFormat::DEPTH24STENCIL8`-only path already exists), PCF 3×3.
- Then **cascaded shadow maps** (4 cascades) once the single-map version works; render the scene depth-only per cascade — this is where the draw-list design (1.2) pays off (re-submit the same list with a depth-only shader).
- Point/spot shadows (cube map / single map) are optional-later.

### 2.3 HDR pipeline

- Add `RGBA16F` to `FramebufferTextureFormat`; render scene into HDR target.
- Fullscreen tonemap pass (ACES or Khronos neutral) + exposure. This is the first "post-processing" pass — build a tiny `PostProcessPass` helper (fullscreen triangle + shader) that later passes (bloom, FXAA) reuse.

### 2.4 Environment / IBL

- Skybox: equirectangular HDR → cubemap conversion. **No compute on GL 4.1** — do the conversion and the irradiance/prefilter convolutions with fragment-shader render-to-cubemap-face passes (6 faces × mips), or precompute offline at import.
- `SkyLightComponent` = environment handle + intensity; BRDF LUT generated once at startup.

**Milestone: PBR model lit by sun + environment, casting soft shadows, tonemapped.**

---

## Phase 3 — 3D physics

### 3.1 Library choice

|                     | Notes                                                                                                                                              |
| ------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Jolt Physics** ✅ | Modern C++17, MIT, excellent multicore, used by Horizon; CMake but easy to wrap in a premake static-lib project like `imgui.lua`. **Recommended.** |
| Bullet              | Battle-tested but aging API, quirky maintenance                                                                                                    |
| PhysX 5             | Excellent but heavyweight build + BSD-with-caveats; overkill here                                                                                  |

Add as submodule `GanymedEngine/extern/JoltPhysics` + `extern/Jolt.lua` (same pattern as the other deps — build script lives _outside_ the submodule tree).

### 3.2 Components (pure data, engine-agnostic)

```
RigidBodyComponent   { BodyType Static/Dynamic/Kinematic; float mass, linearDamping, angularDamping; bool useGravity; }
BoxColliderComponent     { vec3 halfExtents; vec3 offset; PhysicsMaterial mat; }
SphereColliderComponent  { float radius; vec3 offset; ... }
CapsuleColliderComponent { float radius, halfHeight; ... }
MeshColliderComponent    { AssetHandle mesh; bool convex; }   // later
PhysicsMaterial          { float friction, restitution; }
```

Keep Jolt types out of the headers — components hold plain data, a `PhysicsScene` (owned by `Scene`) owns `JPH::Body` handles in a `UUID → BodyID` map.

### 3.3 Runtime lifecycle

- `Scene::OnRuntimeStart` (called from `OnScenePlay`, after the scene copy from 0.3): create Jolt world, create bodies from all rigidbody+collider entities.
- `OnUpdateRuntime`: **fixed timestep accumulator** (1/60) driving `physicsScene->Step()`; after stepping, write body transforms back into `TransformComponent` (world → local via the 0.4 hierarchy). Interpolate render transforms between the last two physics states to avoid 60 Hz stutter at high frame rates.
- `Scene::OnRuntimeStop`: destroy world.
- Editor: physics does **not** run in Edit state — exactly what the play/stop split already gives us.

### 3.4 Editor & debug

- Collider gizmos: wireframe box/sphere/capsule overlay in the viewport (needs a small `Renderer3D::DrawLine/DrawWireBox` immediate-ish debug path — also useful for light radii and frustum visualization).
- Optional: Jolt's `DebugRenderer` hook piped into the same line renderer.
- Collision events → `ScriptableEntity::OnCollisionEnter/Exit` callbacks (via Jolt contact listener + UUID map).

**Milestone: press Play, a stack of boxes collapses onto a floor plane, press Stop, everything is back where it was.**

---

## Phase 4 — Asset system (starts mattering by Phase 1, essential by Phase 3)

Right now assets are ad-hoc relative path strings. 3D multiplies asset counts (meshes, materials, textures, environments, physics materials).

- `AssetHandle = UUID`; `AssetMetadata { handle, type, filepath }`.
- `AssetManager`: `GetAsset<T>(handle)` with an in-memory cache; asset registry file (`assets/AssetRegistry.gr`, YAML) mapping handles ↔ paths.
- Components reference **handles, not paths**; serializer writes handles. (Do the `StaticMeshComponent` with a path string in Phase 1, migrate to handles here — don't block meshes on this.)
- Content browser: right-click → Import, per-type icons, and (later) rendered thumbnails for meshes/materials.
- Import cache: parse glTF once → dump a fast binary blob (`.assets/`), reload from cache when source is unchanged (hash/timestamp).

---

## Phase 5 — Renderer maturity (as needed, not up front)

Roughly in payoff order:

1. **Frustum culling** (AABB per submesh, test against camera frustum before submission).
2. **Instancing** for repeated meshes (`glDrawElementsInstanced`, per-instance transform VBO — the `Mat3/Mat4 + glVertexAttribDivisor` layout path in `OpenGLVertexArray` already exists and gets its first real user).
3. **Bloom** (threshold + separable blur mip chain — fragment passes, since no compute) and **FXAA**; MSAA alternative: multisampled framebuffer + resolve blit for the editor viewport.
4. **Transparency pass** (sorted back-to-front after opaque).
5. **Render graph-lite**: once passes multiply (shadow → depth pre → opaque → sky → transparent → post), formalize pass ordering/resources instead of hand-wiring framebuffers in `EditorLayer::OnUpdate`.
6. **2D-in-3D**: keep `Renderer2D` for sprites/UI/billboards; it already renders correctly into the 3D view since it takes the same camera.

---

## Phase 6 — Runtime & scripting (parallel track, any time after Phase 0)

- **Standalone runtime app** (`GanymedRuntime` premake project): loads a scene, no ImGui/editor — this is what "shipping a game" means and it flushes out every editor-only assumption.
- Scripting: `NativeScriptComponent` works but requires recompiling the app. Options when ready: Lua (sol2, small) or C# (mono/hostfxr, big commitment). Not a 3D blocker — a gameplay convenience.

---

## Phase 7 — Things to explicitly _not_ do yet

- Deferred rendering (forward + culling is plenty at this scale; revisit with many lights).
- Skeletal animation (big; slot after Phase 2 stabilizes — needs mesh skinning data from 1.1, so keep glTF skin data parsed-but-ignored).
- Terrain, GI, streaming, LODs.

## Phase 8 — The GL 4.1 ceiling (long-term)

macOS caps OpenGL at 4.1 and deprecates it entirely. The current `RendererAPI` abstraction is the right seam: when compute/SSBO/bindless become blocking (usually at GPU culling / clustered lighting / big post stacks), implement a second backend rather than forking GL paths — **Vulkan + MoltenVK** covers all three platforms with one API. Everything in Phases 1–5 above is designed to survive that swap (draw lists, UBO blocks, material abstraction, no GL types in engine headers).

---

## Suggested order of attack (summary)

```
0.1 UUIDs → 0.2 entt upgrade → 0.3 scene copy on play → 0.5 UBOs   (~small, sequential)
0.4 hierarchy                                                       (medium)
1.x meshes + Renderer3D + materials + grid/fly-cam                  (the big one)
2.1 lights → 2.2 shadows → 2.3 HDR → 2.4 IBL
3.x Jolt physics
4.x asset handles/registry (interleave once path-strings start hurting)
5.x renderer maturity, on demand
6.x runtime app + scripting, parallel
```

Each numbered milestone leaves the editor fully working — the same build/verify loop we use now (MSBuild + run editor on Windows, `wsl -d Ubuntu` make for the Linux check) applies at every step.
