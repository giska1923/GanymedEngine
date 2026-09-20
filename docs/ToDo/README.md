# ToDo

Work that is **not done yet**: milestone plans, known bugs, deferred follow-ups, and gaps in
verification coverage. Everything here is open by definition — if an item is finished it does not
belong in this folder.

## Where things live

| Folder | Holds | Tense |
|---|---|---|
| `docs/ToDo/` | What is planned, broken, or deferred | future |
| `docs/engine/`, `docs/editor/`, `docs/runtime/` | What the code does **now** | present |
| `docs/history/` | Why it got that way — completed milestone records, with the rationale and the verification evidence | past |

## The lifecycle of an item

1. **Plan it here.** A milestone plan, a bug, or a one-line follow-up — whichever the work is.
2. **Build it**, updating the matching `docs/engine`/`editor`/`runtime` doc *in the same change*.
   A code change with no doc update is incomplete work, not a follow-up.
3. **Close it.** Delete the item from this folder. If it was a milestone with execution notes and
   verification evidence worth keeping, move that record to `docs/history/` and link it from
   [`docs/README.md`](../README.md). If it was a small fix, the live doc update *is* the record —
   do not manufacture history for it.

The rule that makes this work: **an item leaves this folder only when the thing is actually done and
documented.** A file here is a promise, not a description.

## Open items

| Document | Covers | Items |
|---|---|---|
| [PROVING_GROUND.md](PROVING_GROUND.md) | **Milestone plan** — the test game, and the engine work that must land first | Phase 0 + P1-P7 |
| [SKELETAL_ATTACHMENTS.md](SKELETAL_ATTACHMENTS.md) | **Milestone plan** — attaching entities to joints, so a character can hold something | A2 done; A1, A3 remain |
| [rendering.md](rendering.md) | All four backends render, pick and match on colour; MSAA deferred; dead 2D-era types; offline IBL | 3 |
| [reflection.md](reflection.md) | Per-field override marking on hand-written sections (permanent) | 1 |
| [assets.md](assets.md) | Dependency hashing; mesh-apply file I/O; indivisible texture uploads | 3 |
| [cross-cutting.md](cross-cutting.md) | macOS coverage, WSL-vs-native gaps, Tracy, build residue, Linux system packages, first-frame timestep spike, triplicated Input files, six gaps around character controllers and contacts, a fixed HUD data model, an unchecked collider-vs-mesh gap, and two shipping gaps | 18 |

**The Proving Ground record is split across two branches, and part of it does not exist.** By that
milestone's own branch policy the game lives on `first-game`, so its phase write-ups land there:
this copy of [PROVING_GROUND.md](PROVING_GROUND.md) carries P0.1–P0.4 and nothing else, while
P0.5–P0.7, P1 and P4–P7 are written up on the branch. Two things follow, both worth knowing
before this moves to `docs/history/`:

- **P2 and P3 have no record on either branch.** They were built and gated; the runs were never
  written up, and both sections are still only their plan. Reconstructing that honestly means
  re-running the gates, not copying numbers out of a transcript.
- **The buildings' colliders disagreed with their meshes, and two gates rest on the old geometry.**
  Found on `first-game` and fixed in the scene: the Warehouse had a 0.90 m hole in a wall of a
  building that has no door, and the Blockhouse had three holes, none of them at its one real
  doorway, which was itself walled off. P2's gate ("walk inside and out of every building") has
  therefore never been met for the Warehouse and is open. P4's occlusion probe was authored against
  one of the Blockhouse holes in the belief that it was the doorway; its conclusion stands but its
  probe positions do not, and it wants a re-run on the `+Z` side. Both are written up in
  [PROVING_GROUND.md](PROVING_GROUND.md).
- **Step-up is covered only by accident, and never near its limit.** The P1-era `Step` box was
  deleted and nothing was authored to replace it, but the `GroundTile` pad's 0.19 m collider lip
  turns out to do the job every lap of the autopilot. Nothing in the map sits between 0.2 m and
  `CharacterControllerComponent::StepHeight` (0.4), so where step-up actually stops working has
  never been measured. The Blockhouse doorway's 0.12 m threshold is deliberately left uncollided
  and is not the answer: collide it and the flat-bottomed enemy boxes can no longer follow the
  player through the door. A ledge near 0.4 m, or a ramp, would settle it.
- The merge direction is master → game, so nothing written on the branch ever arrives here. The
  milestone has to be assembled from `first-game` when it is retired.

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
FXAA already ships. Nothing there blocks anything else.

The frame loop is instrumented and the profiler backend was rewritten to make that affordable, so
what is left of that item is only the Tracy question — worth having, blocking nothing. macOS is the
largest genuinely open item.

Threading is **done**: T1–T4 and decision 4 have all landed, so there is no threading file here any
more. Jolt now runs on `Core/JobSystem` — see [physics.md](../engine/physics.md#the-job-system).

## Known-stale entries in `docs/history/`

Those documents are historical and are **not** rewritten when the code moves on, so some of them
describe problems that were fixed later. Verified stale, recorded here so nobody re-opens them:

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
