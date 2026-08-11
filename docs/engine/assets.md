# Assets

`GanymedEngine/source/GanymedE/Assets/` — handle-based asset identity, a YAML registry, and the
mesh import pipeline (cgltf + a binary cache).

## Handles & metadata

[`AssetTypes.h`](../../GanymedEngine/source/GanymedE/Assets/AssetTypes.h):

- `AssetHandle` is a `UUID`; `InvalidAssetHandle` is 0 (`IsAssetHandleValid` checks).
- `AssetType`: `StaticMesh`, `Environment`, `Texture`, `Material`, `Scene`, `Script` — derived from
  file extension by `AssetTypeFromExtension` (`.gltf/.glb` → StaticMesh, `.hdr` → Environment,
  `.png/.jpg/...` → Texture, `.ganymede` → Scene, `.lua` → Script).
- `AssetMetadata` = handle + type + file path **relative to `assets/`**.

Components reference handles, never paths (`StaticMeshComponent.Mesh`,
`SkyLightComponent.Environment`), and the scene serializer writes handles — paths can move without
breaking scenes, as long as the registry moves with them.

## AssetManager

[`AssetManager`](../../GanymedEngine/source/GanymedE/Assets/AssetManager.h) is a static facade over
one registry + per-type in-memory caches:

| API | Behavior |
|---|---|
| `Init()` / `Shutdown()` | Load / save the registry. The editor calls these in `EditorLayer::OnAttach/OnDetach` |
| `ImportAsset(relativePath)` | Idempotent registration: existing path returns its handle; otherwise mint a UUID, infer the type, persist the registry immediately. Unsupported extensions log a warning and return the invalid handle |
| `GetHandle(path)` / `GetMetadata(handle)` / `GetAssetType(handle)` | Lookups |
| `GetAsset<T>(handle)` | Cached load. Specialized for `Mesh`, `Environment`, `Texture2D` |
| `Reload(handle)` | Evict the loaded asset so the next `GetAsset` re-reads it from disk |

The registry lives at `assets/AssetRegistry.gr` — YAML, one `{Handle, Type, FilePath}` entry per
asset. It is data, checked into the repo alongside the assets it describes.

`GetAsset<T>`'s **primary template is defined**, not just declared: its body is a
`static_assert(sizeof(T) == 0, ...)`, so an unsupported `T` is a compile error naming the supported
types rather than an unresolved-external at link time. The three specializations are *declared* at
namespace scope in the header — a specialization must be visible before any use that would
otherwise implicitly instantiate the primary template. Adding an asset type therefore means: one
declaration in the header, one definition in the `.cpp`, one `Loaded*` cache map.

Load paths:
- **Mesh** — try `MeshCache` first; on miss, `MeshImporter::Load` then write the cache.
- **Environment** — `Environment::Create` (runs the IBL bake; see
  [rendering.md](rendering.md#environment--ibl)).
- **Texture2D** — `TextureImporter::LoadFromFile` against the asset root, unflipped.
- Material/Scene/Script are registered types without a `GetAsset` path (scenes load via
  `SceneSerializer`; scripts by path in `ScriptEngine`, since there is no runtime object to cache —
  see [scripting.md](scripting.md)).

Cache lookups on all three paths are plain `unordered_map` hits, deliberately: `RenderSystem`
re-fetches by handle every frame for every entity.

## Texture loading

[`TextureImporter`](../../GanymedEngine/source/GanymedE/Assets/TextureImporter.h) is the single
decode path for asset-layer textures. stb's flip flag is process-global, so both entry points set it
explicitly immediately before decoding — no call site inherits whatever a previous load left behind.
Everything is forced to 4 channels (bgfx has no 24-bit RGB8 format).

| Entry point | Used by |
|---|---|
| `LoadFromFile(fullPath, flip=false)` | `AssetManager::LoadTexture` |
| `LoadFromMemory(bytes, size, flip=true)` | glTF images embedded in a buffer view |
| `LoadMaterialMap(relativePath)` | Material maps recorded as a path — the de-duplicating resolve |

`LoadMaterialMap` is where texture **de-duplication** happens. When the recorded path stays inside
the asset root it goes through `ImportAsset` (idempotent) + `GetAsset<Texture2D>`, so two meshes
referencing the same `albedo.png` share one decode and one bgfx texture. Paths that escape the root
(`std::filesystem::relative` returns `../../foo.png` with no error code) have no registry identity
and are decoded directly — they must not get a registry entry. `MeshImporter` (cold import) and
`MeshCache` (cache replay) both call it, so the rule exists in one place instead of two copies of a
decode loop.

Embedded (`.glb`) images have no file identity, hence no registry entry and no de-duplication;
content-hash de-dup is a possible later refinement.

**Side effect worth knowing:** `AssetRegistry.gr` now accumulates `Texture` entries as meshes load,
and because `ImportAsset` persists the registry immediately, a cold import of a mesh with *n* unique
external maps rewrites the registry *n* times.

**Flip discrepancy (known, deliberate):** file-based maps load unflipped, embedded glTF images
flipped. Unflipped is the correct one — glTF UVs are top-left origin and bgfx normalizes texture
origin to top-left, which is why `Texture2D(const std::string&)` explicitly does not flip. The
embedded path flips only because that is what it did before the loaders were consolidated;
reconciling it changes rendering on that path, so it is a separate change.

Editor chrome (`EditorLayer` icons, `ContentBrowserPanel` icons, the checkerboard) deliberately
stays on the `Texture2D(path)` constructor with hard-coded `resources/` paths — outside the asset
cache, since it has no asset identity and no reason to be evictable.

## Reload

`Reload(handle)` is plain eviction, dispatched on the metadata type:

| Type | Action |
|---|---|
| Texture, Environment | Erase from the per-type map. |
| StaticMesh | Three steps, **in this order**: (1) walk the cached mesh's materials' `Get*MapPath()`s, resolve each through `GetHandle`, erase those from `LoadedTextures`; (2) erase from `LoadedMeshes`; (3) `MeshCache::Invalidate` — delete the `.meshcache` file. |
| Material, Scene, Script | Nothing; these have no `GetAsset` cache. |

Step (1) is not optional: skip it and the reimported mesh silently rebinds the stale cached textures
through `LoadMaterialMap`. Step (3) is what makes Reload mean *reimport now* rather than *recheck the
timestamp* — the cache's timestamp check already catches source edits, but a referenced external
texture can change while the `.gltf`'s own timestamp does not.

**Why plain eviction is safe** — this is the invariant the asset layer relies on:

- Components store `AssetHandle`s, never `Ref`s.
- `RenderSystem` re-fetches by handle every frame, so an eviction lands on the next frame; the
  inspector fetches per draw; the serializer only warms the cache;
  `Renderer3D::s_Data.ActiveEnvironment` is overwritten by the next `SubmitEnvironment`.
- Evicted objects drain via `shared_ptr` refcount, and bgfx defers handle destruction to frame end,
  so evicting mid-frame from ImGui code cannot pull a texture out from under an in-flight draw.

The rule for future consumers: **re-fetch by handle each frame, or accept staleness across a
Reload.** Caching a `Ref` on a component breaks it.

Asset roots: paths resolve against `GetAssetRoot()`
([`AssetPaths.h`](../../GanymedEngine/source/GanymedE/Assets/AssetPaths.h)) — the relative
directory `assets/`, i.e. **relative to the working directory**, which is why the apps must run
with their project folder as CWD (each app has its own `assets/`; the editor's is
`GanymedEditor/assets/`).

## Mesh import (cgltf)

[`MeshImporter`](../../GanymedEngine/source/GanymedE/Renderer/MeshImporter.cpp) loads glTF 2.0
(`.gltf`/`.glb`) via the header-only cgltf:

- Walks the node tree **depth-first into a vector**, flattening every mesh primitive into one
  interleaved vertex/index buffer with a `Submesh` per primitive. Traversal order is part of the
  contract: submesh order must be stable across runs or cache diffs and joint↔submesh correlation
  become impossible to reason about.
- Reads position/normal/tangent/texcoord; missing normals/tangents get defaults. A primitive with
  no index accessor is legal glTF (triangle soup in draw order) and gets a synthesized `0..n-1`
  index list, since the engine always draws indexed.
- Materials map from glTF PBR metallic-roughness: base color factor/texture, normal map,
  metallic-roughness map, two-sided flag, alpha mode → `IsTransparent`. External texture URIs are
  recorded as paths and resolved through `TextureImporter::LoadMaterialMap` (so they de-duplicate
  through the registry); **embedded** (glb) images are kept as compressed bytes on the `Material` so
  the cache can persist them.
- `MeshImporter::Instantiate(scene, path)` — used by viewport drag-drop — imports the asset
  (registry) and creates an entity with a `StaticMeshComponent`.

### Skinning data

A rigged glTF carries its skeleton and its clips **inside the `Mesh` asset**
([`Animation.h`](../../GanymedEngine/source/GanymedE/Renderer/Animation.h)): a `.glb` ships mesh,
skin and animation in one file, so separate clip assets would buy registry surgery and nothing else.

Only the **first** `cgltf_skin` is imported; files with more log a warning. What the importer does
with it:

- **Joints are topologically sorted** (parents before children) and `JOINTS_0` values are remapped
  through that sort. glTF does not order `skin.joints`, but composing joint globals in one forward
  pass requires it.
- `JOINTS_0` is read with `cgltf_accessor_read_uint`, *not* `read_float` — the accessor is an
  unnormalized integer one. `WEIGHTS_0` goes through `cgltf_accessor_unpack_floats` (which honors
  the accessor's `normalized` flag and handles sparse data) and is then **renormalized**; exporters
  ship weights that do not sum to 1 often enough that skipping this shows up as limbs swelling and
  shrinking as the rig moves.
- Inverse bind matrices come from the skin's accessor, identity if it is absent (the spec allows it).
- **`Skeleton::RootTransform`** is the matrix that seeds root joints when the runtime composes
  global joint transforms. It folds two corrections into one value:
  `inverse(skinnedMeshNodeWorld) * rootJointParentWorld`.
  - The right-hand term is the world transform of whatever sits above the root joints — Blender's
    `Armature`, a Y-up correction node. The inverse binds already account for it, so leaving it out
    skins the whole mesh by its inverse.
  - The inverse is required by the glTF spec: the transform of the node a skinned mesh hangs off
    MUST be ignored, and the joint matrices carry its inverse instead. Skinning output is already
    in mesh space, so applying that node transform as well applies it twice.

  It is kept out of `LocalRestPose` because animation channels replace joint locals wholesale. The
  correctness test is that at the rest pose `Global[i] * InverseBind[i]` comes out as identity for
  every joint; anything else means one of the two terms is wrong. Of the Khronos samples, both
  CesiumMan and RiggedFigure exercise it and Fox does not — every node in Fox is at identity, so it
  cannot distinguish a right answer from several wrong ones.
- **Conditional world bake.** Skinned primitives skip the world-space bake and keep their vertices
  in skin space, because glTF places them via `globalJointTransform * inverseBindMatrix` and the
  spec says a skinned mesh node's own transform is ignored. Static primitives are baked exactly as
  before. `Submesh::IsSkinned` is the single gate every downstream consumer honors.
- **A skinned submesh keeps its `LocalTransform`**, where a static one has it reset to identity
  because the bake already spent it. That looks like the spec violation the point above just
  avoided, and is not one: `RootTransform` carries the inverse of the very same matrix, so
  re-applying it at draw time (`Renderer3D` composes `worldTransform * LocalTransform`) cancels
  instead of double-applying. Clearing it leaves the mesh in raw bind space, which for any file with
  a Y-up correction node means the character renders lying on its side. Both halves of that
  cancellation have to be present; either alone is wrong.
- **Clips**: LINEAR and STEP are supported; CUBICSPLINE degrades to linear (the middle value of each
  in-tangent/value/out-tangent triple) with a warning. Morph-target weight channels are skipped.
  Duration is the maximum key time across channels. Rotation values are stored **xyzw** — glTF's
  order, not `glm::quat`'s `(w, x, y, z)` constructor order.
- Skin weights ride a **second vertex stream** (`SkinVertex`) rather than widening `MeshVertex`,
  which would tax every static mesh 32 bytes a vertex. Because both bgfx streams are bound with one
  `startVertex`, `Mesh::GetSkinVertices()` is either empty or exactly parallel to the vertex array:
  in a file mixing static and skinned primitives, the static vertices carry zeroed skin entries.
- A skin declared but used by no primitive is dropped along with its clips.

## MeshCache

[`MeshCache`](../../GanymedEngine/source/GanymedE/Assets/MeshCache.h) dumps the fully-parsed mesh
(vertices, indices, submeshes, material scalars/paths/embedded texture bytes, skin vertices,
skeleton and animation clips) as a binary blob under `assets/.assets/`, keyed by source path with
the source file's timestamp stored for invalidation. `TryLoad` returns null on version/timestamp
mismatch, falling back to a full re-import. The content browser hides the `.assets/` directory.

The format is at **v6** (v4 added the skeleton, clips, the skin vertex stream and
`Submesh::IsSkinned`; v5 and v6 have that same layout and exist only to discard caches whose stored
*values* were stale — v5 the pre-correction `RootTransform`, v6 skinned submeshes written with an
identity `LocalTransform`). Bumping the version *is* the migration: every existing cache fails
the version check on first load and gets re-imported. A bump is the right move whenever the
*meaning* of a stored field changes, not just its layout — a stale cache with a silently wrong
value is far harder to diagnose than a re-import.

Practical notes:
- Delete `assets/.assets/` to force a full re-import (e.g. after changing importer code — the
  cache has a version field, bump it when the format changes). `MeshCache::Invalidate` does the
  same for one mesh, and is how `AssetManager::Reload` forces a reimport.
- The cache stores material *data*, not GPU resources; textures are created on load either from
  the recorded paths (via `TextureImporter::LoadMaterialMap`, same as cold import) or the embedded
  bytes.
