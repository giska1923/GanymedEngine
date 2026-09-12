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
| [rendering.md](rendering.md) | All four backends render, pick and match on colour; MSAA deferred | 1 (parked) |
| [reflection.md](reflection.md) | Apply-to-prefab, template cache, multi-entity editing gaps | 4 |
| [assets.md](assets.md) | Dependency hashing, parse backpressure | 2 |
| [cross-cutting.md](cross-cutting.md) | macOS coverage, WSL-vs-native gaps, frame instrumentation | 3 |

**Priority, as a recommendation rather than a schedule:** **Linux is now built and run** — all
three configurations, editor and runtime, with the status table in
[build-and-tooling.md](../engine/build-and-tooling.md#platform-status). What is left in
[cross-cutting.md](cross-cutting.md) is macOS (never compiled) and the gap between "runs in WSL2"
and "runs on Linux" — chiefly Vulkan, which WSL cannot load.

[assets.md](assets.md)'s one-liner batch is done; what is left there is two design questions, each
wanting a measurement first. [rendering.md](rendering.md) is effectively closed — all four backends
render, pick and agree on colour. Its one remaining entry, **MSAA, is parked on purpose**: the abort
is diagnosed and the fix is one line, but whether MSAA is wanted at all is the open question, and
FXAA already ships. Nothing there blocks anything else.

That leaves frame instrumentation — really the question of whether to adopt Tracy — as the largest
open item, with macOS behind it.

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
