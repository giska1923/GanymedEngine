# GanymedEngine — Skeletal Animation Roadmap

Status: **Phases 1–4 executed; Phase 5 outstanding.** Written 2026-08-02, against the
post-scripting/post-bgfx engine (branch point: the Linux/macOS build-fix work). Each phase carries
its own "execution notes" section recording where the plan was wrong or diverged from — read those
alongside the plan, not instead of it.

This is the plan of record for the fifth milestone: skeletal animation, from glTF import to
animated, shadow-casting characters driven by Lua. It follows the format of
[`3D_ROADMAP.md`](3D_ROADMAP.md), which explicitly parked this work ("Skeletal animation
(big; slot after Phase 2 stabilizes — needs mesh skinning data from 1.1...)"). Like the other
documents in this folder it records *why* decisions were made, so read the decision notes even
if you skip the code sketches.

## Decisions of record

Settled up front, before any code:

1. **The plan lives here**, indexed from `docs/README.md`, matching how the three previous
   milestones were run.
2. **Phase 1 is asset-pipeline prep**, scoped to: a `GetAsset<Texture2D>` cache + shared
   texture loader, explicit-specialization declarations in `AssetManager.h`, deletion of the
   dead `Renderer3D` environment cache, a typed drag-drop helper for the editor, and an
   `AssetManager::Reload(handle)` eviction primitive. Explicitly **out**: `.gmat` material
   serialization, a sprite texture field, registry-save hygiene (sorted YAML, batched writes).
3. **Skeleton and animation clips live inside the `Mesh` asset** and are loaded/cached with
   it. `AnimatorComponent` references clips **by name**. The registry keeps its one-path →
   one-handle model; no sub-asset identity. Rationale: a `.glb` carries mesh + skin + clips in
   one file, and v1 has no retargeting, so separate clip assets buy nothing but registry
   surgery. The upgrade path (sub-asset registry entries, Unity-style) stays open.

## Where the engine is today (facts this plan is built on)

Verified against the tree at planning time; re-verify anything load-bearing before executing a
later phase.

- **Importer** (`GanymedEngine/source/GanymedE/Renderer/MeshImporter.cpp`): cgltf 1.14
  (vendored at `GanymedEngine/extern/cgltf/`, full skin/animation support). Reads only
  POSITION/NORMAL/TANGENT/TEXCOORD_0 — `joints`/`weights` attributes fall through a
  `default: break` (~L221–235). Vertices are **baked into world space at import** (position
  ~L260, normals/tangents via normalMatrix ~L254/265/275) and `Submesh::LocalTransform` is
  wiped to identity afterwards (~L327) — the glTF node hierarchy exists nowhere after import.
  Node traversal iterates an `unordered_map`, so submesh order is **non-deterministic**.
- **Mesh cache** (`GanymedEngine/source/GanymedE/Assets/MeshCache.cpp`):
  `MESH_CACHE_VERSION = 3`; `MeshVertex` (pos3/normal3/tangent3/uv2, 44 bytes) is memcpy'd
  wholesale — any vertex-struct change is a silent format change unless the version is bumped.
- **Renderer**: the *only* mesh draw path is instanced. The model matrix reaches the vertex
  shader exclusively as `i_data0..3` via a transient instance buffer
  (`RenderCommand::DrawIndexedInstanced`); `MeshInstanceData` is 80 bytes — a bone palette
  cannot ride the instance stream. All mesh materials share one program (`MeshShader::Get()` →
  Phong); the shadow pass has its own `ShadowDepth` program. `Shader::SetMat4Array` exists
  (used for `u_LightSpaceMatrices` mat4[4]); bgfx fixes an array uniform's size at creation.
  Free texture slots: 3, 4, ≥13. `BGFX_CONFIG_MAX_UNIFORMS = 512` (counts *handles*).
- **Asset layer** (`GanymedEngine/source/GanymedE/Assets/`): `GetAsset<T>` is declared in the
  header with no definition; the Mesh/Environment specializations are defined only in
  `AssetManager.cpp` with **no declarations in the header** — technically IFNDR, practically
  "unsupported T fails at link time with an opaque error". There is **zero texture
  de-duplication** (every material map is an independent decode + GPU upload; the
  importer and the cache-replay path even duplicate the decode code). There is **no way to
  invalidate a loaded asset** — once in `LoadedMeshes`/`LoadedEnvironments`, frozen for the
  process lifetime. Components store `AssetHandle`s, never `Ref`s; `RenderSystem` re-fetches
  by handle every frame.
- **ECS**: adding a component means `Components.h` + `ComponentList`
  (`ECS/ComponentTraits.h`) + both `SceneSerializer` sides + the inspector — the serializer
  and editor UI are the two hand-maintained per-component lists with no compile-time
  enforcement. Adding a system means a CRTP `ECS::System<Impl>` with declared views and a
  registration slot in the `Scene` constructor — **registration order is execution order**,
  and `ValidateOrdering` only catches reader-before-writer, not writer-vs-writer.
- **Editor**: five copy-pasted extension-sniffing drag-drop accept blocks (viewport scene,
  viewport mesh, mesh inspector, script inspector, skylight inspector), none of which use the
  existing `AssetTypeFromExtension`.

---

## Phase 1 — Asset pipeline prep

**Goal:** the asset layer patterns that Skeleton/AnimationClip loading will lean on are
solid: textures are cached and de-duplicated through the same handle-based path as meshes and
environments, unsupported `GetAsset<T>` fails at compile time, drag-drop is typed, and there
is a reload primitive so animation authoring can iterate on source files without restarting
the editor.

### 1.1 Shared texture loader — `TextureImporter`

New files `GanymedEngine/source/GanymedE/Assets/TextureImporter.h/.cpp` (the engine project
globs `source/**` — **regenerate premake projects** after adding files).

```cpp
namespace GanymedE {

	// One decode path for every asset-layer texture load. The stb flip flag is
	// global state; both entry points set it explicitly immediately before
	// decoding so no call site depends on what a previous load left behind.
	class TextureImporter
	{
	public:
		static Ref<Texture2D> LoadFromFile(const std::filesystem::path& fullPath,
		                                   bool flipVertically = false);
		static Ref<Texture2D> LoadFromMemory(const uint8_t* bytes, size_t size,
		                                     bool flipVertically = true);
	};
}
```

Both decode with stb forced to 4 channels, then share one private upload helper:
`Texture2D::Create(w, h)` + `SetData(pixels, w*h*4)` + `stbi_image_free`. Do **not** route
through the `Texture2D(path)` constructor — it bakes `flip = 0` internally. That constructor
stays for editor chrome only (`EditorLayer.cpp` icons, `ContentBrowserPanel.cpp` icons);
editor icons use hard-coded `resources/` paths and deliberately stay outside the asset cache.

**Flip policy: preserve current behavior exactly.** File-based material textures load
unflipped (matches `Texture2D(path)` today); embedded glTF images load flipped (matches the
current `stbi_set_flip_vertically_on_load(1)` in both `MeshImporter` and `MeshCache`). The
discrepancy becomes an explicit parameter with a comment instead of ambient global state.
Actually reconciling the two orientations changes rendering on one path — flagged follow-up,
not part of this phase.

### 1.2 `GetAsset<Texture2D>` + header specialization declarations

`AssetManager.h`:

- Forward-declare `Texture2D`; add private `static Ref<Texture2D> LoadTexture(AssetHandle);`.
- Replace the bare `GetAsset` declaration with a *defined* primary template that hard-fails,
  plus namespace-scope specialization declarations after the class:

```cpp
	// in class AssetManager:
	template<typename T>
	static Ref<T> GetAsset(AssetHandle handle)
	{
		static_assert(sizeof(T) == 0,
			"AssetManager::GetAsset<T> is only specialized for Mesh, Environment, Texture2D");
		return nullptr;
	}

// after the class, namespace scope:
template<> Ref<Mesh>        AssetManager::GetAsset<Mesh>(AssetHandle handle);
template<> Ref<Environment> AssetManager::GetAsset<Environment>(AssetHandle handle);
template<> Ref<Texture2D>   AssetManager::GetAsset<Texture2D>(AssetHandle handle);
```

This fixes the existing use-before-declaration of the specializations and converts
"unsupported T links fine, fails at link with an opaque error" into an immediate compile
error with a message. Every future asset type (Phase 2's in-mesh skeleton data does *not*
need one — it rides `GetAsset<Mesh>`) adds one declaration + one definition + one cache map.

`AssetManager.cpp`:

- `LoadedTextures` map joins `AssetManagerData`; cleared in `Shutdown()` with the others.
  This inherits the existing GPU-lifetime ordering: `Shutdown` runs from
  `EditorLayer::OnDetach` while `Renderer::IsGpuAlive()` is still true, so the `Texture2D`
  destructors release real bgfx handles instead of tripping the is-alive guard.
- `LoadTexture` mirrors `LoadEnvironment` step for step: handle guard → cache hit →
  `GetMetadata` + `Type == AssetType::Texture` check → resolve against `GetAssetRoot()` →
  `TextureImporter::LoadFromFile(fullPath, false)` → insert on success. Add a
  `GE_CORE_TRACE` on cache hit; the verification checklist uses it.
- The cache-hit path stays a plain `unordered_map` lookup — `RenderSystem` calls
  `GetAsset<Mesh>`/`GetAsset<Environment>` every frame per entity and any texture consumer
  may end up on that path too.
- Extend `AssetTypeFromExtension` (`AssetTypes.cpp`) if sample content needs more image
  extensions (e.g. `.tga`). The extension mapping is not ordinal-persisted — safe to extend.
  The `AssetType` **enum** is ordinal-persisted in the registry YAML: append-only, never
  reorder.

### 1.3 Consolidate the material-texture call sites (the de-dup payoff)

Today two meshes sharing `albedo.png` produce two decodes and two GPU textures, and the
decode logic is duplicated between cold import and cache replay.

- `MeshImporter.cpp` `CreateTextureFromImage`:
  - **URI case**: keep the relative-path computation; when the image resolves under the asset
    root, route through the manager — `ImportAsset(relative)` (idempotent) +
    `GetAsset<Texture2D>(handle)`. This is the de-duplication: one decode, one bgfx texture,
    shared `Ref`. Images outside the asset root fall back to
    `TextureImporter::LoadFromFile(imagePath, false)` directly.
  - **Embedded (`buffer_view`) case**: `TextureImporter::LoadFromMemory(bytes, size, true)`;
    keep capturing the compressed bytes for the mesh cache. Embedded images have no file
    identity, hence no registry entry and no de-dup in this phase (content-hash de-dup is a
    possible later refinement).
- `MeshCache.cpp` `ReadMaterials`: delete the duplicated `CreateTextureFromEmbedded`;
  path-based maps go through `ImportAsset` + `GetAsset<Texture2D>`; embedded blobs through
  `LoadFromMemory`.

**Intentional side effect** (state it in the commit message): `AssetRegistry.gr` now
accumulates `Texture` entries as meshes load. Registry-save hygiene is out of scope.

### 1.4 Delete dead code

`Renderer3D::LoadEnvironment` and its path-keyed `EnvironmentCache` (declared in
`Renderer3D.h`, defined in `Renderer3D.cpp`) are leftovers from the pre-handle era and are
never called — `AssetManager::LoadEnvironment` superseded them. Delete before anyone copies
them as a model. `s_Data.ActiveEnvironment` **stays** — it is live, refreshed by
`SubmitEnvironment` each frame.

### 1.5 `AssetManager::Reload(handle)`

`static void Reload(AssetHandle handle);` — switch on the metadata type:

| Type | Action |
|---|---|
| Texture, Environment | Erase from the per-type map. Next `GetAsset` reloads from disk. |
| StaticMesh | Three steps, in order: (1) walk the cached mesh's materials' `Get*MapPath()`s, resolve each via `GetHandle(path)`, erase those from `LoadedTextures` — otherwise the reloaded mesh silently rebinds stale cached textures; (2) erase from `LoadedMeshes`; (3) `MeshCache::Invalidate(sourceRelativePath)` — a new function that removes the `.meshcache` file. |

Why (3): the cache's timestamp check already catches *source* edits, but Reload is a user
action meaning "reimport now" — e.g. a referenced external texture changed while the `.gltf`
timestamp did not.

**Why plain eviction is safe** (this is the invariant, record it in `assets.md` when
implementing): components store handles, not `Ref`s. `RenderSystem` re-fetches by handle
every frame, so eviction lands next frame; the inspector fetches per draw; the serializer
only warms; `Renderer3D::s_Data.ActiveEnvironment` is overwritten next submit. Evicted
objects drain via `shared_ptr` refcount and bgfx defers handle destruction to frame end, so
evicting mid-frame from ImGui code cannot pull a texture out from under an in-flight draw.
The consumer rule going forward: **re-fetch by handle each frame, or accept staleness across
Reload.**

Editor exposure: a right-click context menu ("Reload") on file items in
`ContentBrowserPanel.cpp`, shown only when `GetHandle(relativePath)` is valid.

### 1.6 Typed drag-drop helper — `EditorUI::AcceptAssetDrop`

New files `GanymedEditor/source/AssetDragDrop.h/.cpp`.

```cpp
namespace GanymedE::EditorUI {

	// Call immediately after the widget that should accept drops. Wraps
	// BeginDragDropTarget / AcceptDragDropPayload("CONTENT_BROWSER_ITEM") /
	// EndDragDropTarget. Returns the dropped item's path (relative to assets/)
	// iff AssetTypeFromExtension matches `type`.
	std::optional<std::filesystem::path> AcceptAssetDrop(AssetType type);

	// Convenience for component handle fields: ImportAsset (idempotent) on match.
	AssetHandle AcceptAssetDropHandle(AssetType type);
}
```

This makes `AssetTypeFromExtension` the single source of truth for the editor too, replacing
five copy-pasted `AcceptDragDropPayload` → `extension()` → `::tolower` → string-compare
blocks:

1. `EditorLayer.cpp` viewport — `AcceptAssetDrop(AssetType::Scene)` → `OpenScene`;
   `AcceptAssetDrop(AssetType::StaticMesh)` → the Edit-state-guarded
   `MeshImporter::Instantiate`.
2. `SceneHierarchyPanel.cpp` Static Mesh inspector — `AcceptAssetDropHandle(StaticMesh)`.
3. Script inspector — `AcceptAssetDropHandle(Script)`, **keeping `component.Fields.clear()`**
   inside the match (field overrides are keyed against the old script).
4. Sky Light inspector — `AcceptAssetDropHandle(Environment)`.

Multi-type targets (the viewport accepts Scene *and* StaticMesh) call the helper twice in a
row after the same widget — each call opens/closes its own target against the same last item
and the extension filters are disjoint. **Verify this during the smoke test**; if ImGui
rejects a double-`BeginDragDropTarget` on one item, fall back to an
`initializer_list<AssetType>` overload returning `{type, path}`.

> **Resolved during execution (2026-08-11): the double call does not work; the
> `initializer_list` overload is what shipped.** `ImGui::EndDragDropTarget` calls
> `ClearDragDrop()` as soon as `payload.Delivery` is set, and `BeginDragDropTarget` early-returns
> on `!g.DragDropActive` — so on the frame the drop lands, the second call sees no active drag.
> The premise that "the extension filters are disjoint" does not save it: both calls request the
> same `CONTENT_BROWSER_ITEM` payload type and the extension filter is applied *after* accepting,
> so the first call unconditionally wins the delivery. Dropping a `.glb` on the viewport would
> have silently done nothing.

### 1.7 Sequencing and docs

Commit order: (1) 1.1–1.4 as one change (loader + cache + consolidation + dead code);
(2) 1.5 Reload; (3) 1.6 drag-drop helper. Docs travel in the same commit as the code they
describe (per `AGENTS.md`): `docs/engine/assets.md` gains the texture load path, the Reload
semantics, and the re-fetch invariant; `docs/editor/editor.md` gets the drag-drop section
rewritten around `AcceptAssetDrop` plus the content-browser Reload entry.

### Phase 1 verification

1. Regenerate projects; MSBuild x64 Debug — engine lib, editor, sandbox all link.
2. Temporarily compile `AssetManager::GetAsset<int>(h)` → the static_assert message fires.
   Remove.
3. Smoke every drop target: `.ganymede`→viewport opens scene; `.glb`→viewport instantiates
   (Edit state only); `.gltf`→mesh inspector assigns; `.lua`→script assigns *and clears
   Fields*; `.hdr`→skylight assigns. Negatives: `.lua` on mesh, `.hdr` on script — silently
   ignored.
4. De-dup observable: two `.gltf`s referencing one shared external `.png` → exactly one
   "Imported asset" log for the png and a cache-hit trace on the second mesh load; one
   registry entry; bgfx debug-stats `numTextures` doesn't double.
5. Flip regression: one `.glb` (embedded textures) and one `.gltf` (external png) render
   identically to pre-change.
6. Reload: mesh placed in scene and selected in the inspector → edit the source file →
   right-click Reload → fresh import logs (no cache-hit), viewport updates next frame, no
   crash.
7. Existing scenes load unchanged; a play/stop cycle is clean.

---

## Phase 2 — Import: skeleton, clips, skinned vertex data

**Goal:** a rigged `.glb` imports with skeleton + clips stored inside the `Mesh` asset,
survives the mesh cache, and every existing static mesh renders bit-identically to before.

### 2.1 Skin data rides a second vertex stream

**Decision: separate stream, not a widened `MeshVertex`.** Widening taxes every static mesh
(+32 bytes/vertex), forces a cache migration for all content, and touches the one struct the
cache memcpy's wholesale. bgfx natively supports multiple vertex streams
(`setVertexBuffer(0, static)` / `setVertexBuffer(1, skin)`), so skinned meshes carry an
optional second buffer and static meshes are untouched:

```cpp
struct SkinVertex
{
	glm::vec4 JointIndices;   // 4 joints as floats (v1)
	glm::vec4 JointWeights;   // normalized
};
```

Joints ship as floats first: `AttribTypeFromShaderType` in `Renderer/Buffer.h` forces
everything to `AttribType::Float` today, and packing to Uint8 is an optimization, not a
requirement. `AttribFromName` (same header — it *asserts* on unknown names) gains
`a_JointIndices → bgfx::Attrib::Indices` and `a_JointWeights → bgfx::Attrib::Weight`, and the
attribute table in `assets/shaders/src/varying.def.sc` gets matching rows — its header comment
already demands the two stay in sync.

> **Correction, made while executing.** The plan originally named these `a_indices`/`a_weight`
> on both sides. That conflates two namespaces: `AttribFromName`'s keys are the *engine's*
> `BufferElement` names (`a_CapitalCase`), while `a_indices`/`a_weight` are shaderc's fixed
> vertex-input names (`s_allowedVertexShaderInputs` in `tools/shaderc/shaderc.cpp` — anything
> else is rejected outright). So the three-column table reads
> `a_JointIndices → BLENDINDICES → a_indices`, matching how every other row already works.

**Skin vertices are parallel to the position stream, not a compacted array.** Both bgfx streams
are bound with a single `startVertex`, so a mixed static/skinned file must still carry a
`SkinVertex` for every vertex — zeroed for the static ones. `Mesh::GetSkinVertices()` is
therefore either empty or exactly `GetVertices().size()` long; `Mesh`'s constructor drops the
skin data outright if that invariant is violated.

### 2.2 Runtime-facing types

New header `GanymedEngine/source/GanymedE/Renderer/Animation.h`:

```cpp
struct Skeleton
{
	static constexpr uint32_t MaxBones = 128;

	// Parents-before-children order, so one forward pass composes globals.
	std::vector<int32_t>     ParentIndices;   // -1 = root
	std::vector<glm::mat4>   InverseBind;
	std::vector<JointPose>   LocalRestPose;   // TRS with quat rotation — NOT matrices
	std::vector<std::string> JointNames;

	// Everything above the root joints in the glTF scene graph. See below.
	glm::mat4 RootTransform{ 1.0f };
};

struct AnimationClip
{
	struct Channel
	{
		uint32_t              Joint;
		enum class Path       { Translation, Rotation, Scale } Target;
		enum class Interp     { Linear, Step } Mode;
		std::vector<float>    Times;      // sorted; sampled by binary search
		std::vector<glm::vec4> Values;    // quat in xyzw for Rotation
	};
	std::string          Name;
	float                Duration;
	std::vector<Channel> Channels;
};
```

Rest pose is TRS with quaternions, not matrices: rotation keys must slerp, and future
blending needs poses, not matrices. Joints never become entities —
`TransformComponent` stores Euler angles (lossy for quat keys) and `WorldFromLocals` does
per-level UUID lookups; a 60-joint rig would pay all of that per joint per frame plus flood
the serializer and `ComponentList` for no benefit. The flat array matches how the palette
must be uploaded anyway.

`Mesh` gains: `Skeleton m_Skeleton` (+`HasSkeleton()`), `std::vector<AnimationClip> m_Clips`
(+`FindClip(name)`), `std::vector<SkinVertex> m_SkinVertices`, `Submesh::IsSkinned`.

> **`RootTransform`, added while executing.** The original `Skeleton` had nowhere to put the
> transform of whatever sits *above* the root joints — Blender's `Armature`, a Y-up correction
> node. Composing globals purely from `LocalRestPose` + `ParentIndices` is wrong whenever that
> node is not identity, and it usually isn't: the inverse bind matrices come from the file and
> already include it, so at bind pose you get
> `Global_ours[i] * InverseBind[i] = inverse(Armature) * GlobalBind[i] * inverse(GlobalBind[i])`
> `= inverse(Armature)` instead of identity — the whole character transformed by the inverse of
> its own armature node. Of the three verification models, CesiumMan and RiggedFigure both have
> a non-identity one.
>
> It is stored separately rather than baked into the root joints' `LocalRestPose` because
> animation channels *replace* joint locals wholesale and would clobber it on the first
> translation key. Global composition (Phase 3, step 3) is therefore:
>
> ```cpp
> Global[i] = Parent[i] >= 0 ? Global[Parent[i]] * Local[i]
>                            : Skeleton.RootTransform * Local[i];
> ```
>
> Root joints under *different* parents are possible in principle; the importer warns and uses
> the first. No sample rig does this.

### 2.3 Importer changes (`MeshImporter.cpp`)

- Parse the **first** `cgltf_skin` only; warn if the file has more (v1 degradation, logged).
- **Topologically sort the joints and remap `JOINTS_0` through the sort.** The plan assumed
  `skin.joints` already arrives parents-before-children; glTF guarantees no such ordering. The
  sort is what makes Phase 3's single-forward-pass composition legal — without it the
  guarantee stated in 2.2 is just a hope.
- Joints via `cgltf_accessor_read_uint` — **not** `cgltf_accessor_read_float`, which
  normalizes integer accessors. Weights via `cgltf_accessor_unpack_floats`, then
  renormalize (exporters ship non-normalized weights more often than you'd think).
  Out-of-range joint indices are clamped to 0; a stray index would otherwise sample past the
  end of the palette uniform.
- Inverse binds from the skin's accessor; identity fallback if absent (spec allows it).
- **Conditional world-bake.** Skinned primitives skip the world-space bake entirely — their
  vertices stay in skin space, because glTF skinning places them via
  `jointMatrix = globalJointTransform * inverseBindMatrix`. Static primitives keep the
  current baking, byte for byte. `Submesh::IsSkinned` is the **single gate** every downstream
  consumer honors (color pass, shadow pass, bounds).
- Clips: LINEAR and STEP supported; CUBICSPLINE degraded to linear with a warning (v1).
  Duration = max input time across channels.
- **Fix nondeterministic submesh order while in the file**: replace the
  `unordered_map<node*, mat4>` traversal with an explicit DFS over scene roots collecting
  into a vector. Today's order changes run to run, which makes cache diffs and joint↔submesh
  correlation flaky. (The map survives as a lookup table for `RootTransform`; only the
  *iteration* moved to the vector.)
- **Non-indexed primitives.** Pre-existing gap found by the verification below: the importer
  bailed on any primitive without an index accessor, which glTF permits (triangle soup in draw
  order) and Khronos' Fox uses — so Fox imported as "no triangle geometry". Since the engine
  always draws indexed, a trivial `0..n-1` list is synthesized. Unrelated to skinning, fixed
  here because it blocks this phase's own checkpoint.

### 2.4 Cache format v4 (`MeshCache.cpp`)

Bump `MESH_CACHE_VERSION` 3 → 4; serialize skeleton (parent indices, inverse binds, rest
pose, names), clips, skin vertices, and `Submesh::IsSkinned`. The version bump
auto-invalidates every existing cache on first load — that *is* the migration.

### Phase 2 verification

- Import the Khronos sample models **Fox, CesiumMan, RiggedFigure**; log joint counts and
  clip names/durations and check them against a glTF viewer.
- Cache roundtrip: cold import vs cache load produce identical skeleton/clip/skin data.

Results (temporary probe in `EditorLayer::OnAttach`, since import needs a live bgfx device;
probe removed afterwards). Cold import and cache load agreed on every field:

| Model | Joints | Clips | Verts / skin verts | `RootTransform` |
|---|---|---|---|---|
| Fox | 24 | Survey 3.417s, Walk 0.708s, Run 1.158s — 21 channels each | 1728 / 1728 (non-indexed) | identity |
| CesiumMan | 19 | 1 × 2.000s, 57 channels | 3273 / 3273 | non-identity (Y-up node) |
| RiggedFigure | 19 | 1 × 1.250s, 57 channels | 370 / 370 | non-identity (90° X) |
| BoxTextured (static control) | 0 | 0 | 24 / 0 | — |

> **Corrected in Phase 3.** The `RootTransform` values recorded here were wrong for the
> non-identity cases: they omitted the `inverse(meshNodeWorld)` term the glTF spec requires. See
> the Phase 3 execution notes. Nothing else in this table changed.

57 channels = 19 joints × 3 paths, and 24 joints for Fox, both matching the published models.
- Every existing static scene renders pixel-identically (the conditional bake must be a
  no-op for static content).
- A skinned model imported but not yet animated renders **wrong** until Phase 4 (skin-space
  vertices drawn by the static shader). Acceptable mid-milestone state; do not ship a demo
  scene with a rigged model until Phase 4 lands.

---

## Phase 3 — Runtime: `AnimatorComponent` + `AnimationSystem`

**Goal:** a component that names a clip and a system that produces a correct joint palette
every frame, in play mode *and* in the editor.

### 3.1 Component design

**Decision: reuse `StaticMeshComponent`; add `AnimatorComponent`.** An entity is skinned iff
its mesh asset `HasSkeleton()` *and* it has an animator. A separate `SkinnedMeshComponent`
would duplicate drag-drop, serialization, inspector, and RenderSystem plumbing for zero
information — the asset already knows it is skinned.

```cpp
struct AnimatorComponent
{
	std::string Clip;                  // by name into the mesh's clips
	float       Speed   = 1.0f;
	bool        Playing = true;
	bool        Loop    = true;
	float       Time    = 0.0f;        // serialized as 0

	// Runtime-only, rebuilt every frame; cleared by the Scene::Copy fixup.
	std::vector<glm::mat4> Palette;
};
```

**Palette lives on the component** (not in system-owned storage): lifetime is tied to the
entity, `Scene::Copy` handles it generically, and `RenderSystem` reads it through declared
access (`RO<AnimatorComponent>`) instead of reaching into another system's private map. The
play-mode snapshot needs a fixup sweep clearing `Palette` after the copy — same shape as the
existing `NativeScriptComponent::Instance = nullptr` sweep in `Scene::Copy`.

**Untracked** (no `ComponentTraits` specialization): the system polls every frame, so change
tracking buys nothing, and untracked means Lua setters don't need `MarkChanged` pairing.

New-component checklist (the two hand-maintained lists have no compile-time enforcement —
forgetting them fails silently): struct in `Components.h`; `ComponentList` entry in
`ECS/ComponentTraits.h`; `SerializeEntity` + `Deserialize` blocks in `SceneSerializer.cpp`
(Clip/Speed/Playing/Loop; Time and Palette are not persisted); Add Component menu entry and
`DrawComponent<AnimatorComponent>` in `SceneHierarchyPanel.cpp` (Phase 5 fleshes out the UI).

### 3.2 `AnimationSystem`

New `Scene/Systems/AnimationSystem.h/.cpp`, CRTP `ECS::System<AnimationSystem>`:

```cpp
using AnimView = ECS::IterView<ECS::EntityId,
	ECS::RW<AnimatorComponent>, ECS::RO<StaticMeshComponent>>;
using Views = TypeList<AnimView>;
```

No reactive views → no editor drain obligation. **Registration: after `LuaScriptSystem`,
before `TransformSystem`** in the `Scene` constructor — scripts set clip state this frame and
the palette reflects it the same frame. Placement relative to `PhysicsSystem` is unconstrained
(no shared component writes) and `ValidateOrdering` cannot arbitrate writer-vs-writer anyway;
the chosen order is a documented decision, not an enforced one.

Per-frame update:

1. `Time += ts * Speed` when `Playing`; wrap (Loop) or clamp (one-shot) by clip duration.
2. Per channel: binary-search the key pair, lerp translation/scale, **slerp** rotation
   (STEP holds the left key).
3. Compose local poses over the rest pose, one parents-first pass (the importer sorts the
   array, see 2.3) for globals — seeding roots with `Skeleton::RootTransform`, not identity —
   then `Palette[i] = Global[i] * InverseBind[i]`.
4. Unknown clip name (the failure mode of clips-by-name, e.g. a DCC rename): warn **once**
   per component, output the bind pose (identity palette). Loud, not broken.

**`OnUpdateEditor` samples too.** Editor preview is wanted — scrubbing Time in the inspector
must move the model. This deliberately diverges from the "systems simulate only in play mode"
norm; say so in the system's header comment and in `scene.md` when documenting. Reset per-run
state on `OnRuntimeStart` (borrow the discipline from `PhysicsSystem`, but **not** its
fixed-step accumulator — animation is per-frame variable-step by design; note the asymmetry:
physics writeback is alpha-interpolated, animation is exact-at-t, visible only if a clip ever
drives a kinematic body).

### Phase 3 verification

- Log one joint's palette matrix at t=0 vs t=0.5s — values change and match a reference
  viewer's pose.
- Scrub Time via a temporary DragFloat — palette responds in edit mode.
- Play/stop resets Time; scene save/load roundtrips Clip/Speed/Playing/Loop.
- `ValidateOrdering` passes with the new registration (it will catch AnimationSystem placed
  after TransformSystem, since TransformSystem reads transforms via ChangeView — leave a
  comment at the registration site anyway).

### Phase 3 execution notes

**"Samples too" was implemented as sample-without-advance.** Edit mode evaluates the pose at the
current `Time` but never runs the clock. Everything 3.2 asks for is satisfied by evaluation;
advancing as well would leave every rigged model in the scene permanently moving while placing
things, and `Time` is not serialized so it would drift with no record. A preview-play toggle, if
wanted, belongs with Phase 5's inspector work.

**The `ValidateOrdering` claim above is wrong.** The check only fires on a reader-vs-writer pair
over a *shared* component. `AnimationSystem` writes `AnimatorComponent` and reads
`StaticMeshComponent`; `TransformSystem` reads `TransformComponent`/`RelationshipComponent` and
writes `WorldTransformComponent`. They have nothing in common, so placing AnimationSystem *after*
TransformSystem passes validation silently. The slot before TransformSystem is a documented
intention only. The slot after the script systems is unenforceable for a different reason already
noted in 3.2 (writer-vs-writer). The one part that ever becomes checked is staying ahead of
`RenderSystem`, and only once Phase 4 makes it declare `RO<AnimatorComponent>`. A comment at the
registration site says all of this.

**Phase 2 bug found by Phase 3: `RootTransform` was missing the mesh-node inverse.** Composing a
rest-pose palette is the first thing that can actually falsify the Phase 2 import, and it did.
Per the glTF spec the transform of the node a skinned mesh hangs off MUST be ignored, with the
joint matrices carrying `inverse(meshNodeWorld)` instead — skinning output is already in mesh
space, so applying the node transform as well applies it twice. Phase 2 captured only the root
joints' ancestor transform. `RootTransform` is now
`inverse(skinnedMeshNodeWorld) * rootJointParentWorld`, and the mesh cache is at **v5** to discard
caches holding the old value (same layout — the *meaning* of a field changed, which is just as
much a reason to bump).

The test that caught it, and the one to reuse: **at the rest pose `Global[i] * InverseBind[i]` must
be identity for every joint.** Note that Fox cannot detect this class of bug — every node in it is
at identity, so it passed both before and after the fix. Always validate skinning against a model
with a non-trivial node hierarchy.

Results (temporary probe in `EditorLayer::OnUpdate` across successive frames, removed afterwards):

| Check | Result |
|---|---|
| Fox clips resolve by name | Survey 3.4167s, Walk 0.7083s, Run 1.1583s — matches Phase 2 |
| Fox palette at t=0 / 0.100 / 0.354 | 24 joints, all three distinct and non-degenerate |
| Fox rest pose max deviation from identity | 1.0e-5 |
| CesiumMan rest pose, before the RootTransform fix | **1.0** (a pure 90° rotation, zero translation) |
| CesiumMan rest pose, after | 1.0e-6 across all 19 joints |
| Unknown clip name | one warning, bind pose held, no repeat on later frames |
| Edit mode does not advance Time | Time exactly 0.1000 after a frame with `Playing = true` |

---

## Phase 4 — Rendering: the skinned draw path

**Goal:** the palette moves the vertices, in the color pass *and* the shadow pass.

### 4.1 Shaders

The toolchain is drop-in: new `.sc` files in `assets/shaders/src/` are picked up by
`scripts/compile_shaders.*` globs, no script edits; per-shader varyings follow the
`varying.<Name>.def.sc` convention.

- `vs_PhongSkinned.sc` — `vs_Phong` plus `a_indices, a_weight` inputs; skin position, normal,
  and tangent by the 4-weight blend of `u_Bones[]`, **then** apply the instance model matrix
  from `i_data0..3` exactly as `vs_Phong` does (submitted with instance count 1 — one
  vertex-input convention everywhere). Reuse `fs_Phong` unchanged in a new `PhongSkinned`
  program; varyings identical. `varying.PhongSkinned.def.sc` per convention.
- `vs_ShadowDepthSkinned.sc` — `a_position + a_indices + a_weight + i_data`, paired with the
  existing shadow fragment shader. **Same phase, not a follow-up**: T-pose shadows under an
  animating character read as a bug.
- `.sc` gotchas apply (`vec3_splat`, `mul(m,v)`, `mtxFromCols`, varyings only in `main`'s
  signature — see `rendering.md`).

### 4.2 Bone palette

**`MaxBones = 128`.** One `u_Bones` mat4[128] uniform — a *single handle* against
`BGFX_CONFIG_MAX_UNIFORMS = 512` (the limit counts handles, not vec4s), and 512 vec4s sits
comfortably inside D3D11's 4096-vec4 cbuffer. Humanoid rigs run 60–90 joints. Caveat to
document: GL's minimum `MAX_VERTEX_UNIFORM_VECTORS` is 256, so a minimal GL fallback may not
link — the primary backends are D3D/Vulkan/Metal; make it a compile-time constant and note
it, don't engineer around it. bgfx fixes an array uniform's size at creation, so the array is
always MaxBones; the live joint count goes through `Shader::SetMat4Array`'s count argument.
Rigs over 128 joints: warn at import, clamp (draw wrong, loudly).

### 4.3 Renderer3D

- `Mesh::Build` creates the optional second vertex buffer when skin vertices exist (skin
  stream layout: Float4 `a_indices`, Float4 `a_weight`).
- New `Renderer3D::SubmitSkinnedMesh(mesh, transform, palette, jointCount, entityID)`.
  Skinned commands go into their own list and **bypass the instancing merge** — one submit
  per skinned entity: `SetMat4Array(u_Bones, ...)`, bind stream 0 + stream 1, then the
  existing `DrawIndexedInstanced` with a single-entry transient instance buffer (entity-ID
  picking keeps working unchanged). Sorted into the existing opaque/transparent ordering.
- **Copy the palette into the command buffer** at submit (≤8 KB worst case per draw). This
  matches how instance data is already staged CPU-side and removes any lifetime coupling
  between the component's vector and the flush.
- `RenderSystem`: entity has an animator *and* the mesh `HasSkeleton()` → skinned submit with
  `animator.Palette`; otherwise the current static path. Submeshes with `IsSkinned == false`
  in a mixed file still go down the static path.
- Skinned bounds: bind-pose AABB inflated by a fixed factor — a known, documented
  approximation (exact skinned bounds need per-frame vertex work; not worth it here).

Accepted cost: N skinned characters = N submits + N palette uploads. The escape hatch
(palette-in-texture instanced skinning) is a different milestone — see "Not doing".

### Phase 4 verification

- CesiumMan/Fox animate in the viewport at correct speed; pose matches a reference viewer.
- Shadows deform with the mesh (all four cascades).
- A static-only scene's draw-call count is unchanged pre/post (bgfx stats) — the skinned
  branch must not perturb batching.
- Entity picking works on a skinned entity mid-animation.
- Two skinned entities play *different* clips simultaneously and independently.

### Phase 4 execution notes

**No `varying.PhongSkinned.def.sc` was needed.** 4.1 called for one "per convention", but the shared
`varying.def.sc` already declares `a_indices`/`a_weight` (Phase 2 added them when it documented the
attribute table), and the skinned varyings are identical to Phong's. A per-shader varying file is
for layouts that *differ* from the engine's, which is why only ImGui and RmlUi have one.

**The loader forbade reusing `fs_Phong`, which 4.1 assumed was free.** `Shader` paired `vs_<name>`
with `fs_<name>`, so a `PhongSkinned` program could only have had a `fs_PhongSkinned` — a duplicate
of a 300-line PBR shader existing solely to satisfy a naming rule. `Shader::CreateFromStages(name,
vertexStage, fragmentStage)` names the two stages independently instead. Both skinned programs use
it. This was not in the plan and is the one piece of new engine surface Phase 4 added beyond the
skinning path itself.

**Skinned commands stayed in the main draw list**, not "their own list" as 4.3 specified: a `bool
IsSkinned` and a `uint32_t PaletteOffset` on `DrawCommand`. A separate list would have needed its
own copy of the culling, the opaque/transparent split, the depth sort and the material-bind cache to
get the same ordering the plan also demanded. The merge loop instead grew guards that exclude
skinned commands from a run. The cost of that choice is that the guards are load-bearing in a way a
separate list would not have been — hence measuring static batching rather than trusting it.

**Palettes are staged once per entity, not per draw.** 4.3 said "copy the palette into the command
buffer"; a mesh with several skinned submeshes would then copy the same 8 KB once per submesh. All
submeshes of one submit share an offset into a frame-lifetime `PaletteStorage`.

**Two bgfx behaviours had to be designed around, both of the same shape: a uniform or texture set
twice with no submit between is fatal, not an overwrite.**
- `u_LightSpaceMatrix` is set once per cascade and covers the skinned draws too. Setting it again
  inside the skinned caster loop asserts — but only in a scene with *no* static casters, since
  otherwise a static submit intervenes. Worth noting how narrow that repro is.
- `DrawSkinnedCommand` calls `bgfx::discard()` on its early-out rather than returning, because the
  caller has already queued the material's uniforms and textures for a draw that is not going to
  happen.

Also: a skinned draw must rebind its material even when the material pointer is unchanged (bgfx
discards texture bindings at submit), and must invalidate the `boundMaterial` cache on the way out,
or the next static draw inherits the skinned program and reads a stream 1 that is not bound.

**Phase 2 bug found by Phase 4: skinned submeshes were losing their `LocalTransform`.** The import
reset `LocalTransform` to identity for *every* submesh, correct for static ones whose vertices were
baked to world space and wrong for skinned ones. `Skeleton::RootTransform` carries
`inverse(skinnedMeshNodeWorld)` (the Phase 3 correction), and nothing was re-applying the matrix it
was meant to cancel — so a file with a Y-up correction node rendered its character lying on its
side. Skinned submeshes now keep the node transform, `Renderer3D` re-applies it as
`worldTransform * LocalTransform`, and the two cancel. Mesh cache is at **v6** to discard caches
holding the identity.

Note the shape of this bug and of the Phase 3 one: both were a *half* of a cancellation, and both
came from design tension 5 (conditional baking) — one mesh holding world-baked static and
bind-space skinned vertices means every consumer of `Submesh` needs the `IsSkinned` gate, and the
two that were missed were the two that looked like they had nothing to do with skinning.

**Bounds padding is a fraction of the largest extent, not per axis.** A per-axis 50% pad let
CesiumMan's arms leave its box: it stands arms-down with an X extent of 0.31 against a height of
1.51, and a walk cycle swings a limb about as far as the rig is long regardless of how thin the bind
pose is on that axis. `SkinnedBoundsPadding` is 25% of `max(extent)` applied to every axis.

Results (temporary probe in `EditorLayer`, two `BoxTextured` + two `Fox` entities on different
clips, removed afterwards):

| Check | Result |
|---|---|
| Static-only scene | 7 draws, 5 instanced, 0 skinned, 0 culled |
| Same scene + 2 skinned entities | 17 draws, **5 instanced** (unchanged), 10 skinned |
| Skinned draws per entity | 5 = 1 color pass + 4 cascades — every cascade, as intended |
| Fox skin data | 24 joints, 1728 skin vertices, stream 1 created, 1/1 submeshes skinned |
| Stream 1 layout (bgfx log) | `Attrib::Indices` num 4 + `Attrib::Weights` num 4, float, stride 32 |
| `u_Bones` uniform | one handle, `num 128`; both skinned programs report `r.count 512`, CB size 8256 B |
| `PhongSkinned` fragment stage | resolves to `fs_Phong`'s constants (CB 2656 B) — the reuse works |
| Two entities, different clips, same frame | Walk vs Run at t=0: 24/24 joints, max palette element difference 61.7 |
| Skinned submesh bounds | padded extent (102.5, 156.4, 232.1) against a bind-pose (25.2, 79.0, 154.7) |

**Not verified programmatically: entity picking on a skinned entity.** The probe projected the posed
AABB and read the entity-ID attachment back across a 5×5 grid of pixels inside it, and got −1
everywhere — but so did a *static* box used as a control, so the probe's readback harness is what
failed, not the skinned path. The skinned draw builds the same `MeshInstanceData` (transform +
entity ID) and submits with instance count 1 through the same `DrawIndexedInstanced*` entry point as
a static draw, and its fragment stage *is* `fs_Phong`, so there is no code path by which its ID
would differ; that is an argument, not a measurement. Worth a separate look at why an off-mouse
`RequestEntityID`/`PollEntityID` pair reads cleared pixels — it is not a Phase 4 change and hover
picking works normally in the editor.

**Pose correctness** was established the other way round: the lying-down bug above was spotted by eye
in the viewport, and the fix checked by replaying the vertex shader's blend on the CPU and confirming
the result stands upright in Y. Matching a reference viewer frame-for-frame was not done.

**Shadow deformation** is verified structurally, not visually: the skinned depth program is issued
once per cascade per skinned caster (the 10-draw figure above), which is the mechanism. That the
resulting silhouette tracks the pose follows from `vs_ShadowDepthSkinned` running the same blend as
`vs_PhongSkinned`, which is read, not measured.

---

## Phase 5 — Editor, scripting, docs

**Goal:** the feature is usable without touching C++.

- **Inspector** (`SceneHierarchyPanel.cpp`): clip combo populated from the sibling mesh's
  clip names (disabled with a hint when the entity has no skinned mesh), Play/Pause, Time
  scrub (drag pauses and sets Time), Speed drag, Loop checkbox.
- **Lua** (`ScriptBindings.cpp`; single file on purpose — `scripts-src/types/ganymed.d.ts`
  mirrors it by hand; bind by value, never references into entt storage):
  `PlayAnimation(name)` (sets Clip, Time=0, Playing=true — hard cut, no crossfade),
  `StopAnimation()`, `SetAnimationSpeed(f)`, `SetAnimationLooping(b)`,
  `IsAnimationPlaying()`, `GetCurrentAnimation()`. Component is untracked → no `MarkChanged`
  pairing. Mirror every signature in `ganymed.d.ts`.
- **Docs audit** (each item belongs to the phase that caused it; listed here as the final
  sweep): `scene.md` — component catalog + built-in systems + serialization note, and fix the
  pre-existing stale "five built-in systems" (six today, seven after AnimationSystem);
  `ecs.md` — the named system chain in the SystemManager section; `rendering.md` — skinning
  path, the two new programs, MaxBones, the non-instanced skinned submit; `assets.md` —
  skeleton/clips inside the Mesh asset, cache v4; `editor.md` — Animator inspector;
  `scripting.md` — the new bindings.

### Phase 5 verification

- A Lua script switches clips on a key press in play mode.
- A scene with an animator survives save → load → play → stop.
- `ganymed.d.ts` matches the bindings exactly; every doc claim spot-checked against code.

---

## Explicitly not doing (v1)

Same spirit as `3D_ROADMAP.md` Phase 7 — named so nobody half-builds them in passing:

- **Blend trees / state machines** — single active clip, hard cut on `PlayAnimation`. A
  linear crossfade is the *first* acceptable follow-up, and it's a follow-up.
- **GPU/compute skinning and instanced skinning** (palette-in-texture) — a different
  milestone; the one-submit-per-character cost is accepted at this scale.
- **Morph targets**, **retargeting** (clips bind to their own mesh's skeleton), **root
  motion**, **animation events**.
- **Uint8 joint-index packing** — floats first; packing is an optimization pass.
- **`.gmat` material serialization**, **sprite texture field**, **registry-save hygiene**,
  **registry sub-assets** — considered during planning, cut from scope.

## Design tensions, recorded

1. **Second vertex stream vs widened `MeshVertex`** → second stream. Static content and the
   cache format stay untouched; cost is a two-stream bind on the skinned path only.
2. **Palette on the component vs system-owned** → component. `Scene::Copy` and declared
   access handle it; system-owned storage needs a service + destroy-cleanup for no benefit.
3. **MaxBones 128 vs uniform budget** → one handle, 512 vec4s; safe on D3D/Vulkan/Metal,
   marginal on minimal GL. Documented, not engineered around.
4. **Skinned draws break instancing** → accepted; escape hatch named and deferred.
5. **Conditional baking** → one mesh may hold world-baked static and skin-space skinned
   vertices. `Submesh::IsSkinned` is the single gate; color pass, shadow pass, and bounds all
   honor it or the bug is subtle.
6. **Clips by name** → DCC renames fail soft (warn-once + bind pose) and visibly (inspector
   combo shows the real clip list).
7. **Editor-mode sampling** → `AnimationSystem::OnUpdateEditor` really samples; an explicit,
   documented divergence from the systems norm.
