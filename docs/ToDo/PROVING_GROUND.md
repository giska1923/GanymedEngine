# Milestone — Proving Ground (the test game)

**Status: planned, not started.** This is a roadmap. Nothing here is built.

A small third-person shooter, built to find out what is wrong with the engine. The game is the
instrument, not the goal: every phase below is chosen for the engine surface it puts under load,
and a phase that would be fun but tests nothing already built is cut.

The premise is that this engine has never had a *consumer* that runs for more than a minute. Every
subsystem was verified by a probe written for that change and deleted afterwards, so what is
untested is not any one function but the **interaction** of the pieces over time: spawning while
audio plays while assets stream while physics runs.

---

## What it is

Flat ground. A handful of buildings composed from box colliders, enterable. A capsule player who
walks, looks, and shoots. Enemies who patrol, notice the player by line of sight, and charge.
Projectile weapons that can be picked up. Health, healing spots, and an upgrade station. A HUD.
Sound. It runs in `GanymedRuntime`, not only in the editor.

## What it deliberately is not

- **No cover-seeking AI, no navmesh, no pathfinding.** This was in the original sketch and is cut
  on purpose. It is the largest item by effort and the smallest by engine coverage — it is Lua
  logic sitting on the ECS, and it would teach us nothing about the renderer, the asset pipeline,
  physics or audio. Buildings still matter because they **break line of sight**, so cover
  behaviour emerges from the geometry instead of being authored. If the enemies feel too stupid
  later, cover is a Lua change, not an engine change.
- **No mesh colliders.** Only box, sphere and capsule exist. Buildings are box-composed. That is
  normal practice and cheap, but it constrains how the art is authored, so it is said here rather
  than discovered by whoever models the first building.
- **No hitscan.** See Decision 2.
- **No multiplayer, no save system, no inventory beyond "which weapon".**

---

## Branch policy

The game lives on a branch; engine work does not.

| Where | What | Rule |
|---|---|---|
| `master` | Everything under `GanymedEngine/source/`, `GanymedEditor/source/`, `GanymedRuntime/source/`, premake, scripts | Engine features the game needs are built, verified and merged **here first** |
| `game/proving-ground` | The game's assets, scenes, prefabs and Lua | **No change under `GanymedEngine/source/`**, ever |

Checkable, which is the point:

```
git diff --stat master.. -- GanymedEngine/source GanymedEditor/source GanymedRuntime/source
```

Anything it prints is a rule violation and belongs on master instead. **All three paths, not just
the engine** - the first version of this check named only `GanymedEngine/source` and would have
reported clean while editor source was being changed on the game branch, which is exactly what
happened. The temptation will be real —
"just this one small engine tweak" is how long-lived branches rot — so the check is mechanical
rather than cultural.

**Merge direction is master → game, often.** After every engine change lands on master, merge it
into the game branch rather than batching. Batching means the game is exercising an engine that is
no longer the tip, which defeats the reason for building it.

**On `AssetRegistry.gr`:** a tracked registry churns whenever an asset is added, which would make
it the branch's worst merge conflict — except that P0.1 puts the game in `Game/assets/`, a tree
that does not exist on master. Nothing upstream to conflict with. If the game is ever authored
directly into `GanymedRuntime/assets/` instead, that protection is gone and this becomes the file
to watch, since the runtime's registry is tracked on purpose (a shipped game needs it — see
[assets.md](../engine/assets.md)).

---

## Phase 0 — engine prerequisites, on `master`, before the branch exists

These are known *now*. Doing them first collapses most of the branch-juggling that would otherwise
happen three separate times mid-game.

### P0.1 — Give the engine a concept of a project — **DONE**

Built: `SetAssetRoot`/`GetAssetRoot` in
[`AssetPaths.cpp`](../../GanymedEngine/source/GanymedE/Assets/AssetPaths.cpp) (a new file — project
regeneration required), a root parameter on `AssetManager::Init`, `--project=<path>` on the editor,
and an `AssetRoot` key in `runtime.yaml` with `StartScene`/`UIDocument` made project-relative. See
[assets.md](../engine/assets.md#the-project-root). Verified: the editor opened an out-of-tree
project and scanned 19 assets adopted from sidecars with 0 minted; the default path and the runtime
both boot unchanged, zero errors and zero warnings.

**Two things this plan got wrong, kept visible rather than edited away:**

- **The size.** Decision 4 called this the largest Phase 0 item, most likely to be underestimated,
  and predicted leaks into the `.compiled/` cache location, the watcher's directory and the content
  browser's home. All three **already derived from a single accessor** — `AssetPaths.h` had
  centralised the root before this milestone was written. The change was small.
- **The blocker.** The paragraph below argued the cheap escape fails because `EditorLayer` loads
  its checkerboard and HUD from disk. That is only true of moving the *working directory*, which
  the actual fix does not do. It moves the project root and leaves engine- and editor-owned assets
  resolving against CWD, so the chrome was never at risk.

What it did turn up was a real latent bug: `ContentBrowserPanel.cpp` defined
`extern const std::filesystem::path g_AssetPath = GetAssetRoot();` at namespace scope, a
static-initialisation-time snapshot taken before `main`. It would have frozen the default root
forever. Removed, and the reason recorded in `AssetPaths.cpp` so it is not reintroduced.

*Original text follows.*

`AssetManager::Init(bool writableAssets)` takes no asset root. The root is hard-coded `assets/`
relative to each app's working directory, so the editor can only ever open `GanymedEditor/assets/`
and the runtime only `GanymedRuntime/assets/`. The documented workflow is to author in the first
and copy a snapshot to the second.

For a demo that is fine. For a game iterated on daily it means the content exists **twice on the
branch** and is hand-synced every session.

Changing the working directory almost works, and then does not: `EditorLayer` loads
`assets/textures/Checkerboard.png` and `assets/ui/hud.rml` from disk, so the editor's own chrome
breaks when pointed at a tree that has no editor assets.

**What gets built:** an asset-root parameter on `AssetManager::Init`, plus the editor's own few
disk-loaded assets resolved against the *editor's* install location rather than the project's.
Every other engine has this concept — Unity's project folder, UE's `.uproject` — and this one
skipped it because it only ever had one project.

**Where the game's content then lives: a new top-level `Game/assets/` tree**, opened by both apps
through that parameter. The folder name is arbitrary — it is a parameter, not a constant — but one
named tree that both apps point at is the whole point of the change.

Two consequences worth stating, because they change the branch policy above:

- **`Game/` does not exist on `master` at all.** It is created on the game branch. So merges from
  master can never touch it, and the game's `AssetRegistry.gr` has no counterpart upstream to
  conflict with. This is strictly better than authoring into `GanymedRuntime/assets/`, where the
  game would be overwriting tracked demo content that master still owns.
- **The copy-to-runtime step does not disappear — it moves.** A shipped game's working directory
  is its install folder, so P7 still copies `Game/assets/` next to the runtime executable. What
  P0.1 buys is that the copy stops being a *per-iteration* step and becomes a *packaging* step.
  That is the actual win; it is not "no more copying".

**This is the largest Phase 0 item and the one most likely to be underestimated.** See Decision 4.

### P0.2 — A character that does not fall over — **DONE (the flag; not `CharacterVirtual`)**

Built: `RigidBodyComponent::LockRotation`, applied as Jolt `mAllowedDOFs` with all three
translation bits kept and all three rotation bits dropped. Reflected, so serialization and the
inspector came free. See [physics.md](../engine/physics.md#locked-rotation).

Verified on a capsule (r 0.35, half-height 0.6, mass 70) pushed at 6 m/s over a 0.15 m kerb, after
three seconds: unlocked ended at `rot=(0, 0, -1.141)` — **65 degrees over** — and its centre
dropped to 0.60; locked held `(0, 0, 0)` upright at 0.95. Travel was 3.29 vs 3.28, so the
constraint costs nothing in translation.

Decision 1 stands for now: this is the flag, not `CharacterVirtual`. Step-up, slope limits and
wall-sticking are still absent, and P1's gate is what decides whether they are needed.

**The probe found an engine issue that has nothing to do with physics:** the first frame's timestep
is **1.38 seconds** (boot: asset scan, shader loads, first mesh applies), and frame 2 is still
0.047. The first version of this test gated its push on `if t < 0.5`, which was already false the
first time it ran, so nothing was pushed and the test *passed cleanly for the wrong reason*. Any
gameplay timer will misfire the same way. Recorded in
[cross-cutting.md](cross-cutting.md#the-first-frames-timestep-is-over-a-second); it will be met
again in P1.

*Original text follows.*

`RigidBodyComponent` is `{Type, Mass, LinearDamping, AngularDamping, UseGravity}`. **There is no
rotation lock.** A dynamic capsule driven by `SetLinearVelocity` tips over the first time it
touches anything, and then lies down.

Two options, and they are not the same size:

- **A `LockRotation` flag** (Jolt `EAllowedDOFs`, or zeroing the inertia tensor). Small. Gets a
  capsule that stays upright. Does **not** get step-up over a kerb, slope limits, or
  not-sticking-to-walls.
- **`CharacterVirtual`** — Jolt's kinematic character controller. Medium. Gets all of the above,
  and is what a production engine ships. **DONE** — built after P1's gate failed on sticking, which
  is the trigger Decision 1 set. `CharacterControllerComponent`, stepped after the solver, with
  `IsGrounded` in Lua. Measured A/B against the flag on the same wall with the same push: the
  character slid **10.07 m** along it and carried on past its end; the rotation-locked capsule slid
  **0.00 m** and had already stopped dead against a 0.15 m kerb. See
  [physics.md](../engine/physics.md#character-controllers).

Start with the flag, because it unblocks P1 in an afternoon and the game will say whether the rest
is needed. See Decision 1 — this is the call I am least sure about.

### P0.3 — Raycast — **DONE**

Built: `PhysicsScene::CastRay` over Jolt's `NarrowPhaseQuery`, returning a `RaycastHit`
(`Hit`/`Entity`/`Point`/`Normal`/`Distance`), plus `Physics.Raycast` in Lua and typings in
`ganymed.d.ts`. See [physics.md](../engine/physics.md#raycasts) and
[scripting.md](../engine/scripting.md#physics-queries).

Verified against authored geometry — eight cases, every distance and normal analytically checked:
a floor hit at exactly 5.000 with normal `(0,1,0)`, a wall at 9.500 with `(-1,0,0)`, misses for an
upward cast and for a 2 m ray at a 5 m floor, an unnormalised direction `(0,-37.5,0)` giving the
identical result to the normalised one, the capsule hit at 1.650 without `ignore` and the wall at
11.500 with it, and a zero-length direction missing without crashing.

*Original text follows.*

There is not one raycast call in the engine. Jolt has `NarrowPhaseQuery::CastRay`; it needs
wrapping and binding to Lua. Needed **twice**: enemy line-of-sight acquisition, and any weapon that
is not a projectile. Small, and the highest value-per-line item in Phase 0.

### P0.5 — Cursor capture — **found during P1, not planned**

There is no cursor capture in the engine. `glfwSetInputMode(GLFW_CURSOR, GLFW_CURSOR_DISABLED)`
appears nowhere, `Window` exposes no cursor mode, and `Input` offers only `IsKeyPressed`,
`IsMouseButtonPressed` and `GetMousePosition`.

Mouse look is therefore impossible, not merely awkward: an uncaptured cursor leaves the window,
and its delta goes to zero the moment it reaches a screen edge, so looking right stops working
when you have looked right enough.

Needed: a cursor mode on `Window` (normal / hidden / disabled), plumbed through the platform
layer for all three backends, plus `Input.SetCursorMode` and a raw mouse **delta** in Lua rather
than an absolute position - a captured cursor has no meaningful absolute position.

It belongs on `master` under the branch rule. P1 is built with keyboard turning meanwhile, because
P1's gate is about whether the capsule walks without tipping, and that does not depend on how the
player aims.

**This is the milestone working as intended** - Phase 0 was an attempt to name the engine gaps in
advance, and it missed one. Expect more.

### P0.6 — The editor hard-codes a default environment that not every project has

Found by opening `Game/assets` with `--project`. `EditorLayer.cpp:1385` calls

```cpp
AssetManager::ImportAsset("environments/studio_small_08_1k.hdr")
```

a **project-relative** path baked into the editor. Any project without that exact HDR at that exact
location logs `Failed to load HDR environment` at boot. `Game/` has no `environments/` at all.

It is one error and nothing else breaks, so it is not urgent - but it is the same class of bug
P0.1 was built to remove, and it survived because nothing had ever opened a second project. The fix
is for the editor's default environment to be an editor asset resolved against the editor, or for
there to be no default at all.

`master` work, like the two above.

### P0.4 — Sensors (optional)

No `IsSensor` anywhere, so a trigger volume is a solid body you bump into rather than walk through.
`OnCollisionEnter` already reaches Lua, so pickups and heal spots **work without this** — they just
feel wrong, because you stop when you touch them. Small. Do it if P5 feels bad, not before.

---

## Phases, on the game branch

Each phase names what it is actually testing, because that is the reason it exists.

### P1 — Ground, player, camera

Flat ground plane, capsule player, WASD plus mouse look, camera follow.

- **Tests:** the character controller from P0.2, the `Input` bindings under continuous use rather
  than a probe, transform hierarchy, and the editor's authoring loop end to end.
- **Gate:** walk the full extent of the map for two minutes without the capsule tipping, sinking
  through the ground, or sticking to a wall. **PASSED** on `CharacterVirtual`, after failing on a
  rotation-locked rigid body. Both runs are kept below; the failure is the more useful of the two.

**Gate, re-run on `CharacterControllerComponent` - 130 s:**

```
maxTilt 0.0000   minY 0.950 (settled 0.950, zero penetration)
worstStuck 0.00s   stuck-escapes 0   grounded 100%
extent  x -10.9..8.8   z -8.6..7.2
arrivals  wp1=13  wp2=13  wp3=13  wp4=13  wp5=13
0 errors, 0 warnings
```

Thirteen complete laps, every waypoint reached on every lap, **never stuck once**. Waypoint 4 at
`(-11, 5)` is the one that requires getting past Block B - the exact obstacle that jammed the rigid
capsule ten times - and it was passed thirteen times cleanly. Penetration went from 12 mm to zero,
because collide-and-slide never lets the shape overlap in the first place.

Nothing in `Player.lua` changed for the swap except the component named in the scene:
`SetLinearVelocity` routes to either kind, which is what that API accepting both was for.

**A measurement error worth recording.** The first character run reported `x -0.6..3.6` and only
ever sampled `wp=1` or `wp=4`, which read as "the route is not being followed". It was: a 5 s
report against a ~9 s lap aliases onto two phases, and **sampled extents are not extents**. Fixed
by tracking min/max every frame. Had it gone the other way, the same aliasing would have hidden a
real failure behind clean-looking numbers.

**The superseded run, on a rotation-locked rigid body - two of three pass, sticking fails:**

**Gate run - 130 s of autopilot across a flat plane with three obstacles:**

| Failure mode | Result |
|---|---|
| tipping | **PASS** - `maxTilt = 0.0000` for the whole run; the rotation lock never slipped |
| sinking | **PASS** - y range 0.940-1.380 against a settled 0.950. The low is 12 mm of solver penetration, the high is the capsule climbing the step. No downward drift |
| sticking | **FAIL** - jams on Block B's corner at `(-8.0, 7.0)` on **every lap**, ten times in 130 s, always the same spot |

Two useful answers fell out of it:

- **A rotation-locked capsule does climb a 0.2 m kerb.** Logged y of 1.06, then 1.30, then 1.38
  while crossing the step: the hemisphere at the capsule's base rides it. Step-up is not the
  missing piece at this height.
- **It does not slide along walls, and that is the entirety of the sticking failure.** Drive a
  velocity-set capsule straight at a surface and Jolt cancels the velocity; nothing tangential is
  left to carry it sideways, so it stops dead and stays. Deterministic - same corner, every lap.

The route's stuck-escape (skip to the next waypoint after 1.5 s pinned) keeps the run going, but it
is a **workaround in the game**, not a fix in the engine, and it is the only reason `worstStuck`
reads 1.51 s instead of 124 s.

**This fires Decision 1's own trigger.** The plan said: *if P1's gate needs more than one attempt,
stop and do `CharacterVirtual`.* It took three. Two of those were route bugs of mine - an arc that
drifted off the map, then waypoints authored inside solid blocks - but the third failure belongs to
the engine, and it is precisely the one Decision 1 named as most likely to bite.

Wall sliding is the one thing `CharacterVirtual` gives that this needs. Recommended: build it on
`master` before P2, rather than letting every later phase inherit a controller that stops dead on
contact.

### P2 — The map

Several buildings from box colliders, enterable, with interiors. Lighting.

- **Tests:** static mesh rendering at scene scale, box colliders, the four light types, shadows,
  and the asset pipeline under a real content load rather than the eight committed fixtures.
- **Gate:** walk inside and out of every building; no tunnelling through a wall at full speed.

#### Content layout

```
Game/assets/
  models/buildings/    blockhouse, warehouse - the things with interiors
  models/props/        crates, drums, barriers - cover, and what Meshy is best at
  models/characters/   player and enemy
  models/weapons/      rifle, projectile
  materials/           .gmat sidecars the importer writes
  textures/            maps extracted out of GLBs on first import
  scenes/  scripts/
```

Git does not track empty directories, so these appear in a clone only once they hold a file.

#### What a model has to satisfy

| Requirement | Why |
|---|---|
| **GLB (glTF 2.0)** | `cgltf` is the only importer |
| **Albedo + Normal + *combined* MetallicRoughness** | The only three map handles `Material` has. A separate AO or emissive map is imported and then ignored |
| **1 unit = 1 metre** | The player capsule is 1.9 m tall: radius 0.35, half-height 0.6 |
| **Y-up, -Z forward** | What `Player.lua` assumes when it derives forward from yaw |
| **Box-composable silhouette** | **There are no mesh colliders.** Every collider is a box, sphere or capsule placed by hand, so a curved wall looks right and collides wrong |
| **Low poly** | Not for framerate. A cold mesh apply is 5.50 ms and two thirds of that is `GenerateSidecars` writing files on the main thread ([assets.md](assets.md)) |

#### Importing one

1. Drop the `.glb` into the right folder. The content browser's tree refreshes every 0.25 s, so it
   appears on its own - but appearing is not importing.
2. **Content Browser → Rescan assets/.** This is the step that mints the handle and writes the
   `.meta`. The asset watcher does *not* do it: it tracks known assets for modification and has no
   notion of a file that was not there before.
3. Check the log. `Asset scan: N files, ... M handles minted, M sidecars written` should name your
   model. Nothing `quarantined`, no `handle collisions`.
4. Drag it into the scene, then check **in this order**, because each one masks the next:
   - **Scale.** Stand it next to the player capsule. A model authored in centimetres arrives 100x
     too big and everything after this is meaningless.
   - **Orientation.** Facing -Z, upright.
   - **Materials.** Albedo, normal and metallic-roughness all resolved - a missing map shows as
     flat grey rather than as an error.
   - **Collider.** Add box colliders by hand. Nothing is automatic and nothing warns you.
5. Commit the `.glb` and its `.meta` together. A `.glb` without its sidecar has no identity, and
   every scene referencing it breaks on the next clone. `.compiled/` is gitignored on purpose -
   it is derived, and it rebuilds.

#### The measurement P2 exists to take

The pipeline has only ever been exercised on eight committed fixtures. Before importing a set,
import **two or three** and record:

- wall-clock cost of the first import of each (the cold `GenerateSidecars` path)
- whether the frame hitches visibly on that first apply, and for how long
- what the scan reports on the second boot, when everything is warm

`assets.md` predicts 6-9 ms frames, worse when the disk is busy, and that it is entirely a
first-import cost. That prediction has never met real content. Finding it wrong on three models is
far cheaper than on fifteen.

### P3 — Shooting and dying

Projectile weapon. Enemies with health that despawn on death.

- **Tests:** `Scene.Spawn` and the 64-per-frame cap under sustained fire, `OnCollisionEnter`
  dispatch, `Entity:Destroy` subtree despawn, and physics with many small dynamic bodies.
- **Gate:** fire continuously for a minute; entity count returns to baseline; no leaked Jolt
  bodies.

### P4 — Enemies with eyes

Waypoint patrol, line-of-sight acquisition via raycast, charge. Animation on the enemies.

- **Tests:** P0.3's raycast, the animator under many simultaneous instances, and whether buildings
  actually occlude.
- **Gate:** an enemy behind a building does not acquire the player; stepping into the doorway does.

### P5 — Pickups, health, upgrades

Weapon pickups, heal spots, an upgrade station.

- **Tests:** `OnCollisionEnter` as a trigger mechanism, and the existing `UI.SetHealth` /
  `UI.SetScore` data model, which is bound but has never been driven by real gameplay.

### P6 — It looks and sounds like something

RmlUi HUD, footsteps, weapon sound, a music bed, muzzle flash and impact particles.

- **Tests:** the RmlUi document under live data binding, the audio pipeline and 3D listener driven
  by a *moving* player for the first time, and `EmitBurst` under load.

### P7 — Ship it

Run the whole thing in `GanymedRuntime`, Dist configuration, from a copied asset tree.

- **Tests:** the read-only asset path, `.meta` sidecar completeness, registry portability, the
  `WindowedApp` / `mainCRTStartup` Dist configuration, and boot with no editor present.
- **This phase is where I expect the most bugs.** It is the least-exercised code in the engine, and
  its failure mode — a shipped game that cannot find its own assets — is invisible from the editor.

---

## Decisions, with the ones most likely wrong marked

Recorded now so that when this moves to `docs/history/` we can see which held.

1. **⚠ `LockRotation` before `CharacterVirtual`.** Betting that a rotation-locked dynamic capsule
   is good enough to build on, and that step-up and slope handling can wait. **This is the decision
   most likely to be wrong.** The usual outcome is that velocity-driven dynamic capsules feel bad
   in a way that is hard to attribute, and the fix turns out to be the controller that was skipped.
   If P1's gate needs more than one attempt, stop and do `CharacterVirtual`.
2. **⚠ Projectiles, not hitscan.** Chosen because projectiles exercise spawning, the spawn cap,
   physics and collision dispatch — all landed recently, none under load. The costs are that
   projectiles feel worse to shoot than hitscan, and that at some speed they tunnel. Accepted: this
   is a test instrument and feel is not the deliverable.
3. **Buildings from box colliders, no mesh colliders.** Cheap, standard, and it keeps a large
   physics feature out of Phase 0.
4. **⚠ P0.1 described as "an asset root parameter" — WRONG, and in the safe direction.** I said
   it was understated and would be the largest item, because asset roots leak into scene-relative
   paths, the `.compiled/` cache, the content browser's home and the watcher's directory. Every
   one of those already went through `GetAssetRoot()`; `AssetPaths.h` had centralised the root
   long before. The change was small and landed in one sitting. The lesson is not "estimates are
   pessimistic" — it is that **I estimated without reading `AssetPaths.h`**, and one grep would
   have answered it.
5. **No cover AI.** Argued above. The risk is that enemies charging in the open makes the buildings
   pointless and the map read as flat — in which case the answer is more enemies and tighter sight
   lines before it is pathfinding.

## How we will know it worked

Not "the game is fun". The milestone succeeds if it produces a list of **engine** defects that the
probe-per-change method never found — and the prediction, recorded now so it can be checked later,
is that they cluster in three places: P7's read-only asset path, the animator under many
simultaneous instances, and memory or handle growth over a session longer than any probe has ever
run.

If the game ships and finds nothing, that is also a result, and it means the probes were better
than I think they are.
