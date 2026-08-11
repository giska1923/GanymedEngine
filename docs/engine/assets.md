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

Asset roots: paths resolve against `GetAssetRoot()`
([`AssetPaths.h`](../../GanymedEngine/source/GanymedE/Assets/AssetPaths.h)) — the relative
directory `assets/`, i.e. **relative to the working directory**, which is why the apps must run
with their project folder as CWD (each app has its own `assets/`; the editor's is
`GanymedEditor/assets/`).

## Mesh import (cgltf)

[`MeshImporter`](../../GanymedEngine/source/GanymedE/Renderer/MeshImporter.cpp) loads glTF 2.0
(`.gltf`/`.glb`) via the header-only cgltf:

- Walks the node tree, flattening every mesh primitive into one interleaved vertex/index buffer
  with a `Submesh` per primitive (node transform → `Submesh::LocalTransform`).
- Reads position/normal/tangent/texcoord; missing normals/tangents get defaults.
- Materials map from glTF PBR metallic-roughness: base color factor/texture, normal map,
  metallic-roughness map, two-sided flag, alpha mode → `IsTransparent`. External texture URIs are
  recorded as paths and resolved through `TextureImporter::LoadMaterialMap` (so they de-duplicate
  through the registry); **embedded** (glb) images are kept as compressed bytes on the `Material` so
  the cache can persist them.
- `MeshImporter::Instantiate(scene, path)` — used by viewport drag-drop — imports the asset
  (registry) and creates an entity with a `StaticMeshComponent`.

## MeshCache

[`MeshCache`](../../GanymedEngine/source/GanymedE/Assets/MeshCache.h) dumps the fully-parsed mesh
(vertices, indices, submeshes, material scalars/paths/embedded texture bytes) as a binary blob
under `assets/.assets/`, keyed by source path with the source file's timestamp stored for
invalidation. `TryLoad` returns null on version/timestamp mismatch, falling back to a full
re-import. The content browser hides the `.assets/` directory.

Practical notes:
- Delete `assets/.assets/` to force a full re-import (e.g. after changing importer code — the
  cache has a version field, bump it when the format changes).
- The cache stores material *data*, not GPU resources; textures are created on load either from
  the recorded paths (via `TextureImporter::LoadMaterialMap`, same as cold import) or the embedded
  bytes.
