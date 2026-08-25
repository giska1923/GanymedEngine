# GanymedEngine — Content Authoring Roadmap

Status: **Phases 1-4 complete; Phase 5 planned.** Written 2026-08-25, against the post-runtime/post-audio
engine (branch point: the standalone-runtime + audio milestone, complete). Follows the format of
[`ANIMATION_ROADMAP.md`](ANIMATION_ROADMAP.md) and
[`RUNTIME_AUDIO_ROADMAP.md`](RUNTIME_AUDIO_ROADMAP.md): each phase carries goal, steps, decisions
with rationale, risks, and a verification table; execution notes get appended as phases run. Read
the decision notes even if you skip the code sketches.

This is the plan of record for the seventh milestone: **content authoring** — the editor stops
being a scene *assembler* and becomes a scene *authoring tool*. Four pillars: full-scene
**undo/redo**, **linked prefabs** (`.gprefab`), **material assets** (`.gmat` + per-entity
override slots), and the **serialization hygiene** that makes all three verifiable (deterministic
saves, registry discipline, the RmlUi debugger play/stop crash). After this milestone an author
can iterate on a scene for an hour without fear, share a reusable entity subtree, and give two
instances of one mesh two different looks — none of which is possible today.

## Decisions of record

Settled up front, before any code:

1. **The plan lives here**, indexed from `docs/README.md`, matching the previous milestones. No
   new doc files are proposed: undo and prefab authoring land in `docs/editor/editor.md` (heavy
   rewrite), asset formats in `docs/engine/assets.md`, save-order and component changes in
   `docs/engine/scene.md`. `AGENTS.md`'s directory→doc table is unchanged (no new top-level
   directories).
2. **Undo covers scene editing only.** Inspector property edits, add/remove component,
   create/delete/duplicate entity, reparenting, and gizmo drags are undoable. Asset-level edits —
   `.gmat` field changes, registry writes, prefab Apply — are explicitly **not** undoable.
   Rationale: scene edits live in one document with one owner (`m_EditorScene`) and a natural
   transaction boundary; asset edits are shared, cross-scene, disk-backed state where a
   scene-local undo stack would lie about scope (undoing a `.gmat` edit would silently change
   every scene using it). Unity draws the same line for most asset properties; Unreal's
   transaction system does cover assets — Ganymed diverges toward Unity's model because it has
   no per-asset dirty/transaction infrastructure and building one is a milestone of its own.
3. **Prefabs are v1-simple**: a `.gprefab` is an authored entity subtree; an instance carries its
   source handle; propagation is explicit ("Apply to prefab" / "Revert instance"), whole-subtree,
   no per-field overrides, no nesting, no auto-update of open scenes. The Unity/Unreal norm
   (override tracking, propagation on apply, nested prefabs) is a serialization-diff engine
   Ganymed does not have; the v1 model is the largest useful subset that needs none of it. The
   upgrade path (per-field overrides) stays open because instances already serialize their
   source handle.
4. **Materials follow the Unity/Unreal renderer-slots model**: material assets (`.gmat`), and a
   per-slot override list on `StaticMeshComponent`. The mesh's imported materials stay untouched
   inside the mesh asset (and its cache); `.gmat` is an *additive layer*. This diverges from the
   full Unreal norm (mesh assets reference material assets directly) — argued in Phase 3.
5. **Hygiene rides along**: deterministic scene saves, registry sort + batched writes +
   stale-entry cleanup + parse hardening, and the RmlUi debugger play/stop fix. Explicitly
   **not** riding along: the off-mouse picking-readback failure found in the animation milestone
   stays flagged debt (re-flagged in "Not doing").
6. **Hygiene ships first, undo second.** The provisional structure had undo first. Swapped, with
   the argument: the milestone's single best verification instrument is *"apply N scripted
   edits, undo all, save, byte-compare against the pre-edit save"* — and that instrument only
   exists once saves are deterministic. Deterministic saves + registry + RmlUi are ~days of
   low-risk work; undo is the milestone's largest and riskiest phase and deserves the instrument
   on day one, not retroactively. The "fail fast on the highest-risk unknown" precedent
   (runtime-before-audio) argued the other way, but that case gated a later phase on the risky
   one; here nothing downstream is *blocked* by undo, while undo's own verification is blocked
   by hygiene. The `DeserializeEntity` extraction also lands in Phase 1, de-risking Phase 4
   early.

## Where the engine is today (facts this plan is built on)

Verified against the tree at planning time (all file:line refs current); re-verify anything
load-bearing before executing a later phase.

- **There is no undo substrate.** No before-values exist anywhere: `ChangeBuffer` stores
  `vector<entt::entity>` dirty flags with a 2-frame history (`CollectSince` asserts if a
  consumer skips a frame) — unusable as undo storage. The `Graveyard` holds values but 1 frame
  deep and only for NativeScript/Script. Undo must self-snapshot. The `on_destroy`-handler
  technique (`Scene::OnFiniDestroy`, `GanymedEngine/source/GanymedE/Scene/Scene.cpp:92-97`) is
  proven and generalizable for snapshotting removals.
- **The inspector has one choke point.** `SceneHierarchyPanel.cpp` `DrawComponent<T>`
  (:316-360): raw `T&` at :322, `uiFunction(component)` at :351, `RemoveComponent` at :357. All
  16 component sections flow through it (Tag is edited separately above them). `DrawVec3Control`
  (:162-235) writes per-tick during drags and returns `void`. No commit-boundary detection
  exists anywhere (`IsItemDeactivatedAfterEdit`/`IsItemActivated` appear nowhere in
  GanymedEditor). Trap: the Transform lambda does an unconditional degrees↔radians round-trip
  (:523-525) that can trip value-diffs with no user edit.
- **The gizmo destroys its own before-value.** `EditorLayer.cpp:386-441` writes every frame
  while `ImGuizmo::IsUsing()`; rotation is delta-accumulated (:432-434), so the pre-drag value
  is unrecoverable post-hoc. Snapshot on the `IsUsing()` rising edge or lose it.
- **Entity lifecycle**: `CreateEntityWithUUID` gives ID/Transform/WorldTransform/Relationship/
  Tag. `DestroyEntity` (`Scene.cpp:314-340`) is immediate and orphans children to root
  **without** `MarkChanged<Relationship>` (pre-existing bug: stale transform cache on orphans).
  `RemoveChildFromParent` erases without recording sibling index; `SetParent` push_backs
  (sibling order lost on undo) and silently no-ops on cycles. **No `DuplicateEntity` exists** —
  `Scene::Copy`'s `ForEachType(ComponentList)` loop (`Scene.cpp:203-207`) is the template;
  `TagComponent` is outside `ComponentList` and must be handled explicitly.
  `Entity::operator bool` does not check registry validity — undo records must key on UUID +
  `FindEntityByUUID`.
- **Shortcut routing trap**: `ImGuiLayer::BlockEvents(!viewportFocused && !viewportHovered)`
  (`EditorLayer.cpp:345`) means `OnKeyPressed` (:506-572) only fires with viewport hover/focus.
  Ctrl+Z over the Properties panel never arrives, and ImGui's `InputText` swallows its own
  Ctrl+Z. No Ctrl+Z/Y/S/D or Delete key exists today.
- **Play mode**: panels retarget `m_ActiveScene` on play (`EditorLayer.cpp:651-664`); play-mode
  inspector edits mutate the throwaway copy. `NewScene`/`OpenScene` construct new `Scene`
  objects (:586/:631).
- **Serializer**: `SerializeEntity(YAML::Emitter&, Entity)` is a file-static free function
  (directly reusable once un-static'd). Deserialize is a ~300-line monolith loop
  (`SceneSerializer.cpp:449-754`); no per-entity read exists. The UUID-collision remap
  (:471-474) mints a fresh UUID but does **not** fix up Parent/Children referencing the old one
  (latent bug). Save order is `view<IDComponent>` = entt packed order, iterated backwards in
  entt 3.16, reshuffled by play-mode copies — nondeterministic in practice.
- **No UUID remapping code exists anywhere.** `Scene::Copy` preserves UUIDs;
  `RelationshipComponent` is copied bitwise (correct only because of that). Trap: `AssetHandle`
  *is* `UUID` (same C++ type) — any remap must be whitelisted to `IDComponent` +
  `RelationshipComponent`. Components carrying AssetHandles: `StaticMesh.Mesh`,
  `SkyLight.Environment`, `Script.Script`, `AudioSource.Clip`. No other entity-UUID carriers
  exist (script Fields have no entity type; no physics joints).
- **Materials**: fields are shader/name/albedo/metallic/roughness/3×(texture Ref + path string
  + embedded bytes)/twoSided/transparent — no AssetHandle anywhere on `Material`; texture
  identity is path strings (handles minted by `TextureImporter::LoadMaterialMap` are
  discarded). Materials are owned by `Mesh` and shared via `LoadedMeshes` — editor edits are
  global and unpersisted. `Bind()` asserts on null shader; any loader must inject
  `MeshShader::Get()` (the `MeshCache::ReadMaterials` precedent). `MeshCache` v6 serializes
  materials inline, keyed on source mtime only. The per-submesh
  `SubmitMesh(mesh, submeshIndex, material, transform, entityID)` overload **exists with no
  callers** (`Renderer3D.h:25-26`) — the override seam is already cut. Instancing merges on
  `Ref<Material>` identity (`Renderer3D.cpp:808-814`); the transparent flag drives both pass
  and shadow-caster partition (:756).
- **Asset layer**: `AssetType` is ordinal-persisted, append-only. **`Material` already exists at
  ordinal 4** with `.gmat` extension-mapped (`AssetTypes.cpp:21`), content-browser tinted and
  importable — a reserved type with no reader, writer, or `GetAsset` path; Phase 3 fills it in
  without touching the enum. **`Prefab` must be appended as 8** after `Audio = 7`.
  `GetAsset<Material>` needs five edits (fwd decl, loader, header specialization decl,
  static_assert message text at `AssetManager.h:49`, cache map + `Shutdown` clear). Two asset
  patterns exist: cached `GetAsset` (Mesh/Environment/Texture2D; `LoadTexture` at
  `AssetManager.cpp:200-229` is the template) vs path-resolved (Script/Audio, rationale at
  `AssetManager.h:40-44`). The `writableRegistry` guard lives *inside* `SaveRegistry`
  (`AssetManager.cpp:344`); new asset-file writers are outside it.
- **Registry**: `SaveRegistry` iterates an `unordered_map` (identity hash on random uint64 →
  full reorder on rehash), is called from Shutdown + **every** `ImportAsset` (8 sites),
  truncates in place with no atomicity. `LoadRegistry` has no try/catch (malformed `.gr`
  terminates the process out of `Init`) and no duplicate-path detection. Confirmed stale
  entries in `GanymedEditor/assets/AssetRegistry.gr`: doubled-prefix Fox (handle
  5236765339365056590) and `scripts/_P5Probe.lua` (handle 9149174465144929038).
- **RmlUi debugger crash, precisely**: `UIEngine::CloseAllDocuments` (`UIEngine.cpp:204-208`)
  calls `Context->UnloadAllDocuments()`, destroying the 5 debugger-owned documents (GE_DEBUG
  only, ids prefixed `rmlui-debug-`); `DebuggerPlugin` logs an error and `ReleaseElements()`
  nulls members while staying registered → `SetVisible` after the first stop is a null deref
  (`DebuggerPlugin.cpp:111-117` unguarded).
- **Editor GUI cannot be driven programmatically** (established: SendKeys fails against ImGui).
  Verification leans on scripted harness probes (the audio milestone's lifecycle pattern), log
  evidence, and screenshots; interactive checks are named as such.

---

## Phase 1 — Serialization hygiene + serializer groundwork

**Goal:** saving the same scene twice produces byte-identical files, per-entity deserialization
exists as a reusable unit, the registry is sorted/batched/hardened/clean, the RmlUi debugger
survives play/stop cycles, and two latent scene bugs are fixed. Everything here is small,
independent, and the substrate for every later phase's verification.

### 1.1 Deterministic scene saves — hierarchy DFS

`SceneSerializer::Serialize` stops iterating `view<IDComponent>` and instead walks: **roots
sorted by UUID, then depth-first through each root's `Children` vector in authored order**, with
a visited set (the `TransformSystem::RecomputeSubtree` precedent) and a safety net — any entity
not reached from a root (should not exist; log a warning) is appended in UUID order so nothing
is ever silently dropped.

**Decision: DFS, not a flat UUID sort.** Both are deterministic. Flat UUID sort gives
minimally-scoped diffs (a reparent touches only Relationship fields, entities never move in the
file) — that is the pure-VCS argument, and it is real; record it. DFS wins anyway because (a) a
subtree is a *contiguous block*, which is exactly the layout `.gprefab` files must have (Phase 4
reuses this walk verbatim — one canonical order for both formats, not two), (b) the file reads
as the hierarchy panel does, and (c) sibling order — which is authored, user-visible state — is
what orders the file, so the file order *is* content, not an artifact. Cost accepted: a reparent
moves a block in the diff. Production norm: Unity's scene files are effectively
insertion-ordered with stable fileIDs (diff-friendly by ID stability, not layout); Godot sorts
by node path — DFS is the Godot-shaped choice and the one that pays twice here.

Deserialization is order-independent (Relationship is persisted on both sides, no second pass),
so this changes bytes only, never meaning.

### 1.2 `DeserializeEntity` extraction

Split the ~300-line per-entity body out of the `Deserialize` monolith
(`SceneSerializer.cpp:449-754`) into a member `Entity DeserializeEntity(const YAML::Node&,
Scene&)`; un-static `SerializeEntity` and declare both in `SceneSerializer.h`. `Deserialize`
becomes a loop over the extracted function. Pure mechanical move — **zero behavior change is
the acceptance bar**, checked by the byte-identity test below. This is Phase 4's foundation
(prefab instantiation shares the exact same component-reading code as scene load); doing it
while the code is otherwise quiet de-risks that phase.

Two fixes in passing, both in code this step touches or exercises:

- **UUID-collision remap fixup**: when the deserializer mints a fresh UUID for a collision
  (:471-474), record old→new and patch `Parent`/`Children` in the affected
  `RelationshipComponent`s after the loop. Today a collision silently severs the hierarchy.
- **`DestroyEntity` orphan `MarkChanged`**: `Scene.cpp:314-340` reparents children to root
  without `MarkChanged<RelationshipComponent>`, leaving stale world-transform caches. One line
  per orphan.

### 1.3 Registry discipline

`AssetManager.cpp`, four small changes:

- **Sorted output**: `SaveRegistry` collects entries into a vector sorted by `FilePath` before
  emitting. Stable diffs; a rehash no longer rewrites the world.
- **Batched writes**: `ImportAsset`'s 8 unconditional `SaveRegistry()` calls become
  `m_RegistryDirty = true`; flush points are `Shutdown`, the end of
  `SceneSerializer::Deserialize` (imports minted during load), and the end of each
  editor-initiated import action (drag-drop handlers, content-browser import). Rationale for
  explicit flush points over "dirty + flush on Shutdown only": an editor crash mid-session
  should not lose an afternoon of imports. The `writableRegistry` guard stays where it is
  (inside `SaveRegistry`) and keeps covering every flush.
- **Atomic-ish write**: emit to `AssetRegistry.gr.tmp`, then rename over. Three lines; removes
  the truncate-then-crash = empty-registry failure mode.
- **Parse hardening + hygiene**: wrap `LoadRegistry`'s parse in try/catch (log + empty registry
  + keep running — the `SceneSerializer::Deserialize` posture from the runtime milestone; today
  a malformed `.gr` terminates the process); warn on duplicate `FilePath` entries (keep first).
  Hand-remove the two confirmed stale entries from `GanymedEditor/assets/AssetRegistry.gr` (a
  committed data fix): the doubled-prefix Fox and `scripts/_P5Probe.lua`.

**Runtime write posture, stated**: the runtime never writes asset files. Phase 3's sidecar
generation reuses `IsRegistryWritable()` (expose the existing flag) as the general "this
process may write into `assets/`" gate — one flag, one meaning, documented in `assets.md`,
rather than a parallel guard.

### 1.4 RmlUi debugger play/stop fix

**Decision: option (a) — skip debugger documents in `CloseAllDocuments`, not (b) —
Shutdown/Initialise the debugger around the unload.** `UIEngine::CloseAllDocuments`
(`UIEngine.cpp:204-208`) iterates `GetNumDocuments()`/`GetDocument(i)` (reverse, since unload
mutates the list), skips ids starting with `"rmlui-debug-"`, and unloads the rest. Arguments:
the prefix is the discriminator RmlUi's own `DebuggerPlugin` uses internally
(`DebuggerPlugin.cpp:129-133`), so this is leaning on a de-facto contract, not inventing one;
the debugger's documents never die, so the `SetVisible` null deref (`DebuggerPlugin.cpp:111-117`)
becomes unreachable rather than guarded; no `#ifdef` — in Release there simply are no matching
documents and the loop degenerates to "unload everything", one code path in both configurations;
and the debugger keeps its open/closed state across play/stop instead of resetting. Option (b)
fixes the crash but tears down and rebuilds debugger state every stop under a `GE_DEBUG`
bracket — more code, worse behavior. Leave a comment naming the prefix contract and the RmlUi
version it was verified against.

### Phase 1 risks

- The DFS walk meeting a corrupted hierarchy (cycle, dangling child UUID) must terminate — the
  visited set is mandatory, and the safety-net append is what makes corruption *visible* (warn)
  instead of data loss.
- The extraction diff is large and mechanical; the byte-identity check below is the only honest
  reviewer.
- Registry flush-point placement is judgment; if an import path is missed, the cost is a write
  deferred to Shutdown, not data loss — acceptable failure direction.

### Phase 1 verification

Probes: temporary code in `EditorLayer::OnAttach` / a scripted driver where possible;
interactive checks named as such. x64 Debug.

| Check | Evidence |
|---|---|
| Save determinism | Load an existing multi-entity scene, save to A, save to B → `fc /b` byte-identical. Then play → stop → save to C → identical to A (immune to the play-copy reshuffle) |
| Save canonical form | Save every committed scene once (canonicalizing commit); load → save again → byte-identical (fixed point) |
| Extraction is a no-op | Post-extraction, canonical saves of all scenes byte-match their pre-extraction canonical saves |
| UUID-collision fixup | Hand-craft a scene with a duplicated UUID in a parent/child pair → load → log shows remap, hierarchy intact (probe: walk Relationship and assert no dangling UUIDs) |
| Orphan MarkChanged | Probe: parent at non-identity transform, delete parent via `Scene::DestroyEntity`, tick → child's `WorldTransformComponent` recomputed (log before/after matrices) |
| Registry batching | Fresh session, drag-import 5 assets → log exactly 1 registry write at the action flush (count "registry saved" lines), file sorted by FilePath (a re-save is byte-identical) |
| Registry hardening | Truncate `.gr` mid-file → editor boots, one error log, empty registry, no crash. Stale entries gone (`grep _P5Probe` → nothing) |
| Atomic write | The kill-between-write-and-rename case is not scriptable; assert the rename path in code review + confirm a normal save leaves no `.tmp` |
| RmlUi debugger | Interactive: open the debugger, play → stop → SetVisible → play → stop ×3 — no error log, no crash, debugger contents alive. Release build: play/stop clean, HUD documents unloaded on stop (log document count) |

### Phase 1 execution notes

Executed 2026-08-25. x64 Debug and Release, MSBuild; engine, editor, runtime and Sandbox all build
clean. Verification ran from a temporary `Phase1Probe` block in `EditorLayer::OnAttach` (25 scripted
checks, `Application::Close()` at the end so a run is a single scripted command); probe removed.
**25/25 pass in Debug, 25/25 in Release.**

**`DeserializeEntity` takes the UUID as a third parameter, not the two the plan sketched.** The
signature is `static Entity DeserializeEntity(const YAML::Node&, Scene&, UUID)`. Who decides an
entity's UUID differs per container — a scene keeps the file's and remaps collisions, a prefab
instance always mints a fresh one — so leaving that decision inside the function would have forced
Phase 4 to either re-implement it or pass a flag. Both halves are public statics taking their scene
explicitly, for the same reason: a `.gprefab` is read into a scene the serializer does not own.

**The collision fixup became a uniform pass, not a patch-up after a rare event.** The plan said
"record old→new and patch `Parent`/`Children` in the affected `RelationshipComponent`s". What
landed is `ResolveHierarchy(scene, created, fileUUIDs)`, which runs on *every* load and translates
every reference from file UUIDs to created UUIDs. With no collisions it is the identity, so nothing
is special-cased — and a repair path that only executes on rare input is a path that rots. It also
made the disambiguation honest: when a file genuinely repeats a UUID, "which instance did this
`Children` entry mean?" has no general answer, so `Children` is treated as authoritative (each slot
claims one instance in order) and `Parent` follows from the claim. An entity naming a parent that
does not list it keeps a translated reference and warns — the same inconsistency the save-time
unreachable warning trips on, reported from both ends.

**The plan's "extraction is a no-op" check could not be run as specified, and was replaced by two
stronger ones.** It asked to compare post-extraction canonical saves against *pre-extraction*
canonical saves — but canonical ordering is part of this same change, so no pre-extraction canonical
save exists. Instead: (a) the extracted 274-line body was diffed textually against
`HEAD:SceneSerializer.cpp` lines 476-749, dedented, modulo three documented renames — **identical**,
which makes the extraction provably behavior-free rather than probably; and (b) the pre-change
binary was rebuilt (revert, patch in a minimal save-every-scene probe, build, run, restore) and its
output compared block-by-block against the new canonical saves. `BoxesPhysicsExample` and
`Phase5Test` (48 entities, 10 component types) are **block-sorted byte-identical** old vs new; the
other two match once entity-UUID lines are stripped, for the reason below.

**None of the four committed scenes has a single parented entity.** The DFS walk therefore had no
real data to exercise, so the probe builds its own: two roots, a three-level subtree, entities
created in a scrambled order so entt's packed order cannot accidentally agree with the expected
file order. Verified roots-by-UUID + contiguous subtrees + authored sibling order, that swapping a
`Children` vector swaps the file blocks, and that clearing a parent's `Children` leaves its former
children reachable from no root — both are still written, each with a warning, which is the safety
net doing its job.

**`3DExample.ganymede` and `Example.ganymede` are exactly the legacy case the collision path exists
for**: each contains the hardcoded UUID `12837192831273` three times, so every load remaps three
entities to fresh random UUIDs. Consequence worth stating: the first canonical save of either
rewrites those UUIDs, and two loads of the same file produce different UUIDs. Nothing was severed —
both scenes are flat — but they are the reason those two files cannot be byte-compared across
binaries. Re-saving them once would canonicalize them permanently; not done here, because it is a
content commit and belongs with whatever change actually edits them.

**The editor's `AssetRegistry.gr` is gitignored, so the stale-entry removal is not "a committed data
fix" as the plan called it.** `.gitignore:10` excludes `GanymedEditor/assets/AssetRegistry.gr`;
only the runtime's registry is tracked, and it has neither stale entry. The two entries (the
doubled-prefix `assets/models/Fox.glb` and `scripts/_P5Probe.lua`) were removed locally after
confirming nothing references either handle and neither path exists.

**Added beyond the plan: an unreadable registry is moved aside, not overwritten.** Parse hardening
as specified (log, empty registry, keep running) left the failure only half-handled — the session's
first import would then flush a nearly-empty registry over the corrupt file, and the handle mappings
a hand-repair could have recovered are gone. `LoadRegistry` now renames it to `AssetRegistry.gr.bad`
first. This needed the `ifstream` scoped closed before the rename: Windows refuses to rename an open
file, and the first attempt silently logged "rename failed" — which is why the message names that
outcome explicitly rather than claiming a quarantine that may not have happened.

**Registry write counting needed a log line, so `SaveRegistry` and `CloseAllDocuments` each gained
one `GE_CORE_TRACE`.** Both are kept: "the registry was written, with N entries" and "N documents
unloaded, M kept (debugger)" are the two facts you want when either behavior surprises you, and the
verification table asks for exactly those counts. Measured from an empty registry: an editor boot
writes the file **twice**, once per user-visible action (default environment, then the scene load's
path-based import), where every import used to write it. A boot that imports nothing now writes it
**zero** times — `Shutdown` used to write unconditionally.

**Small plan inaccuracy:** `ImportAsset` had *one* `SaveRegistry()` call inside it, reached from
eight `ImportAsset` call sites — not "eight unconditional `SaveRegistry()` calls". The fix is the
same.

**`AssetHandle` has no `operator<`.** Sorting registry entries by handle needed
`static_cast<uint64_t>`; `UUID` defines `==`/`!=` and a `std::hash` specialization but no ordering.
Phases 2 and 4 will meet this again wherever they want an ordered container keyed by UUID. Adding
`operator<` to `UUID` would be a one-line fix and is deliberately not done here — it is not this
phase's file.

**RmlUi: `Context::UnloadDocument` per document, not `ElementDocument::Close`.** `Close()` defers
the unload to the next context update, which would have changed when documents disappear;
`UnloadDocument` is what `UnloadAllDocuments` called per document and removes it from the root
immediately, rebuilding the hover chain itself. The debugger owns **six** documents, not the five
the plan estimated. The crash reproduces with no UI interaction at all — `SetDebuggerVisible(true)`
is the same call the Ctrl+U menu item makes — so this check is scripted rather than interactive:
three cycles of load-document → debugger visible → `CloseAllDocuments` → `SetVisible` again, clean
in Debug (`1 unloaded, 6 kept`) and in Release (`1 unloaded, 0 kept`), no `destroyed externally`
error, no crash.

**Cosmetic finding, flagged not fixed:** `3DExample.ganymede`'s scene name is
`UntitledAdd commentMore actions` — GitHub review-UI text pasted into the file at some point. It is
harmless (the name is only logged) and lives in a file this phase deliberately does not rewrite.

---

## Phase 2 — Editor undo/redo

**Goal:** every scene edit an author can make in the editor is one Ctrl+Z away from not having
happened, verified by scripted command sequences whose undo-all restores a byte-identical scene
file.

### 2.1 The stack and the command currency

New files `GanymedEditor/source/EditorUndo.h/.cpp` (editor-side, not engine — undo is an
authoring concern; the engine ships no undo, matching how `EditorCamera` lives engine-side but
the *editing model* lives in the editor. Divergence from Unreal, where the transaction system is
engine-core, accepted: Ganymed's runtime has no consumer for it).

```cpp
class EditorCommand
{
public:
	virtual ~EditorCommand() = default;
	virtual void Undo(Scene& scene) = 0;
	virtual void Redo(Scene& scene) = 0;
	virtual const char* Label() const = 0;   // "Edit Transform", "Delete 'Fox'"
};

class EditorUndoStack
{
public:
	void Push(Scope<EditorCommand>);   // clears the redo stack
	void Undo(Scene&); void Redo(Scene&);
	bool CanUndo() const; bool CanRedo() const;
	void Clear();
	void MarkSaved(); bool IsDirtySinceSave() const;  // Phase 5's dirty indicator
private:
	std::vector<Scope<EditorCommand>> m_Undo, m_Redo;
	size_t m_SavedMark = 0;
	static constexpr size_t MaxDepth = 100;
};
```

**Every command keys entities by UUID + `FindEntityByUUID`** — `entt::entity` handles are not
validity-checked (`operator bool` gap) and do not survive destroy/recreate cycles. A command
whose UUID no longer resolves logs a warning and no-ops (loud, not broken — the
`AnimationSystem` unknown-clip posture).

**The snapshot currency is an in-memory component tuple, not YAML.** `EntitySnapshot` holds
UUID, parent UUID, sibling index, Tag, and a `tuple<optional<Ts>...>` over `ComponentList`
filled via `ForEachType` (the `Scene::Copy` `CopyComponent` pattern; `TagComponent` handled
explicitly because it is outside `ComponentList`). Argument vs YAML-based snapshots: lossless
(round-trips `AnimatorComponent::Time`, which YAML deliberately drops; trivially preserves
`ScriptComponent::Fields` *absence* — "no key = track script default" is distinct from "key set
to default"), no string parsing on the hot undo path, and no coupling between undo correctness
and serializer completeness. Production norm: Unreal snapshots through its serialization layer;
Ganymed diverges because `ComponentList` + `ForEachType` already *is* a complete compile-time
component enumeration — new components join undo for free, which the YAML path cannot promise
(its two hand-maintained lists have no compile-time enforcement).

Restore path: recreate/overwrite components, then `MarkChanged` for the tracked ones
(Transform, Relationship) so `TransformSystem` recomputes — restoring a component behind the
change-tracker's back is the silent-staleness trap.

Commands (each a thin subclass over the snapshot currency):

- `ComponentEditCommand<T>` — UUID, before-value, after-value.
- `AddComponentCommand<T>` / `RemoveComponentCommand<T>` — remove stores the full value (Script
  re-add fires init → Lua re-instantiation: side-effectful but correct, note it in the header).
- `CreateEntityCommand` — snapshot taken post-create; undo destroys, redo recreates with the
  same UUID.
- `DeleteEntityCommand` — vector of `EntitySnapshot` for the whole subtree, parents-first; undo
  recreates in order (Relationship restores bitwise-correct because UUIDs are preserved).
- `ReparentCommand` — UUID, old parent + **old sibling index** (recorded explicitly; `SetParent`
  push_backs and the index is otherwise lost), new parent. Recorded *after* confirming
  `SetParent` had effect (it silently no-ops on cycles — recording a no-op would corrupt
  sibling order on undo).
- `DuplicateEntityCommand` — wraps the new `Scene::DuplicateEntity`; undo is subtree delete by
  recorded UUIDs.

### 2.2 `Scene::DuplicateEntity`

Engine-side (`Scene.h/.cpp`): deep-copy an entity subtree with **fresh UUIDs**, using
`Scene::Copy`'s `ForEachType(ComponentList)` loop as the template plus explicit Tag handling,
remapping **only** `IDComponent` and `RelationshipComponent` through the old→new UUID map (the
`AssetHandle == UUID` type-identity trap: a naive "remap every UUID-typed field" would corrupt
`StaticMesh.Mesh`, `SkyLight.Environment`, `Script.Script`, `AudioSource.Clip`). Copy `Children`
vectors before iterating (the `DrawEntityNode` mutation-during-iteration defense). This function
is also Phase 4's instantiation core in all but its input source — build it once, generally.

### 2.3 Inspector interception — commit boundaries at the choke point

`DrawComponent<T>` (`SceneHierarchyPanel.cpp:316-360`) is the single interception point, with
**one contract change**: `uiFunction` goes from `void(T&)` to `bool(T&)` — "did any widget in
this section edit the component this frame", the OR of the widget returns the 16 lambdas
already receive and discard. Mechanical update across all sections; `DrawVec3Control`
(:162-235) returns `bool` (it must anyway — see the Transform trap below).

Per-frame protocol inside the wrapper:

1. Before `uiFunction`: copy `component` into a scratch buffer (cheap — components are
   handle/POD/small-string sized).
2. Call `uiFunction`; capture `edited`.
3. Commit-boundary tracking via ImGui's active-item ID, captured before/after the section's
   widgets: when the active ID first lands inside this section (drag grabbed, text field
   focused), the *pending edit* begins and that frame's pre-copy becomes the recorded
   before-value. While pending, accumulate `edited`. When the active ID leaves the section —
   or, for instant widgets (checkbox, combo, drag-drop assignment) that never hold active
   state, on the same frame — and any frame reported `edited`, push
   `ComponentEditCommand<T>{before, current}`. One command per drag, never per tick.
4. The `RemoveComponent` branch (:357) pushes `RemoveComponentCommand<T>` with the value
   captured before removal; the Add Component menu pushes `AddComponentCommand<T>`.

**The phantom-edit trap, defused at the source**: the Transform lambda's unconditional
degrees↔radians round-trip (:523-525) can change bits without a user edit, which under a
value-diff scheme mints garbage commands. The `bool(T&)` contract sidesteps diffing entirely —
a command is pushed only when a widget *reported* an edit — and the Transform lambda is
additionally fixed to write back only on `DrawVec3Control() == true` (which also makes its
existing `MarkChanged` guard honest). Record the rule in `editor.md`: *inspector lambdas mutate
the component only on actual widget edits and return true when they do*.

Tag rename (the InputText above the component sections) gets the same pending/commit treatment
as a `ComponentEditCommand<TagComponent>`; entity context-menu Delete routes through
`DeleteEntityCommand`.

**Editor delete becomes recursive.** Today "Delete Entity" orphans children to root —
surprising, and no production editor does it (Unity/Unreal/Godot all delete the subtree). With
subtree-snapshot undo the safety argument for orphaning evaporates. `Scene::DestroyEntity`
keeps its orphan semantics as engine API (with the Phase 1 `MarkChanged` fix); the *editor*
delete command walks the subtree. This also pre-builds exactly what Phase 4's Revert needs.

### 2.4 Gizmo edge-snapshot

`EditorLayer.cpp:386-441`: on the `ImGuizmo::IsUsing()` rising edge, snapshot
`TransformComponent` (mandatory — rotation is delta-accumulated at :432-434; the before-value
is unrecoverable one frame later); on the falling edge, push
`ComponentEditCommand<TransformComponent>`. The gizmo is already Edit-state-gated, so no
play-mode leakage from this path.

### 2.5 Shortcuts and the routing decision

**Decision: route editor-global shortcuts ImGui-side; do not change the `BlockEvents` policy.**
New checks in `EditorLayer::OnImGuiRender` via `ImGui::IsKeyChordPressed`, gated on
`!ImGui::GetIO().WantTextInput` (an active InputText keeps its own Ctrl+Z text-undo, matching
every production editor): **Ctrl+Z** undo, **Ctrl+Y** + **Ctrl+Shift+Z** redo, **Ctrl+D**
duplicate selection, **Delete** delete selection, **Ctrl+S** plain save (current path; falls
back to Save-As when unset). Argument: the alternative — relaxing `ImGuiLayer::BlockEvents`
(`EditorLayer.cpp:345`) so `OnKeyPressed` fires everywhere — leaks *every* key to the engine
event path while typing in panels (camera keys, Q/W/E/R gizmo switches), trading one routing
bug for a family of them. Production norm is an editor-global command/shortcut layer above
widget focus; polling ImGui inside the ImGui frame *is* that layer at Ganymed's scale. Migrate
the existing Ctrl+N/O/Shift+S file shortcuts to the same path in passing (they currently
dead-zone over panels too — same bug, same fix); Q/W/E/R stay viewport-gated deliberately
(gizmo mode switches while typing a name would be a regression).

### 2.6 Scope guards

- Record only when `SceneState == Edit` **and** the panel's context is `m_EditorScene` —
  `EditorLayer` hands the panel its undo-stack pointer on scene-state transitions (null during
  play), so play-mode inspector edits to the throwaway copy (:651-664) are structurally
  unrecordable rather than filtered.
- `Clear()` on `NewScene`/`OpenScene` (:586/:631 — new `Scene` object; every UUID in the stack
  is meaningless). The stack **survives play/stop** (the editor scene object persists).
- Undo/Redo shortcuts are Edit-state-gated at the call site.

### Phase 2 risks

- The commit-boundary protocol is the phase's subtle core; the failure modes are
  command-per-tick spam (boundary too eager) and lost edits (boundary missed for instant
  widgets). The scripted byte-identity harness drives commands, not ImGui — so drag granularity
  gets a dedicated log probe plus an interactive pass.
- Restoring tracked components without `MarkChanged` fails *silently* (stale world transforms)
  — the verification table probes it explicitly.
- The 16-lambda contract change is wide but shallow; a lambda returning a stale `false` loses
  that section's undo quietly. Review checklist: every ImGui widget call's return contributes
  to the OR.

### Phase 2 verification

Harness: a temporary probe in `EditorLayer` (commands need the editor scene + panels; Sandbox
has neither) driving scripted command sequences against a loaded scene, leaning on Phase 1's
deterministic saves. Interactive checks named as such.

| Check | Evidence |
|---|---|
| The round-trip theorem | Load scene, save → A. Apply a scripted 30-op sequence (creates, duplicates, reparents incl. sibling-order-sensitive cases, component edits on ≥8 component types, add/remove component, subtree deletes), save → B. Undo ×30, save → A′. Redo ×30, save → B′. **A ≡ A′ and B ≡ B′ byte-identical** (`fc /b`) |
| Lossless delete-undo | Delete an entity with `AnimatorComponent.Time = 1.234` (unserialized field), undo → probe reads Time == 1.234 (the in-memory snapshot beats YAML) |
| Script Fields absence | Entity with one overridden field + one tracking-default (absent key); remove ScriptComponent, undo → probe: overridden key present with value, default key **absent** (not present-with-default-value) |
| Tracked-component restore | Undo a Transform edit on a parent → child `WorldTransformComponent` matches the pre-edit value next frame (MarkChanged fired on restore) |
| Reparent sibling order | Entity at index 1 of 3 siblings; reparent away, undo → back at index 1 (log the Children vector) |
| Gizmo granularity | Interactive: one rotation drag across ~60 frames → log shows exactly 1 command pushed; Ctrl+Z restores the pre-drag rotation exactly (the delta-accumulation case) |
| Drag commit boundary | Interactive: inspector DragFloat held 2 s → 1 command; checkbox click → 1 command same frame; type in Tag, Ctrl+Z while typing → ImGui's text-undo; Ctrl+Z after commit → tag reverts |
| Shortcut routing | Interactive: Ctrl+Z with the Properties panel focused (viewport neither hovered nor focused) → undo fires (the exact case that dead-zones today) |
| Play-mode guard | Play; scripted component edit on the active scene → stack size unchanged; stop → editor-scene stack intact and functional |
| Scene-switch guard | Ops → OpenScene → CanUndo() == false; no stale-UUID warnings on further edits |
| Stack cap | 150 scripted ops → stack holds 100, oldest dropped (log command count) |
| Dead-UUID resilience | Push edit command, delete the entity via a *non-command* path, Ctrl+Z → one warning, no crash |

### Phase 2 execution notes

Executed 2026-08-25. x64 Debug, MSBuild; engine, editor, runtime and Sandbox build clean with no
new warnings. **New files were added (`GanymedEditor/source/EditorUndo.h/.cpp`), so `premake5
vs2022` was re-run.** Verification ran from two temporary probes - a `Phase2Probe` block driving
commands against scenes built in code, and a frame-counted block in `EditorLayer::OnUpdate` for
the real play/stop and scene-switch paths. **26/26 scripted checks pass**; probes removed.

**The round-trip theorem holds.** 30 scripted ops - component edits across eight component types,
add/remove component, three creates, two duplicates (one of a whole subtree), six reparents
including sibling-order-sensitive cases, four subtree deletes - then save, undo ×30, save, redo
×30, save. The pre-edit and post-undo files are byte-identical, and so are the post-edit and
post-redo files. That check is only meaningful because Phase 1 made saves canonical, which was the
argument for doing hygiene first.

**Three command classes collapsed into one mechanism.** The plan listed `CreateEntityCommand`,
`DeleteEntityCommand` and `DuplicateEntityCommand` separately. They are all "a subtree appeared or
disappeared", differing only in which direction `Undo` runs, so what landed is
`EntitySubtreeCommand` with `AddEntitiesCommand` / `DeleteEntitiesCommand` over it. Redoing an add
replays the snapshot rather than re-running the operation, which is what guarantees identical
UUIDs on every redo - re-running `DuplicateEntity` would mint new ones and break the redo half of
the round trip. Phase 4 gets prefab instantiate and Revert from the same two classes.

**`Scene::CollectSubtree` moved out of `SceneSerializer` and onto `Scene`.** Phase 1 put the
canonical DFS walk in the serializer; `DuplicateEntity` and undo's subtree snapshots need exactly
the same walk, and hierarchy traversal is a scene concern rather than a serialization one. The
serializer now calls `m_Scene->CollectSubtree`. Phase 1's docs were corrected in place.

**The commit-boundary protocol landed as planned, and the popup case fell out for free.** Reading
ImGui's `ActiveId` before and after each section gives: first frame active inside the section
starts a pending edit, first frame not active commits it, and a pending edit that no frame
reported an edit for is dropped. That last rule is what makes the multi-frame cases correct
without special-casing them - a combo opens (active set, no edit), the popup stays open for
frames (pending dropped), then a `Selectable` is pressed and released (new pending, one command).
A checkbox is the same shape across its press and release frames. Drop assignments never take
`ActiveId` in the inspector at all, because the drag source is the content browser item, so they
fall to the immediate-push branch and commit on the drop frame.

**The `bool(T&)` contract cost less than budgeted and paid a second time.** All 16 lambdas were
converted, plus `DrawVec3Control` and `DrawScriptFields`. While in there, the Add Component popup
went from 14 hand-rolled `HasComponent`/`MenuItem`/`AddComponent` blocks to 14
`DrawAddComponentEntry<T>` lines - the same change that had to record an `AddComponentCommand`
anyway, and the repetition was the risk the review checklist was worried about.

**The Transform phantom-edit trap was real and is fixed at the source.** The lambda used to write
`component.Rotation = glm::radians(glm::degrees(component.Rotation))` unconditionally, and the
round-trip is not exact. Under the `bool(T&)` contract that could not mint a command on its own,
but the pre-existing `MarkChanged` guard was already dishonest about it - selecting an entity
could dirty its transform. Rotation is now written back only when `DrawVec3Control` reports an
edit, which makes both the guard and the undo boundary correct.

**The StaticMesh section's material fields deliberately report `false`.** They edit the mesh's
shared `Material` objects, not the component - the change is global, unpersisted, and visible in
every scene using that mesh. An undo command claiming to own it would lie about its scope
(Decision 2), so the section returns true only for the mesh-handle assignment. Phase 3 replaces
that block with `.gmat` override slots, which are component state and pick up undo for free.

**Editor delete became recursive, and had to become deferred at the same time.** Destroying a
subtree from inside `DrawEntityNode` would destroy entities the enclosing entt view is still
iterating - the old single-entity delete had the same hazard latently and got away with it. The
request is now recorded as a UUID and serviced after the walk.

**`RemoveSubtree` destroys children-first, which is not optional.** `Scene::DestroyEntity`
unparents a destroyed entity's children to root rather than destroying them, so removing the
parent first would strand the rest of the subtree as roots for the remainder of the loop.

**A probe bug worth recording, because it is the exact hazard the design documents.** The first
run of the layer probe crashed on `Assertion failed: Entity does not have component!` - it held an
`Entity` handle across an undo that destroyed that entity, then called `GetUUID()` on it.
`Entity::operator bool` does not check registry validity, so the handle looked live. This is
precisely why every command keys on UUID rather than `entt::entity`; the assert caught it in the
test code rather than in the feature.

**A real crash found in review and fixed, with the bug reproduced before and after.** Undo can
destroy the entity the hierarchy panel has selected - Ctrl+Z on a "Create Entity" is exactly that
- and the selection is the one piece of editor state that has to hold an `Entity` rather than a
UUID. `Entity::operator bool` does not check registry validity, so the dead handle looked live all
the way into `GetComponent` and the next frame's Properties panel asserted (`Entity does not have
component!`, exit code 3, confirmed by removing the guard and re-running).
`SceneHierarchyPanel::ValidateSelection` now checks `registry.valid()` once a frame. This is the
same hazard that took down the layer probe, arriving through the feature instead of the test.

**`EditorCommand::Label()` returns `const std::string&`, not `const char*`.** Labels like
`Delete 'Fox'` are composed per command, so they need storage. The label is carried rather than
derived because the entity it names may no longer exist when it is read.

**Duplicated entities keep the source's name.** Unity would append a suffix. Inventing a naming
scheme is not this phase's job and two identically named siblings are at worst mildly confusing,
so this is flagged rather than decided - it is a one-line change in `Scene::DuplicateEntity` if it
turns out to annoy.

**An Edit menu was added.** Undo / Redo / Duplicate / Delete with their shortcuts, plus a plain
**Save** entry, because a shortcut with no discoverable menu entry is not a feature anyone finds.
Plain save needed `m_EditorScenePath`, which the editor did not track at all before - it only had
Save-As.

**Interactive checks not run, and they are the ones a script cannot reach.** Gizmo drag
granularity, inspector drag granularity, ImGui's text undo inside the Tag field, and Ctrl+Z with
the Properties panel focused all need a human at the keyboard, since ImGui cannot be driven
programmatically (established in the audio milestone). The log evidence they need is already
permanent: `EditorUndoStack::Push` traces every command with its label and the resulting depth, so
one drag producing one `Undo: pushed 'Edit Transform'` line is directly readable in `GanymedE.log`.

---

## Phase 3 — Materials: `.gmat` assets + per-entity override slots

**Goal:** materials become assets — importable, editable, savable, reloadable — and two entities
sharing one mesh can look different, without touching the mesh cache or breaking instanced
batching for entities that share a material.

### 3.1 The shape, argued

**Decision: the mesh keeps its imported materials untouched; `.gmat` is an additive override
layer populated at instantiation.** Import generates `.gmat` sidecars (idempotently, never
overwriting a user-edited file); `MeshImporter::Instantiate` fills the new entity's override
slots with the sidecar handles, so drag-dropped entities author against `.gmat` from birth;
entities without overrides (every existing scene) render the mesh's built-in materials,
bit-identically to today.

Argued against the fuller Unreal norm (mesh assets reference material assets directly, no
"built-in" materials): that shape requires the mesh cache to depend on `.gmat` mtimes — and
`MeshCache` v6 embeds materials inline, keyed on *source* mtime only, so editing a `.gmat`
would either silently not invalidate the cache or require a dependency-tracking index Ganymed
doesn't have (and which "Not doing" declines to build). The additive shape costs a duplicated
default (the mesh's material and its sidecar start identical) and buys: zero cache-format
changes, zero risk to existing scenes, and a reverse-dependency index made *unnecessary* —
`Reload(.gmat)` needs no material→mesh map because meshes never reference `.gmat`; only
override slots do, and `RenderSystem` re-fetches by handle every frame (the established
posture), so eviction lands next frame for free. State that in `assets.md`; it is the shape's
best property.

### 3.2 `.gmat` format + `MaterialSerializer`

New files `GanymedEngine/source/GanymedE/Assets/MaterialSerializer.h/.cpp` (reader + writer —
the first asset *writer* outside the registry; premake regen after adding files). YAML schema:
`Material:` → `Name`, `Albedo` (vec4), `Metallic`, `Roughness`,
`AlbedoMap`/`NormalMap`/`MetallicRoughnessMap` (asset-root-relative **paths**, omitted when
unset), `TwoSided`, `Transparent`. Shader is implicit (`MeshShader::Get()` injected by the
loader — `Bind()` asserts on null shader; the `MeshCache::ReadMaterials` precedent) until
shader variants exist ("Not doing").

**Textures by path, not handle**, argued: `.gmat` files must be self-describing and
hand-editable/mergeable — the runtime milestone's registry lesson (bare handles are meaningless
without a shipped registry; a path survives a fresh clone). The loader resolves each path via
`ImportAsset` (idempotent) + `GetAsset<Texture2D>` — de-duplicated through the animation
milestone's texture cache. Scene files keep storing handles (for the *`.gmat` asset itself* in
override slots) — handles are the scene↔registry currency, paths are the asset↔asset currency;
the split already exists (`.gr` maps between them) and this follows it. Emit keys in fixed
order; a `.gmat` write→read→write must be byte-identical (the Phase 1 discipline applied to a
new format).

### 3.3 `GetAsset<Material>` + Reload

No enum or extension work — `AssetType::Material` has existed at ordinal 4 since the asset
system landed, `.gmat` is already extension-mapped (`AssetTypes.cpp:21`), and the content
browser already tints and imports it (both currently no-ops beyond a registry entry). Phase 3
fills the reserved type in.

The five-edit checklist (`AssetManager.h/.cpp`): forward-declare `Material`; private
`LoadMaterial(AssetHandle)` mirroring `LoadTexture` (`AssetManager.cpp:200-229`) step for step;
header specialization declaration; **update the static_assert message text** (`AssetManager.h:49`
enumerates supported types verbatim); `LoadedMaterials` map + `Shutdown` clear + definition.
Cached-pattern, not path-resolved: override slots resolve per frame from `RenderSystem`, and
Ref-identity is load-bearing (instancing merges on `Ref<Material>` — a shared `.gmat` handle
must yield one Ref so entities batch; per-call loads would shatter every batch).

`Reload` Material branch: evict its 3 textures **first** (reuse the StaticMesh branch's loop
shape, `AssetManager.cpp:277-288`), then erase from `LoadedMaterials`; must be added *above*
`default:` (which early-returns and skips the log). Consumers re-fetch per frame → next-frame
visual update, no reverse index needed (3.1).

### 3.4 Sidecar generation + embedded-texture extraction

At successful mesh load (cold import *and* cache replay — both paths produce the materials
vector), gated on `AssetManager::IsRegistryWritable()` (the Phase 1 posture — the runtime never
writes into `assets/`):

- Per material slot `i`, generate `<meshdir>/<meshstem>_mat<i>_<sanitizedName>.gmat` **iff
  absent** — the index is the identity (glTF names are non-unique/absent/unsanitized;
  `Submesh::MaterialIndex` already speaks index; the name is for humans), and *iff absent* is
  what makes user edits sacred (a re-import never clobbers an authored `.gmat`). `ImportAsset`
  each (registry-batched by Phase 1).
- **Embedded (`.glb`) textures are extracted once** to
  `<meshdir>/<meshstem>_textures/<map>_<i>.png`, idempotently (skip if the file exists), so
  `.gmat` references real files only — a `.gmat` that references bytes inside another asset's
  blob would be neither self-describing nor editable. Extracted files are `ImportAsset`ed like
  any texture. Registry consequence stated in the commit: `.glb` imports now mint texture +
  material entries (one batched write).

`MeshImporter::Instantiate` (`MeshImporter.cpp:776-791`) computes the sidecar paths by the same
naming rule, `ImportAsset`s them (idempotent), and fills `MaterialOverrides` on the new entity.

### 3.5 `StaticMeshComponent.MaterialOverrides` + the render seam

`std::vector<AssetHandle> MaterialOverrides` parallel to the mesh's material list (index =
`Submesh::MaterialIndex`; `InvalidAssetHandle` = use mesh default). Serialize both sides
guarded (emit only when any slot is set — existing scenes' files stay byte-identical,
protecting Phase 1's canonical saves). Inspector display joins the component checklist.

`RenderSystem`: per entity, resolve each set slot via `GetAsset<Material>`; submeshes whose
slot resolves go through the **existing no-caller per-submesh overload**
`SubmitMesh(mesh, submeshIndex, material, transform, entityID)` (`Renderer3D.h:25-26` — the
seam was cut for this); unset slots take the current whole-mesh path unchanged. Consequences
to encode, not discover: an override's `Transparent` flag legitimately moves that submesh
across the pass partition *and* changes its shadow-caster status (`Renderer3D.cpp:756`) — that
is the feature, but note it beside the partition; and the raw-pointer `boundMaterial` cache
with its skinned-draw invalidation rules now sees interleaved override/default materials —
audit the invalidation sites, add the override case to the comment. Batching property to
*measure*, not trust: N entities sharing one `.gmat` handle share one Ref and merge
(`Renderer3D.cpp:808-814`); assigning a different `.gmat` to one entity splits exactly that
entity out.

### 3.6 Editor material UI rewrite

`SceneHierarchyPanel.cpp` (:615-668 today — 5 global-mutating scalar fields, replaced):
per-slot list showing the override handle's `.gmat` name or `(default: <imported name>)`; drag
a `.gmat` onto a slot via `AcceptAssetDrop(AssetType::Material)` (slot assignment is a
*component edit* → **Phase 2's undo covers it automatically** with zero new code — state this
in `editor.md`); a clear-override button (also a component edit, also free undo). Below, an
inline `.gmat` editor for the selected slot's asset: scalar/bool fields, texture map assignment
by dragging texture assets (stored as paths), an explicit **Save** (writes YAML via
`MaterialSerializer`, editor-only gate) and **Revert** (= `Reload(handle)`). `.gmat` field
edits are live-preview on the shared Ref (visible in every scene/entity using it — say so in
the UI header text) and are **not undoable** (Decision 2); Save/Revert is the asset-level
transaction model. `.gmat` drops join the viewport's `AcceptAssetDrop` **initializer_list
overload** (`AssetDragDrop.h:27-33` — the double-call form is a documented-broken pattern).

### Phase 3 risks

- Sidecar generation runs inside asset loading — an error there must degrade to a warning,
  never fail the mesh load.
- The transparent-flag repartition and `boundMaterial` cache are the two places an override can
  corrupt *other* entities' draws — both get explicit probes.
- Idempotence bugs here destroy user work (overwriting an authored `.gmat`); the never-overwrite
  check is the one line to review hardest.

### Phase 3 verification

Probes in `EditorLayer::OnAttach`/`OnUpdate` + bgfx stats, removed afterwards.

| Check | Evidence |
|---|---|
| `.gmat` roundtrip | Write→read→write byte-identical; hand-edit a value → `Reload` → probe reads the new value; malformed `.gmat` → warning + null, no crash |
| Sidecar idempotence | Import a 2-material `.gltf` → 2 sidecars + registry entries (1 batched write). Delete `.meshcache`, reload → sidecars untouched (mtime unchanged). Hand-edit a sidecar, re-import → **edit survives** |
| Embedded extraction | `.glb` (embedded textures) → `_textures/` files exist once, second load extracts nothing (mtimes), `.gmat` references the files, entity renders identically to pre-change (screenshot compare) |
| Override renders | Two entities, one mesh: A default, B overridden albedo → visibly different, picking works on both, existing whole-mesh entities pixel-unchanged |
| Batching measured | Baseline: 4 entities, shared mesh, no overrides → record instanced-draw count. Same 4 sharing one `.gmat` → **same count** (shared Ref merges). One entity on a second `.gmat` → exactly +1 draw |
| Transparent override | Override with `Transparent: true` → submesh moves to the transparent pass and stops casting (cascade draw counts drop by its count); revert → counts restore |
| `boundMaterial` audit | Scene mixing default/override/skinned draws renders correctly across 100 frames (the stale-bind failure is visual: wrong program/textures) |
| Reload propagation | Edit `.gmat` on disk externally, Reload from content browser → next-frame update on every entity using it; bgfx `numTextures` stable across 5 reloads (textures evicted, not leaked) |
| Undo interplay | Slot assign → Ctrl+Z → slot cleared; Ctrl+Y → restored (the free-coverage claim, measured) |
| Runtime posture | GanymedRuntime boots a scene with overrides: renders correctly, writes **nothing** into `assets/` (dir mtime sweep) |

### Phase 3 execution notes

Executed 2026-08-26. x64 Debug, MSBuild; engine, editor, runtime and Sandbox build clean with no new
warnings. **New files (`GanymedE/Assets/MaterialSerializer.h/.cpp`), so `premake5 vs2022` was
re-run.** Verification ran from two temporary probes - a scripted `Phase3Probe` block and a
frame-stepped render probe reading `Renderer3D::GetStats()` after real frames. **31/31 checks pass**;
probes removed.

**The plan's "no callers" claim about the per-submesh `SubmitMesh` overload was wrong, and the seam
landed somewhere better.** That overload has always had a caller: `SubmitMesh(mesh, transform,
entityID)` loops submeshes through it. More importantly, driving it from `RenderSystem` as the plan
sketched **cannot support skinned meshes at all** - `SubmitSkinnedMesh` stages its joint palette
through `Renderer3D`-internal storage, so a caller cannot reproduce its loop, and an overridden
rigged character would have silently kept its imported materials. What landed instead: both
`SubmitMesh` and `SubmitSkinnedMesh` take an optional `(const Ref<Material>* overrides, uint32_t
count)` array indexed by `Submesh::MaterialIndex`, and one `ResolveMaterial` helper serves both
paths. `RenderSystem` resolves handles to `Ref`s once per entity into a reused scratch vector. Less
code at the call site, no duplicated submesh loop, and the skinned path is covered.

**Embedded textures needed no encoder, but did need a magic-byte sniff.** glTF stores embedded
images as already-compressed PNG/JPEG bytes, so "extraction" is a byte copy. The plan specified
`.png` for the output; that would have been a lie on disk for half the sample content - `BoxTextured.glb`
embeds PNG and `CesiumMan.glb` embeds JPEG, and both showed up in the generated registry as
`albedo_0.png` and `albedo_0.jpg` respectively once the sniff was in. It matters because the registry
types assets by extension. Anything that is neither PNG nor JPEG warns and is not extracted, leaving
the slot on the mesh default rather than writing a file nothing can read.

**Sidecar generation hangs off `AssetManager::LoadMesh`, not `MeshImporter`.** That is the one point
both the cold import and the `MeshCache` replay converge on. Putting it in `MeshImporter::Load` would
have meant a cached mesh never regenerates its sidecars, so deleting `.meshcache` would be the only
way to recover a deleted `.gmat` - and the whole idempotence design exists so that deleting things is
recoverable.

**Extraction only runs on the write path.** A mesh whose sidecars already exist never re-touches its
texture directory, which makes the idempotence check meaningful rather than merely "the file compare
passed". Verified explicitly: appending a marker to a generated `.gmat`, then `Reload`ing the mesh
(which deletes the `.meshcache` and forces a genuine cold re-import) leaves the file byte-identical
with an unchanged mtime.

**Batching was measured, not trusted.** Four boxes sharing one mesh: 7 draws / 5 instanced with no
overrides, **identical** with all four pointed at one shared `.gmat`, and exactly **+1 draw** when
one entity moves to a second `.gmat`. That is the `Ref`-identity property the cached loader exists to
provide, confirmed end to end rather than argued from the instancing code.

**The transparent-override repartition works and is worth one more warning than the plan gave it.**
Setting `Transparent` on an override moves that submesh into the transparent pass *and* removes it
from the shadow-caster list, because the partition reads `cmd.Material->IsTransparent()` and the
material it reads is now the override. Both directions verified. The consequence for authors is that
a material swap can change a scene's shadows, which is recorded in `rendering.md` beside the
partition.

**Assigning a different mesh clears the entity's overrides.** Slot *i* of one mesh has nothing to do
with slot *i* of another, so carrying overrides across a mesh swap would apply an arbitrary material
to arbitrary geometry. This is the `ScriptComponent::Fields` decision applied to a second component.

**Not done, deliberately: `.gmat` on the viewport drop target.** The plan lists it in 3.6, but names
only the mechanism (the `initializer_list` overload), never a behaviour - there is no obvious meaning
for dropping a material onto empty space, and "assign to whatever is under the cursor" is UI invented
rather than specified. Slot assignment in the inspector is the specified interaction and it works.
Phase 4 adds `.gprefab` to that target, where the behaviour *is* defined; the list overload is still
the right mechanism when it does.

**Runtime posture verified by content hash, not by inspection.** A full runtime boot that loads its
demo scene leaves all 145 files under `GanymedRuntime/assets/` byte-identical - no sidecars, no
extracted textures, no registry write - with zero errors logged. `IsRegistryWritable()` is the single
gate, shared with the registry writer, exactly as Phase 1 set up.

**Generated sidecars are currently untracked files, and whether to commit them is an open decision.**
Importing the editor's sample meshes now produces `assets/models/*.gmat` and
`assets/models/*_textures/`. They are authorable content, so committing them is the defensible
default (that is what makes a hand-edited material travel with the project); the alternative is a
`.gitignore` rule that quietly re-generates defaults per machine and loses every edit on clone. Not
decided here because it is a repository-policy call, not a code one.

**Interactive checks not run:** the visual ones - two entities with visibly different albedo, picking
still working on both, existing scenes pixel-unchanged, a `.gmat` edited externally updating on the
next frame. The measurable halves of each are covered above (draw counts, pass partition, reload
eviction and re-read, byte-identical scene saves), but "it looks right" needs eyes.

---

## Phase 4 — Linked prefabs

**Goal:** an authored subtree becomes a `.gprefab` asset; instances spawn from it, remember it,
and can be explicitly re-applied or reverted; all of it undoable except the asset write itself.

### 4.1 Format + `PrefabSerializer`

New `GanymedEngine/source/GanymedE/Scene/PrefabSerializer.h/.cpp`. A `.gprefab` is the scene
format's entity list under a `Prefab:` root — same blocks, emitted by the now-shared
`SerializeEntity`, read by Phase 1's `DeserializeEntity`, ordered by the same DFS (root first;
a subtree is contiguous by construction). One schema, two containers; the serializer monolith
split earns its keep here.

**Decision: canonical UUIDs in the file — deterministic sequential (1..N in DFS order), not
preserved instance UUIDs.** Create-from-selection and Apply both remap the subtree's entity
UUIDs to 1..N before emitting (whitelist: `IDComponent` + `RelationshipComponent` only — the
`AssetHandle == UUID` trap again; `StaticMesh.Mesh`, `SkyLight.Environment`, `Script.Script`,
`AudioSource.Clip`, and Phase 3's `MaterialOverrides` pass through untouched). Argument: with
preserved UUIDs, applying the same content from two different instances produces two different
files — a lie in the diff; with canonical UUIDs, **identical content ⇒ identical bytes**, Apply
from anywhere is diff-stable, and the file carries no trace of which scene birthed it. This is
the fileID-stability property Unity gets from its own local-ID scheme, reached the cheap way.
Cost: an Apply that only *reorders* children renumbers UUIDs below the moved block — accepted;
sibling order is content (Phase 1's argument).

`AssetType::Prefab` **appended as 8** after `Audio = 7` (ordinal-persisted, append-only);
`.gprefab` in `AssetTypeFromExtension`/`ToString`; a content-browser tint. **Path-resolved, no
`GetAsset<Prefab>`** — the Script/Audio precedent (`AssetManager.h:40-44`): instantiation is a
rare editor action reading a small YAML; a cached parsed form adds a staleness surface (Apply
rewrites the file; a subsequent instantiate must see it) for zero measurable win. The
static_assert message is untouched by Prefab (Phase 3 already rewrote it for Material).

### 4.2 `PrefabInstanceComponent` + instantiation

```cpp
struct PrefabInstanceComponent { AssetHandle Source = InvalidAssetHandle; };  // instance root only
```

Full new-component checklist (`Components.h`, `ComponentList`, both serializer sides, inspector
display — read-only source path + the Apply/Revert buttons live here too). Untracked.

Instantiate = `DeserializeEntity` per block into the scene, then fresh-UUID remap through
Phase 2's `DuplicateEntity` machinery (same whitelist, same visited-set discipline),
`PrefabInstanceComponent{handle}` onto the root. Instantiation never trips the deserializer's
UUID-collision path (fresh UUIDs always), so the latent Parent/Children fixup bug — already
fixed in Phase 1 — is doubly avoided.

**Root Transform ownership, decided symmetrically**: the file stores the root Transform
captured at creation (the spawn default); *instantiate* applies it; thereafter the root
Transform belongs to the scene — **Apply does not write the instance's root Transform into the
file, Revert does not overwrite the instance's root Transform** (the Unity norm: placement is
per-instance). Everything below the root is wholly file-owned on Revert and wholly
instance-owned on Apply.

### 4.3 Editor operations

- **Create Prefab** — entity context menu (`SceneHierarchyPanel.cpp:127-135`, beside Delete):
  serialize the selection's subtree (canonical remap) to a chosen path under `assets/`,
  `ImportAsset`, attach `PrefabInstanceComponent` to the source entity (creation links the
  original — the Unity behavior authors expect).
- **Apply to prefab** — instance-root context menu + inspector button: re-serialize the current
  subtree over the file (canonical remap, root Transform per 4.2), registry-flush. **An asset
  write — not undoable** (Decision 2); it is also the milestone's only silently-destructive
  click, so it gets a confirmation modal naming the file. Other open instances of the same
  prefab do **not** update (no auto-propagation — Decision 3; say so in the modal text).
- **Revert instance** — recursive delete of the subtree below the root (Phase 2's recursive
  delete, reused), re-instantiate from file under the preserved root entity/UUID/Transform.
  Implemented as **one composite `EditorCommand`** (delete-snapshot + instantiate record) so
  Ctrl+Z restores the pre-revert state — revert is a scene op and *is* undoable, unlike Apply.
- **Instantiate** — drag `.gprefab` onto the viewport via the `AcceptAssetDrop`
  initializer_list overload (`AssetDragDrop.h:27-33`; Scene/StaticMesh/Prefab now share the
  target — the list overload is mandatory, the double-call form is documented-broken); also the
  blank-space hierarchy context menu (:63-71). Undoable (subtree-delete inverse). Spawns at the
  file's root Transform, matching the mesh-drop behavior (`MeshImporter::Instantiate`
  analogue).
- Structural freedom inside instances is **allowed and unmarked**: add/remove/reparent children
  of an instance at will — Apply captures whatever the subtree is now, Revert discards it. No
  divergence tracking (that is the per-field-override engine, "Not doing"). Selection is
  single-entity today; Create-from-selection therefore means "selected entity's subtree" — no
  multi-select semantics invented.

### Phase 4 risks

- The remap whitelist is the phase's data-corruption hazard; the verification table
  byte-compares AssetHandles across an instantiate to prove the whitelist held.
- Composite Revert is the first command containing commands — the round-trip harness must
  include it.
- Apply's confirmation modal is the only guard on the only irreversible click; interactive
  check required.

### Phase 4 verification

| Check | Evidence |
|---|---|
| Canonical determinism | Create prefab from instance A → file F1; instantiate F1, Apply from the new instance unchanged → F2; **F1 ≡ F2 byte-identical** |
| Whitelist held | Probe: subtree with StaticMesh (+MaterialOverrides), SkyLight, Script, AudioSource → instantiate → every AssetHandle field byte-equal to the source's; every entity UUID fresh; Relationship UUIDs internally consistent (walk + assert) |
| Double instantiate | Two instances of one prefab in one scene → zero UUID collisions, independent transforms, scene save/load roundtrips both with sources intact |
| Revert semantics | Move root + mutate a child + add an entity → Revert → child restored from file, added entity gone, **root Transform unchanged**; Ctrl+Z → pre-revert state back (save-diff against a pre-revert save, byte-identical) |
| Apply semantics | Mutate child, Apply → file diff shows exactly the child change (canonical IDs stable); a second pre-existing instance in the scene is *unchanged* (no auto-propagation, measured) |
| Undo round-trip extended | Phase 2's harness sequence extended with instantiate/revert/recursive-delete ops → undo-all/redo-all byte-identity still holds |
| Missing-source resilience | Delete the `.gprefab`, load the scene → instance loads as plain entities + one warning; Revert on it → error log, no crash, subtree untouched |
| Drag-drop matrix | Interactive: `.gprefab`/`.glb`/`.ganymede` each dropped on the viewport route correctly (three types through the list overload); negatives ignored |
| Runtime | GanymedRuntime loads a scene containing instances: renders/plays correctly, `PrefabInstanceComponent` inert (no editor ops, no writes) |

### Phase 4 execution notes

Executed 2026-08-26. x64 Debug, MSBuild; engine, editor, runtime and Sandbox build clean with no new
warnings. **New files (`Scene/PrefabSerializer.h/.cpp`, `Scene/SceneYaml.h`), so `premake5 vs2022`
was re-run.** Verification ran from a temporary `Phase4Probe` block. **31/31 checks pass**; probe
removed.

**Instantiation needed no new remap machinery, because Phase 1 already built it.** The plan said to
`DeserializeEntity` each block and then remap through Phase 2's `DuplicateEntity` machinery. What
actually works is simpler: mint a fresh UUID per block, pass it to `DeserializeEntity`, then call
`SceneSerializer::ResolveHierarchy` - the exact pass a scene load runs, doing the exact job needed
(translate the file's `Parent`/`Children` to the UUIDs the entities were really created with). This
is the direct payoff of Phase 1's decision to make that pass uniform rather than a rare repair
path: a path only taken on unusual input would not have been trustworthy here.

**Canonical renumbering happens in a scratch `Scene`, not in place.** Renumbering the live scene,
serializing, and putting the UUIDs back would avoid an allocation. It would also mean that any
throw or early return in between leaves the *real* scene carrying UUIDs 1..N - unrecoverable
corruption, in exchange for one editor click's worth of work. The scratch copy also gets the
nesting rule for free: `PrefabInstanceComponent` is stripped from the copy, so a subtree containing
an instance saves as plain entities rather than smuggling a nested prefab into a v1 file.

**A shared YAML dialect header was extracted.** The `convert<glm::vec3>` / `convert<glm::vec4>`
specializations were file-local to `SceneSerializer.cpp`, so `PrefabSerializer` could not read a
transform. They now live in `Scene/SceneYaml.h` with the emitter operators. This is a small Phase 1
refactor, done for the right reason: the two serializers write the same blocks, so the encoding of a
vec3 should have one definition rather than two that can silently drift.

**Canonical determinism, measured across scenes rather than argued.** Two different scenes whose
subtrees hold identical content - different entity UUIDs, same components - write **byte-identical**
files; instantiating one and applying it back unchanged reproduces the file byte for byte. The DFS
order in the file was checked by name, not just by count: `1=PrefabRoot 2=ChildA 3=Grandchild
4=ChildB`, which is the contiguous-subtree property the canonical order exists for.

**The remap whitelist was tested with something to get wrong.** The probe's subject subtree carries
`StaticMesh.Mesh`, `MaterialOverrides` (including a deliberately unset slot), `SkyLight.Environment`,
`Script.Script` and `AudioSource.Clip`. All came through byte-equal across a save and an
instantiate, every entity UUID came out fresh with none of them a canonical 1..4, and the hierarchy
walked clean with exactly one root.

**A probe assertion was wrong before the code was.** The first run failed "UUIDs are 1..N in DFS
order" - the check searched for `"- Entity: 1\n"` while the file, like every other serializer output
here, is CRLF from a text-mode `ofstream`. The file was correct; the test was not. Worth recording
because it is the second time this milestone that a red result came from the harness rather than the
feature, and both times the cheap move was to look at the artifact before touching the code.

**A portability break I introduced and caught before it shipped.** The "is this path inside
`assets/`" guard in `CreatePrefabFrom` was written as `relativePath.native().rfind(L"..", 0)`.
`std::filesystem::path::native()` is `wstring` on Windows and `string` on POSIX, so the wide literal
would simply not have compiled on the Linux and macOS targets - which this project has makefiles for
and which nothing in a Windows build would have caught. It is `generic_string()` now.

**Apply is deferred to a modal outside the popup that requests it.** ImGui cannot open a modal from
inside the context-menu popup that triggers it, so the request is recorded as a UUID and the modal
is drawn at the end of the panel's frame. The modal names the file, says the write cannot be undone,
and says that other instances will not follow - that last line is the one that stops "Apply" from
reading like "propagate", which is what every author will assume it means.

**Revert restores the old subtree if instantiation fails**, rather than leaving a hole where the
instance was. Cheap, and the alternative failure mode - a missing or corrupt file silently deleting
an instance - is exactly the kind of thing that makes people distrust an editor.

**Runtime posture verified two ways.** A boot against a hand-written scene carrying a
`PrefabInstanceComponent` loads it, runs, and leaves every file under `GanymedRuntime/assets/`
byte-identical (the only logged errors are "no primary camera", which that throwaway test scene
genuinely lacks). The component has no system and nothing reads it outside the editor, so "inert" is
structural rather than a claim.

**Interactive checks not run:** the drag-drop matrix (`.gprefab` / `.glb` / `.ganymede` each landing
correctly on the one viewport target, and negatives ignored) and the Apply modal's actual buttons.
The list-overload mechanism they depend on is the one already in use for the two existing types, and
the probe covers everything the drop handler calls once it fires.

---

## Phase 5 — Integration polish + docs sweep

**Goal:** the four pillars work *through* each other, the last cheap UX win lands, and the docs
match the tree.

- **Cross-feature scripted sequence** (the milestone exit proof): one probe run — instantiate a
  prefab, override a material slot on a child, gizmo-move the root, duplicate the instance,
  delete one, undo to the start, save → byte-identical to the pre-sequence save; redo to the
  end, save → byte-identical to the post-sequence save. Every phase's machinery in one
  transcript.
- **Dirty-scene indicator** — cheap because Phase 2 built `MarkSaved()`/`IsDirtySinceSave()`:
  an asterisk in the title bar / viewport tab, cleared on save, set by stack position ≠ saved
  mark (correctly clears when you undo *back to* the saved state — the property ad-hoc dirty
  flags always get wrong). Caveat to record: `.gmat` edits and Apply don't touch the scene
  stack and don't set it — correct, they are asset dirt, and the `.gmat` editor's explicit Save
  button is their indicator.
- **Docs sweep** (each item belongs to the phase that caused it; this is the audit):
  `docs/editor/editor.md` heavy rewrite — undo model + shortcut table + the commit-boundary
  rule for inspector lambdas, prefab workflows, the material slot UI, recursive delete;
  `docs/engine/scene.md` — canonical save order, `DeserializeEntity`, `DuplicateEntity`,
  `PrefabInstanceComponent`, the orphan-MarkChanged fix; `docs/engine/assets.md` —
  `.gmat`/`.gprefab` formats, sidecar + extraction rules, `GetAsset<Material>`, registry flush
  points + the `IsRegistryWritable` posture, the no-reverse-index argument;
  `docs/engine/rendering.md` — the per-submesh override seam, transparent-override
  repartition, batching-by-Ref note; `docs/engine/ecs.md` only if component/system surfaces
  moved; this file gains execution notes per phase, per house rule.

### Phase 5 verification

| Check | Evidence |
|---|---|
| Cross-feature round-trip | The scripted sequence above: both byte-identity comparisons pass |
| Dirty indicator | Edit → asterisk; Ctrl+Z back to saved mark → clears (the stack-position property, interactively); save → clears; `.gmat` edit alone → does **not** set it |
| Doc audit | Every claim in the touched docs spot-checked against code; stale-claims grep for the rewritten sections |
| Full regression | Editor smoke (open/edit/play/stop/save), Sandbox boot, GanymedRuntime demo scene boot — all clean logs |

---

## Explicitly not doing (v1)

Named so nobody half-builds them in passing:

- **Multi-select** — every operation is single-entity (`m_SelectionContext` is single today;
  undo/prefab semantics for selections is its own design).
- **Nested prefabs, per-field prefab overrides, auto-propagation on Apply** — the
  serialization-diff engine underneath all three is a milestone, not a feature.
- **Undo for asset edits** — `.gmat` values, registry, prefab Apply (Decision 2; Apply gets a
  confirmation modal instead).
- **Material graphs / shader variants** — every `.gmat` binds `MeshShader::Get()`; the shader
  field is implicit until variants exist.
- **Texture-map editing beyond slot assignment** — no tiling/offset/UV-transform/sampler UI;
  assign-by-drag only.
- **Asset dependency index** (material→mesh, prefab→asset reverse maps) — Phase 3's shape was
  chosen specifically so v1 doesn't need one; build it when something does.
- **Thumbnails / content-browser previews** for `.gmat`/`.gprefab`.
- **Undo history UI / persistent undo across sessions / command coalescing keys** — a linear
  capped stack with labels; the label surface makes a history panel a cheap follow-up.
- **Off-mouse picking readback** — the `RequestEntityID`/`PollEntityID` cleared-pixels failure
  from the animation milestone stays flagged debt, re-flagged here; hover picking works and
  nothing in this milestone needs the off-mouse path.
- **Embedded-texture content-hash de-dup** — extraction is per-mesh-directory; two `.glb`s
  embedding the same bytes extract twice.

## Design tensions, recorded

1. **Hygiene-first vs risk-first phase order** → hygiene first. The byte-identity instrument
   must exist before the phase that needs it most; the risk-first precedent applied to a
   *blocking* unknown, which undo is not.
2. **In-memory snapshots vs YAML snapshots for undo** → in-memory tuple over `ComponentList`.
   Lossless (unserialized fields, Fields-absence semantics), compile-time complete; the cost is
   a second component-enumeration consumer, mitigated because `ForEachType` makes it
   enumeration-driven, not hand-listed.
3. **Shortcut routing: ImGui-side polling vs relaxing `BlockEvents`** → ImGui-side. Relaxing
   the policy trades one dead zone for every-key leakage into camera/gizmo handling while
   typing.
4. **`bool(T&)` lambda contract vs value-diffing for commit detection** → contract change.
   Diffing needs 16 `operator==`s or serialize-compare, and still trips on the Transform
   deg/rad round-trip; "the widget said so" is the ground truth ImGui already computes.
5. **Editor delete: orphan (today) vs recursive (norm)** → recursive in the editor, orphan
   preserved as engine API. Undo removes the safety argument for orphaning; Phase 4's Revert
   needs recursive anyway.
6. **Mesh materials: additive `.gmat` layer vs meshes referencing `.gmat` directly (Unreal
   norm)** → additive. Direct reference requires cache dependency tracking and a reverse index;
   additive requires neither and leaves existing scenes byte-identical. Cost: the imported
   default is duplicated into the sidecar once.
7. **`.gmat` textures by path vs handle** → path. Asset files must be self-describing (the
   shipped-registry lesson); handles stay the scene↔registry currency.
8. **Prefab UUIDs on Apply: canonical-sequential vs preserved** → canonical. Identical content
   ⇒ identical bytes; Apply is diff-stable from any instance. Cost: child reorder renumbers the
   tail of the file.
9. **Scene save order: DFS vs flat UUID sort** → DFS. One canonical order shared with
   `.gprefab`, human-readable, sibling-order-as-content; cost: reparents move blocks in diffs.
10. **RmlUi fix: skip-by-prefix vs debugger Shutdown/Initialise bracket** → skip-by-prefix. One
    code path in Debug and Release, null deref unreachable, debugger state survives; cost: a
    dependency on RmlUi's own internal id prefix, commented with the verified version.
11. **Apply's irreversibility** → not undoable by decision, so it is the one operation behind a
    confirmation modal — scope honesty surfaced as UX.
