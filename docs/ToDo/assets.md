# ToDo — Asset pipeline

All six phases and five follow-ups of
[`ASSET_PIPELINE_ROADMAP.md`](../history/ASSET_PIPELINE_ROADMAP.md) are done. See
[assets.md](../engine/assets.md) for the pipeline as it stands.

What remains is hygiene. The one-liner batch is **done** — the `IsAssetsWritable` rename, orphaned
`.meta` detection plus an explicit clean action, deleting the abandoned `assets/.assets/` trees, and
a one-off re-save of all seven committed fixtures (which fixed the duplicate entity UUIDs in
`3DExample` and `Example`, so `git diff` is a valid byte-identity check against them again).

**Both have now been measured**, which was the thing standing in front of them. The numbers changed
the shape of both: one is smaller than it looked, the other is real but is a *memory* bound rather
than the frame bound it was written up as. Details below; neither is implemented.

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

## Parse results in flight are unbounded — confirmed, and it is a memory bound

**This one is real and the numbers are worse than the write-up suggested.** 24 meshes requested in a
single frame, x64 Release:

| | |
|---|---|
| Peak parses in flight | **24** — i.e. all of them; nothing throttles the queue |
| Working set before | 196 MB |
| Working set at peak | **330 MB** |
| Working set once drained, all 24 meshes still loaded | 203 MB |

So the 24 loaded meshes *retain* about **7 MB** between them, while the parse results waiting for
Apply held about **127 MB** — roughly **5.3 MB per pending asset, 18x what the finished asset
keeps**. That is decoded image data sitting in memory until the main thread gets to it, and it
scales linearly with how many assets are accepted at once: a 200-asset cold open extrapolates to
~1.1 GB of transient footprint, on top of everything else.

Backpressure — a cap on parses in flight, with the watcher deferring rather than queueing past it —
is the right fix, and the cap can now be chosen from a number rather than guessed.

### The frame-time half of that entry is a *different* bug, and backpressure would not fix it

Worth separating, because the two were recorded together. The same burst:

| | Peak Apply frame (budget 4.00 ms) |
|---|---|
| Cold, compiling | 12.6 ms |
| Warm, 24 cache hits | **30.1 ms** |

Warm is *worse* because parses finish sooner and land together. This is not queue depth — it is
[the Apply budget's documented limitation](../engine/assets.md#the-apply-budget): `HasRoom()` always
lets the first apply of a frame run and an apply cannot be interrupted, so a frame costs 4 ms plus
one whole apply. Capping how many parses are in flight does not make one apply cheaper.

This does not contradict Phase 6's table in [assets.md](../engine/assets.md#the-apply-budget), which
measured a 22-asset **texture reload** at a 5.3 ms worst apply. A skinned mesh apply is a much larger
unit of work — vertex and index buffers plus every texture it pulls — so mesh-heavy bursts are where
the "cannot subdivide one apply" limit actually bites. Fixing it means subdividing a mesh apply
(buffers and textures as separate budgeted steps), which is a bigger change than backpressure and
should be judged separately.

---

## Deferred to other milestones, not to here

- **Packaging `.compiled/` with a shipped runtime** is distribution work, not asset-layer work.
- **The sRGB pipeline** is scoped in [rendering.md](../engine/rendering.md) and is a rendering
  change. Note that Phase 4 dropped the `sRGB` config key rather than reserving it, because there is
  no sRGB handling to configure yet.
