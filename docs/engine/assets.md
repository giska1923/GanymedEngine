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

Components reference handles, never paths — through an [`AssetRef<T>`](#assetreft) for the four
managed types (`StaticMeshComponent.Mesh`, `SkyLightComponent.Environment`) and as a bare
`AssetHandle` for the path-resolved ones (`ScriptComponent.Script`, `AudioSourceComponent.Clip`,
`PrefabInstanceComponent.Source`). Either way the scene serializer writes the handle. A handle
resolves because the `.meta` sidecar beside the asset says so, so a path can move — including via
`git mv` — without breaking a scene, as long as its sidecar moves with it.

## AssetManager

[`AssetManager`](../../GanymedEngine/source/GanymedE/Assets/AssetManager.h) is a static facade over
one derived index plus a registry of per-type managers (see *Managers and caching*):

| API | Behavior |
|---|---|
| `Init(writableAssets = true)` / `Shutdown()` | Read the legacy registry if present, then `ScanAssets()`; `false` makes every write into `assets/` a no-op. The editor calls these in `EditorLayer::OnAttach/OnDetach`, the runtime in `RuntimeLayer::OnAttach/OnDetach` |
| `ScanAssets()` | Walk `assets/` and rebuild the index from the `.meta` sidecars found there, minting and writing one where a recognized asset has none. Called by `Init`; safe to call again to pick up files added outside the editor |
| `ImportAsset(relativePath)` | "Ensure this file has a sidecar, and tell me its handle." Idempotent: an indexed path returns its handle. Unsupported extensions log a warning and return the invalid handle |
| `GetHandle(path)` / `GetMetadata(handle)` / `GetAssetType(handle)` | Lookups |
| `GetAsset<T>(handle)` | Cached load through the type’s manager. Available for `Mesh`, `Environment`, `Texture2D`, `Material` — and only those, enforced by an `IsAssetType<T>` `static_assert`. Prefer an [`AssetRef<T>`](#assetreft) member; this is for one-shot lookups |
| `Reload(handle)` | Evict the loaded asset so the next `GetAsset` re-reads it from disk |
| `GetCacheStats()` | One `{TypeName, Resident, Retained}` row per registered manager, for the editor’s Stats panel |
| `OrphanedMetaCount()` / `CleanOrphanedMeta()` | `.meta` sidecars the last scan found with no asset beside them, and the action that deletes them. Split because detection is safe and deletion is not — see [Orphaned sidecars](#orphaned-sidecars) |
| `IsAssetsWritable()` | "This process may write into `assets/`" — one flag, one meaning, for sidecars and every other asset-file writer |

`Init(false)` is for a **shipped game**: it must not write into its own install directory (under
Program Files that fails outright), and it has nothing to persist anyway. The guard is checked at the
one place identity is decided rather than at each call site, so no future caller can bypass it — and
that matters, because imports happen *during scene deserialization* for path-based components, long
before any explicit save. `IsAssetsWritable()` exposes the same flag for code that writes other
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

### Orphaned sidecars

A `.meta` whose asset is no longer on disk is **detected on every scan and deleted only on
request**, and the split is the whole design.

Detection is free: the walk already visits the file, so an orphan costs one `exists()` on the path
with `.meta` stripped. Each one is logged **by path**, not counted:

```
1 orphaned `.meta` sidecar(s) - the asset each one names is not on disk. ...
  orphaned sidecar: textures/_deleted_ghost.png.meta
```

Deleting is a separate, explicit action — **Content Browser → right-click the background → Clean
orphaned `.meta`** — for a reason that outweighs the tidiness. A sidecar is the *only* record of the
handle every scene uses to name that asset. "The asset is not there" and "the asset is not there
*yet*" look identical on disk, and the second case is ordinary: a partial checkout, a branch that
does not carry it, a file mid-move, an LFS pointer not yet fetched. A boot-time sweep would silently
destroy identity in exactly the situations where the tree is least trustworthy, and the damage is
unrecoverable — the next scan mints a fresh handle and every scene reference is dead. Reporting is
always safe; reaping never is. Reaping therefore needs a person who has looked at the paths.

Two narrower rules fall out of the same reasoning:

- **`.bad` files are never orphans.** A quarantined sidecar exists precisely so its handle can be
  recovered by hand; reaping it would defeat the quarantine.
- **A sidecar beside an unindexed file is not an orphan.** `notes.txt.meta` has its asset — it is
  simply not an extension the scan recognizes. Treating it as debris would make the sweep an
  arbiter of which extensions are allowed to exist.

`CleanOrphanedMeta()` re-checks each path before removing it, because the list is as old as the last
scan and the asset may have arrived in between — verified: a sidecar listed as an orphan whose asset
appears before the sweep runs is skipped, and both files survive. It is a no-op when `assets/` is
read-only, which is every shipped runtime.

### The scan

`ScanAssets()` walks `assets/` with a `recursive_directory_iterator`, calls
`disable_recursion_pending()` on any directory whose name starts with `.` (`.compiled/`, and the
abandoned `.assets/`), and skips `.bad` files and any extension
`AssetTypeFromExtension` does not recognize. A `.meta` is skipped as an *asset* but not ignored:
the walk is already standing on it, so it costs one `exists()` to notice that the file it names is
gone (see [Orphaned sidecars](#orphaned-sidecars)). Paths are collected, **`std::sort`ed**, and only then
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

Orphaned sidecars are reported separately and by name rather than as another counter on that line,
because each one needs a decision rather than a tally.

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
(`IsAssetsWritable()` becomes false) instead of quarantining the file. Nothing overwrites it any
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
[`ASSET_PIPELINE_ROADMAP.md`](../history/ASSET_PIPELINE_ROADMAP.md) decision 14.

### Type ids

`AssetTypeIdOf<T>()` returns a dense `uint8_t` assigned from a counter on that type’s first use, so
resolving a manager is an **array index, not a `std::type_index` hash**. It sat on the render path
when every consumer re-fetched by handle per entity per frame; `AssetRef` moved that to once per
reference, and the array index is what keeps the re-resolve after an eviction cheap too.
`MaxAssetManagers` is 16, sized for
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
| **Texture2D** | `CompiledCache::Open` — the compiled DDS, block-compressed by `TextureCompiler` if stale | `Texture2D::CreateFromContainer` |
| **Material** | `MaterialSerializer::ReadDesc` — `.gmat` YAML into a `MaterialDesc` | `MaterialSerializer::Build` — creates the `Material` and resolves its map paths through `LoadMaterialMap` |
| **Mesh** | `CompiledCache::Open` then `MeshCompiler::Read` — the compiled blob, built by `MeshCompiler` if stale, deserialized into a `MeshSource` — then `DecodeEmbeddedMaps` | `BuildMesh`; then `MaterialSerializer::GenerateSidecars` (see *Materials*) |
| **Environment** | `Environment::Load` — `stbi_loadf` of the equirectangular HDR | `Environment::Create` — uploads it and submits the IBL bake, see [rendering.md](rendering.md#environment--ibl) |

The two empty `Parse` stages are not the same kind of gap:

- **Environment is correct as it stands.** The separable CPU part is one `stbi_loadf` of the
  equirectangular HDR; everything after it is the bake — six cube faces plus prefilter mips rendered
  through bgfx views — which can never leave the submit thread. Splitting would move a few percent
  of the cost and cost `Environment` its filepath constructor.
- **Mesh is fully split as of Phase 5**, and it was the last holdout. `MeshImporter::Import` emits a
  [`MeshSource`](../../GanymedEngine/source/GanymedE/Renderer/MeshSource.h) — vertices, indices,
  submeshes, material *descriptions*, skin data, skeleton, clips — with no bgfx call anywhere below
  it, and `BuildMesh` is the single main-thread step that turns one into a live `Mesh`. Before that,
  the compiler had to build a `Mesh` (buffers, materials, textures) purely so it could serialize it,
  which meant compiling on the submit thread and creating every GPU object twice on a cold import.

`GenerateSidecars` now runs inside `ApplyMesh`, i.e. *before* the manager caches the mesh, where it
used to run after the cache insert. Safe because it only ever reaches `ImportAsset`, never
`GetAsset<Mesh>`: it writes files and registers handles, so nothing in it can re-enter the load.

Scene, Script, Audio and Prefab have no manager at all — see *Path-resolved types* above.

### The cache is weak, and its owner is the scene

Each manager caches `std::weak_ptr<T>`, so an asset stays resident exactly as long as something
references it. That is the fix for "nothing is ever unloaded": the four strong maps this replaced
had no eviction path but `Reload` and `Shutdown`, so opening a 200-asset scene and then switching
scenes left every texture from the first one resident.

The owner is an `AssetRef<T>`, which means it is a component, which means it is a scene. Close a
scene and its meshes, materials, textures and environments go with it — measured, not asserted:

```
PROBE stats [scene live]       Mesh: resident=1 tracked=1   ... Material: resident=1 tracked=1
PROBE stats [scene destroyed]  Mesh: resident=0 tracked=1   ... Material: resident=0 tracked=1
```

`ResidentCount()` counts live objects; `TrackedCount()` counts cache entries, including ones whose
object has been collected. An expired entry is reclaimed the next time that handle is loaded, and
nothing sweeps them otherwise — a bounded cost that would need a great many one-shot handles to
matter. `AssetManager::GetCacheStats()` returns both per manager and the editor renders them under
**Stats → Asset Cache**.

The consequence to keep in mind: **a transient `Ref` is not ownership**. Code that loads an asset
and drops the reference before anything else takes it will see the object collected immediately and
re-imported on the next request. `MeshImporter::Instantiate` is the case that hit this — it now
resolves through the `AssetRef` it is about to store rather than through a local `GetAsset`.

Two things that hold assets alive and are easy to forget: `Renderer3D` keeps a `Ref<Environment>`
for the duration of a frame, and the editor's undo stack snapshots whole components, so an
`AssetRef` in undo history keeps its asset resident. Both are correct — they are real references —
but they mean resident counts lag a scene close until the undo stack is cleared. The content
browser needs no such care: its icons are fixed `resources/` textures outside the asset cache, so
the thumbnail-cache hazard the design anticipated does not exist here.

## `AssetRef<T>`

[`AssetRef.h`](../../GanymedEngine/source/GanymedE/Assets/AssetRef.h) is the primary asset API.
A component holds one instead of a bare `AssetHandle`:

```cpp
struct StaticMeshComponent
{
    AssetRef<Mesh> Mesh;
    std::vector<AssetRef<Material>> MaterialOverrides;
};
```

and a consumer resolves it instead of calling `GetAsset<T>`:

```cpp
const Ref<Mesh>& mesh = meshComponent.Mesh.Get();
if (!mesh)
    continue;
```

| Member | Behaviour |
|---|---|
| `Handle()` / `SetHandle(h)` / `Reset()` | The identity. `SetHandle` drops the cached object with it |
| `HasHandle()` | "Is a reference authored here?" Cheap, never loads — the replacement for `IsAssetHandleValid(field)` |
| `Ready()` | "Is there an object to use?" Resolves. A valid handle whose asset is missing answers false |
| `Get()` | `const Ref<T>&`, resolving on first use. Null for an unset handle *and* for one that failed to load |
| `operator->` / `operator*` | For code that has already established the asset is there |
| `operator==` | Handle equality — two references to one asset are equal whether or not either has resolved |

There is deliberately **no `operator bool`**: "has a handle" and "has an object" are different
questions, and one implicit answer would hide which a call site meant.

**It is composition, not inheritance from `Ref<T>`.** The plan
([`ASSET_PIPELINE_ROADMAP.md`](../history/ASSET_PIPELINE_ROADMAP.md) decision 6) called for
deriving from `Ref<T>` the way BlankEngine's `ResPtr` derives from `shared_ptr`, on the argument that
slicing is harmless because the wrapper "adds no data members, so a slice loses only API, not
state." That is true of `ResPtr`, which has no members because it resolves eagerly at construction.
It is not true here: lazy resolution *requires* the handle to be a member, so a slice to `Ref<T>`
would silently drop identity and leave a reference that can never be re-resolved or reloaded — and
`reset`, `operator=` and `swap` would each be a public way to desync it. The drop-in benefit that
justified deriving was worth about 24 call sites; the few that want a plain `Ref<T>` say `.Get()`.

**Serialization is unchanged.** An `AssetRef` writes as `static_cast<uint64_t>(field.Handle())` and
reads back through `AssetRef<T>(AssetHandle(node.as<uint64_t>()))`, so every key and value in every
`.ganymede` and `.gprefab` is byte-identical to the bare-handle format. Verified by round-tripping a
scene through load → save twice: the second pass is byte-for-byte identical to the first, with every
handle preserved including unset interior slots in a `MaterialOverrides` sequence.

### What resolves and what does not

`Mesh`, `Material`, `Environment` and `Texture2D` fields are `AssetRef<T>`. `ScriptComponent::Script`,
`AudioSourceComponent::Clip` and `PrefabInstanceComponent::Source` stay **bare `AssetHandle`s** —
they are the path-resolved types, they have no manager, and `AssetRef<T>` refuses to instantiate for
them with a `static_assert` naming the reason.

### Eviction and the epoch

`AssetRef` caching the object breaks an invariant the asset layer used to rely on: consumers
re-fetched by handle every frame, so `Reload` landed in the viewport on the next frame for free. A
reference that remembers the object would keep the stale one forever.

The fix is a process-wide `Detail::g_AssetEvictionEpoch`, bumped by every `Evict`/`EvictAll`. `Get()`
compares it against the epoch it last resolved at and re-resolves when they differ. One counter for
every type, not one per manager: eviction is a rare editor action, and over-invalidating costs a
single manager cache hit per live reference, once.

**The subtle part, and it was a real bug before it was a rule:** `Get()` resolves into a temporary
and only then assigns. Releasing the cached `Ref` *first* would drop the last reference to an asset
this happens to be the sole owner of, the weak cache entry would expire, and one `Reload` of any
handle would re-import the entire scene. Holding the old object across the `Load` call means an
unaffected handle gets a cache hit and the very same pointer back. Measured: reloading one `.gmat`
replaces that material's object and leaves the mesh's pointer identical.

The roadmap expected Phase 6 to replace this with reload-in-place — re-running parse/apply into the
*existing* object. **It did not, and the epoch is the better mechanism.** Reload-in-place would swap
a live `Mesh`'s vertex buffer while a pass may already have submitted with it, which is precisely the
mid-frame hazard the plan names as hot reload's main risk; evicting cannot cause it, because the old
object stays alive and unchanged for whoever holds it and the new one appears at the next `Get()`.
The epoch is what makes that invisible to the holder, which was reload-in-place's entire argument.


## Asynchronous loading

`AssetManager::GetAsset<T>` and `AssetRef<T>::Get()` **return null while an asset is still
loading**, and both are non-blocking. A load is two halves:

```
main thread                    worker                    main thread
Load(handle) ───► queue parse ─► ParseFn ──► ready ─► AssetManager::Update ─► ApplyFn ─► cache
   returns null                  (file IO,                (once per frame,      (bgfx)
                                  compile,                 from Application::Run)
                                  deserialize)
```

`Load` is **main-thread only and asserts it**. That is stronger than the roadmap's "assert on Apply",
and it is what makes the manager's three maps — cache, pending, failed — plain containers with no
lock: a second `Load` for the same handle joins the first rather than racing it, because both happen
on one thread. Everything that can be slow is inside `ParseFn`, and every bgfx call is inside
`ApplyFn`. The assert was negative-tested by loading from a worker: it fires and takes the process
with it.

**Measured, on this project's five textures with a cold `assets/.compiled/`:**

| | |
|---|---|
| Same loads forced synchronous (`WaitFor` each) | **845 ms**, blocking the frame loop |
| Asynchronous, worst steady frame while ~970 ms of compiles ran | **~10 ms** |

The compiles themselves did not get faster — they moved. The frame loop ran at 7–11 ms throughout,
including the frame during which a 566 ms BC3 encode was in flight.

### The Apply budget

Apply cannot leave the main thread — creating bgfx resources belongs to the submit thread — so it is
the one half of an asynchronous load that can still cost a frame. It used to be unbounded: every
parse that had finished was applied in the same `Update`, which was invisible while the only burst
was a cold open of five textures and very visible once hot reload made bursts easy to cause.

`AssetManager::Update` now runs on a **4 ms budget shared by every manager**. Anything ready that
does not fit stays pending and lands on a later frame; nothing is lost and no work is repeated,
because the parse is already done and only the GPU-side half is deferred. The editor's Stats panel
shows `Apply: N done, M deferred, x / 4.0 ms`.

Two properties are deliberate:

- **Time, not a count.** Applies are not comparable to each other — a `Material` is a few uniforms,
  a `Mesh` is vertex buffers plus every texture it pulls. A count low enough to spread meshes would
  throttle materials for no reason.
- **The first apply of a frame always runs**, whatever the clock says. An apply cannot be
  interrupted half way, so a budget allowed to refuse everything would stall loading permanently the
  moment one asset costs more than the whole allowance — and one does. Guaranteed forward progress
  beats a ceiling that cannot be honoured anyway.

Measured on a burst of 22 assets reloading at once, same build, same burst:

A burst of 22 assets reloading at once, **x64 Release**, both changes measured independently:

| | Applies in the worst frame | Worst Apply |
|---|---|---|
| Unbounded, decode in Apply *(what it used to do)* | 12 | **34.9 ms** |
| 4 ms budget, decode in Apply | 6 | 23.2 ms |
| Unbounded, decode in Parse | 12 | 12.1 ms |
| 4 ms budget, decode in Parse *(what it does now)* | 4 | **5.3 ms** |

Neither change is redundant and neither is sufficient. The budget bounds how many applies stack in a
frame but cannot subdivide one; moving the decode shrinks the individual apply but still lets a
dozen uploads land together. Together they are worth **6.6x** in Release. The same burst in Debug
runs 108.7 ms unbounded against 7.7 ms with both, i.e. 14x — Debug exaggerates the win because the
decode is the part that optimises, but it does not invent it: **34.9 ms is more than two frames at
60 Hz.**

A cost-predicting budget (refuse an apply whose estimated cost will not fit) was considered and
rejected before the cause was found: it would have recovered about 10 ms of the 70 ms Debug figure
while the floor stayed exactly where it was. In Release a typical apply is ~1.5 ms, so the 4 ms
budget admits two or three of them.

`WaitFor` is deliberately **not** budgeted: it means "I cannot proceed without this".

### Embedded textures are decoded during Parse

An image embedded in a `.glb` has no file, and therefore no `AssetHandle`, so it cannot go through
the texture manager the way a map that names a file does. `BuildMesh` used to hand its compressed
bytes straight to stb and decode them inline — on the submit thread, inside Apply. That single
detail was the dominant cost of applying a mesh, and it dwarfed the GPU work beside it:

**x64 Release**, with the Debug figure in brackets:

| `BuildMesh`, main thread | CesiumMan.glb | Fox.glb | RiggedFigure.glb |
|---|---|---|---|
| Resolving materials — **decode** in Apply (before) | 23.0 ms *(62.8)* | 15.5 ms *(52.3)* | 0.00 ms *(0.04)* |
| Resolving materials — **upload only** (now) | 1.4 ms *(1.7)* | 1.8 ms *(1.5)* | 0.00 ms *(0.03)* |
| `Mesh::Create`, the actual bgfx buffers | 0.4 ms *(1.5)* | 0.1 ms *(0.5)* | 0.05 ms *(0.4)* |

**The upload cost is the same in Release and Debug** — 1.4–1.8 ms either way — because it is driver
and GPU work, not compiled C++. The decode is what optimises, 3–4x. So what is left on the main
thread after this change is genuinely GPU-bound, which is the right place for it to be.

`DecodeEmbeddedMaps` runs in the mesh Parse stage, on a worker, filling the `*Decoded` slots of
[`MeshSource`](../../GanymedEngine/source/GanymedE/Renderer/MeshSource.h) with `DecodedImage`s — the
same Parse/Apply seam type file-backed textures already use. Apply is left with `Upload` and nothing
else. RiggedFigure has no texture at all and always applied in ~0.4 ms; that is what a mesh apply
costs when it is only doing GPU work, and now every mesh is close to it.

The decode did not get cheaper — it moved. It shows up on a worker at 23.0 ms (CesiumMan) and
14.5 ms (Fox) in Release, 58 ms and 61 ms in Debug, which is what the job system is for.

Three details worth knowing:

- **The image cannot have changed.** `TextureImporter::LoadFromMemory(bytes, size, flip)` is defined
  as `Upload(DecodeFromMemory(bytes, size, flip))`. The change is that exact composition split
  across a thread boundary, with `flip = true` preserved, so the result is identical by
  construction rather than by inspection.
- **The compressed bytes are still carried.** `MaterialSerializer::GenerateSidecars` extracts them
  to a real file on first import (see [Sidecar generation](#sidecar-generation-and-embedded-texture-extraction)),
  and re-encoding RGBA8 to recover them would be absurd. So a `MeshSource` in flight holds both
  forms; a decoded 1K map is 4 MB against a few hundred KB compressed, and it lives from the end of
  Parse until Apply. That is the cost of this trade, and it is the same unbounded-work-in-flight
  question a cold open already raises.
- **`MeshSource` is move-only now**, because `DecodedImage` owns stb's buffer through a
  `unique_ptr`. That is the correct shape for a type carrying tens of megabytes, and nothing was
  copying one.

### There are no placeholders, and that is a decision

[`ASSET_PIPELINE_ROADMAP.md`](../history/ASSET_PIPELINE_ROADMAP.md) decision 8 called for a
per-type placeholder — checkerboard texture, unit cube mesh, deliberately-ugly error material — on
the argument that returning null *"forces every call site to branch, and 24 call sites in a render
loop is exactly where you don't want that."*

**The premise does not hold here.** Every one of those call sites has branched on a null asset since
long before this phase, because a missing asset has always been possible. What placeholders would
actually change is what a *pending* asset looks like — and the fallbacks that already exist are
better than the ones proposed:

| Pending | What happens today | What a placeholder would do |
|---|---|---|
| A material's texture | `Material::Bind` gates on a null map; the surface renders with its albedo colour | A checkerboard, briefly, on every surface |
| `SkyLightComponent::Environment` | The procedural sky, which is the authored fallback | Black |
| A `MaterialOverrides` slot | The mesh's own material | A deliberately-ugly error material, and one shared `Ref` would collapse every pending material into one instancing batch |
| `StaticMeshComponent::Mesh` | The entity draws nothing | A unit cube at whatever scale the entity has |

A unit cube at the wrong scale is more confusing than nothing. Placeholders are a good idea in an
engine whose fallbacks are worse than the placeholder; this one's are better.

### The handoff window

This is the part that does not fall out of the design, and it was a live bug before it was a rule.

`Load` returns null while a parse is in flight, so **the caller walks away with nothing**. When
`Update` finally applies the asset, the only reference to it is a local inside the manager. Insert
that into a `weak_ptr` cache and it is collected before the function returns; the next frame's
`Load` sees an expired entry and starts the whole parse again. That is exactly what happened the
first time this ran: one mesh stuck at "pending" forever, nothing rendered, and the compiled-cache
hit counter climbed past 3800 in half a minute.

So a manager holds a **strong** reference to anything it has just applied, for `kHandoffFrames`
(four) calls to `Update`. **`AgeHandoff` must therefore run on every `Update`, including one with
nothing pending** — it used to sit after an early `return` taken when the pending set was empty, so
the window never closed once the last load completed and the manager kept a strong reference to
every asset it had ever applied asynchronously. The weak cache could not collect anything until some
later load happened to make the function run to the end again. Symptom: close a scene and `resident`
stays where it was instead of falling to 0. Anything that wants the asset is polling every frame — an `AssetRef` whose
`Get()` returned null re-asks by design — so it takes ownership on the next call and the manager's
grip stops mattering. Nothing takes it, it ages out and is collected, which is the weak cache
behaving exactly as intended. One frame would do in principle; four leaves slack for a consumer that
skips a frame.

### Materials re-ask for their maps

A `Material` is not an `AssetRef` holder: it captures `Ref<Texture2D>` once, when it is built. Under
synchronous loading that was fine. Under asynchronous loading a material built while its albedo was
still compiling would hold a null map **forever**, because nothing re-resolves it.

So each map carries its `AssetHandle` beside it, and `Material::Bind` re-asks once, for a map that is
null and has an identity. The lookup only happens while a map is missing; after it resolves this is
one null check per bind. An embedded texture — one that lives inside a `.glb` rather than as a file —
has no handle, is decoded directly during `BuildMesh`, and is never pending.

### `WaitFor`, and its one call site

`AssetManager::WaitFor(handle)` blocks until an asset is loaded and applied. It pumps other queued
jobs while it waits, so it cannot deadlock the pool — in fact enkiTS will often run the very parse
being waited on, on the calling thread, which is precisely the synchronous behaviour wanted here
(see [core.md](core.md#job-system)).

There is **one** call site: `MeshImporter::Instantiate`, which has to read a mesh's material list to
size and fill the new entity's override slots and cannot act on "not yet". Everything on a frame path
tolerates a null asset for a frame or two instead. Keep the list this short.

### Cancellation, eviction and shutdown

- **A scene swap cancels nothing, and does not need to.** Dropping a scene destroys its `AssetRef`s,
  but the manager's pending loads are keyed by handle and simply complete. If the new scene wants
  the asset it is already there; if not, it ages out of the handoff and is collected. No asset from
  the old scene can be "applied into" the new one — Apply only writes the manager's own cache.
- **`Evict` cancels and sets aside, rather than waiting.** enkiTS cannot dequeue a started task, so
  the choice is to block or to hold the `Future` somewhere until it retires. Blocking was measured
  at **186 ms** for a Reload issued while a 2560×1664 texture was mid-encode. The cancelled `Future`
  now moves to a draining list that `Update` reaps once `IsReady()`, and the stall is gone. Holding
  it is safe because the pending entry is already erased, so nothing can read the result.
- **Compiles poll for cancellation at coarse boundaries**, which is what the threading milestone's
  contract asks of any long body: `CompiledCache::Open` checks before invoking a compiler (the cheap
  and common case — a cold open queues everything at once, so most cancelled parses have not started
  yet), and the texture encoder checks between mips.
- **Shutdown cancels everything first, then destroys.** `AssetManager::Shutdown` calls
  `CancelPending` on every manager before `Clear`, so a scene's worth of in-flight parses drain
  concurrently rather than one `Future` destructor at a time. Verified by closing the editor with
  three loads pending and four compiles running: clean exit, code 0.

### What is still synchronous

**Everything inside Apply**, and only that. Creating bgfx buffers and textures is main-thread work by
construction. What moved off is file IO, decode, deserialization and compilation.

`Environment` was the last holdout and is no longer one. The note that used to sit here said its
separable CPU part was "a few percent" of an environment load — **that was wrong, and measuring it is
what closed the gap.** Release, 1K panorama:

| | Before | Now |
|---|---|---|
| `stbi_loadf` of the panorama | 21–29 ms, main thread | 16–25 ms, **worker** (`Environment::Load`) |
| Two forced `bgfx::frame()` calls | 24–26 ms, main thread | **gone** |
| Upload + bake submission | ~4 ms, main thread | ~4 ms first load, **2.2–3.3 ms** after |
| **Total on the main thread** | **54–62 ms** | **6.3 ms** first load, **2.2–3.3 ms** after |

The decode was 45% of it and the forced frames were another 45%; the bake's own GPU submission — the
part that genuinely cannot move — was never more than about 4 ms, and the bake programs and BRDF LUT
are now shared across environments rather than rebuilt per load. See
[rendering.md](rendering.md#the-ibl-bake-is-a-prepass) for why the forced frames existed and what
replaced them.

An environment now loads like everything else: **null for one frame**, during which a sky light falls
back to the procedural sky, then resident. Verified frame by frame — frame 0 renders the procedural
gradient, frame 1 renders the baked skybox, and frames 2–7 are pixel-identical to frame 1.

## Hot reload

Edit an asset on disk, see it in the viewport — no restart, no button.
[`AssetWatcher`](../../GanymedEngine/source/GanymedE/Assets/AssetWatcher.h) polls `assets/` and turns
a file change into an eviction; everything after that is machinery Phases 3–5 already built.

Watching is **on in the editor, off in the runtime** — the switch is `IsAssetsWritable()`, because
an install that treats `assets/` as read-only has no editor to reload into and its content does not
change under it.

### A poll, not a file watcher

The roadmap's step 1 offered `ReadDirectoryChangesW` behind a `FileWatcher` interface or an mtime
poll, recommended the poll, and said not to build both. The poll it is, and the number that would
justify changing that is on screen: the editor's Stats panel shows **ms/poll** live.

| | |
|---|---|
| Poll interval | 0.25 s |
| Cost, 23 indexed assets | **0.5–1.5 ms** per poll, i.e. every 15th frame does one extra millisecond |

`last_write_time` plus `file_size` per indexed asset, keyed by handle. That is *not* a content check
— deciding whether the bytes really changed is `CompiledCache`'s job one layer down, and it already
does it properly. The poll's only question is "did anything move".

### The debounce, and why it is not just coalescing

A change has to still be there, **unchanged**, on a later poll before it counts (0.2 s). Coalescing
a burst of writes is the obvious reason. The load-bearing one is different: editors save by writing
a temp file and renaming, sometimes touching the result again, and reading one mid-write parses into
a *failure* — which a manager remembers until something evicts it. Waiting for the writer to stop is
what keeps a hot reload from turning a good asset into a broken one. A file whose stamp changes and
changes back inside the window produces no event at all, which is what a `git checkout` landing on
identical content looks like.

The new stamp is accepted *before* the reload is attempted. A source that is broken right now must
not be retried every poll forever; the next real edit moves the stamp again, so a mid-write read
recovers on the following save rather than needing a manual Reload.

### What a change actually does

`AssetManager::OnAssetModified(handle)` — and it is **narrower than `Reload` in both directions**:

- It **evicts, and does not invalidate the compiled output.** `Reload` deletes the `.gres` because it
  means *reimport now*; a file event does not. The epoch record already tells a real edit apart from
  a rewrite that lands identical bytes, and forcing a recompile on every mtime move would turn a
  no-op save of a 2K texture into a multi-second BC7 encode. This is the case the content hashing in
  [Epoch invalidation](#epoch-invalidation) was built for, and hot reload is where it pays.
- It **does not reach down into an asset's own textures.** `Reload` evicts a material's maps so a
  reimport re-reads them; here the texture did not change, so dropping it would only cost a re-upload.
- It **does reach outward**, to whatever captured the changed asset — see below.
- It returns false for a type with no manager, so saving a `.ganymede` from the editor is silent
  rather than logging a reload of something nothing caches.

### Dependency propagation, by scanning rather than by a map

The rule stated under [Reload](#reload) is *hold an `AssetRef<T>`, not a `Ref<T>`* — and the engine
breaks it in exactly two places, for good reasons: a `Material` captures `Ref<Texture2D>` when it is
built, and a `Mesh` owns its `Ref<Material>`s outright. Neither notices the eviction epoch. So a
changed texture has to evict them too, or the viewport keeps the old image.

| Changed | Also evicted | Why |
|---|---|---|
| Texture | every resident `Material` whose three map handles include it | it captured the `Ref` and nothing re-resolves it |
| Texture | every resident `Mesh` one of whose materials does | a mesh's materials come from the compiled blob's material *descriptions*, so only rebuilding the mesh re-resolves them; the blob is a cache hit, so this is a file read and a buffer upload, not a re-import |
| Material (`.gmat`) | nothing | components hold `AssetRef<Material>`, which the epoch already covers |
| Mesh, Environment | nothing | nothing captures either by raw `Ref` |

Roadmap decision 11 asks for an `unordered_map<AssetHandle, vector<AssetHandle>>` reverse-edge map
populated as assets load. **Scanning the resident set is the better shape here**, for one specific
reason: the cache is *weak*, so "which materials reference this texture" is only ever a question
about resident objects. A maintained map would accumulate edges to objects that have since been
collected, the weak cache offers no hook to prune them, and it would be wrong between sweeps. The
scan cannot go stale, needs no bookkeeping on the load path, and costs three handle compares per
resident material — a few hundred integer comparisons, at the rate a human saves a file. Comparing
`AssetHandle`s rather than paths also means a material built *before* its map finished loading (null
texture, valid handle) still answers correctly.

### Deletion

A deleted asset **keeps its handle and its index entry** and simply stops loading: the parse finds
no source, the manager records the failure, and consumers see null — a mesh draws nothing, a
material renders its albedo colour. Dropping the index entry would be wrong, because scenes still
reference it and the file may come back; a branch switch does exactly that. Restoring the file moves
the stamp again, which evicts (clearing the failed record) and reloads it.

### The storm switch

`git checkout` across a branch that touches many assets changes hundreds of files at once. Two
things bound it:

- **At most 16 reloads are accepted per poll.** The rest keep their settled state and land on the
  next one. Every accepted change queues a parse that holds its compiled bytes until Apply, so
  unbounded, a texture-heavy branch would put the whole set in flight simultaneously — gigabytes for
  2K BC7. (The same unbounded-in-flight property is true of a cold open and is not new here; this
  only declines to make it easy to trigger.)
- **A switch in the editor's Stats panel.** Turning watching back on *adopts* what is on disk rather
  than reloading it — otherwise the switch would defeat itself.

Measured, rewriting all 23 indexed assets in one go: the first poll accepted 16 and deferred 7, the
next took the rest, and the worst frame in between was **110 ms** in a Debug build. That is the
batched Apply — 16 assets creating GPU resources in one frame — not the watcher, whose poll stayed
under 1.5 ms throughout. Under a live 40-mesh scene, four consecutive rounds of touching every asset
produced 48 reloads, **zero recompiles**, and identical draw statistics after every round.

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
the mesh blob serializes materials inline and is invalidated by the source's own epoch — so editing a `.gmat`
would either silently not invalidate the cache, or require a dependency-tracking index the engine
does not have. The additive shape costs one duplicated default (the mesh's material and its sidecar
start identical) and buys three things: no cache-format change, no risk to existing scenes, and
**no reverse-dependency index anywhere**. That last one is the shape's best property and worth
stating plainly: `Reload(.gmat)` needs no material→mesh map because meshes never reference `.gmat`.
Only override slots do, and each of those is an `AssetRef<Material>` that re-resolves itself after
an eviction, so `Reload` lands in the viewport with no index to maintain.

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
needing its `.meshcache` deleted first. Gated on `IsAssetsWritable()`: the runtime never writes
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
`MeshCompiler` (blob replay) both call it, so the rule exists in one place instead of two copies of a
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
| Texture | `CompiledCache::Invalidate` then `manager->Evict(handle)` — evicting the object alone would re-open the same stale `.gres`. |
| Environment | `manager->Evict(handle)`. |
| Material | Two steps, in this order: (1) read the cached material with `Find(handle)` and evict its three map paths from the texture manager; (2) evict the material. |
| StaticMesh | Three steps, **in this order**: (1) walk the cached mesh’s materials’ `Get*MapPath()`s, resolve each through `GetHandle`, evict those from the texture manager; (2) `CompiledCache::Invalidate` — delete the compiled blob and its `.dep`; (3) evict the mesh. |
| Scene, Script, Audio, Prefab | Nothing; `Find(type)` returns null, because they have no manager. |

Step (1) is not optional, and **weak caching did not make it unnecessary** — which is worth stating
because it is the natural assumption. It is a rule about the *texture* cache, not about who owns the
material: the reloaded material or mesh re-resolves its map paths through `LoadMaterialMap`, which is
a `GetAsset<Texture2D>`, and a texture still cached would be handed straight back. The mesh step (2)
is what makes Reload mean *reimport now* rather than *recheck the timestamp* — the cache’s timestamp
check already catches source edits, but a referenced external texture can change while the `.gltf`’s
own timestamp does not.

`Evict` erases the whole cache entry, dropping the manager’s own reference, and bumps the eviction
epoch. Anything still holding a `Ref` keeps the old object unchanged; anything holding an `AssetRef`
notices the epoch and re-resolves, so the next `Get()` builds a *new* object beside the old one.

**Why plain eviction is safe** — this is the invariant the asset layer relies on:

- Components hold an `AssetRef<T>` or a bare `AssetHandle`, never a raw `Ref`.
- Every `AssetRef` re-resolves after an eviction (see [Eviction and the epoch](#eviction-and-the-epoch)),
  so a `Reload` lands on the next access rather than needing a per-frame re-fetch;
  `Renderer3D::s_Data.ActiveEnvironment` is overwritten by the next `SubmitEnvironment`.
- Evicted objects drain via `shared_ptr` refcount, and bgfx defers handle destruction to frame end,
  so evicting mid-frame from ImGui code cannot pull a texture out from under an in-flight draw.

The rule for future consumers: **hold an `AssetRef<T>`, not a `Ref<T>`.** A raw `Ref` cached on a
component is invisible to eviction, keeps its asset resident forever, and goes stale across a
`Reload` with nothing to notice.

Asset roots: paths resolve against `GetAssetRoot()`
([`AssetPaths.h`](../../GanymedEngine/source/GanymedE/Assets/AssetPaths.h)) — the relative
directory `assets/`, i.e. **relative to the working directory**, which is why the apps must run
with their project folder as CWD (each app has its own `assets/`; the editor's is
`GanymedEditor/assets/`).

## Mesh import (cgltf)

[`MeshImporter::Import`](../../GanymedEngine/source/GanymedE/Renderer/MeshImporter.cpp) reads glTF
2.0 (`.gltf`/`.glb`) via the header-only cgltf into a
[`MeshSource`](../../GanymedEngine/source/GanymedE/Renderer/MeshSource.h) — **CPU data only, no bgfx
call anywhere below it**, which is what lets it run on a worker. `BuildMesh` is the main-thread half
that turns one into a live `Mesh`.

- Walks the node tree **depth-first into a vector**, flattening every mesh primitive into one
  interleaved vertex/index buffer with a `Submesh` per primitive. Traversal order is part of the
  contract: submesh order must be stable across runs or cache diffs and joint↔submesh correlation
  become impossible to reason about.
- Reads position/normal/tangent/texcoord; missing normals/tangents get defaults. A primitive with
  no index accessor is legal glTF (triangle soup in draw order) and gets a synthesized `0..n-1`
  index list, since the engine always draws indexed.
- Materials map from glTF PBR metallic-roughness: base color factor/texture, normal map,
  metallic-roughness map, two-sided flag, alpha mode → `IsTransparent`. The importer produces
  `MeshMaterialSource` *descriptions* rather than `Material` objects: an external texture URI is
  recorded as an asset-root-relative path, an **embedded** (glb) image as its compressed bytes.
  `BuildMesh` is where a path becomes a texture, through `TextureImporter::LoadMaterialMap` so it
  de-duplicates through the asset index.
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

## Compiled outputs

`assets/.compiled/` is the source → artifact layer: a `.png` becomes a block-compressed, mipped
DDS, and a `.glb` becomes the binary mesh blob that used to live in `assets/.assets/`. It is
**derived and gitignored**; deleting the tree costs one recompile.

```
assets/.compiled/<h0h1>/<h>.gres    the artifact
assets/.compiled/<h0h1>/<h>.dep     its epoch record
```

`h` is a 64-bit FNV-1a of the asset-root-relative *source path*, so the tree is flat and bounded
however deep `assets/` gets. The two-character bucket keeps any one directory to a few hundred
files in a project with tens of thousands of assets. Hashing happens in one function
(`CompiledCache::OutputPath`), which is what makes adding a platform tag to the key a one-line
change the day a second build target exists — the machinery for that is deliberately *not* built
(roadmap decision 9).

### Compilers

[`IAssetCompiler`](../../GanymedEngine/source/GanymedE/Assets/AssetCompiler.h) is source bytes in,
compiled bytes out, plus a `Version()` and a list of discovered source dependencies. Registered per
`AssetType` at `Init`:

| Type | Compiler | Output |
|---|---|---|
| StaticMesh | [`MeshCompiler`](../../GanymedEngine/source/GanymedE/Assets/MeshCompiler.h) | the v7 binary mesh blob |
| Texture | [`TextureCompiler`](../../GanymedEngine/source/GanymedE/Assets/TextureCompiler.h) | a mipped BCn DDS |
| Environment, Material, Scene, Script, Audio, Prefab | *none* | — |

**A type with no compiler is not a gap.** `CompiledCache::Open` hands back the source bytes
untouched, which is the right answer for an `.hdr` environment — its expensive step is a GPU bake
that cannot be precomputed into bytes — and for a `.gmat`, which is already the compact form of
itself.

The contract is that **a compiler is a pure function of its `CompileInput` and touches no GPU**,
because the epoch decides staleness from exactly what that carries and because every compile now runs
on a worker. `MeshCompiler` used to break the second half — it built a live `Mesh` purely so it could
serialize it — which forced compilation onto the submit thread and created every GPU object twice on
a cold import. `MeshSource` split that in Phase 5; both compilers honour the contract now.

### Epoch invalidation

The `.dep` holds compiler version, source size, source mtime, source **content hash**, a hash of
the `.meta` Config block, and a `{path, hash}` pair per declared dependency. `EpochDiff` is a
bitflag set naming which of those moved, and it is in the recompile log line because "why did this
rebuild" is the question an invalidation bug makes you ask:

```
Compiled 'models/BoxTextured.glb' with MeshCompiler in 3 ms (6 KB -> 5 KB) [stale: compiler-version|config]
```

Only `CompilerVersion | SourceSize | SourceHash | Dependencies | Config` force a rebuild. **A moved
mtime alone does not** — mtime and size are a cheap pre-filter that decides whether hashing is worth
doing at all, and when the hash comes back unchanged the record is refreshed so the next boot
short-circuits again:

```
'models/BoxTextured.glb': mtime moved but content is unchanged - kept the compiled output
```

That is the whole reason the epoch record replaced `MeshCache`'s mtime compare. `touch` on a source,
a checkout that rewrites files, an editor that saves to a temp and renames — all of those moved
mtime without changing content, and all of them used to reimport.

Bumping a compiler's `Version()` invalidates every output it ever produced, at once, with nothing
to clear by hand. That is the property that makes editing an importer safe, and it is why the
constant is on the compiler rather than in the blob format.

**Dependencies are recorded but nothing declares many of them yet.** `MeshImporter` reports a
`.gltf`'s external `.bin` buffers and external image files, which is a real edge — edit the `.bin`
and the mesh blob rebuilds. A self-contained `.glb` reports none, and every mesh shipped here is a
`.glb`, so the machinery is exercised and the outcome is not. The roadmap's "edit a texture
referenced by a `.gmat`, confirm the dependent recompiles" is **not** a case that exists: `.gmat`
has no compiler, and a mesh blob stores texture *paths*, not pixels, so an edited texture correctly
invalidates only itself. The depth-1 reverse map of decision 11 waits for hot reload, which is the
thing that actually needs it.

### Persisting is best effort

The compiled bytes are returned whether or not they could be written to disk, so an install that
cannot write its own directory still runs — it just recompiles every boot, and says so once:

> This install treats assets/ as read-only but had to compile '…' — the assets/.compiled tree was
> not shipped with it.

**Compilation is a build-time step, and a shipped game should ship its `.compiled/` tree**, the way
UE ships cooked content. The fallback exists so a missing tree is slow rather than fatal.

## Texture compilation

[`TextureCompiler`](../../GanymedEngine/source/GanymedE/Assets/TextureCompiler.h) decodes the source
with the same stb path every other texture load uses, then hands RGBA8 pixels to
[`Platform/Bimg/TextureEncode`](../../GanymedEngine/source/Platform/Bimg/TextureEncode.h), which
generates mips and block-compresses into a DDS. `Texture2D` takes the container straight to
`bgfx::createTexture`, which parses DDS itself — so a mipped, compressed texture costs the engine no
more code than an uncompressed one.

`.meta` Config keys, all optional:

| Key | Values | Default |
|---|---|---|
| `Format` | `auto`, `BC1`, `BC3`, `BC4`, `BC5`, `BC7`, `RGBA8` | `auto` |
| `GenerateMips` | `true`, `false` | `true` |
| `MaxSize` | integer, longest edge; 0 for no limit | `0` |

**`auto` is BC1, or BC3 when the source actually uses its alpha channel — not BC7.** That is a
measured decision, not a preference. bimg's BC7 encoder is NVIDIA's AVPCL *reference*
implementation: an exhaustive mode and partition search, correct and extremely slow. Measured on an
optimised build:

| Texture | `auto` (BC1/BC3, libsquish) | `Format: BC7` (AVPCL) |
|---|---|---|
| 256×256 + mips | 9 ms | 6 399 ms |
| 1024×1024 + mips | 188 ms | 58 575 ms |

311× on the 1024². A 2048² albedo under BC7 would be minutes, which is not a pipeline. BC7 stays
available for the texture that is worth the wait; `auto` picks the format that lets an import
finish.

`MaxSize` drops top mips rather than resampling — the chain already holds every halving of the
source, so it is exact and needs no second filter.

**Block alignment is required for compression.** bimg derives its destination row pitch as
`width * bpp / 8`, which is only a whole number of blocks when the width is; a 1181×1181 source
would be written short. Rather than emit a subtly corrupt texture, the compiler falls back to RGBA8
and warns. Worth knowing what that costs: with mips on, an uncompressed fallback uses *more* VRAM
than the old no-mips path did (1181×1181 goes 5 448 KB → 7 259 KB). The fix is to resize the source
to a multiple of four.

Measured VRAM, this project's textures:

| Texture | Before (RGBA8, no mips) | After |
|---|---|---|
| 256×256 albedo | 256 KB | 42 KB (BC1, 9 mips) |
| 1024×1024 albedo | 4 096 KB | 682 KB (BC1, 11 mips) |
| 64×64 checkerboard | 16 KB | 2 KB (BC1, 7 mips) |

**There is deliberately no `sRGB` key.** See [rendering.md](rendering.md#colour-space) — the engine
has no sRGB pipeline at all, so the flag would either do nothing or silently change the look of
every scene, and that is a rendering change rather than an asset-pipeline one.

### Parallel encoding

The mip encode is split into horizontal bands and dispatched through
[`JobSystem::ParallelFor`](core.md#job-system) — the threading milestone's first consumer
([`THREADING_ROADMAP.md`](../history/THREADING_ROADMAP.md) T3), and the right one to be first:
offline work with no frame budget and no lifetime hazards, where a bug costs import time rather
than a corrupted frame.

A band is safe on another thread because the per-mip encode is a pure function of
(dst, src, width, height): rows of `blockHeight` pixels are independent block rows, a band of full
width and a block-multiple height is contiguous on both sides, and bimg allocates its own scratch
per call. Bands are only used when the arithmetic is exact — a width that is not a whole number of
blocks, or a mip smaller than one, is encoded in a single call, which is what bimg itself does.

**Only libsquish's formats are split.** BC1/BC3/BC4/BC5 go through squish, which has static
functions and no mutable state. The BC7 path is AVPCL, which writes four file-scope `bool`s per
block; they are set to the same constant every time, which makes the race benign in practice, and
"benign in practice" is not a thing to build a thread on.

Measured, 2560×1664 → BC3 with 12 mips, 15 workers:

| | Time |
|---|---|
| Serial | 597 ms |
| Parallel | 324–331 ms |

**1.8×, and the honest reading is that the fast encoder made threading matter less.** The same
split on BC7 measured 4.7× (30 306 ms → 6 402 ms) because the encode dominated everything. With
squish the serial remainder — mip generation, the alpha scan, container allocation, the DDS write —
is roughly half the call, and Amdahl does the rest.

## The mesh blob

[`MeshCompiler`](../../GanymedEngine/source/GanymedE/Assets/MeshCompiler.h) writes the fully-parsed
mesh (vertices, indices, submeshes, material scalars/paths/embedded texture bytes, skin vertices,
skeleton and animation clips) as a binary blob. This was `MeshCache`; what moved out is path and
invalidation policy, which `CompiledCache` now owns for every type.

The format is at **v7** (v4 added the skeleton, clips, the skin vertex stream and
`Submesh::IsSkinned`; v5 and v6 have that same layout and exist only to discard caches whose stored
*values* were stale — v5 the pre-correction `RootTransform`, v6 skinned submeshes written with an
identity `LocalTransform`; v7 drops the embedded source timestamp, which the `.dep` owns now). The
old `assets/.assets/` tree is simply abandoned: it is derived output, so a one-time recompile is
cheaper than carrying a reader for it.

`MESH_CACHE_VERSION` in the blob and `MeshCompiler::Version()` have to move together. The epoch
compares the latter; the magic and version inside the blob are a second line of defence against a
file that got past it, not the primary check.

Practical notes:
- Delete `assets/.compiled/` to force a full rebuild of everything. **Reimport** in the content
  browser's context menu does it for one asset, and is `CompiledCache::Invalidate` plus a `Reload`.
- The blob stores material *data*, not GPU resources; textures are created on load either from the
  recorded paths (via `TextureImporter::LoadMaterialMap`, same as cold import) or the embedded
  bytes. **Embedded textures are not block-compressed** — they have no file identity, so they never
  reach the texture compiler. Extracting them (which `MaterialSerializer::GenerateSidecars` already
  does on first import) is what puts them on the compiled path.
