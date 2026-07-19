# Assets

`GanymedEngine/source/GanymedE/Assets/` — handle-based asset identity, a YAML registry, and the
mesh import pipeline (cgltf + a binary cache).

## Handles & metadata

[`AssetTypes.h`](../../GanymedEngine/source/GanymedE/Assets/AssetTypes.h):

- `AssetHandle` is a `UUID`; `InvalidAssetHandle` is 0 (`IsAssetHandleValid` checks).
- `AssetType`: `StaticMesh`, `Environment`, `Texture`, `Material`, `Scene` — derived from file
  extension by `AssetTypeFromExtension` (`.gltf/.glb` → StaticMesh, `.hdr` → Environment,
  `.png/.jpg/...` → Texture, `.ganymede` → Scene).
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
| `GetAsset<Mesh>(handle)` / `GetAsset<Environment>(handle)` | Cached load (explicit specializations; extend per type) |

The registry lives at `assets/AssetRegistry.gr` — YAML, one `{Handle, Type, FilePath}` entry per
asset. It is data, checked into the repo alongside the assets it describes.

Load paths:
- **Mesh** — try `MeshCache` first; on miss, `MeshImporter::Load` then write the cache.
- **Environment** — `Environment::Create` (runs the IBL bake; see
  [rendering.md](rendering.md#environment--ibl)).
- Texture/Material/Scene are registered types without a `GetAsset` path yet (textures are loaded
  directly via `Texture2D::Create`; scenes via `SceneSerializer`).

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
  recorded as paths; **embedded** (glb) images are kept as compressed bytes on the `Material` so
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
  the recorded paths or the embedded bytes.
