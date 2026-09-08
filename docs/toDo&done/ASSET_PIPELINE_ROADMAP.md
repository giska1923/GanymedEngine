# Asset Pipeline & Resource Management Roadmap

Status: written 2026-09-08, branching from `master` at `ee4148f` ("Particles (#8)"), working tree
clean. Follows the format of [`PARTICLE_ROADMAP.md`](PARTICLE_ROADMAP.md) and
[`CONTENT_AUTHORING_ROADMAP.md`](CONTENT_AUTHORING_ROADMAP.md): decisions of record first, then
phases with per-phase verification, with execution notes appended as phases land.

The milestone thesis: Ganymed's asset layer is a **handle → path → eagerly-loaded object** map with
four hardcoded caches and one shared registry file. It works, and it has been enough to ship meshes,
materials, environments, audio, scripts, prefabs and particles. What it cannot do is the thing every
production engine's asset layer is actually built around: **separate the source file an artist edits
from the compiled artifact the runtime consumes, and separate "I reference this asset" from "this
asset is resident in memory right now."** Those two separations are what buy you async loading, hot
reload, texture compression, per-asset import settings, and a mergeable project — none of which
Ganymed can express today. This roadmap adds them, in the order that makes each step independently
useful and lets the call sites stop changing after Phase 3.

The reference design studied for this plan is BlankEngine (`D:\game`), specifically its
`AssetService`/`Asset`/`IAssetCompiler` layer and its `ResourceService`/`IResourceManager`/`ResPtr<T>`
layer. Appendix A maps every BlankEngine file this plan draws from. **This plan deliberately does not port
most of BlankEngine.** Read "Decisions of record" and "Not doing" before anything else — the honest
scope assessment is the most load-bearing part of the document.

---

## The honest scope assessment (read this first)

BlankEngine's asset + resource system is on the order of 15–20k lines of engine code, and it does not
stand on its own. It rests on four foundational subsystems that Ganymed does not have:

| BlankEngine dependency | What it provides | Ganymed equivalent |
| --- | --- | --- |
| **RTTR reflection** | Every registration DSL (`registration::Resource<T>`, `registration::ResourceManager<T,R>`, `registration::AssetCompilerBase<T>`), compiler config serialization + config versioning, `rttr::variant` metadata blobs, and `ResPtr<T>`-as-path serialization | None. No reflection system, no type registry. |
| **`ThreadService` + `Future<T>`** | Priority-tiered background tasks (`BackgroundPriority::{Low,Normal,High}`), cancellable futures, drained-cancellation bookkeeping — itself ~1,500 lines wrapping cpp-taskflow | None. `std::thread` appears in exactly two files, and one of them is Jolt's own job system (`Physics/PhysicsScene.cpp`); the other is the profiler (`Debug/Instrumentor.h`). Now scoped as its own milestone: [`THREADING_ROADMAP.md`](THREADING_ROADMAP.md). |
| **`VirtualFileSystem`** | Alias-mounted `IFileSystem`s (native + zip package), `IFile::readAsync` → `IAsyncData` with cancellation | None. Direct `std::filesystem` + `std::ifstream` everywhere. |
| **`ServiceManager` + `ExecutionGraph`** | Service locator with construction-order topological sort (`assignBefore`/`assignAfter`) and a per-frame taskflow graph (`runBefore`/`runAfter`, `tf::Subflow`) | None. Layers + scene systems with hand-ordered updates. |

So "port BlankEngine's asset system" is not an upgrade to `AssetManager` — it is building four foundational
subsystems and then rebuilding the asset layer on top of them. That is a multi-month project, and
most of the payoff is concentrated in a small fraction of it.

Where the payoff actually is, ranked honestly:

- **Identity and reference abstraction (Phases 1–3).** High value, ~1–1.5 weeks, no new
  subsystems required. This is the part the request is really asking for — `ResPtr<T>` is the
  "abstracted resource usage" — and it is also the part that pays for itself immediately: it
  removes the four hardcoded caches, removes the strong-reference leak, kills the
  `static_assert(sizeof(T) == 0)` primary template, makes the project mergeable, and — critically —
  **it is the shim that lets Phase 5 make loading asynchronous without touching a single call site.**
- **Compiled outputs (Phase 4).** Medium-high value, ~1 week. Generalizes the existing `MeshCache`
  into an asset-compiler interface and adds a BCn/DDS texture compiler. This is the single largest
  *runtime* win available: textures are currently PNG-decoded on the main thread at load and
  uploaded uncompressed. bimg is already vendored and already builds, so there is no new dependency.
- **Async loading (Phase 5).** High perceptual value, ~1.5 weeks, but it *requires* a job system —
  now its own milestone, [`THREADING_ROADMAP.md`](THREADING_ROADMAP.md), sequenced to land with
  Phase 4 so its first consumer is offline encode rather than live loading — and it collides with
  bgfx's single-threaded-submit model. Deliberately last of the "doing" phases.
- **Hot reload (Phase 6).** Editor quality-of-life. Cheap *if* Phases 1–3 landed; expensive before.
- **Everything else in BlankEngine** — generators and complex assets, mip streaming with GPU feedback and
  bindless residency, platform contexts and per-API outputs, the zip-mounted virtual filesystem, the
  statistics service, `AssetDependenciesService` at arbitrary depth, RTTR-driven config versioning —
  is either solving a problem Ganymed does not have or costs an order of magnitude more than it
  returns at this scale. See "Not doing" for the per-item argument.

**The cheaper 80% option, if you want one milestone rather than six phases:** do Phases 1–3 only
(`.meta` sidecars + typed manager registry with weak caches + `AssetRef<T>`) and stop. That is
roughly a third of the work, it fixes every structural wart in the current asset layer, and it
leaves the door open for Phases 4–6 without rework — because after Phase 3 the call sites no longer
know or care how loading happens. If you want a single-phase option, do Phase 2 + 3 and keep the
registry file; the merge-conflict problem is annoying but not blocking.

---

## How BlankEngine is structured (the design being drawn from)

Worth stating explicitly, because the two-layer split is the whole idea and Ganymed currently has
neither layer cleanly.

### Layer 1 — Assets are source files, compiled on demand

`Asset` (`assets/asset.hpp`) is not a runtime object. It is a triple of paths plus bookkeeping:

- `m_source` — the file the artist edits (`.png`, `.fbx`, `.mat`).
- `m_assetFilePath` — a sidecar `.asset` file holding the *compiler config* (e.g.
  `TextureCompiler::Cfg` with format, quality, mip settings). RTTR-serialized, versioned via
  `Cfg::C_VERSION` with a `m_cfgUpdateVersionFn` migration hook.
- `m_outputFilePath` — the compiled artifact, at a **hashed** path
  (`AssetService::createHashedFileName(source, platformCtx)` →
  `generateOutputPath(source, useOutputBuckets, platformCtx)`). Hashing the path means the output
  tree is flat and content-addressed per source+platform, so two platforms or two graphics APIs
  coexist without colliding.
- `m_depFilePath` — an intermediate `.dep` file holding `IntermediateData`: the compilation
  `Epoch`, the source-dependency set, and `CompileInfo` (platform context, compile duration, output
  size).

The invalidation model is the interesting part. `IntermediateData::Epoch` records compiler version,
file size, modification date, content hash, source-dependency hashes and compressor hash;
`Epoch::calculateDiff()` returns an `EpochDiff` bitflag set naming *which* of those changed, and
`checkRecompile()` decides from the flags. This is strictly better than Ganymed's `MeshCache`
mtime-only check: a touched-but-unchanged file doesn't recompile (hash matched), a changed *compiler*
recompiles everything (version bumped), and a changed *dependency* recompiles the dependent
(`SourceDependencies` flag) while a changed *external* reference does not.

`AssetService::openAsset(path, flags, ...)` is the front door, with `OpenFlags{FindInCache, Async,
ForceImport, ReimportDependents}`. `openResourceFile(relativePath, platformCtx)` is the runtime-side
call: give me the compiled bytes for this source path, compiling first if stale. Compilation itself
goes through `IAssetCompiler::compile(Blob&& data, const Context&, Blob& outputData,
rttr::variant& metadata)` — note that compilers *return metadata*, which is how the statistics panel
can show "compiled size / compile time / source size" per asset without a second pass.

### Layer 2 — Resources are runtime objects, owned by per-type managers

`IResource` (`resource/resource.hpp`) has a `Status{Loading, Ready, Failed, Unknown}` and a runtime
type id (`IResource::type<T>()`, capped at `C_MAX_RESOURCES = 64`). `ResourceService` holds
`eastl::array<unique_ptr<IResourceManager>, C_MAX_RESOURCES>` indexed by that id — a flat array, not
a map, because the id is dense and assigned at registration.

Each manager is a `ResourceManager<T, CacheStrategy>`. The cache is
`unordered_map<InternedString, weak_ptr<IResource>>` — **weak**, so a resource nobody references is
collected, and the cache strategy is a policy parameter (`CacheDefault` / `CacheDisabled`). Most
managers derive from `SimpleResourceManager<Derived, ResType, S>`, a CRTP template method where the
derived type supplies only `parse()` (background thread: bytes → intermediate data) and
`applyLoadResult()` (main thread: intermediate data → live GPU/engine object). `SkeletonResourceManager`
is the minimal example — it overrides `parse`, `applyLoadResult` and `gatherStatistics`, nothing
else. `AdvancedResourceManager` is the escape hatch for pipelines that don't fit
(`TextureResourceManager`'s mip streaming, `MaterialResourceManager`'s inheritance reload queue).

The async mechanics live in `ParseResourceRoutine<ResType, DataSize>`: a three-stage state machine
(WaitForTask → WaitForRead → parse task) over `LoadVariant = std::variant<IFile::IAsyncDataPtr,
LoadResult>`, with cancellation that *drains* cancelled futures rather than detaching them, and
priority re-scheduling via `changeLoadPriority`. Managers are ordered relative to each other twice:
`assignAfter`/`assignBefore` for construction order, `runAfter`/`runBefore` for the per-frame
`ExecutionGraph` that schedules `update(tf::Subflow&, float)`.

### The abstraction the request points at

```cpp
template <typename T>
struct ResPtr : std::shared_ptr<T>
{
    using ResourceType = std::remove_cv_t<std::remove_reference_t<T>>;
    ResPtr() noexcept = default;
    ResPtr(std::nullptr_t) noexcept {}
    explicit ResPtr(const std::shared_ptr<T>& p) : std::shared_ptr<T>(p) {}
    bool ready() const { return this->get() != nullptr && this->get()->ready(); }
};
```

Thirteen lines, and it is the keystone. It works because of what surrounds it:

1. `makeResPtr<T>(path, optional<priority>)` resolves a path to a resource through
   `ResourceService::get<T>()->load(path, priority)`. The manager returns a resource **immediately** —
   a dummy in `Status::Loading` if the bytes aren't there yet. So `ResPtr<T>` is never a null hole
   waiting to be filled; it is a valid object whose `ready()` is false.
2. `registration::ResourceManager<T, R>` auto-registers a reflected type named `ResPtr_<name>` whose
   RTTR converters go `ResPtr<R>` ↔ `std::string` (the asset path), with `AssetType<R>` property
   metadata so the editor knows which file picker to open. A component field declared
   `ResPtr<TextureResource> m_albedo` therefore serializes as a path, inspects as a typed asset
   slot, and loads on deserialization — with no per-field code.
3. Because the manager owns resolution, *when* and *how* the bytes arrive is invisible to the field.
   Sync today, async tomorrow, streamed the day after.

That third point is the argument for building the abstraction *before* the async machinery, not
after. It is why Phase 3 precedes Phase 5 in this plan.

---

## Where the engine is today (facts this plan is built on)

Verified against `ee4148f`. Re-verify anything load-bearing before implementing — line numbers move.

- **`AssetManager` is a static facade over one struct.**
  [`AssetManager.cpp:18`](../../GanymedEngine/source/GanymedE/Assets/AssetManager.cpp#L18) declares
  `AssetManagerData` with `Registry` (handle → metadata), `PathToHandle`, and **four hardcoded strong
  caches** — `LoadedMeshes`, `LoadedEnvironments`, `LoadedTextures`, `LoadedMaterials`
  ([`:23-26`](../../GanymedEngine/source/GanymedE/Assets/AssetManager.cpp#L23)). Adding a fifth
  cached asset type means editing this file in at least six places.
- **The type dispatch is a `static_assert` wall.**
  [`AssetManager.h:46-56`](../../GanymedEngine/source/GanymedE/Assets/AssetManager.h#L46) defines the
  primary `GetAsset<T>` template as `static_assert(sizeof(T) == 0, ...)`, with four explicit
  specializations declared at namespace scope
  ([`:85-88`](../../GanymedEngine/source/GanymedE/Assets/AssetManager.h#L85)) and four private
  `LoadX` functions ([`:76-79`](../../GanymedEngine/source/GanymedE/Assets/AssetManager.h#L76)).
  It is a deliberate, well-commented design that gives a clean compile error — but it means the set
  of loadable types is closed and lives in the engine's asset header.
- **Nothing is ever unloaded.** The caches hold `Ref<T>` (strong). There is no eviction path except
  `Reload` (which evicts one handle) and `Shutdown` (which clears everything,
  [`:87-90`](../../GanymedEngine/source/GanymedE/Assets/AssetManager.cpp#L87)). Load a 200-asset
  scene, switch scenes, and every texture from the first scene is still resident.
- **Every load is synchronous on the calling thread.** `LoadMesh` is
  `MeshCache::TryLoad` → else `MeshImporter::Load` + `MeshCache::Write` → then
  `MaterialSerializer::GenerateSidecars`, all inline. A cold scene open does full glTF parse + PNG
  decode + bgfx uploads before the frame ends.
- **Identity lives in one un-mergeable file.** `AssetRegistry.gr` is a single YAML document mapping
  handle → path, written atomically (`.tmp` + `rename`) and quarantined to `.gr.bad` on parse
  failure. [`docs/engine/assets.md`](../engine/assets.md) already names this as the deeper unfixed
  gap and already names the production answer: *per-asset committed metadata, as Unity's `.meta`
  files carry the GUID beside the asset*. Phase 1 is that.
- **The call-site surface is small — migration is bounded.** 24 `AssetManager::GetAsset` call sites
  across 10 files, 14 `AssetManager::ImportAsset` call sites. The hot ones are in
  [`RenderSystem.cpp:60,83,97,138,144`](../../GanymedEngine/source/GanymedE/Scene/Systems/RenderSystem.cpp#L60)
  — i.e. a handle→`Ref` hash lookup **per entity per frame**, for meshes, materials, environments and
  particle emitters.
- **Components store bare `AssetHandle`s.**
  [`Components.h:103,114,133,218,272,293,425-428`](../../GanymedEngine/source/GanymedE/Scene/Components.h#L103):
  `StaticMeshComponent::Mesh`, `MaterialOverrides`, `PrefabInstanceComponent::Source`,
  `SkyLightComponent::Environment`, `ScriptComponent::Script`, `AudioSourceComponent::Clip`, and the
  particle emitter's `Texture`/`Mesh`/`Material`. These are the fields Phase 3 converts.
- **One compiled-output pipeline already exists, ad hoc.** `MeshCache` writes v6 binary blobs under
  `assets/.assets/`, keyed by source mtime
  ([`MeshCache.h`](../../GanymedEngine/source/GanymedE/Assets/MeshCache.h), 435 lines of `.cpp`).
  It is a single-purpose `Asset`+`IAssetCompiler` with a weaker invalidation rule. Phase 4
  generalizes it rather than replacing it.
- **`.gmat` materials already reference textures by path, not handle** — a deliberate decision
  recorded in [`assets.md`](../engine/assets.md). This is the closest thing Ganymed has to a
  `ResPtr`-style path reference, and Phase 3 should keep it working unchanged.
- **`AssetType` is persisted by ordinal and append-only**
  ([`AssetTypes.h:14-27`](../../GanymedEngine/source/GanymedE/Assets/AssetTypes.h#L14)). Any
  registration scheme must not reorder it.
- **bimg is already vendored and already builds.**
  `GanymedEngine/extern/bimg` is a submodule with a generated `bimg.vcxproj` and output in
  `bin/Debug-windows-x86_64/bimg`. Its public headers expose `bimg::imageEncode` /
  `imageEncodeFromRgba8` / `imageGenerateMips` (`include/bimg/encode.h`) and `bimg::imageParse`
  (`include/bimg/decode.h`), and `extern/bimg/tools/texturec/texturec.cpp` is a complete working
  reference for the offline conversion path. Phase 4 needs **no new dependency**.
- **There is no job system, no thread pool, no reflection.** Confirmed by grep: `std::thread` /
  `std::mutex` appear only in `Debug/Instrumentor.h` and `Physics/PhysicsScene.cpp`. Jolt brings its
  own `JobSystemThreadPool`; nothing else in the engine is threaded.

---

## Decisions of record

1. **Two layers, but only one new vocabulary word.** BlankEngine names them *Asset* (source, editor-side)
   and *Resource* (runtime object). Ganymed already overloads "asset" to mean both, and 24 call
   sites plus the whole editor say `AssetManager`. Renaming to `ResourceService` would be churn for
   nothing. **Decision:** keep `AssetManager` as the front door and keep `AssetHandle` as the
   identity currency; introduce the second layer as `AssetRef<T>` (the reference) and
   `IAssetManager` / `TypedAssetManager<T>` (the owner). The source/compiled split becomes explicit
   in Phase 4 as `AssetCompiler` + a compiled-output directory, not as a new service.

2. **Handles stay the identity, paths stay the *serialized* form of a reference where it already
   works.** BlankEngine resolves everything by path (`InternedString`) and has no stable GUID; Unity and UE
   both use GUIDs; Ganymed already uses UUID handles in scenes and prefabs. Handles survive a file
   rename, paths don't — that is the argument for handles and it is the right call for a scene-heavy
   engine. **Decision:** `AssetRef<T>` serializes as the handle (matching every existing scene
   file), *except* `.gmat`, which keeps its path-based texture references as documented. Do not
   convert `.gmat`; it is a working decision, not debt.

3. **Per-asset `.meta` sidecars replace the single registry, and the registry becomes derived.**
   This is the Unity model, and it is the standard answer for exactly Ganymed's problem: two people
   importing assets on two branches currently conflict in one YAML file with no meaningful merge.
   UE takes the other road (identity is the `.uasset` path, and it has a heavyweight redirector
   system to survive renames) — that road requires an asset *format* the engine owns end-to-end,
   which Ganymed does not have for meshes or textures. **Decision:** `foo.png` gets `foo.png.meta`
   holding `{Handle, Type, ImportConfigVersion, <compiler config>}`, committed to git. The in-memory
   `Registry` is rebuilt by scanning `assets/`. `AssetRegistry.gr` is read once for migration
   (adopt existing handles so no scene breaks), then stops being read. Keeping stale handles is
   non-negotiable — scenes reference them.

4. **The manager registry is hand-rolled, not reflected.** Every BlankEngine registration path goes
   through RTTR. Adding RTTR (or building reflection) to get a registration DSL is a large
   dependency decision serving one feature. **Decision:** a compile-time-assigned dense type id
   (`AssetTypeId<T>()`, backed by a counter incremented at static-init in each manager's
   registration TU), a flat `std::array<Scope<IAssetManager>, MaxAssetTypes>`, and a
   `GE_REGISTER_ASSET_MANAGER(Type, Manager)` macro that fills in the loader/extension/friendly-name
   table entries. Explicit over clever, per house style. If reflection arrives later for other
   reasons (an inspector generator, script binding), the registration table can move onto it without
   changing the interface.

5. **Weak caches, with strong retention pinned by `AssetRef`.** BlankEngine caches
   `weak_ptr<IResource>` and lets holders own lifetime. Ganymed's strong caches are why nothing ever
   unloads. **Decision:** `TypedAssetManager<T>` caches `std::weak_ptr<T>`. An `AssetRef<T>` holds a
   `shared_ptr`, so an asset stays resident exactly as long as something references it. Consequence
   to accept: an asset referenced only by an editor panel that closes gets collected and reloaded —
   correct behaviour, but it means the content browser's thumbnail cache must hold its own refs.

6. **`AssetRef<T>` derives from `Ref<T>` (= `std::shared_ptr<T>`), exactly as `ResPtr` derives from
   `shared_ptr`.** Public inheritance from a standard smart pointer is normally a smell — no virtual
   destructor, slicing on assignment to the base. It is nonetheless the right call here for the same
   reason BlankEngine made it: it means `AssetRef<Mesh>` is a drop-in for every existing
   `Ref<Mesh>`-typed local and parameter, so Phase 3's migration is mechanical and reversible. The
   slicing risk is real but bounded — `AssetRef` adds no data members, so a slice loses only the
   `ready()`/`handle()` API, not state. **Decision:** derive, add no members beyond the handle
   needed for lazy resolution, and document the constraint in a comment at the declaration.

7. **Resolution is lazy and synchronous until Phase 5.** `AssetRef<T>` stores a handle and resolves
   on first dereference through the manager. In Phase 3 that resolve is a blocking load — identical
   observable behaviour to today's `GetAsset<T>`, so nothing regresses and there is nothing to debug
   except the migration itself. In Phase 5 the same resolve returns a placeholder in
   `Status::Loading` and the call sites do not change. This staging is the entire reason to do the
   abstraction first.

8. **Placeholder-first loading, not null-checking.** BlankEngine's `load()` always returns a resource; the
   dummy is in `Loading` state. The alternative (return null until ready) forces every call site to
   branch, and 24 call sites in a render loop is exactly where you don't want that. **Decision:**
   Phase 5 introduces a per-type placeholder (checkerboard texture, unit cube mesh, error material)
   supplied by the manager, so `AssetRef<T>::operator->` is always safe and `ready()` is advisory.
   Systems that must not draw a placeholder (e.g. a mesh with the wrong bounds) check `ready()`.

9. **Compiled outputs live in a gitignored `assets/.compiled/` tree, hashed per source.** BlankEngine
   hashes the source path into a flat bucketed output tree, keyed by `PlatformContext` (platform +
   graphics API + domain). Ganymed builds one platform at a time and bgfx already abstracts the API
   at the shader level. **Decision:** hash the source *relative path* into
   `assets/.compiled/<hash-prefix>/<hash>.gres` plus a sibling `.dep` for the epoch record. No
   platform context in the key yet — add a platform tag to the hash input the day a second target
   exists, which is a one-line change because the key is computed in one function. Do not build the
   multi-platform machinery speculatively.

10. **Invalidation is epoch-based from the start, not mtime-based.** `MeshCache`'s mtime check
    recompiles on `touch` and misses a changed importer. BlankEngine's `Epoch`/`EpochDiff` costs maybe 80
    lines more and gets it right. **Decision:** the `.dep` record stores compiler version, source
    size, source mtime, source content hash, and the hashes of source dependencies; the diff is a
    bitflag set; mtime is used only as a cheap pre-filter before hashing. Bumping a compiler's
    `Version` constant invalidates every output it produced — which is the property that makes
    changing an importer safe.

11. **One dependency edge type, built at load, not persisted.** BlankEngine has `AssetDependenciesService`
    with forward/backward queries at arbitrary depth, source-vs-external edge classification, and a
    persisted compact DB. Ganymed needs to answer exactly two questions: "which materials reference
    this texture" and "which meshes reference this material", for hot reload and recompile
    invalidation. **Decision:** a `std::unordered_map<AssetHandle, std::vector<AssetHandle>>`
    reverse-edge map populated as assets load, rebuilt from scratch on demand, not written to disk.
    Depth-1 queries only. Revisit if a real depth-N need appears; it probably won't.

12. **The job system is a separate milestone, and it is now sketched.** BlankEngine uses `ThreadService` +
    `Future<T>` + taskflow — and a later investigation of `core/thread/` showed that BlankEngine did not
    write a scheduler at all, it wrapped cpp-taskflow in ~1,500 lines. Ganymed needs a scheduler with
    two shapes: a parallel-for and a cancellable background task with a main-thread completion
    handoff. That belongs to `Core/`, not to the asset layer. **Decision:** it is its own milestone,
    scoped in [`THREADING_ROADMAP.md`](THREADING_ROADMAP.md) — **`Core/JobSystem` over enkiTS**, with
    its own doc section in [`core.md`](../engine/core.md). Two consequences for this plan. First, its
    *first* consumer should be Phase 4's parallel BCn encode, not Phase 5's async loading, because
    import-time work has no frame budget and no lifetime hazards — prove the scheduler on the boring
    consumer. Second, if Phase 5 is cut, Phases 1–4 stand alone and nothing has been built
    speculatively.

13. **bgfx's submit-thread constraint dictates the `parse`/`apply` split, not taste.** bgfx API calls
    (`createTexture2D`, `createVertexBuffer`, …) must come from the thread that owns the bgfx
    context. This is *why* BlankEngine splits `parse()` (background: bytes → CPU-side intermediate) from
    `applyLoadResult()` (main thread: intermediate → GPU object). **Decision:** adopt the same split
    in `TypedAssetManager<T>` from Phase 2 — even while everything is synchronous — so that Phase 5
    only changes *where* `parse` runs, not the shape of any manager. Getting this boundary in early
    and for free is the cheapest thing in this plan.

14. **Reflection is a separate milestone that lands *after* Phase 3, not before Phase 1 — and it is
    `entt::meta`, not RTTR.** The question was raised directly: integrate reflection first, since it
    also serves the editor. Three findings say no to *first* and no to *RTTR*.
    **(a)** The asset layer needs reflection for essentially nothing. Decision 4's registration table
    is a macro filling an array; `AssetRef<T>` serializes through a hand-written `SceneSerializer`;
    the typed asset slot is a known field in a hand-written inspector; `.meta` configs are plain YAML
    structs. Where BlankEngine uses RTTR in its asset layer it is because RTTR was already there for
    everything else — the asset layer is a consumer of reflection, never a driver.
    **(b)** RTTR would be a *second* reflection system: entt is already vendored at **3.16.0** with
    the complete `entt/meta` module, by the same author as the ECS, and it interops with entt's
    component storage — which is what "add component by type name", copy/paste-a-component and
    prefab diffing actually need.
    **(c)** Sequencing rework is a wash either way (~100 lines of asset registration moved, in
    whichever direction), so the tiebreaker is that going reflection-first means designing the
    attribute vocabulary before its consumers exist — and the concrete consumers that should shape it
    (`AssetRef<T>` slots, `FloatCurve` fields, enum dropdowns, `.meta` import-config panels) arrive
    in Phases 3 and 4.
    **Decision:** Phases 1–3, then the reflection milestone, then Phase 4. Sketched in
    [`REFLECTION_ROADMAP.md`](REFLECTION_ROADMAP.md), which also records the one condition that
    would re-order this: prefab per-property overrides and multi-entity editing are *impossible*
    without member reflection rather than merely tedious, so if either becomes near-term the
    reflection milestone moves ahead of them — but still not ahead of the asset work.

15. **Per-asset import configs come with `.meta` (Phase 1) but stay empty until Phase 4.** The
    `.meta` schema reserves a config block from day one so that adding "sRGB: false" or
    "generateMips: true" later is a schema addition, not a file-format migration. Config versioning
    is a single `ImportConfigVersion` integer plus a migration switch — not BlankEngine's per-compiler
    `m_cfgUpdateVersionFn` + `m_legacyCfgNames` machinery, which exists because BlankEngine has years of
    shipped configs to migrate and Ganymed has none.

---

## Phase 1 — Identity: per-asset `.meta` sidecars

**Goal.** Move handle↔path identity out of the single `AssetRegistry.gr` and into a committed
sidecar beside each asset. Make the in-memory registry a derived index. Preserve every existing
handle so no scene, prefab or `.gmat` breaks.

**Steps.**

1. Define the sidecar format in a new `AssetMeta.h/.cpp` under `Assets/`:
   `{ Handle: <uuid>, Type: <string name>, ImportConfigVersion: <int>, Config: <map, empty for now> }`.
   Serialize `Type` **by name**, not by the `AssetType` ordinal — the ordinal constraint at
   [`AssetTypes.h:14`](../../GanymedEngine/source/GanymedE/Assets/AssetTypes.h#L14) exists because
   the current registry persists ordinals; a fresh format should not inherit that trap. Keep the
   enum append-only anyway for the legacy reader.
2. `AssetMeta::Read(fullPath)` / `Write(fullPath, meta)` with the same atomic `.tmp` + `rename`
   discipline `SaveRegistry` already uses, and the same quarantine-on-parse-failure behaviour
   (`.meta.bad`) so one corrupt sidecar cannot take down a project scan.
3. `AssetManager::ScanAssets()` — walk `assets/` (skipping `.assets/`, `.compiled/`, `.meta`), and
   for each recognized extension: read the sidecar if present, otherwise mint a handle and write
   one. Populate `Registry` + `PathToHandle` from the result. `ImportAsset` becomes "ensure a
   sidecar exists and return its handle."
4. Migration: on `Init`, if `AssetRegistry.gr` exists, read it *first* and seed a path→handle map
   used by the scan, so existing assets adopt their existing handles instead of minting new ones.
   Write the sidecars, then log once that the registry file is now redundant. Do not delete it —
   leave that to the user, and keep the reader for one release.
5. Handle collision policy: two sidecars claiming the same handle (a copy-pasted `.meta`) is a real
   failure mode. First-scanned wins, the second gets a fresh handle and a warning naming both paths.
   Silently aliasing would corrupt scene references.
6. Editor: the content browser must copy/move/delete the `.meta` alongside the asset. Find every
   filesystem mutation in `GanymedEditor/source/Panels/ContentBrowserPanel.cpp` and pair it.
7. Runtime: `GanymedRuntime`'s assets snapshot must include `.meta` files. Update the packaging step
   and [`runtime.md`](../runtime/runtime.md).
8. `.gitignore`: `.meta` must be **committed** (that is the point). Verify no existing glob excludes
   it.

**Decisions.** Type-by-name in the new format (3); scan-derived registry (3); adopt legacy handles
(3); first-wins on collision with a loud warning.

**Risks.** The scan cost on a large `assets/` tree is now paid at startup — mitigate by hashing
nothing during the scan (sidecar read only) and by not recursing into compiled/cache directories.
The editor's file operations are the likely source of orphaned sidecars; a "clean orphaned .meta"
maintenance action is cheap insurance but not required for the phase.

**Verification.**

| Check | How |
| --- | --- |
| Existing project opens unchanged | Open a scene authored pre-migration; every mesh/material/texture resolves, handles in the `.gscene` unchanged (diff the file after a save — should be identical modulo formatting) |
| Sidecars appear and are stable | Run `Init` twice; second run mints zero handles, writes zero files |
| Corrupt sidecar is contained | Truncate one `.meta`; project still opens, that asset gets a fresh handle, `.meta.bad` written, one warning logged |
| Collision handled | Copy a `.meta` onto a second asset; both load, warning names both paths |
| Runtime boots from snapshot | Build `GanymedRuntime`, run the packaged snapshot, scene loads |
| Docs | [`assets.md`](../engine/assets.md) identity/registry sections rewritten in place; the "production answer is per-asset metadata" note becomes a description of what now exists |

---

## Phase 2 — The manager registry: `IAssetManager`, dense type ids, weak caches

**Goal.** Replace the four hardcoded caches and the `static_assert` primary template with a
registration-driven, type-indexed manager array. Introduce the `parse`/`apply` split. Stop leaking.

**Steps.**

1. `AssetTypeId.h`: `template<typename T> AssetTypeId Id()` returning a dense `uint8_t` assigned on
   first use from a static counter, with `GE_ASSERT` on exceeding `MaxAssetManagers` (start at 16 —
   Ganymed has 4 cached types and 9 enum entries; BlankEngine's 64 is sized for a much larger engine).
2. `IAssetManager`: `Load(AssetHandle) -> Ref<void>`-free interface — prefer
   `virtual Ref<AssetBase> Load(AssetHandle)` if a common base is acceptable, otherwise keep the
   type erasure inside `TypedAssetManager<T>` and have `IAssetManager` expose only
   `Evict(AssetHandle)`, `EvictAll()`, `CachedCount()`, `TypeName()`. Decide by looking at whether
   `Mesh`/`Texture2D`/`Material`/`Environment` can share a trivial base without disturbing the
   renderer — if not, type erasure is the lesser evil and `AssetManager::GetAsset<T>` static-casts
   through the typed manager it already knows.
3. `TypedAssetManager<T>`: `std::unordered_map<AssetHandle, std::weak_ptr<T>>` cache;
   `Ref<T> Load(handle)` = cache hit → lock; miss → `ParseFn` then `ApplyFn`. The two function
   pointers are supplied at registration, which is where the existing `LoadMesh`/`LoadTexture`/
   `LoadMaterial`/`LoadEnvironment` bodies move — split at the first bgfx call (decision 13).
   For `LoadMesh` the natural cut is: `MeshCache::TryLoad`/`MeshImporter::Load` producing CPU vertex
   and index data = `parse`; bgfx buffer creation = `apply`.
4. `AssetManagerRegistry`: `std::array<Scope<IAssetManager>, MaxAssetManagers>` +
   `template<typename T> TypedAssetManager<T>& Get()`. `GE_REGISTER_ASSET_MANAGER` macro filling the
   slot at `Init`.
5. `AssetManager::GetAsset<T>` becomes a one-liner forwarding to `Registry().Get<T>().Load(handle)`.
   The `static_assert` primary template goes away; an unregistered `T` now fails at link or asserts
   at runtime, which is *worse* diagnostics than today. Keep a `static_assert` on a
   `IsAssetType<T>` trait specialized by the registration macro, so the good compile error survives.
   This matters — the comment at
   [`AssetManager.h:36`](../../GanymedEngine/source/GanymedE/Assets/AssetManager.h#L36) says the
   clean error was deliberate, and regressing it would be a real loss.
6. Rewrite `Reload(handle)` against the registry. The current implementation has strict eviction
   ordering (textures before their owning material/mesh, `MeshCache::Invalidate` for meshes) — with
   weak caches most of that ordering becomes unnecessary, but the `MeshCache::Invalidate` call is
   still required. Keep the documented safety invariant ("re-fetch by handle each frame, or accept
   staleness") true.
7. Delete `LoadedMeshes`/`LoadedEnvironments`/`LoadedTextures`/`LoadedMaterials` and the four private
   `LoadX` declarations.

**Decisions.** Dense type id over `std::type_index` map (2 machine words, array indexing in the
render path); weak caches (5); `parse`/`apply` split even while synchronous (13); preserve the
compile-time error for unsupported `T` via a trait.

**Risks.** The weak-cache switch will surface every place that relied on the asset staying alive
because *the cache* held it. Editor panels and the particle system are the likely candidates. This is
the phase's main behavioural risk and the reason the verification table has a "no premature
collection" row. Second risk: whether a common asset base exists — resolve it in step 2 before
writing the interface, not during.

**Verification.**

| Check | How |
| --- | --- |
| Behaviour unchanged | Open the demo scenes; visually identical, no new warnings |
| No premature collection | Play a scene, stop, open the content browser, reopen — nothing reloads that shouldn't; add a `CachedCount()` readout to the debug UI and watch it |
| Eviction actually happens | Load a scene, load a second scene with disjoint assets, verify `CachedCount()` for textures drops |
| Reload still works | Edit a `.gmat` on disk, `Reload` its handle, material updates in the viewport |
| Unsupported type still a clean compile error | `AssetManager::GetAsset<int>(h)` in a scratch TU produces the trait `static_assert`, not a link error |
| Build | MSBuild x64 Debug, `GanymedEngine.vcxproj` then editor + runtime; premake regen noted if files added |
| Docs | [`assets.md`](../engine/assets.md) API table + a new "Managers and caching" section; [`architecture.md`](../engine/architecture.md) module-boundary note |

---

## Phase 3 — `AssetRef<T>`: the reference abstraction

**Goal.** The thing the request is actually about. Replace bare `AssetHandle` fields and per-frame
`GetAsset<T>` lookups with a typed reference that resolves itself, and make later async invisible.

**Steps.**

1. `AssetRef.h`:

   ```cpp
   // Derives from Ref<T> so it substitutes for one at every existing call site (see
   // docs/toDo&done/ASSET_PIPELINE_ROADMAP.md decision 6). Adds no data members beyond
   // the handle, so slicing to Ref<T> loses API, never state.
   template<typename T>
   class AssetRef : public Ref<T>
   {
   public:
       AssetRef() = default;
       explicit AssetRef(AssetHandle handle) : m_Handle(handle) {}

       AssetHandle Handle() const { return m_Handle; }
       bool Ready() const;          // resolved and usable
       T* Resolve() const;          // resolves on first use, caches in the base shared_ptr
       // operator-> / operator* inherited; they require a prior Resolve() until Phase 5
       // installs placeholders.
   private:
       AssetHandle m_Handle = InvalidAssetHandle;
   };
   ```

   `Resolve()` is `const` and mutates the base pointer — it needs `mutable` or a const_cast on the
   base; prefer storing the resolved pointer in the base via a small private non-const helper called
   from a `mutable`-friendly wrapper. Keep it explicit; do not get clever.
2. Serialization: `AssetRef<T>` writes as its handle. In `SceneSerializer` and `PrefabSerializer`
   this should be a *no-op change* — the YAML keys and values stay identical. Verify by diffing a
   round-tripped scene byte-for-byte. This is the single most valuable regression check in the phase.
3. Convert component fields (`Components.h` lines listed above) from `AssetHandle` to
   `AssetRef<Mesh>` / `AssetRef<Material>` / `AssetRef<Environment>` / `AssetRef<Texture2D>`.
   `ScriptComponent::Script`, `AudioSourceComponent::Clip` and `PrefabInstanceComponent::Source`
   stay bare handles — they are path-resolved types with no `GetAsset` specialization, and
   [`assets.md`](../engine/assets.md) records that as deliberate. Do not widen the change to them.
4. Migrate the 24 `GetAsset` call sites. `RenderSystem`'s five become direct dereferences, which is
   the measurable win: one hash lookup per entity per frame removed, replaced by a pointer already
   in the component. Do not claim a frame-time number for this without measuring it — the honest
   claim is "removes a hash lookup per draw," not "saves N ms."
5. Editor: the inspector's asset slots and drag-drop targets currently write handles. Make them
   write through `AssetRef` so type checking happens at the slot (`AssetRef<Material>` only accepts a
   Material drop) instead of by convention.
6. `Ready()` audit: find the places that today implicitly tolerate a null `Ref` from `GetAsset`
   (a missing asset) and make the tolerance explicit. `RenderSystem.cpp:97` already ternaries on the
   handle — that pattern becomes `Ready()`.

**Decisions.** Derive from `Ref<T>` (6); serialize as handle (2); lazy synchronous resolve (7);
leave path-resolved types on bare handles.

**Risks.** Slicing — an `AssetRef<T>` assigned into a `Ref<T>` silently loses `Ready()`/`Handle()`.
Compiles fine, works fine, but the reference stops being reloadable. Grep for `Ref<Mesh> x =` after
the migration. Second risk: scope creep into the editor's asset-slot UI, which is adjacent and
tempting; keep it to making existing slots type-safe, not redesigning them.

**Verification.**

| Check | How |
| --- | --- |
| Scene round-trip is byte-identical | Load + save every scene and prefab in `assets/`, `git diff` shows nothing |
| Prefab instances still track | Instantiate a linked prefab, edit the source, confirm propagation |
| Drag-drop type safety | Drop a texture on a material slot — rejected, not silently accepted |
| Missing asset degrades | Delete a referenced `.gmat` on disk, reopen: entity renders with the fallback path it uses today, no crash |
| Lua bindings | `ScriptEngine.cpp`'s `GetAsset` use still compiles and works; a script setting a mesh handle still works |
| Build | MSBuild x64 Debug: engine, editor, runtime |
| Docs | [`assets.md`](../engine/assets.md) gets an `AssetRef` section (this is the new primary API); [`scene.md`](../engine/scene.md) component catalog field types updated; [`editor.md`](../editor/editor.md) asset-slot behaviour |

---

## Phase 4 — Compiled outputs: `AssetCompiler`, epoch invalidation, BCn textures

**Goal.** Generalize `MeshCache` into a source→compiled pipeline with correct invalidation, and use
it to stop shipping PNG-decoded uncompressed textures.

**Sequencing note.** Per decision 14 the reflection milestone
([`REFLECTION_ROADMAP.md`](REFLECTION_ROADMAP.md)) lands between Phase 3 and this phase. If it has,
step 4's `.meta` config block gets its import-settings panel for free from the generic inspector; if
it has not, hand-write that one panel and do not block on it.

**Steps.**

1. `AssetCompiler.h`: `IAssetCompiler { virtual uint32_t Version() const; virtual bool Compile(const
   CompileInput&, CompileOutput&) const; }` where `CompileInput` carries source bytes, source path,
   and the `.meta` config block, and `CompileOutput` carries the output blob plus discovered source
   dependencies plus compile stats. Modeled on BlankEngine's `IAssetCompiler::Context` /
   `compile(Blob&&, ctx, Blob&, variant&)` minus the RTTR variant — stats go into a plain struct.
2. `CompiledCache` (the generalization of `MeshCache`): output path =
   `assets/.compiled/<h[0:2]>/<h>.gres` where `h = hash(relative source path)`; sibling `<h>.dep`
   holding the epoch record from decision 10. `TryOpen(handle)` → compile-if-stale → return bytes.
   `EpochDiff` as a bitflag enum: `CompilerVersion | SourceSize | SourceMtime | SourceHash |
   Dependencies | Config`.
3. Port `MeshCache` onto it: `MeshCompiler::Version()` returns the current format version 6, the v6
   blob becomes the `.gres` payload, `Invalidate` becomes generic. Keep the ability to read existing
   `assets/.assets/` blobs, or just accept a one-time full recompile — the cache is gitignored, so
   **accept the recompile**; carrying a legacy reader for a derived artifact is pure cost.
4. `TextureCompiler` using bimg: `bimg::imageParse` the source, `imageGenerateMips`, then
   `imageEncode`/`imageEncodeFromRgba8` to BC7 (colour), BC5 (normal maps), BC4 (single-channel
   masks), writing a DDS the existing texture load path can already hand to bgfx. `extern/bimg/tools/texturec/texturec.cpp`
   is the working reference for the exact call sequence — read it rather than guessing flags. Config
   block in `.meta`: `{ Format: auto|BC7|BC5|BC4|RGBA8, GenerateMips: bool, sRGB: bool, MaxSize: int }`.
   `Format: auto` inferred from the asset's role where known (a material's normal-map slot → BC5).
5. Wire `TypedAssetManager<T>::parse` to read from `CompiledCache` instead of the source file. The
   `parse`/`apply` split from Phase 2 means this touches the parse side only.
6. Dependency edges (decision 11): compilers report discovered source dependencies; `CompiledCache`
   records their hashes in the `.dep` and the reverse map is populated as assets load.
7. Editor affordance: a "Reimport" action on the content browser context menu that forces recompile,
   and a visible indicator while a compile is running. Compilation is still **blocking** here — a
   large texture set will hitch on first import. That is acceptable for Phase 4 and is fixed by
   Phase 5; say so in the UI rather than pretending otherwise.
8. Threading, per decision 12: this phase is the threading milestone's first consumer
   ([`THREADING_ROADMAP.md`](THREADING_ROADMAP.md) T3). `TextureCompiler` runs its mip encode through
   `JobSystem::ParallelFor`, which shortens the hitch but does not remove it — the *dispatch* is still
   synchronous on the calling thread, and only Phase 5 makes it non-blocking. Report measured
   before/after import times for a real scene; that measurement is what justifies the threading
   milestone existing.

**Decisions.** Hashed flat output tree, no platform context yet (9); epoch invalidation (10);
accept a one-time full recompile rather than reading legacy `MeshCache` blobs; bimg, no new
dependency.

**Risks.** BCn encoding is slow — BC7 at high quality is seconds per 2K texture. A first import of a
full material library will feel broken if it is synchronous and silent, hence the progress
indicator. Mitigate further by defaulting to `Quality::Default` rather than the highest setting, and
by only re-encoding on a real epoch diff. Second risk: colour-space correctness. sRGB handling is
the classic place this goes wrong — decide the convention once (albedo/emissive sRGB, everything else
linear), write it in [`rendering.md`](../engine/rendering.md), and check against a known reference
image rather than by eye.

**Verification.**

| Check | How |
| --- | --- |
| Meshes still load, cache still works | Delete `.compiled/`, open a scene (cold compile), reopen (cache hit — verify by log or timing) |
| Epoch diff is correct | `touch` a source without editing → no recompile. Edit it → recompile. Bump `MeshCompiler::Version` → recompile everything |
| Dependency invalidation | Edit a texture referenced by a `.gmat`, confirm the dependent recompiles |
| Texture output is right | Compare a BC7-compiled albedo against the PNG path side by side; confirm VRAM drop in the bgfx stats overlay (report the actual measured numbers, not estimates) |
| sRGB not double-applied | Reference gradient image renders identically pre/post compiler |
| Build | MSBuild x64 Debug; bimg link added to `premake5.lua` → **premake regeneration required**, state it |
| Docs | [`assets.md`](../engine/assets.md) compiled-output + compiler sections replacing the `MeshCache`-specific text; [`build-and-tooling.md`](../engine/build-and-tooling.md) for the `.compiled/` tree and gitignore; [`rendering.md`](../engine/rendering.md) for the colour-space convention |

---

## Phase 5 — Async: non-blocking loads on `Core/JobSystem`

**Goal.** Stop blocking the main thread on I/O, decode and compile. The job system is no longer part of
this phase's scope — it is [`THREADING_ROADMAP.md`](THREADING_ROADMAP.md) and lands with Phase 4. This
phase is the asset layer on top of it.

**Steps.**

1. `Core/JobSystem` should already exist by now — it is its own milestone
   ([`THREADING_ROADMAP.md`](THREADING_ROADMAP.md), decision 12) and its first consumer is Phase 4's
   parallel BCn encode. What this phase needs from it: `hardware_concurrency()-1` workers, three
   priority tiers, `Future<T>` with a cancel flag, and a **main-thread completion queue** drained once
   per frame from `Application::Run` (enkiTS's `IPinnedTask`/`RunPinnedTasks`). Cancellation drains
   rather than detaches, and note the enkiTS semantics: cancel means the task still runs and returns
   immediately, it is not dequeued. BlankEngine's `m_cancelledTasks`/`m_cancelledFutures` vectors exist
   because a detached task writing into a freed resource is the bug you cannot debug. Document in
   [`core.md`](../engine/core.md).
2. `TypedAssetManager<T>::Load` becomes: cache miss → create the object in `Status::Loading`, insert
   into the cache, enqueue `parse` as a job, return immediately. `Update()` (called once per frame
   from wherever `AssetManager` gets a tick — it currently has none, so add one) drains completed
   parses and runs `apply` on the main thread. This is BlankEngine's `SimpleResourceManager::update` loop,
   minus the taskflow scheduling.
3. Placeholders (decision 8): each manager supplies a placeholder instance returned by `operator->`
   while `Status::Loading`. Texture → the existing editor checkerboard. Mesh → unit cube. Material →
   a visibly-wrong error material, deliberately ugly. Environment → black.
4. `AssetRef<T>::Resolve()` stops blocking. Nothing else changes at any call site — that is the
   payoff from Phase 3, and it is the row in the verification table that proves the abstraction
   earned its keep.
5. Add `AssetManager::WaitFor(handle)` for the places that genuinely cannot proceed with a
   placeholder (scene load completion gates, the runtime's boot sequence, editor thumbnail
   generation). BlankEngine has exactly this as `ResourceService::wait(resource)`. Keep the call sites few
   and listed.
6. bgfx constraint: `apply` runs on the main thread only (decision 13). Assert it — a bgfx call from
   a worker is a corruption bug that manifests far from its cause.
7. Compilation (Phase 4) moves onto the job system too, which is what makes the first-import hitch
   go away.

**Decisions.** Job system as a separate milestone with its own doc (12); placeholder-first (8); drained
cancellation; `apply` main-thread-only with an assert.

**Risks.** This is the phase where things get genuinely hard, and the risks are the usual ones with
teeth: a resource cancelled while its parse job is in flight; two `Load` calls for the same handle
racing to insert; `Shutdown` while jobs are queued; the ABA problem on a weak-cache entry that
expires between miss and insert. Every one of these needs a deliberate answer, and the honest
estimate for this phase is that it takes longer than the other five combined per line of code
written. The mitigation is scope: one scheduler, three priorities, no per-frame graph, and no
scheduler work at all in this phase — that is the threading milestone's job. If this phase starts
tuning a scheduler, stop.

**Verification.**

| Check | How |
| --- | --- |
| No call-site changes | `git diff --stat` for Phase 5 touches `Assets/`, `Core/`, and nothing in `Scene/Systems/` or the editor panels except the new `Update` tick and `WaitFor` sites |
| Cold scene open does not hitch | Frame-time capture over a cold open before/after; report measured numbers |
| Placeholders appear and resolve | Open a heavy scene, observe checkerboards resolving to real textures over a few frames |
| Cancellation is safe | Open a scene, immediately open another; no crash, no leak, no asset from scene 1 applied into scene 2 |
| Shutdown with work in flight | Close the editor mid-load; clean exit under a debugger, no assert |
| Thread sanity | Assert on bgfx-from-worker never fires; run the editor under the VS thread-safety analyzer or add a deliberate violation in a scratch build to confirm the assert works |
| Docs | [`core.md`](../engine/core.md) job system section; [`assets.md`](../engine/assets.md) async loading, `Status`, placeholders, `WaitFor`; [`architecture.md`](../engine/architecture.md) frame-flow update for the drain point |

---

## Phase 6 — Hot reload: file watching and dependency-driven invalidation

**Goal.** Edit an asset on disk, see it in the viewport without restarting.

**Steps.**

1. A platform file watcher. Win32 `ReadDirectoryChangesW` on `assets/` is the right answer on the
   target platform and is ~150 lines behind a `FileWatcher` interface in `Platform/`. The cheap 80%
   is an mtime poll over the scanned registry every N frames — worse latency, zero platform code,
   and genuinely fine for a single-developer editor. **Recommend starting with the poll**, and only
   writing the Win32 watcher if the poll's cost shows up in a profile. Do not build both.
2. Debounce: editors write files in bursts (write + temp + rename). Coalesce events over ~200ms
   before acting, and key the coalescing by asset handle. BlankEngine's watcher queue accumulates an
   `EventType` bitmask per path per frame for exactly this reason.
3. `AssetManager::OnAssetModified(handle)`: invalidate the compiled output, re-run parse/apply into
   **the existing object** rather than creating a new one, so every `AssetRef` holder sees the new
   contents without knowing anything happened. This in-place reload is why BlankEngine's managers can hot
   reload at all, and it is the reason Phase 3's `AssetRef` must hold the object rather than the
   handle-plus-lookup.
4. Dependency propagation using the reverse map from Phase 4 step 6: texture changed → its materials
   re-apply; material changed → nothing else needs to (materials are referenced, not embedded).
5. Deletion: an asset removed from disk keeps its handle and goes to `Status::Failed` with the
   placeholder. Do not evict the registry entry — scenes still reference it and the file may come
   back (a branch switch does exactly this).
6. Editor: a toast when a hot reload happens, and a way to disable watching (a large git operation
   will otherwise generate a storm of events).

**Decisions.** Poll first, Win32 watcher only if measured (cheap 80%); reload in place; deleted
assets go `Failed` rather than being evicted; debounce keyed by handle.

**Risks.** Reload-in-place means the object's identity survives but its contents change under
whatever is reading it — a mesh whose vertex buffer is swapped mid-frame. Reload must be applied at
the frame boundary, in the same main-thread drain as Phase 5's `apply`. Second risk: event storms
during a branch switch — hence the disable switch and the debounce.

**Verification.**

| Check | How |
| --- | --- |
| Texture hot reload | Edit a PNG in an external editor, save; viewport updates within a second, no restart |
| Dependency propagation | Edit a texture used by two materials on three entities; all three update |
| Deletion is survivable | Delete a referenced asset; placeholder appears, no crash; restore the file, it comes back |
| Branch switch | `git checkout` a branch touching many assets; no crash, no storm-induced hang |
| Mid-frame safety | Reload under a heavy scene repeatedly; no flicker of half-applied state |
| Docs | [`assets.md`](../engine/assets.md) hot reload section; [`platform.md`](../engine/platform.md) if a Win32 watcher is written; [`editor.md`](../editor/editor.md) for the toast + toggle |

---

## Not doing (and why)

Each of these is a real BlankEngine feature that this plan deliberately excludes. The point is not that
they are bad — they are load-bearing for BlankEngine — but that they solve problems Ganymed does not have,
at costs Ganymed cannot currently justify.

- **Mip streaming with budgets, GPU feedback and bindless residency**
  (`texture_resource_manager.hpp`, `streaming_service.hpp`). This is the single largest subsystem in
  BlankEngine's resource layer: per-level `LoadingScheme`, atomic loaded/target levels, bandwidth and
  allocation-rate limits, `BottleneckType` classification, a GPU feedback buffer with readback, a
  bindless info buffer, and a whole `StreamingService` merging per-world quality requests behind
  `StreamingRulesOverride`. It exists because BlankEngine streams open worlds that do not fit in VRAM.
  Ganymed loads bounded scenes, does not use bindless, and has no measured VRAM pressure. This is
  weeks of work to solve a problem that does not exist yet; Phase 4's BCn compression captures the
  VRAM win for a fraction of the cost. Revisit only when a scene actually fails to fit.
- **Generators / complex assets** (`asset_generator.hpp`). One source → many assets, with
  `ImportAction{Move, CreateNew, DeleteAndReplace, ModifyExisting}`, `IModifier`, per-output
  `State{Generate, NoGenerate, NoGenerateAndReplace}` and rename tracking. BlankEngine needs it because
  artists drop FBX files containing dozens of sub-assets that must round-trip across reimports.
  Ganymed's `MaterialSerializer::GenerateSidecars` already handles the one case it has (glTF →
  `.gmat` sidecars + embedded texture extraction) in a few hundred lines. Generalizing it into a
  generator framework would be a system with one call site.
- **`PlatformContext` and per-platform/per-API compiled outputs.** Ganymed builds one target at a
  time and bgfx already abstracts the graphics API. Decision 9 keeps the hook (the output key is
  computed in one function) without building the machinery.
- **The virtual filesystem with alias mounts and zip packages** (`virtual_file_system.hpp`).
  Genuinely useful for shipping, and Ganymed's runtime already has a working assets snapshot instead.
  This is packaging work, not asset-layer work; it belongs to a distribution milestone.
- **BlankEngine's config-migration machinery** (`m_cfgUpdateVersionFn`, `m_legacyCfgNames`,
  `ISourceConverter` with `Version{Latest, Outdated, CriticalOutdated}`). BlankEngine has years of shipped
  configs to migrate. Ganymed has zero. A single `ImportConfigVersion` int plus a switch
  (decision 15) covers the next several years. Reflection itself is not excluded — it is deferred to
  its own milestone on `entt::meta` (decision 14, [`REFLECTION_ROADMAP.md`](REFLECTION_ROADMAP.md)),
  which is where the `.meta` import-config panel gets auto-generated. **RTTR specifically is
  rejected**, for the reasons in decision 14(b).
- **`AssetDependenciesService` at arbitrary depth**, with source-vs-external edge classification and
  a persisted compact DB. Decision 11 takes the depth-1 reverse map, which answers both questions
  Ganymed actually asks.
- **The statistics service** (`IResourceStatistics`, `ResourceStatistics<D>`, per-type rows enriched
  from asset intermediate data). Nice panel. Phase 4's `.dep` already stores compile time and output
  size, so if this is wanted later it is a UI over data that exists. Not a phase.
- **Asset edit states** (`IAssetEditState`, `findOrOpenEditState`, `enqueueSave`, `PauseWatchersScope`).
  This is infrastructure for editors that mutate assets in memory with deferred saves. Ganymed's
  editor mutates scenes, not assets, and writes `.gmat` immediately.
- **A per-frame manager execution graph** (`runBefore`/`runAfter`, `tf::Subflow`, `ExecutionGraph`).
  With one job system, three priorities and fewer than ten asset types, a fixed drain order in
  `AssetManager::Update()` is sufficient and vastly easier to reason about. If manager
  interdependencies ever become non-trivial, revisit — but note that `assignAfter` in BlankEngine exists
  mostly to order *construction*, which a fixed registration order handles.
- **Compression as a pluggable policy** (`ICompressor`, `ZStdCompressor`, `PassthroughCompressor`,
  compressor hash feeding invalidation). Adding zstd is a new dependency for a win that BCn already
  delivers on the dominant asset class. If mesh blobs become a size problem, revisit — and note the
  `.dep` epoch already reserves a slot for a compressor hash.

---

## Appendix A — BlankEngine reference map

Files studied for this plan, for anyone re-reading the source design. All under
`D:\game\src\engine\engine\services\`.

| File | What to look at |
| --- | --- |
| `assets/asset_service.hpp` / `.cpp` | `OpenFlags`, `openAsset`, `openResourceFile`, `createHashedFileName`, `generateOutputPath`, `findInCache`, the watcher registry + `WatcherQueue`, `ImportAssetTask` in an `AsyncQueue`, `validateAssetPath`/`PathErrorType` |
| `assets/asset.hpp` | `AssetData`, `IntermediateData`, `Epoch`/`EpochDiff`, `checkRecompile`, `tryCompile`, `openCompiledFile`, the path field set — **the core of Phase 4** |
| `assets/asset_compiler.hpp` | `IAssetCompiler::Context`, `compile(Blob&&, ctx, Blob&, variant&)`, `ISourceConverter`, `MetaInfo`, the `registration::AssetCompilerBase<T>` DSL |
| `assets/asset_watch.hpp` | `EventType{Deleted, Modified, ModifiedDependency}` as a per-frame bitmask — **Phase 6** |
| `assets/asset_dependencies.hpp` / `asset_dependencies_service.hpp` | The source-vs-external dependency rule; forward/backward queries at depth |
| `assets/asset_generator.hpp` | Generators and complex assets — read to understand what is being skipped and why |
| `resource/resource.hpp` | `ResPtr<T>`, `IResource`/`Status`/`type<T>()`, `Resource<T>` CRTP, `ResourceModificationTracker` — **the core of Phase 3** |
| `resource/resource_service.hpp` | `IResourceManager`, cache strategies, `ParseResourceRoutine`, `SimpleResourceManager`, `AdvancedResourceManager`, `ResourceService`, `makeResPtr`, the registration DSLs — **the core of Phases 2 and 5** |
| `resource/resources/skeleton_resource_manager.hpp` | The minimal `SimpleResourceManager`: `parse` + `applyLoadResult` + `gatherStatistics` and nothing else |
| `resource/resources/texture_resource.hpp` / `_manager.hpp` | The streaming case, and the canonical registration block at `texture_resource.cpp:1917` |
| `resource/resources/material_resource_manager.hpp` | The `AdvancedResourceManager` escape hatch: pending-command queue + inheritance reload |
| `resource/resources/effect_resource_manager.hpp` | A `SimpleResourceManager` with a side cache and a non-default priority |
| `resource/streaming_service.hpp` | The streaming budget service — read to understand the cost of what is being skipped |
| `assets/compilers/texture_compiler.hpp` | A concrete compiler with a versioned `Cfg` and reported `Info` stats — the model for Phase 4's `TextureCompiler` |
| `filesystem/virtual_file_system.hpp` | `IFile::readAsync` → `IAsyncData`, mounted filesystems |
