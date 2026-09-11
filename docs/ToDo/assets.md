# ToDo — Asset pipeline

All six phases and five follow-ups of
[`ASSET_PIPELINE_ROADMAP.md`](../history/ASSET_PIPELINE_ROADMAP.md) are done. See
[assets.md](../engine/assets.md) for the pipeline as it stands.

What remains is hygiene. The one-liner batch is **done** — the `IsAssetsWritable` rename, orphaned
`.meta` detection plus an explicit clean action, deleting the abandoned `assets/.assets/` trees, and
a one-off re-save of all seven committed fixtures (which fixed the duplicate entity UUIDs in
`3DExample` and `Example`, so `git diff` is a valid byte-identity check against them again).

The two entries below are what is left, and neither is a one-liner: both are design work with a
measurement in front of them.

---

## `HashDependency` is size+mtime, not content

A deliberate Phase 4 asymmetry — it is checked on every load of every dependent, so content-hashing
it meant a full read per dependency. The consequence is that **touching** a texture recompiles every
mesh that references it, even when the bytes are identical.

The tradeoff still holds on its own terms; what changed is that Phase 6's hot reload made the case
reachable during ordinary editing rather than only on a branch switch. Worth revisiting with the
cost measured rather than assumed.

## Parse results in flight are unbounded

Every accepted change queues a parse that holds its compiled bytes until Apply. The Apply budget
(4 ms/frame) bounds how fast they drain but not how many can accumulate, so a cold open or a large
branch switch can queue far more decoded image data than the budget will retire in reasonable time.

Phase 6's measured worst case was 23 assets rewritten at once: the watcher itself stayed under
1.5 ms/poll, but one Apply frame hit 110 ms in Debug. Backpressure — a cap on parses in flight, with
the watcher deferring rather than queueing past it — is the fix.

---

## Deferred to other milestones, not to here

- **Packaging `.compiled/` with a shipped runtime** is distribution work, not asset-layer work.
- **The sRGB pipeline** is scoped in [rendering.md](../engine/rendering.md) and is a rendering
  change. Note that Phase 4 dropped the `sRGB` config key rather than reserving it, because there is
  no sRGB handling to configure yet.
