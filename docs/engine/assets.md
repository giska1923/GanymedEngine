# Assets

`GanymedEngine/source/GanymedE/Assets/` — handle-based asset identity, a YAML registry, and the
mesh import pipeline (cgltf + a binary cache).

## Handles & metadata

[`AssetTypes.h`](../../GanymedEngine/source/GanymedE/Assets/AssetTypes.h):

- `AssetHandle` is a `UUID`; `InvalidAssetHandle` is 0 (`IsAssetHandleValid` checks).
- `AssetType`: `StaticMesh`, `Environment`, `Texture`, `Material`, `Scene`, `Script`, `Audio` — derived
  from file extension by `AssetTypeFromExtension` (`.gltf/.glb` → StaticMesh, `.hdr` → Environment,
  `.png/.jpg/...` → Texture, `.ganymede` → Scene, `.lua` → Script, `.wav/.mp3/.flac` → Audio).
  **Append only** — the enum is persisted by ordinal, so reordering it retypes every asset in every
  existing registry.
- `AssetMetadata` = handle + type + file path **relative to `assets/`**.

Components reference handles, never paths (`StaticMeshComponent.Mesh`,
`SkyLightComponent.Environment`), and the scene serializer writes handles — paths can move without
breaking scenes, as long as the registry moves with them.

## AssetManager

[`AssetManager`](../../GanymedEngine/source/GanymedE/Assets/AssetManager.h) is a static facade over
one registry + per-type in-memory caches:

| API | Behavior |
|---|---|
| `Init(writableRegistry = true)` / `Shutdown()` | Load the registry; `false` makes every registry write a no-op. The editor calls these in `EditorLayer::OnAttach/OnDetach`, the runtime in `RuntimeLayer::OnAttach/OnDetach` |
| `ImportAsset(relativePath)` | Idempotent registration: existing path returns its handle; otherwise mint a UUID, infer the type, mark the registry dirty. Unsupported extensions log a warning and return the invalid handle |
| `GetHandle(path)` / `GetMetadata(handle)` / `GetAssetType(handle)` | Lookups |
| `GetAsset<T>(handle)` | Cached load. Specialized for `Mesh`, `Environment`, `Texture2D`, `Material` — and only those |
| `Reload(handle)` | Evict the loaded asset so the next `GetAsset` re-reads it from disk |
| `FlushRegistry()` | Write the registry if an import dirtied it (see *Registry writes*) |
| `IsRegistryWritable()` | "This process may write into `assets/`" — one flag, one meaning, for the registry and every future asset-file writer |

The registry lives at `assets/AssetRegistry.gr` — YAML, one `{Handle, Type, FilePath}` entry per
asset, **sorted by `FilePath`**. Sorting is not cosmetic: `Registry` is keyed on a random `uint64`
under an identity hash, so a rehash reordered every entry and one import rewrote the whole file.
Sorted output makes the registry a stable diff and makes "write it twice, compare" a usable check.

`Init(false)` is for a **shipped game**: it must not write into its own install directory (under
Program Files that fails outright), and it has nothing to persist anyway. The guard lives inside
`SaveRegistry()` rather than at its call sites, so no future caller can bypass it — and that
matters, because imports happen *during scene deserialization* for path-based components, which is
how a read-only install would otherwise have written its registry long before `Shutdown()` was ever
asked. `IsRegistryWritable()` exposes the same flag for code that writes other files into
`assets/`; asset writers use that one gate rather than each inventing a parallel guard. Handles
minted in a read-only session still work; they just do not outlive it, which is the right lifetime
for something nobody authored.

### Registry writes

`ImportAsset` sets a dirty flag; `FlushRegistry()` writes if it is set. Flush points are the end of
a user-visible action:

| Flush point | Covers |
|---|---|
| `SceneSerializer::Deserialize` (end) | Every handle minted by a path-based component while the scene loaded |
| `EditorUI::AcceptAssetDropHandle` | A drop onto a component's handle field |
| `EditorLayer` viewport drop | A mesh drop: the mesh handle plus every texture its import minted |
| `ContentBrowserPanel` → Import | The one asset the menu item registered |
| `AssetManager::Shutdown` | Anything an unflushed path missed |

The alternative — dirty flag plus a single flush at `Shutdown` — was rejected: an editor crash
mid-session should not cost an afternoon of imports. A *missed* flush point costs a write deferred
to `Shutdown`, which is the acceptable direction for this to fail in. Measured, from an empty
registry: an editor boot that imports the default environment and one path-based mesh writes the
file exactly twice — once per action — where every import used to write it.

The write is **emit to `AssetRegistry.gr.tmp`, then rename over**. `std::ofstream` truncates on
open, so a crash between open and flush used to leave a zero-length registry: every handle in every
scene dead, and indistinguishable from "never imported anything". The rename is atomic on NTFS and
POSIX, so the file is either the old one or the new one.

`LoadRegistry` wraps its parse in one try/catch — the `SceneSerializer::Deserialize` posture, and
for the same reason: unhandled, a truncated `.gr` terminated the process out of `Init`, before a
window existed to say why. On a parse failure it logs, **moves the unreadable file aside as
`AssetRegistry.gr.bad`**, and continues with an empty registry. The quarantine is the point: the
session's first import would otherwise flush a nearly-empty registry over the file, destroying the
handle mappings a hand-repair could have recovered. Duplicate `FilePath` entries warn and keep the
first — the loser is unreachable through `GetHandle`/`ImportAsset` and only ever surfaces as an
asset that silently fails to resolve.

`Shutdown()` clears the caches either way, and does so while `Renderer::IsGpuAlive()` so the
GPU-resource destructors release real bgfx handles.

### Path-resolved types

`Script` and `Audio` have **no `GetAsset` specialization, and that is deliberate** rather than an
omission. The manager answers handle → path through `GetMetadata` and the consumer loads the file
itself: the Lua VM owns its chunks, and miniaudio's resource manager ref-counts and caches decoded
audio by path (see [audio.md](audio.md)). A cache here would be a second ref-counting owner of the
same resource, and two caches disagreeing about lifetime is the bug class this avoids.

The `GetAsset` primary template is *defined* with a `static_assert` rather than left undeclared, so
asking for one of these is a compile error with a message instead of an unresolved external at link
time.

### Registry portability

**A scene is unusable without the registry that resolves its handles.** Scenes store bare handles,
`GetAsset<T>` resolves handle → path through the registry and nowhere else, and a handle with no
entry loads nothing. So the registry is not an incidental cache — it is half of every scene
reference, and the two halves have to travel together.

The two apps therefore treat the same file differently, and `.gitignore` says so per-path:

| | `GanymedEditor/assets/AssetRegistry.gr` | `GanymedRuntime/assets/AssetRegistry.gr` |
|---|---|---|
| Tracked in git | No | **Yes** |
| Role | Machine-local database the editor grows as you import | Authored content, shipped with the game |
| Written at runtime | Yes (at the flush points above) | No (`Init(false)`) |

What happens when it is missing was measured, not assumed: a runtime booted without its registry
loads its scene, reports the right entity count, and renders **nothing but the procedural sky** —
every mesh and the HDR environment silently absent. Only `ScriptEngine` complained, because it
alone logged the unresolved handle.

That asymmetry is fixed: the three `GetAsset<T>` loaders now warn when a *valid* handle has no
registry entry, naming the handle and the expected type. It fires **once per handle**
(`WarnedUnknownHandles`), because `RenderSystem` re-fetches assets by handle every frame per entity
and an unguarded warning would arrive at frame rate — the same loud-but-not-broken posture
`AnimationSystem` takes for an unknown clip name.

The deeper gap is unfixed and worth naming: handle→path lives in *one* file per app rather than
next to each asset, so it cannot merge, and a hand-edited scene can reference a handle no registry
knows. The production answer is per-asset committed metadata (Unity's `.meta` files carry the GUID
beside the asset). That is a change to the asset layer's identity model, not a Phase 2 fix.

`GetAsset<T>`'s **primary template is defined**, not just declared: its body is a
`static_assert(sizeof(T) == 0, ...)`, so an unsupported `T` is a compile error naming the supported
types rather than an unresolved-external at link time. The three specializations are *declared* at
namespace scope in the header — a specialization must be visible before any use that would
otherwise implicitly instantiate the primary template. Adding an asset type therefore means: one
declaration in the header, one definition in the `.cpp`, one `Loaded*` cache map.

Load paths:
- **Mesh** — try `MeshCache` first; on miss, `MeshImporter::Load` then write the cache. Either way,
  `MaterialSerializer::GenerateSidecars` runs afterwards (see *Materials*).
- **Material** — `MaterialSerializer::Load` on the `.gmat`. Cached rather than path-resolved, and
  the caching is load-bearing rather than a performance choice: instancing merges draws on
  `Ref<Material>` *identity*, so two entities sharing one `.gmat` handle have to receive the same
  `Ref` or every batch shatters.
- **Environment** — `Environment::Create` (runs the IBL bake; see
  [rendering.md](rendering.md#environment--ibl)).
- **Texture2D** — `TextureImporter::LoadFromFile` against the asset root, unflipped.
- Material/Scene/Script are registered types without a `GetAsset` path (scenes load via
  `SceneSerializer`; scripts by path in `ScriptEngine`, since there is no runtime object to cache —
  see [scripting.md](scripting.md)).

Cache lookups on all three paths are plain `unordered_map` hits, deliberately: `RenderSystem`
re-fetches by handle every frame for every entity.

## Materials (`.gmat`)

A material is an asset: importable, editable, savable, reloadable. Two entities sharing one mesh
can look different without touching the mesh or its cache.

**The shape: `.gmat` is an additive override layer, not a replacement.** The mesh asset keeps the
materials it imported, untouched, inside itself and its `.meshcache`. Importing a mesh *also*
generates one `.gmat` sidecar per material slot, and `MeshImporter::Instantiate` fills the new
entity's `StaticMeshComponent::MaterialOverrides` with those handles — so a drag-dropped mesh
authors against `.gmat` from birth, while an entity with no overrides (every scene that predates
this) renders bit-identically to before.

This diverges from the fuller Unreal norm, where mesh assets reference material assets directly and
there are no "built-in" materials. That shape needs the mesh cache to depend on `.gmat` mtimes, and
`MeshCache` v6 serializes materials inline keyed on the *source* mtime only — so editing a `.gmat`
would either silently not invalidate the cache, or require a dependency-tracking index the engine
does not have. The additive shape costs one duplicated default (the mesh's material and its sidecar
start identical) and buys three things: no cache-format change, no risk to existing scenes, and
**no reverse-dependency index anywhere**. That last one is the shape's best property and worth
stating plainly: `Reload(.gmat)` needs no material→mesh map because meshes never reference `.gmat`.
Only override slots do, and `RenderSystem` re-fetches by handle every frame, so eviction lands in
the viewport on the next frame for free.

### Format

YAML under a `Material:` root, in a fixed key order — a write → read → write round trip is
byte-identical, the same discipline canonical scene saves are held to.

```yaml
Material:
  Name: Texture
  Albedo: [1, 1, 1, 1]
  Metallic: 0
  Roughness: 1
  AlbedoMap: models/BoxTextured_textures/albedo_0.png
  TwoSided: false
  Transparent: false
```

Map keys are omitted when unset (an empty scalar reads back as a null node, and `as<std::string>()`
throws on those). The shader is **implicit**: every `.gmat` binds `MeshShader::Get()`, injected by
the loader because `Material::Bind` asserts on a null shader and there is nothing else to choose
until shader variants exist.

**Textures are stored as paths, not handles**, and that split is deliberate: a `.gmat` has to be
self-describing and hand-mergeable, and a bare handle means nothing without the registry that minted
it, while a path survives a fresh clone (the shipped-registry lesson from the runtime milestone).
Handles stay the scene↔registry currency; paths are the asset↔asset currency. The loader resolves
each path through `LoadMaterialMap`, so materials naming one image share a decode.

A malformed or missing `.gmat` logs and returns null — the `SceneSerializer::Deserialize` posture.

### Sidecar generation and embedded-texture extraction

Runs from `AssetManager::LoadMesh`, after either load path succeeds — that is the one point cold
import and cache replay both pass through, so a cached mesh still gets its sidecars instead of
needing its `.meshcache` deleted first. Gated on `IsRegistryWritable()`: the runtime never writes
into `assets/`.

- One `.gmat` per material slot at `<meshdir>/<meshstem>_mat<i>_<name>.gmat`, **iff absent**. The
  *index* is the identity, not the name — glTF material names are optional, non-unique and
  unsanitized, and `Submesh::MaterialIndex` already speaks index; the name rides along for humans
  and is sanitized to `[A-Za-z0-9_-]`. *Iff absent* is what makes user edits sacred: a re-import,
  or a cache rebuild, never clobbers an authored material.
- Textures embedded in a `.glb` are extracted once to
  `<meshdir>/<meshstem>_textures/<map>_<i>.<ext>` and `ImportAsset`ed like any other texture. The
  bytes are already a compressed image, so extraction is a byte copy with no encoder involved — but
  the extension is sniffed from the magic bytes rather than assumed, because the registry types
  assets by extension and a `.png` holding JPEG bytes would be a lie on disk (both occur in the
  sample meshes). Extraction is what makes the `.gmat` self-describing: a material referencing bytes
  inside another asset's blob could be neither hand-edited nor re-pointed.
- Consequence to expect: a `.glb` import now mints texture *and* material registry entries, all in
  one batched write.

`MaterialSerializer::SidecarPath` is the single source of the naming rule, shared by the generator
and by `MeshImporter::Instantiate`. If those two ever disagreed, entities would author against
files nothing generates.

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
| Material | Two steps, in this order: (1) erase its three maps from `LoadedTextures` (same reason as the mesh branch); (2) erase from `LoadedMaterials`. |
| StaticMesh | Three steps, **in this order**: (1) walk the cached mesh's materials' `Get*MapPath()`s, resolve each through `GetHandle`, erase those from `LoadedTextures`; (2) erase from `LoadedMeshes`; (3) `MeshCache::Invalidate` — delete the `.meshcache` file. |
| Scene, Script, Audio | Nothing; these have no `GetAsset` cache. |

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
