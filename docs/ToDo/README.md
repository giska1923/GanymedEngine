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
| [rendering.md](rendering.md) | bgfx Phase 7: Vulkan untested, per-backend checks | 1 + matrix |
| [reflection.md](reflection.md) | Apply-to-prefab, template cache, multi-entity editing gaps | 4 |
| [assets.md](assets.md) | Dependency hashing, parse backpressure, naming and cleanup chores | 6 |
| [cross-cutting.md](cross-cutting.md) | Platform coverage, verification gaps | 2 |
| [editor-visual-parity.md](editor-visual-parity.md) | Editor look-and-feel milestone: theme tokens, fonts/icons, panel furniture, status bar, custom window chrome | 8 phases remaining (0–1 done) |

**Priority, as a recommendation rather than a schedule:** [cross-cutting.md](cross-cutting.md)'s
platform coverage is now the interesting one — D3D11, D3D12 and **OpenGL 3.3** all render correctly
on Windows, and a Linux build defaults to the GL backend, so the thing that used to block it is
gone. Otherwise the [assets.md](assets.md) entries are one-liners worth batching.

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
