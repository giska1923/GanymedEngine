# ToDo — Asset pipeline

All six phases and five follow-ups of
[`ASSET_PIPELINE_ROADMAP.md`](../history/ASSET_PIPELINE_ROADMAP.md) are done. See
[assets.md](../engine/assets.md) for the pipeline as it stands.

What remains is hygiene. Several of these are one-liners and are worth batching into a single pass.

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

## `IsRegistryWritable()` is misnamed

It gates **all** writes into `assets/`, not writes to a registry. Flagged in Phase 1, still true.
The rename was skipped twice because it touched the then-live roadmap doc; that objection is gone now
that the record is in [`docs/history/`](../history/ASSET_PIPELINE_ROADMAP.md) and is not rewritten.

## No orphaned `.meta` cleanup

The editor's file operations are the likely source of orphaned sidecars, and nothing reaps them. A
"clean orphaned `.meta`" maintenance action was called cheap insurance in Phase 1 and never built.

Related and even smaller: the abandoned `assets/.assets/` trees from the pre-`.compiled` layout are
still on disk and can simply be deleted.

## Per-run UUID churn in `3DExample` and `Example`

Those two scenes carry duplicate or zero entity UUIDs, so `SceneSerializer` remaps them on every
load and the entity order in the file changes between runs. Real diff noise for committed scenes,
and it predates the whole asset milestone.

Hit again during R5's byte-identity verification, where it was the only reason two of seven fixtures
did not compare byte-identical. Fixing the two scenes once is enough — the serializer's canonical
order is already correct.

## The committed scene and prefab fixtures are stale

**Both** the old and the new serializer reformat all seven committed `.ganymede`/`.gprefab` files,
because the curve emitters changed style after those files were last saved. So `git diff` is not a
valid byte-identity test against them, which is why R5's verification diffed two *runs* instead.

A one-off re-save commit fixes it and makes `git diff` a meaningful check again.

---

## Deferred to other milestones, not to here

- **Packaging `.compiled/` with a shipped runtime** is distribution work, not asset-layer work.
- **The sRGB pipeline** is scoped in [rendering.md](../engine/rendering.md) and is a rendering
  change. Note that Phase 4 dropped the `sRGB` config key rather than reserving it, because there is
  no sRGB handling to configure yet.
