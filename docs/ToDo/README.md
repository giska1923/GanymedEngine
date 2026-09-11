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
| [rendering.md](rendering.md) | bgfx Phase 7: D3D12 renders nothing, OpenGL hangs, Vulkan untested | 3 + optional |
| [reflection.md](reflection.md) | Prefab serialization, apply-to-prefab, multi-entity editing gaps | 6 |
| [assets.md](assets.md) | Dependency hashing, parse backpressure, naming and cleanup chores | 6 |
| [cross-cutting.md](cross-cutting.md) | Platform coverage, verification gaps | 2 |

**Priority, as a recommendation rather than a schedule:** [rendering.md](rendering.md) is where the
concrete bugs are now. §9.2 and §9.3 are done, and running the other backends turned "multi-backend
hardening" into three specific, reproducible failures — **D3D11 is currently the only backend that
renders correctly.** Fixing those is also what unblocks the Linux/macOS validation in
[cross-cutting.md](cross-cutting.md), since a Linux build defaults to the GL backend. The rest is
polish, and several of the [assets.md](assets.md) entries are one-liners worth batching.

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
  landed; only the per-run UUID churn survives, tracked in [assets.md](assets.md).
- `THREADING_ROADMAP.md` "T4 remains open" — history inside T3's notes; T4 is done.
