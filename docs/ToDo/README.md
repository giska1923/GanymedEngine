# ToDo

Work that is **not done yet**: milestone plans, known bugs, deferred follow-ups, and gaps in
verification coverage. Everything here is open by definition — if an item is finished it does not
belong in this folder.

## Where things live

| Folder                                          | Holds                                                                                               | Tense   |
| ----------------------------------------------- | --------------------------------------------------------------------------------------------------- | ------- |
| `docs/ToDo/`                                    | What is planned, broken, or deferred                                                                | future  |
| `docs/engine/`, `docs/editor/`, `docs/runtime/` | What the code does **now**                                                                          | present |
| `docs/history/`                                 | Why it got that way — completed milestone records, with the rationale and the verification evidence | past    |

## The lifecycle of an item

1. **Plan it here.** A milestone plan, a bug, or a one-line follow-up — whichever the work is.
2. **Build it**, updating the matching `docs/engine`/`editor`/`runtime` doc _in the same change_.
   A code change with no doc update is incomplete work, not a follow-up.
3. **Close it.** Delete the item from this folder. If it was a milestone with execution notes and
   verification evidence worth keeping, move that record to `docs/history/` and link it from
   [`docs/README.md`](../README.md). If it was a small fix, the live doc update _is_ the record —
   do not manufacture history for it.

The rule that makes this work: **an item leaves this folder only when the thing is actually done and
documented.** A file here is a promise, not a description.

## Open items

| Document                               | Covers                                                                                                                                                                                                                                                                                                                                                                                                                                                                                       | Items           |
| -------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------- |
| [PROVING_GROUND.md](PROVING_GROUND.md) | **Milestone plan** — the test game, and the engine work that must land first                                                                                                                                                                                                                                                                                                                                                                                                                 | Phase 0 + P1-P7 |
| [MAP_EDITOR.md](MAP_EDITOR.md)         | **Milestone plan** — in-editor map authoring: surface raycast, placement + snapping, collider↔mesh parity, scatter brush, markers, top-down view                                                                                                                                                                                                                                                                                                                                             | M0–M6 done      |
| [rendering.md](rendering.md)           | All four backends render, pick and match on colour; MSAA deferred; dead 2D-era types; offline IBL; the frustum's near-plane depth convention                                                                                                                                                                                                                                                                                                                                                 | 4               |
| [reflection.md](reflection.md)         | Per-field override marking on hand-written sections (permanent)                                                                                                                                                                                                                                                                                                                                                                                                                              | 1               |
| [assets.md](assets.md)                 | Dependency hashing; mesh-apply file I/O; indivisible texture uploads                                                                                                                                                                                                                                                                                                                                                                                                                         | 3               |
| [cross-cutting.md](cross-cutting.md)   | macOS coverage, WSL-vs-native gaps, Tracy, build residue, dead files left by the asset-tree merge, first-frame timestep spike, triplicated Input files, six gaps around character controllers and contacts, a fixed HUD data model, two shipping gaps, editor text outside Latin-1, the Play-mode game UI taking the wheel over other panels, `DesktopShell` unrun off Windows, two skeletal leftovers (A3 grip picture, `Visible` bit), and the aim-offset probes that were not watched, and the two-hand IK checks not done by hand | 23              |

**The Proving Ground record is split across two branches, and part of it does not exist.** By that
milestone's own branch policy the game lives on `first-game`, so its phase write-ups land there:
this copy of [PROVING_GROUND.md](PROVING_GROUND.md) carries P0.1–P0.4 and nothing else, while
P0.5–P0.7, P1 and P4–P7 are written up on the branch. Two things follow, both worth knowing
before this moves to `docs/history/`:

- **P2 and P3 have no record on either branch.** They were built and gated; the runs were never
  written up, and both sections are still only their plan. Reconstructing that honestly means
  re-running the gates, not copying numbers out of a transcript. **P2's walk was re-run in M6**
  — see [PROVING_GROUND.md](PROVING_GROUND.md).
- **A box taller than `CharacterControllerComponent::StepHeight` (0.4 m) is still missing.**
  M6's 0.30 m ledge is walked; that measures the working side, not the cliff.
- The merge direction is master → game, so nothing written on the branch ever arrives here. The
  milestone has to be assembled from `first-game` when it is retired.

**[MAP_EDITOR.md](MAP_EDITOR.md) is an editor milestone, so it lives off `master`, not off the
game branch.** Every phase touches `GanymedEditor/source/` or `GanymedEngine/source/`, which the
branch policy above puts on `master`; the game branch receives that work by merge and never
sends anything back. The model editor — the other half of the same authoring problem — is
**done**: [MODEL_EDITOR.md](../history/MODEL_EDITOR.md).

**[MAP_EDITOR.md](MAP_EDITOR.md) executed on `map-editor`, which was based on `master`.** M0–M6
have landed. M6's scene and Lua edits (the step-up ledge, the P2 footprint doors, the Sentry's
`+Z` facing) live on `first-game`. The Warehouse rebuild was not timed in the editor — see that
file.

The map editor exists because the Proving Ground's buildings were hand-assembled from typed-in box
colliders, which is what produced the wall holes above — two of its gates (P2's interiors, P4's
`+Z` occlusion re-run) were the milestone's own closing verification. The model editor closed the
other half: an inspector, import settings on the sidecar, a collision _seed_ (not an override —
and not for hollow building shells), a second `SceneRenderer` for preview, and thumbnails. The
collision default does not zero the parity audit on those buildings; M6 already recorded why.

**Skeletal attachments and joint tooling are done.** Mechanism in
[`SKELETAL_ATTACHMENTS.md`](../history/SKELETAL_ATTACHMENTS.md), editor in
[`SKELETAL_TOOLING.md`](../history/SKELETAL_TOOLING.md) (S1–S6). Two leftovers — the A3 grip
picture on `first-game` and a general `Visible` bit — are in
[cross-cutting.md](cross-cutting.md#skeletal-leftovers-after-the-attachment-and-tooling-close).

**Aim offset is done.** The pass, the inspector preview and the viewport handle are on `master`;
the Proving Ground wiring is on `first-game`. The record is
[`AIM_OFFSET.md`](../history/AIM_OFFSET.md). The probes that were not watched, the untuned spine
weights, and the aim-idle the milestone does not supply are in
[cross-cutting.md](cross-cutting.md#aim-offset-leftovers).

**Two-hand IK is done.** The solver, the component and its pass, the inspector and overlay, and the
aim lock are on `master`. The Proving Ground weapon pose is on `first-game`. The record is
[`TWO_HAND_IK.md`](../history/TWO_HAND_IK.md), assembled from `first-game`'s copy because only that
copy carries H5. What was not done by hand, the idle's left-hand clamps (an aim idle fixes them),
and the unmeasured barrel-versus-chest count are in
[cross-cutting.md](cross-cutting.md#two-hand-ik-leftovers).

**Runtime prefab spawning is done** — `Scene.Spawn` and `Entity:Destroy` in Lua, `Prefab` as a
managed asset, physics bodies reconciled per frame, and a spawn cap. The record, including the three
things the plan got wrong, is in
[`docs/history/RUNTIME_PREFAB_SPAWNING.md`](../history/RUNTIME_PREFAB_SPAWNING.md). It also fixed a
latent bug that was never about spawning: `PhysicsScene::CreateBodies` ran only from `Start()`, so
**any** entity that gained a rigid body during play never simulated.

**Priority among the remaining fixes, as a recommendation rather than a schedule:** **Linux is now built and run** — all
three configurations, editor and runtime, with the status table in
[build-and-tooling.md](../engine/build-and-tooling.md#platform-status). What is left in
[cross-cutting.md](cross-cutting.md) is macOS (never compiled) and the gap between "runs in WSL2"
and "runs on Linux" — chiefly native Vulkan. WSL cannot load it; a native Intel UHD box
brought it up and then hung in the IBL bake (`VK_ERROR_DEVICE_LOST`). The bake is split on
Intel+Vulkan now; that still needs a re-run on the machine that hung.

[assets.md](assets.md)'s entries were **measured**, and one of them is now **done**: parse
backpressure was real and was a memory bound — 24 meshes loaded at once held 127 MB of decoded data
waiting for Apply, 18x what the finished assets retain — and an in-flight cap of 8 roughly halves
the peak at no cost in drain time. What remains is dependency hashing (smaller than written up; the
answer is a two-level check rather than content hashing) and a third problem the backpressure entry
was hiding: a 24 ms Apply frame in Release is the budget's inability to subdivide one apply, which
backpressure does not fix and did not. [rendering.md](rendering.md) is effectively closed — all four backends
render, pick and agree on colour. Its one remaining entry, **MSAA, is parked on purpose**: the abort
is diagnosed and the fix is one line, but whether MSAA is wanted at all is the open question, and
FXAA already ships. Nothing there blocks anything else. One entry was added while planning the map
editor: the frustum's near plane is extracted with the OpenGL depth convention under a
`GLM_FORCE_DEPTH_ZERO_TO_ONE` build. It is conservative, so nothing renders wrong — it is one line,
and it matters before anyone tunes culling numbers.

The frame loop is instrumented and the profiler backend was rewritten to make that affordable, so
what is left of that item is only the Tracy question — worth having, blocking nothing. macOS is the
largest genuinely open item.

Threading is **done**: T1–T4 and decision 4 have all landed, so there is no threading file here any
more. Jolt now runs on `Core/JobSystem` — see [physics.md](../engine/physics.md#the-job-system).

## Known-stale entries in `docs/history/`

Those documents are historical and are **not** rewritten when the code moves on, so some of them
describe problems that were fixed later. Verified stale, recorded here so nobody re-opens them:

- Every mention of `GanymedEditor/assets/`, `GanymedRuntime/assets/`, "each app's own `assets/`", or
  running an app from its project folder: the workspace now has one `assets/` tree at the root and
  both apps run from the repository root
  ([build-and-tooling.md](../engine/build-and-tooling.md#assets)).
- Every mention of `scripts/build_shader_tools.*`, `scripts/compile_shaders.*`,
  `Win_GenerateProjects.bat` or the other per-OS scripts (`BGFX_MIGRATION.md`,
  `RUNTIME_AUDIO_ROADMAP.md`, `PARTICLE_ROADMAP.md`, `ANIMATION_ROADMAP.md`,
  `editor-visual-parity.md`, `Scripting-And-UI-Integration.md`): those eleven scripts were replaced
  by `scripts/setup.py`, whose `shadertools`, `shaders` and `generate` steps do the same jobs
  ([build-and-tooling.md](../engine/build-and-tooling.md#workspace)).
- `BGFX_MIGRATION.md` §8.8 "Shadows are inert" — shadows work: 4 cascades, skinned casters
  ([rendering.md](../engine/rendering.md)).
- `REFLECTION_ROADMAP.md` R2 note "`Attr::Section` is unread by the generic path" — it is read, at
  `EditorInspector.cpp`'s section grouping.
- `ASSET_PIPELINE_ROADMAP.md` Phase 3 note "a canonical entity order would fix it" — canonical order
  landed, and the per-run UUID churn it left is gone too: the duplicate handles in `3DExample` and
  `Example` were re-minted by a one-off re-save of all seven fixtures, verified idempotent (a second
  load+save pass remaps nothing and produces byte-identical files).
- `ASSET_PIPELINE_ROADMAP.md` Phase 1 note "orphaned `.meta` cleanup is cheap insurance, not built" —
  built: detected at every scan, reaped only by an explicit editor action
  ([assets.md](../engine/assets.md#orphaned-sidecars)).
- `ASSET_PIPELINE_ROADMAP.md` Phase 1 note on `IsRegistryWritable` being misnamed — renamed to
  `IsAssetsWritable`.
- `THREADING_ROADMAP.md` "T4 remains open" — history inside T3's notes; T4 is done.
- `MODEL_EDITOR.md` "socket authoring is [SKELETAL_ATTACHMENTS.md](SKELETAL_ATTACHMENTS.md)'s territory" — that file now lives at [`docs/history/SKELETAL_ATTACHMENTS.md`](../history/SKELETAL_ATTACHMENTS.md). S5 of [SKELETAL_TOOLING.md](../history/SKELETAL_TOOLING.md) is the clip readout on the Asset Inspector; sockets stay instance-side.
- `RUNTIME_AUDIO_ROADMAP.md` Phase 2 note "every solution build prints `'pwsh.exe' is not recognized`
  from `vendor/premake/premake5.lua`'s postbuild step" — the source was vcpkg's user-wide MSBuild
  integration (its app-local DLL step runs `pwsh.exe`), not premake. Fixed by opting the workspace
  out ([build-and-tooling.md](../engine/build-and-tooling.md#workspace)).
- `AIM_OFFSET.md` A5 note "a `Muzzle` parented to `Yaw` is invisible" — written from `master`'s
  cross-cutting, which still had that parenting. On `first-game`, `Muzzle` is a child of `Rifle`,
  which is a child of `Body`, so the handle's descendant walk does find it. The history file is
  not rewritten; the open note is
  [cross-cutting.md](cross-cutting.md#aim-offset-leftovers).
- `AIM_OFFSET.md` A1 "by its normalised share of yaw then pitch" and the Debug boot timing loop — the
  pass now splits one whole-chain axis-angle by weight (the per-joint split missed the target by up
  to 26° across three joints), and the 2000-iteration timing loop is gone from the boot self-test
  (it cost ~0.2 s on every Debug start). `ComponentEditCommand`'s aim-offset special case is now the
  generic `Trait::Runtime` rule. See [scene.md](../engine/scene.md) and
  [editor.md](../editor/editor.md).
- `AIM_OFFSET.md`, `SKELETAL_ATTACHMENTS.md` and `SKELETAL_TOOLING.md`: "the rifle is socketed to
  `RightHand`". On `first-game` it has been socketed to `Spine` since two-hand IK's H5, and it is
  turned onto the aim with both hands solved onto it. The hand-socket numbers in those records
  describe the Proving Ground before that. See [`TWO_HAND_IK.md`](../history/TWO_HAND_IK.md).
