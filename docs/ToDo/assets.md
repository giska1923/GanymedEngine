# ToDo — Asset pipeline

All six phases and five follow-ups of
[`ASSET_PIPELINE_ROADMAP.md`](../history/ASSET_PIPELINE_ROADMAP.md) are done. See
[assets.md](../engine/assets.md) for the pipeline as it stands.

What remains is hygiene. The one-liner batch is **done** — the `IsAssetsWritable` rename, orphaned
`.meta` detection plus an explicit clean action, deleting the abandoned `assets/.assets/` trees, and
a one-off re-save of all seven committed fixtures (which fixed the duplicate entity UUIDs in
`3DExample` and `Example`, so `git diff` is a valid byte-identity check against them again).

**Both were measured**, which was the thing standing in front of them, and the numbers changed the
shape of both. **Parse backpressure is now done** — it was real, it was a *memory* bound rather than
the frame bound it had been written up as, and the cap that fixed it is in
[assets.md](../engine/assets.md#backpressure-the-in-flight-cap), and the Apply budget now declines
work it cannot fit rather than overshooting by a whole apply. What is left is the dependency hashing
question (smaller than it looked, and now a known small change) and the two halves of the mesh apply
that profiling turned up - neither of which the budget can do anything about.

A note on method, because it constrains what the numbers mean: **the committed tree cannot reproduce
either case.** It indexes 8 compilable assets, and `Phase5Test`'s 40 `StaticMeshComponent`s all
reference one handle. The burst figures below come from 24 generated copies of `CesiumMan.glb`
(skinned, with an embedded texture) loaded in a single frame — 24 to match the "23 assets" figure
Phase 6 recorded — measured in **x64 Release**, then deleted.

---

## `HashDependency` is size+mtime, not content — smaller than it looked

The Phase 4 comment argues that "hashing a 4K normal map's bytes to answer *did it change* costs
more than the recompile it is trying to avoid". **Measured, that is true at 4K and false by more
than an order of magnitude at this project's scale.** FNV-1a (`HashBytes`) runs at 830–1030 MB/s:

| Dependency | Size | `stat` (what it does now) | read + content hash |
|---|---|---|---|
| `BoxTextured` albedo | 4.2 KB | 7.4 us | 61 us |
| `CesiumMan` albedo | 153 KB | 7.4 us | 314 us |
| `studio_small_08_1k.hdr` | 1.47 MB | 6.1 us | 2.9 ms |
| synthetic 4K normal map | 32 MB | 50 us | **71.6 ms** |

Against a **measured mesh recompile of 6.5 ms** (24 cold compiles, 156.2 ms total). So content
hashing a real dependency here is ~20x *cheaper* than the recompile it prevents, and hashing a 32 MB
one is ~11x more expensive. Crossover is around 5 MB of dependency.

**But the framing was wrong, and that matters more than the numbers.** The choice is not "cheap hash
always" versus "content hash always". `Open` already uses the right pattern for the *source*: mtime
and size are a cheap pre-filter, and the content hash runs **only when that pre-filter trips**.
Applying the same two-level shape to dependencies costs nothing in the common case:

- nothing changed → one `stat` per dependency, exactly as today (7 us)
- dependency touched, bytes identical → +314 us to confirm, saving a 6.5 ms recompile
- dependency genuinely changed → +314 us on top of a recompile that was going to happen anyway

The 4K case stays safe with a size cap on the confirm step: above some threshold, decline to
content-hash and keep today's behaviour, since that is the regime where the recompile really is
cheaper than the read.

This is now a small, bounded change with a known payoff rather than an open question. What it needs
is a second hash stored per dependency in the epoch record, which is a format change.

## Get the mesh apply's file I/O off the main thread

*(What is left of the old "parse results in flight" entry. Backpressure landed; so did making the
Apply budget decline work it cannot fit — see
[assets.md](../engine/assets.md#the-apply-budget). Neither helps the case below.)*

A **cold** mesh apply is 5.50 ms, and `MaterialSerializer::GenerateSidecars` is 3.66 ms of it —
two thirds, and **none of it GPU work**. It writes a `.gmat`, extracts embedded images to real
files, and registers each result: roughly five small files per material at about 1 ms each, split
evenly between `ExtractEmbedded` (1.16 ms), `Save` (1.32 ms) and two `ImportAsset` calls
(2.20 ms combined). Warm, the same call is 0.13 ms, so this is entirely a first-import cost.

The budget cannot help. One cold apply exceeds the whole 4 ms allowance, so the
always-allow-the-first rule means the frame costs whatever that apply costs — measured at 6–9 ms,
and swinging to 20–26 ms when the disk is uncooperative, since it is ~120 small file writes for a
24-mesh import.

What blocks the obvious fix: `ImportAsset` mutates the asset registry, which is main-thread state,
so `GenerateSidecars` cannot simply move to the parse stage wholesale. The split that would work is
to do the *writing* on the worker (extract the images, emit the `.gmat` — both are pure functions of
the `MeshSource` the parse already holds) and leave only the registration in Apply. That is roughly
2.5 ms of the 3.66 moved off the frame.

Worth doing when first-import hitches start mattering; today they are one gesture per mesh, since a
mesh is imported by drag-drop.

## Texture uploads are the other half, and they are genuinely indivisible

A **warm** mesh apply is 2.43 ms, of which `Texture2D::SetData` is 1.93 ms — one bgfx upload, which
no budget can subdivide. `BuildMesh`'s vertex and index buffers are 0.25 ms, i.e. the part everyone
assumes is expensive is 10% of it.

Embedded maps bypass the texture manager entirely (`TextureImporter::Upload`, no handle, never
pending), so a mesh with several materials uploads all of them inside one indivisible apply. A mesh
with 3 materials x 3 maps would be ~17 ms in one frame, and nothing currently bounds that. Routing
embedded maps through the texture manager would make each one separately budgeted — but they have no
handle by design, so it is a real change rather than a rewiring.

Not scheduled: the project has no such mesh, and inventing one to justify the work would be
backwards. Recorded so the shape of the limit is known before something hits it.

---

## Deferred to other milestones, not to here

- **Packaging `.compiled/` with a shipped runtime** is distribution work, not asset-layer work.
- **The sRGB pipeline** is scoped in [rendering.md](../engine/rendering.md) and is a rendering
  change. Note that Phase 4 dropped the `sRGB` config key rather than reserving it, because there is
  no sRGB handling to configure yet.
