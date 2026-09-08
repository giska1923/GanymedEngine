# Assets

`GanymedEngine/source/GanymedE/Assets/` — handle-based asset identity in committed per-asset `.meta`
sidecars, a scan-derived in-memory index, and the mesh import pipeline (cgltf + a binary cache).

## Handles & metadata

[`AssetTypes.h`](../../GanymedEngine/source/GanymedE/Assets/AssetTypes.h):

- `AssetHandle` is a `UUID`; `InvalidAssetHandle` is 0 (`IsAssetHandleValid` checks).
- `AssetType`: `StaticMesh`, `Environment`, `Texture`, `Material`, `Scene`, `Script`, `Audio`,
  `Prefab` — derived from file extension by `AssetTypeFromExtension` (`.gltf/.glb` → StaticMesh,
  `.hdr` → Environment, `.png/.jpg/...` → Texture, `.ganymede` → Scene, `.lua` → Script,
  `.wav/.mp3/.flac` → Audio, `.gprefab` → Prefab). `AssetTypeToString` /
  `AssetTypeFromString` round-trip the enum **by name**, which is what a `.meta` sidecar stores.
  The enum is still **append only**, but only for the legacy `AssetRegistry.gr` reader — that file
  persisted the ordinal, so reordering retypes every asset in every registry still on disk. The
  sidecar format does not inherit the trap.
- `AssetMetadata` = handle + type + file path **relative to `assets/`**, keyed on forward slashes
  (`generic_string()`) so a path minted on Windows matches one read on Linux.

Components reference handles, never paths (`StaticMeshComponent.Mesh`,
`SkyLightComponent.Environment`), and the scene serializer writes handles. A handle resolves because
the `.meta` sidecar beside the asset says so, so a path can move — including via `git mv` — without
breaking a scene, as long as its sidecar moves with it.

## AssetManager

[`AssetManager`](../../GanymedEngine/source/GanymedE/Assets/AssetManager.h) is a static facade over
one derived index plus a registry of per-type managers (see *Managers and caching*):

| API | Behavior |
|---|---|
| `Init(writableAssets = true)` / `Shutdown()` | Read the legacy registry if present, then `ScanAssets()`; `false` makes every write into `assets/` a no-op. The editor calls these in `EditorLayer::OnAttach/OnDetach`, the runtime in `RuntimeLayer::OnAttach/OnDetach` |
| `ScanAssets()` | Walk `assets/` and rebuild the index from the `.meta` sidecars found there, minting and writing one where a recognized asset has none. Called by `Init`; safe to call again to pick up files added outside the editor |
| `ImportAsset(relativePath)` | "Ensure this file has a sidecar, and tell me its handle." Idempotent: an indexed path returns its handle. Unsupported extensions log a warning and return the invalid handle |
| `GetHandle(path)` / `GetMetadata(handle)` / `GetAssetType(handle)` | Lookups |
| `GetAsset<T>(handle)` | Cached load through the type’s manager. Available for `Mesh`, `Environment`, `Texture2D`, `Material` — and only those, enforced by an `IsAssetType<T>` `static_assert` |
| `Reload(handle)` | Evict the loaded asset so the next `GetAsset` re-reads it from disk |
| `GetCacheStats()` | One `{TypeName, Resident, Retained}` row per registered manager, for the editor’s Stats panel |
| `IsRegistryWritable()` | "This process may write into `assets/`" — one flag, one meaning, for sidecars and every other asset-file writer |

`Init(false)` is for a **shipped game**: it must not write into its own install directory (under
Program Files that fails outright), and it has nothing to persist anyway. The guard is checked at the
one place identity is decided rather than at each call site, so no future caller can bypass it — and
that matters, because imports happen *during scene deserialization* for path-based components, long
before any explicit save. `IsRegistryWritable()` exposes the same flag for code that writes other
files into `assets/`; asset writers use that one gate rather than each inventing a parallel guard.
Handles minted in a read-only session still work; they just do not outlive it, which is the right
lifetime for something nobody authored. The corollary for a shipped build is that **every shipped
asset needs its sidecar shipped with it** — a read-only scan can adopt an identity, never persist
one.

### The `.meta` sidecar

[`AssetMeta.h`](../../GanymedEngine/source/GanymedE/Assets/AssetMeta.h) defines the file that *is*
an asset's identity: `foo.png` → `foo.png.meta`, YAML, four keys, and **committed to source
control**.

```yaml
Asset:
  Handle: 7862165199193339401
  Type: Environment
  ImportConfigVersion: 1
  Config: {}
```

The extension is *appended*, not substituted, so `tree.gltf` and `tree.png` cannot share one
sidecar. `Type` is the enum **name**: a sidecar written by a newer engine must survive a round trip
through an older one, so an unrecognized name is not corruption — the reader returns
`AssetType::None` and the caller re-derives the type from the extension. `Config` is a flat
scalar→scalar map, empty until the importers of a later phase fill it, and `ImportConfigVersion`
exists so those importers can migrate old settings in a `switch` rather than guess. Unrecognized
`Config` keys are read and written back untouched. Nested config is deliberately not representable;
when something needs it, that is a format version bump, not a `YAML::Node` member in a public
header — this codebase keeps yaml-cpp out of every public header.

Writes go through **emit to `foo.png.meta.tmp`, then rename over**. `std::ofstream` truncates on
open, so a crash between open and flush would otherwise leave a zero-length sidecar: an asset whose
identity is gone, indistinguishable from one never imported. The rename is atomic on NTFS and POSIX,
so the file is either the old one or the new one. `AssetMetaSerializer::Read` is side-effect free
(a read-only session can call it safely) and wraps parse *and* every scalar conversion in one
try/catch — `as<uint64_t>()` on a non-numeric handle throws as loudly as a syntax error. A zero
handle counts as corrupt: it is no identity at all.

An unreadable sidecar is **quarantined to `foo.png.meta.bad`**, never overwritten. That is the whole
point: the handle a hand-repair could recover is the only link between that file and every scene
referencing it. One corrupt sidecar costs one asset a fresh handle and produces one logged error;
it cannot take down a project scan, which is the failure mode a single shared registry had.

The batching machinery this replaced — a dirty flag plus `FlushRegistry()` at seven call sites — is
gone, along with `LoadRegistry`/`SaveRegistry`. It existed only because one import rewrote the whole
shared file, so writes had to be deferred to the end of a user-visible action. A ~90-byte sidecar
with no shared file needs no batching: `ImportAsset` writes, and there is nothing to flush.

### The scan

`ScanAssets()` walks `assets/` with a `recursive_directory_iterator`, calls
`disable_recursion_pending()` on any directory whose name starts with `.` (`.assets/` today,
`.compiled/` when the asset compiler lands), and skips `.meta`/`.bad` files and any extension
`AssetTypeFromExtension` does not recognize. Paths are collected, **`std::sort`ed**, and only then
registered. The sort is load-bearing rather than cosmetic: iteration order is unspecified, and the
duplicate-handle rule below is "first one wins", so an unsorted scan would pick a different winner
on NTFS than on ext4 and two developers with the same copy-pasted `.meta` would see different assets
break.

Every path→handle decision funnels through one internal `EnsureRegistered`, shared with
`ImportAsset` so the rules exist once:

| Situation | Result |
|---|---|
| Sidecar reads Ok, handle unclaimed | Adopt it |
| Sidecar reads Ok, handle already claimed by another path | **First (sorted) path keeps it**, the second is re-minted, one warning names both paths |
| Sidecar reads Ok, `Type` disagrees with the extension | Keep the handle, rewrite the type — the cost of a rename between two recognized extensions |
| No sidecar, legacy registry names the path | Adopt the legacy handle |
| No sidecar, nothing else knows the path | Mint a `UUID` |
| Sidecar unreadable | Quarantine, then treat as "no sidecar" |

Silently aliasing two assets to one handle would corrupt every scene referencing either, which is
why a collision is loud and re-mints rather than merges. A sidecar is written only when the asset
file **exists** — a scene naming a since-deleted asset still gets an in-memory handle (its load path
already warns), but writing `foo.glb.meta` next to a missing `foo.glb` would leave an orphan nothing
cleans up.

The scan logs one line with every counter, because "did this boot mint anything?" is the question a
derived index has to answer at a glance:

```
Asset scan: 19 files, 19 adopted from sidecars, 0 adopted from the legacy registry,
0 handles minted, 0 sidecars written, 0 quarantined, 0 handle collisions, 0 types corrected
```

A second boot over an unchanged tree must read **0 minted, 0 written** — verified on both apps, with
byte-identical handles across runs. Legacy adoption is counted apart from a fresh mint on purpose:
on the one boot that performs the migration, that number is what says the migration was lossless.

The cost this buys is real and worth naming: identity resolution is now startup work proportional to
the size of `assets/`. It is cheap work — one small file read per recognized asset, **nothing
hashed** — and invisible on the current trees (19 and 10 assets), but it scales with the tree and
will need the compiled-cache index of a later phase to stay flat.

### Migration from `AssetRegistry.gr`

The old model was one un-mergeable YAML per app holding every handle→path pair. A scene and its
registry had to travel together; two people importing on two branches produced a guaranteed
conflict; a hand-edited scene could name a handle no registry knew.

`Init` reads `assets/AssetRegistry.gr` **first**, if it exists, into a path→handle seed, and only
then scans. Reversing those two lines is how you break every existing scene. Only identity is
migrated: the file's `Type` field is read past, because the extension types an asset now and the
by-ordinal enum that field persisted is exactly the trap the sidecar format does not inherit. Once
the sidecars exist the registry is redundant and never written again; `Init` logs that once. It is
kept and still read for one release so a downgrade works — deleting it is the user's call.

The seed lives for the **whole session**, not just the scan, and that is not an optimization. Files
created after the scan need it too: a `.gmat` regenerated by `MaterialSerializer::GenerateSidecars`
or a texture extracted from a `.glb` reaches `ImportAsset`, not the scan, and must adopt the handle
the old registry recorded or every scene referencing it breaks. Both editor and runtime registries
name exactly such paths.

If `AssetRegistry.gr` exists but **fails to parse**, the session goes read-only
(`IsRegistryWritable()` becomes false) instead of quarantining the file. Nothing overwrites it any
more, so leaving it in place is what lets a hand-repair work; what matters is that a broken seed must
not bake a lossy migration into the tree. Every asset would otherwise mint a fresh handle and write a
sidecar claiming it, permanently breaking every scene that referenced the old one. Read-only means
handles still work in memory, nothing is persisted, and repairing the file and restarting recovers
completely. Duplicate `FilePath` entries in the legacy file warn and keep the first.

`Shutdown()` clears the caches and the seed, and does so while `Renderer::IsGpuAlive()` so the
GPU-resource destructors release real bgfx handles.

### Path-resolved types

`Script`, `Audio` and `Prefab` have **no `GetAsset` specialization, and that is deliberate** rather
than an omission. The manager answers handle → path through `GetMetadata` and the consumer loads the file
itself: the Lua VM owns its chunks, and miniaudio's resource manager ref-counts and caches decoded
audio by path (see [audio.md](audio.md)). A cache here would be a second ref-counting owner of the
same resource, and two caches disagreeing about lifetime is the bug class this avoids. A prefab is
there for a different reason: instantiating one is a rare editor action reading a small YAML, and a
cached parsed form would add a staleness surface — Apply rewrites the file, and the next instantiate
has to see it — for no measurable win.

The `GetAsset` primary template is *defined* with a `static_assert` rather than left undeclared, so
asking for one of these is a compile error with a message instead of an unresolved external at link
time.

### Identity portability

**A scene is unusable without the identity that resolves its handles.** Scenes store bare handles,
`GetAsset<T>` resolves handle → path through the index and nowhere else, and a handle with no entry
loads nothing. Identity is therefore half of every scene reference, and the two halves have to travel
together.

Which is the argument for putting it beside the asset. A `.meta` sidecar is committed, in the same
directory as the file it identifies, for **both** apps — no per-path `.gitignore` asymmetry, no
machine-local database, nothing to keep in sync. It merges file-by-file, it survives a `git mv`, and
it cannot go missing without the asset going with it. This is Unity's model.

Unreal takes the other route: path *is* identity, and moving an asset leaves a redirector object
behind that rewrites references. That needs every asset to be an engine-owned serialized object so
the engine can rewrite anything pointing at it — which Ganymed does not have and does not want. A
`.png` here is a `.png`, importable by any tool, and a sidecar is the only way to attach identity to
a file the engine does not own. Godot does the same thing for the same reason (`.import` files);
bgfx-based engines that skip it generally end up with path-keyed assets and no rename story at all.

What happens when identity is missing was measured, not assumed: a runtime booted without it loads
its scene, reports the right entity count, and renders **nothing but the procedural sky** — every
mesh and the HDR environment silently absent. Only `ScriptEngine` complained, because it alone logged
the unresolved handle.

That asymmetry is fixed: `TypedAssetManager<T>::Load` warns when a *valid* handle is not in the
index, naming the handle and the manager that wanted it — one place rather than one per loader, and the message names the likely cause — a sidecar
that did not travel with its asset. It fires **once per handle** (`WarnedUnknownHandles`), because
`RenderSystem` re-fetches assets by handle every frame per entity and an unguarded warning would
arrive at frame rate — the same loud-but-not-broken posture `AnimationSystem` takes for an unknown
clip name.

One gap remains and is worth naming: nothing yet detects a sidecar whose asset was deleted. An
orphaned `foo.png.meta` is harmless (the scan never sees it, since it walks assets and not sidecars)
but it accumulates, and a "clean orphaned `.meta`" maintenance action is the cheap fix when it starts
to matter.

`GetAsset<T>` for an unsupported `T` is still a **compile error naming the supported types**, not
an unresolved external at link time. It used to be a primary template whose body was
`static_assert(sizeof(T) == 0, ...)`; forwarding to a manager registry would have regressed that to
a runtime assert, so the check moved into an `IsAssetType<T>` trait that the registration list in
[`AssetManager.h`](../../GanymedEngine/source/GanymedE/Assets/AssetManager.h) specializes. Asking
for `GetAsset<int>` produces:

```
error C2338: static_assert failed: 'AssetManager::GetAsset<T> is only available for Mesh,
Environment, Texture2D and Material. Script, Audio and Prefab are path-resolved by design -
resolve them through GetMetadata (docs/engine/assets.md).'
```

## Managers and caching

Every managed type has a `TypedAssetManager<T>`
([`AssetManagerRegistry.h`](../../GanymedEngine/source/GanymedE/Assets/AssetManagerRegistry.h))
living in a flat slot array, and `GetAsset<T>` is a one-liner into it. This replaced four hardcoded
`unordered_map<AssetHandle, Ref<T>>` members and four private `LoadX` functions inside
`AssetManager.cpp` — adding a cached type used to mean editing that file in six places.

Adding one now means exactly two lines, one file apart:

```cpp
// AssetManager.h, beside the forward declarations
GE_ASSET_TYPE(Skeleton);

// AssetManager.cpp, in RegisterManagers()
AssetManagerRegistry::Register<Skeleton>("Skeleton", AssetType::Skeleton, &ParseSkeleton, &ApplySkeleton);
```

Nothing enforces that the two agree. A type declared but never registered asserts on first use; a
type registered but not declared will not compile at the call site, which is the half that gets
noticed. The shape is BlankEngine’s `IResourceManager` / `ResourceManager<T, Cache>` with its RTTR
registration DSL removed — a reflected registry is not worth a reflection dependency at this scale,
and `entt::meta` is not in the asset layer for the reasons in
[`ASSET_PIPELINE_ROADMAP.md`](../toDo&done/ASSET_PIPELINE_ROADMAP.md) decision 14.

### Type ids

`AssetTypeIdOf<T>()` returns a dense `uint8_t` assigned from a counter on that type’s first use, so
resolving a manager is an **array index, not a `std::type_index` hash** — `RenderSystem` re-fetches
by handle per entity per frame and that lookup sits on the path. `MaxAssetManagers` is 16, sized for
the four managed types and the eight `AssetType` values rather than for BlankEngine’s 64.

The id depends on registration order and is therefore **not stable across builds**. It must never be
persisted; `AssetType`, stored by name in a `.meta`, is the durable form. `RegisterManagers()` runs
first in `Init` so the assignment falls out of registration order rather than out of whichever call
site happened to run first.

### Parse and Apply

Each manager is registered with two function pointers rather than one loader, and the split is
dictated by bgfx rather than by taste: **bgfx resource creation belongs to the thread that owns the
context.** So `Parse` is pure CPU work — file IO, decode, deserialize, and nothing below it may
reach a bgfx call — and `Apply` turns the intermediate into the live object on the main thread. The
intermediate is an `AssetParseResult` subclass, polymorphic rather than a `std::any` so it frees
itself if `Apply` never runs. Having the boundary in now means async loading later changes *where*
`Parse` runs, not the shape of any manager.

| Type | Parse | Apply |
|---|---|---|
| **Texture2D** | `TextureImporter::Decode` — stb decode to RGBA8, which is the whole CPU cost | `TextureImporter::Upload` — `Texture2D::Create` + `SetData` |
| **Material** | `MaterialSerializer::ReadDesc` — `.gmat` YAML into a `MaterialDesc` | `MaterialSerializer::Build` — creates the `Material` and resolves its map paths through `LoadMaterialMap` |
| **Mesh** | *none yet* | `MeshCache::TryLoad`, else `MeshImporter::Load` + `MeshCache::Write`; then `MaterialSerializer::GenerateSidecars` (see *Materials*) |
| **Environment** | *none* | `Environment::Create` — runs the IBL bake, see [rendering.md](rendering.md#environment--ibl) |

The two empty `Parse` stages are not the same kind of gap:

- **Environment is correct as it stands.** The separable CPU part is one `stbi_loadf` of the
  equirectangular HDR; everything after it is the bake — six cube faces plus prefilter mips rendered
  through bgfx views — which can never leave the submit thread. Splitting would move a few percent
  of the cost and cost `Environment` its filepath constructor.
- **Mesh is deferred work, and it is the one that matters.** Both load paths construct a `Mesh`,
  whose constructor calls `Build()` and creates bgfx vertex and index buffers, and both build the
  mesh’s materials inline — which pulls textures. Splitting means threading a CPU-side mesh
  description (vertices, indices, submeshes, material *descriptions*) through `MeshImporter.cpp` and
  `MeshCache.cpp` and deferring `Mesh::Create` to Apply. The cgltf parse is the expensive thing that
  has to leave the main thread, so async loading cannot skip this.

`GenerateSidecars` now runs inside `ApplyMesh`, i.e. *before* the manager caches the mesh, where it
used to run after the cache insert. Safe because it only ever reaches `ImportAsset`, never
`GetAsset<Mesh>`: it writes files and registers handles, so nothing in it can re-enter the load.

Scene, Script, Audio and Prefab have no manager at all — see *Path-resolved types* above.

### The cache is weak, with a temporary owner

Each manager caches `std::weak_ptr<T>`, so an asset stays resident exactly as long as something
references it. That is the fix for "nothing is ever unloaded": the four strong maps this replaced
had no eviction path but `Reload` and `Shutdown`, so opening a 200-asset scene and then switching
scenes left every texture from the first one resident.

**It does not evict yet, and the reason is worth stating rather than discovering.** Nothing outside
the manager holds a reference: components store bare `AssetHandle`s, and every consumer drops its
`Ref` at the end of the frame. A purely weak cache would therefore re-import a glTF *per entity per
frame*. So each entry also holds a strong `Retained` pointer — a stand-in owner until a typed asset
reference becomes the real one, at which point that member is deleted and the weak half starts doing
its job. Until then the two counts in the editor’s Stats panel move together, and the `weak_ptr`
expiry branch in `TypedAssetManager<T>::Load` is unreachable in practice.

`AssetManager::GetCacheStats()` returns one `{TypeName, Resident, Retained}` row per registered
manager; the editor renders it under **Stats → Asset Cache**. Resident is what the weak cache is
tracking, retained is what the manager is keeping alive itself.

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
self-describing and hand-mergeable, and a bare handle means nothing without the sidecar that minted
it, while a path is readable on its own. Handles stay the scene↔asset currency; paths are the
asset↔asset currency. The loader resolves each path through `LoadMaterialMap`, so materials naming
one image share a decode.

A malformed or missing `.gmat` logs and returns null — the `SceneSerializer::Deserialize` posture.

### Sidecar generation and embedded-texture extraction

Runs from the mesh manager’s Apply stage, after either load path succeeds — that is the one point cold
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
  the extension is sniffed from the magic bytes rather than assumed, because the asset layer types
  assets by extension and a `.png` holding JPEG bytes would be a lie on disk (both occur in the
  sample meshes). Extraction is what makes the `.gmat` self-describing: a material referencing bytes
  inside another asset's blob could be neither hand-edited nor re-pointed.
- Consequence to expect: a `.glb` import mints texture *and* material handles, and writes a `.meta`
  sidecar beside each generated file. Those files did not exist when the scan ran, so they reach
  identity through `ImportAsset` — which is exactly why the legacy migration seed has to outlive the
  scan (see *Migration*).

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
| `Decode(fullPath, flip=false)` → `DecodedImage` | The texture manager’s Parse stage. CPU only |
| `DecodeFromMemory(bytes, size, flip=true)` → `DecodedImage` | The CPU half of the embedded-image path |
| `Upload(DecodedImage)` | The texture manager’s Apply stage. Main thread only |
| `LoadFromFile(fullPath, flip=false)` | `Decode` + `Upload`, for callers that want both at once |
| `LoadFromMemory(bytes, size, flip=true)` | glTF images embedded in a buffer view |
| `LoadMaterialMap(relativePath)` | Material maps recorded as a path — the de-duplicating resolve |

`DecodedImage` is the Parse/Apply seam: RGBA8, tightly packed, owning stb’s buffer through a
`unique_ptr` with a custom deleter so `stb_image.h` stays out of the header. Move-only, because
there is exactly one owner of the pixels at a time.

`LoadMaterialMap` is where texture **de-duplication** happens. When the recorded path stays inside
the asset root it goes through `ImportAsset` (idempotent) + `GetAsset<Texture2D>`, so two meshes
referencing the same `albedo.png` share one decode and one bgfx texture. Paths that escape the root
(`std::filesystem::relative` returns `../../foo.png` with no error code) have no asset identity
and are decoded directly — they must not get a handle or a sidecar. `MeshImporter` (cold import) and
`MeshCache` (cache replay) both call it, so the rule exists in one place instead of two copies of a
decode loop.

Embedded (`.glb`) images have no file identity, hence no handle and no de-duplication;
content-hash de-dup is a possible later refinement.

**Side effect worth knowing:** a cold import of a mesh with *n* unique external maps writes *n*
`.meta` sidecars, one beside each extracted image — the reason `ImportAsset` no longer needs the
batched flush it used to: each write touches only the file it identifies.

**Flip discrepancy (known, deliberate):** file-based maps load unflipped, embedded glTF images
flipped. Unflipped is the correct one — glTF UVs are top-left origin and bgfx normalizes texture
origin to top-left, which is why `Texture2D(const std::string&)` explicitly does not flip. The
embedded path flips only because that is what it did before the loaders were consolidated;
reconciling it changes rendering on that path, so it is a separate change.

Editor chrome (`EditorLayer` icons, `ContentBrowserPanel` icons, the checkerboard) deliberately
stays on the `Texture2D(path)` constructor with hard-coded `resources/` paths — outside the asset
cache, since it has no asset identity and no reason to be evictable.

## Reload

`Reload(handle)` is plain eviction, dispatched on the metadata type. It finds the manager through
`AssetManagerRegistry::Find(type)`, so the per-type `switch` over cache maps is gone — but the
*ordering* around it is not:

| Type | Action |
|---|---|
| Texture, Environment | `manager->Evict(handle)`. |
| Material | Two steps, in this order: (1) read the cached material with `Find(handle)` and evict its three map paths from the texture manager; (2) evict the material. |
| StaticMesh | Three steps, **in this order**: (1) walk the cached mesh’s materials’ `Get*MapPath()`s, resolve each through `GetHandle`, evict those from the texture manager; (2) `MeshCache::Invalidate` — delete the `.meshcache` file; (3) evict the mesh. |
| Scene, Script, Audio, Prefab | Nothing; `Find(type)` returns null, because they have no manager. |

Step (1) is not optional, and **weak caching did not make it unnecessary** — which is worth stating
because it is the natural assumption. It is a rule about the *texture* cache, not about who owns the
material: the reloaded material or mesh re-resolves its map paths through `LoadMaterialMap`, which is
a `GetAsset<Texture2D>`, and a texture still cached would be handed straight back. The mesh step (2)
is what makes Reload mean *reimport now* rather than *recheck the timestamp* — the cache’s timestamp
check already catches source edits, but a referenced external texture can change while the `.gltf`’s
own timestamp does not.

`Evict` erases the whole cache entry, dropping the manager’s own reference. Anything still holding a
`Ref` keeps the old object, unchanged, and the next `GetAsset` builds a *new* one beside it — which
is exactly the contract the invariant below describes.

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
  through the asset index); **embedded** (glb) images are kept as compressed bytes on the `Material`
  so the cache can persist them.
- `MeshImporter::Instantiate(scene, path)` — used by viewport drag-drop — imports the asset (minting
  its handle and sidecar if new) and creates an entity with a `StaticMeshComponent`.

### Skinning data

A rigged glTF carries its skeleton and its clips **inside the `Mesh` asset**
([`Animation.h`](../../GanymedEngine/source/GanymedE/Renderer/Animation.h)): a `.glb` ships mesh,
skin and animation in one file, so separate clip assets would buy identity surgery and nothing else.

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
