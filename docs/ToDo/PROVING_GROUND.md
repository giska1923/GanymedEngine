# Milestone — Proving Ground (the test game)

**Status: complete.** Phase 0 and P1–P7 are all built, gated and written up below. The game
runs in `GanymedRuntime`, Dist, from a shipped install. What is left of this document is a
record rather than a plan, and it belongs in `docs/history/` once the branch is merged — see
[ToDo/README.md](README.md) for the one thing missing from it first.

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

### P0.7 — The importer invents tangents instead of generating them

[`MeshImporter.cpp:642`](../../GanymedEngine/source/GanymedE/Renderer/MeshImporter.cpp) on a glTF
with no `TANGENT` attribute:

```cpp
else { vertex.Tangent = { 1.0f, 0.0f, 0.0f }; }
```

Every vertex gets world +X. That is not a tangent, and the shader does consume it -
[`fs_Phong.sc:241`](../../assets/shaders/src/fs_Phong.sc) builds a TBN from it whenever
`u_UseNormalMap` is set:

```glsl
vec3 T = normalize(v_tangent - N * dot(N, v_tangent));
```

Two failure modes, and the second is the bad one:

- On a surface facing anything other than +-X, `T` is world-X projected onto it: a valid vector
  pointing in an arbitrary direction, so the normal map is applied **rotated by an arbitrary
  angle**. Wrong lighting that reads as a bad texture.
- On a surface facing **exactly +-X**, `dot(N, T)` is +-1, the subtraction yields the zero vector,
  and `normalize` produces garbage. A box-shaped building has walls facing exactly +-X.

glTF does not require `TANGENT`, and exporters routinely omit it because the spec says a client
should generate tangents from positions and UVs when a normal map is present. Nothing here does.
The fix is the standard per-triangle accumulation (Lengyel, or MikkTSpace for exactness) at import,
when `TANGENT` is absent and the material has a normal map.

**Currently latent, and only by accident:** Meshy's re-export dropped the normal maps, so
`u_UseNormalMap` is off for every model in the project. The first normal-mapped asset turns this on.

`master` work.

### P0.4 — Sensors — **DONE, and it was not optional**

`RigidBodyComponent::IsSensor` maps to Jolt's `mIsSensor`: a body that reports contacts and causes
none. Landed on `master` with the character inner body, because P5 could not be built without it.

**This entry was wrong, and worth keeping wrong here.** It said pickups and heal spots *work
without this* and would merely feel bad — true when it was written, and false by the time P5
arrived. Giving the player presence made it a **Kinematic** inner body, and Jolt refuses to pair two
non-dynamic bodies; the single exemption in `Body::sFindCollidingPairsCanCollide` is a sensor. So a
plain static box is not solid-when-it-should-be-walkthrough, it is **undetectable**. See
[physics.md](../engine/physics.md#sensors-trigger-volumes).

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

### P4 — Enemies with eyes — **PASSED**

Waypoint patrol, line-of-sight acquisition via raycast, charge. Animation on the enemies.

- **Tests:** P0.3's raycast, the animator under many simultaneous instances, and whether buildings
  actually occlude.
- **Gate:** an enemy behind a building does not acquire the player; stepping into the doorway does.
  **PASSED**, and to within 0.1 m of a position predicted before the run.

#### What was built

Seven enemies, each a two-entity rig: a dynamic, rotation-locked body carrying `Enemy.lua` and the
box collider, and a `Body` child carrying the mesh, the `AnimatorComponent` and the facing. The
child exists for the same reason the player's `Yaw` does — the body is `LockRotation`, so Jolt
hands `SyncTransforms` the same orientation every frame and a yaw written on the parent is erased
before anything can see it.

Six patrol a two-point leg authored per instance as script `Fields` (`label`, `patrolTo`); the
seventh, the **Sentry**, stands still inside the Blockhouse and exists for the gate.

`Fox.glb` is the enemy mesh, copied out of the editor's sample models. It is a placeholder for a
Meshy-authored humanoid and it is the wrong shape for one — 2.8 m long against a 0.7 m collider.
It was chosen anyway because it is the only asset in either tree with **more than one clip**
(`Survey` 3.417 s, `Walk` 0.708 s, `Run` 1.158 s, 24 joints), and "the animator under many
simultaneous instances" is not tested by six copies of the same clip.

#### The premise this phase overturned

`Enemy.lua`'s P3 header said: *P4 makes them move, and that is when they become character
controllers.* **Wrong, and wrong in a way that would have deleted P3's result.**

A `CharacterVirtual` is not a body. It has no `BodyID`, it is not in the broadphase, and
`NarrowPhaseQuery` cannot find it. Making an enemy a character stops projectiles hitting it, stops
its `OnCollisionEnter` firing, and stops any raycast seeing it — all at once. So the enemies are
dynamic rigid bodies with `LockRotation`, which is what the player was before P1 replaced it, and
they inherit P1's sticking failure along with it.

The same fact is what makes line of sight work, inverted: `Enemy.lua` casts at the player and
treats a **miss** as "I can see you", because the player is a character and there is nothing at
the far end to hit. Cheap, correct today, and silently load-bearing — written up in
[cross-cutting.md](cross-cutting.md) with the fix (`mInnerBodyShape`), which P5 will need anyway.

#### Gate run 1 — the occlusion probe (`losgate`, enemies sense but do not move)

The geometry, read off the scene rather than guessed. The Blockhouse's `-Z` wall is two collider
segments at world `z = -9.43`, spanning `x 12.00..17.66` and `x 19.46..20.00`, leaving the 1.8 m
doorway between them. The Sentry stands at `(18.4, -6)` and never turns. A sight line from there
to a player at `(px, -12)` crosses `z = -9.43` at `x = 18.4 + 0.5717 * (px - 18.4)`, so the wall's
inner edge at `x = 17.66` **predicts acquisition at `px = 17.11`**.

```
LOSGATE probe 2 at (13.66, -12.01), holding 8.0s - behind the -Z wall: the Sentry must NOT acquire
  ... 8 s, no Sentry line at all ...
LOS Sentry t=17.7s ACQUIRED player=(17.2, -12.0) self=(18.4, -6.0) d=6.1m
LOSGATE probe 3 at (18.22, -12.00), holding 8.0s - on the doorway's sight line: it must
  ... 8 s, acquired and held ...
LOS Sentry t=28.3s lost player=(16.2, -16.1) self=(18.4, -6.0) d=10.4m blocked-by=Blockhouse Wall Z-
LOSGATE probe 4 at (14.14, -12.32), holding 6.0s - back behind the wall: it must lose me again
  ... 6 s, stays lost ...
0 errors
```

Acquisition at `px = 17.2` against a predicted 17.11. The loss on the way back, at `(16.2, -16.1)`,
puts the crossing at `x = 17.65` against the same 17.66 edge. **Buildings occlude, and they occlude
exactly where their colliders are** — which also says the colliders hand-placed in P2 line up with
the mesh they were measured from.

The probe route stops and stands still at each point, because "does the wall occlude" only has a
clean answer while nothing is moving, and it sets `PG.freeze` so that six enemies converging on the
probe point cannot shove the player off the spot the measurement is taken at.

#### Gate run 2 — everything live, 125 s

Patrol, acquire, charge, search, and the stuck escape, all exercised. Zero errors. 162 LOS
transitions across seven enemies; `blocked-by` names the occluder every time, and enemies occlude
each other as well as the buildings (`blocked-by=Enemy`).

Sixteen enemy stuck-escapes fired, every one logged. That is P1's failure reproducing exactly as
predicted on a velocity-driven rigid body, and the sidestep is a workaround in the game rather than
a fix in the engine — same status as the autopilot's own escape.

**One of them refines P1's result.** With the player standing still at the origin, every enemy
charging from the south stops at `z = -8.8` — its collider's front face at `-8.45`, against the
Step's south face at `-8.5`:

```
Enemy E1: stuck at (-2.0, -8.8) in state 'hunt' - sidestepping (escape #1)
Enemy E3: stuck at ( 3.7, -8.8) in state 'hunt' - sidestepping (escape #1)
```

P1 measured that a rotation-locked rigid **capsule** climbs that same 0.2 m step — "the hemisphere
at the capsule's base rides it". These enemies have a **box** collider, and a flat-bottomed box has
no hemisphere: it stops dead at a kerb a capsule walks over. So the thing that climbs a low step
without a character controller is the *shape*, not the body type, and P1's finding should be read
as being about capsules specifically.

**The player was pinned for 40 s, and it is not an engine bug.** From `t=40s` to `t=80s` the
capsule sat at `x = -12.7` with `z` sliding between `-8.9` and `-10.8`. The Warehouse's `X+` wall
is at world `x = -12.15` with a 0.15 m half-extent and the capsule's radius is 0.35, which puts a
body pressed against its inner face at exactly `-12.65`. The controller was working — it slid along
the wall the whole time. What failed is the autopilot's stuck escape: it advances to the *next
waypoint*, with no notion of whether that waypoint is reachable, so once it was inside a building
with every remaining target outside it, it simply leaned on the nearest wall until a later waypoint
happened to line up with the door. **Decision 5's cost, arriving where Decision 5 said it would.**

#### Gate run 3 — P3 does not regress on dynamic enemies

The worry was specific: P3's kills were all against **static** bodies, and this phase made every
enemy dynamic.

```
fired=2417  despawned=2417  live=0  hits=1875  kills=5  refused=400
0 errors
```

Five of seven killed, every projectile despawned, entity count back to baseline. Contact dispatch,
`Entity:Destroy` on a subtree, and the spawn cap all behave the same against a dynamic target.

#### The animator, under six instances switching three clips

`PlayAnimation` is read back through `GetCurrentAnimation` on every change, because an unresolved
clip name does not fail loudly — `AnimationSystem` warns once and holds the bind pose, so "no
warnings" is weak evidence.

```
CLIP E4 -> 'Walk' (animator reports 'Walk', playing=true)
CLIP E3 -> 'Run'  (animator reports 'Run',  playing=true)
CLIP E6 -> 'Walk' (animator reports 'Walk', playing=true)
CLIP E6 -> 'Run'  (animator reports 'Run',  playing=true)
...
```

All three clips resolve, and six instances switch between them independently in the same frames.
The animation milestone verified two entities on different clips; this is the first time more than
two have been driven at once, and nothing broke.

#### Left undone, deliberately

- **Nobody has looked at this.** Every result above is from the log. The Fox's world scale
  (`0.018`, giving ~1.4 m tall) and its facing axis (assumed `-Z`, like everything else) are
  **unverified by eye** and may need a nudge in the editor.
- `Fox.glb` has no `.compiled` output in the tree — it is gitignored and derived — so the runtime
  recompiles it at every boot (26 ms) and says so. Running the editor once over `Game/assets` fixes
  it locally; P7 is where it stops being cosmetic.
- No cover AI, no pathfinding. Decision 5 stands; the 40 s pin above is the first real evidence
  about what it costs, and it cost the *autopilot*, not the enemies.

### P5 — Pickups, health, upgrades — **PASSED**

Weapon pickups, heal spots, an upgrade station.

- **Tests:** `OnCollisionEnter` as a trigger mechanism, and the existing `UI.SetHealth` /
  `UI.SetScore` data model, which is bound but has never been driven by real gameplay.
- **Gate (defined here, because the plan did not state one):** in one run, with nobody touching the
  keyboard — take damage from an enemy, heal it back at a heal spot, pick up a weapon and see the
  fire interval change, bank score from kills and spend it at an upgrade station, and have
  `UI.GetHealth()` / `UI.GetScore()` read back exactly what gameplay wrote at every step.

#### This phase could not start until the engine changed. Twice.

**The player had no presence in the world.** A `CharacterVirtual` has no `BodyID` and is not in the
broadphase, so enemies passed through it, projectiles passed through it, and no trigger volume
could notice it. Nothing in P5 is expressible without that. Landed on `master` as the character
inner body ([physics.md](../engine/physics.md#presence-the-inner-body)), which was already named as
the fix in the ToDo entry P4 wrote.

**And a static box is still invisible to it.** The inner body is *Kinematic*, and Jolt refuses to
pair two non-dynamic bodies — with exactly one exemption, which is sensors. So P0.4, filed as
"optional, do it if P5 feels bad", turned out to be **required**: without `IsSensor` a pickup is not
merely solid-when-it-should-be-walkthrough, it is undetectable. P0.4's own text said pickups "work
without this", and that was true when it was written and false by the time P5 arrived.

#### What was built

`Pickup.lua`, one script for all three kinds, each a static **sensor** box with a cube child so
there is something to see:

| Tag | Kind | Behaviour |
|---|---|---|
| `Heal Spot` | heal | permanent; heals 14/s while you stand in it, using enter/exit to count |
| `Weapon Crate` | weapon | consumed on touch, destroys itself, halves the fire interval |
| `Upgrade Station` | upgrade | permanent; spends 2 score for +1 projectile damage, refuses if poor |

The effect is applied by the **player**, not the pickup: contacts dispatch to both participants, so
the player's `OnCollisionEnter` reads the tag off whatever it touched. The pickup script only
handles what happens to the pickup. That split is not a preference — reaching into another entity's
script instance is not something the API can express, and it is already how `Enemy.lua` counts its
own hits. Damage and score cross the same way, through the shared `PG` table.

#### Gate run — 165 s, 0 errors

```
probe 1 (2.09, -2.30)  hold until hurt   -> hp 100 -> 82, taken=3, ended at (2.16, -2.26)
TRIGGER enter Heal Spot        t=20.7s
probe 2 (-5.77, -5.75) hold 8s           -> hp 82 -> 100, healed=18
TRIGGER enter Weapon Crate     t=34.1s   -> weapon level 2, fireInterval 0.120 -> 0.060, consumed
TRIGGER enter Upgrade Station  t=43.9s   -> UPGRADE bought: projectile damage -> 2, score left 0
PLAYER DOWN at 20 hits taken                 ... and up again 3 s later
final: hp=94/100 taken=21 healed=18 weapon=2 dmg=2 score=0 upgrades=1 triggers=3 ui-mismatch=0
       fired=144 despawned=144 live=0 hits=17 kills=2
```

`ui-mismatch=0` is the whole of the second test: every `SetHealth` and `SetScore` is read straight
back and compared, because a setter that silently dropped its value would look exactly like one
that worked. `despawned == fired` says P3 still holds with sensors in the scene — projectiles fly
*through* the pickups rather than dying on them.

#### Three defects this phase found, all of them real

**1. `UI.SetScore` took an `int`, and every script property is a Lua float.** Buying an upgrade
computed `score - cost` where the cost came from a property, so the result was a float, and sol2
with `SOL_ALL_SAFETIES_ON` refused it — "not a numeric type that fits exactly an integer". The
throw escaped into the frame and took the rest of `OnUpdate` with it, so the run continued looking
healthy while the player stopped updating. Fixed on `master`: the binding takes a `double` and
truncates, which is what `EmitBurst` and `SetParticleMaxParticles` already did. **The precedent
existed and this binding had not followed it.**

**2. An enemy shoved the player through the ground plane.** Giving the player presence made a
charging enemy able to move it, and a `CharacterVirtual` resolves an overlap by moving *itself*,
with no mass and no resistance. The first full gate run:

```
GATE t=60s pos=(18.4, 0.95,  8.4)
GATE t=70s pos=(27.2, 0.95, 26.3)
GATE t=80s pos=(35.9, 0.95, 44.2)
GATE t=85s pos=(36.6, -14.47, 45.1)   <- through the ground, not off it: the plane is +-50
GATE t=95s pos=(10.3, -678.57, 2.4)
```

Steady 1 m/s of bulldozing for over a minute, then through the floor. Jolt can refuse the push per
contact (`CharacterContactSettings::mCanPushCharacter`) but only through a
`CharacterContactListener`, and the engine installs none — recorded in
[cross-cutting.md](cross-cutting.md). The game-side fix is what melee AI does anyway: land a touch,
then back off to 2.2 m for 1.5 s. Drift during a 14 s hold went from **21 m to 0.07 m**.

**3. The gate reported success while falling out of the world.** Horizontal control still works in
freefall and the route measures arrival in XZ only, so 700 m below the map it was still "reaching"
the next waypoint. `Diagnose` was shouting `GATE FAIL: left the ground plane` the whole time and
the route did not listen. A probe route now aborts on `fell`, so a failed run cannot read as a
passing one. **This one is mine, not the engine's, and it is the more dangerous kind**: the
instrument agreeing with itself.

#### What P5 could not express

- **A character cannot be teleported from script.** Its `TransformComponent` is overwritten from
  the controller every frame and nothing exposes `CharacterVirtual::SetPosition`, so "respawn at
  the spawn point" does not exist. Death recovers the player *where it fell* instead, which is not
  what anyone would ship.
- **There is no `OnCollisionStay`.** "An enemy is on me" has to be reconstructed from enter/exit
  pairs, and that counter leaks the moment an enemy dies while touching. Contact damage is a
  bounded budget of ticks per touch instead, which cannot leak.
- **A script cannot ask whether a contact was with a sensor.** Projectiles had to be told which
  names to fly through, published into `PG` by the pickups themselves.

All three are in [cross-cutting.md](cross-cutting.md).

### P6 — It looks and sounds like something — **PASSED**

RmlUi HUD, footsteps, weapon sound, a music bed, muzzle flash and impact particles.

- **Tests:** the RmlUi document under live data binding, the audio pipeline and 3D listener driven
  by a *moving* player for the first time, and `EmitBurst` under load.
- **Gate (defined here):** one run of autopilot plus autofire, the whole circuit. Every shot that
  is actually fired flashes and bangs and nothing else does; every impact emitter is reaped;
  component-owned voices track what is alive; one-shots drain to zero; the HUD reads back what
  gameplay wrote. Zero errors.

#### What was built

| Piece | What it exercises |
|---|---|
| `ui/hud.rml` + `hud.rcss` | the data model P5 drives, live, plus a crosshair |
| Footsteps on the player | `AudioSourceComponent` and the `StopSound`-then-`PlaySound` retrigger |
| Weapon report | unspatialised `Audio.PlayOneShot`, at the listener by definition |
| Impact sound | **positional** one-shot — the only sound with real work for the 3D listener |
| A hum on every enemy | seven spatialised *moving* looping sources, which the listener had never been driven by |
| Music bed | streamed, looping, unspatialised, on the `Music` group |
| Muzzle flash | `EmitBurst` on a local-space emitter parented to `Yaw`, so it points where you look |
| `Impact.gprefab` | one emitter entity per hit, spawned at the contact point, self-destructing |

The audio is four files carried over from the runtime demo, and the map from files to roles is
**three SFX for four jobs**: `impact.wav` is the shot, the impact and (pitched down and quieter)
the footstep. It sounds like a placeholder because it is one.

#### Gate run — 140 s, 0 errors

```
fired=1894  refused=708  despawned=1894  live=0
muzzle-bursts=2602    <- see below
impacts=1506  impacts-despawned=1506  live-impacts=0
steps=323 over 140 s
voices  9 -> 8 -> 6 -> 5 -> 4      (settles at 2 surviving enemies + music + footsteps)
one-shots  peak 115 during the burst phase -> 0
grounded 100%, inWall=0, no GATE FAIL
```

`impacts == hits` and `impacts-despawned == impacts` at every report: 1506 emitter entities
spawned at contact points, each bursting 16 particles, all reaped. That is `EmitBurst` under the
load the phase asked for, on top of 2602 muzzle bursts.

#### Three defects, and the counters found two of them

**1. A destroyed entity's voice was never released.** The first gate run killed six humming enemies
and `Audio.GetVoiceCount()` sat at **nine** for the rest of the session. `AudioSystem`'s voice map
was emptied by exactly one thing, `OnRuntimeStop`, so a voice outlived its entity — playing, at the
last position it had been pushed to. Audible rather than merely leaked: a dead enemy hums over its
own grave. Fixed on `master`; `AudioSourceComponent` now carries `EnableFini` and `OnUpdate` drains
a `FiniView` first. The reactive view is not a preference — the whole-entity-destroyed case is
precisely the one a plain iteration cannot see, because the component is gone by the time you look.
After the fix the same run reads `9 -> 8 -> 6 -> 5 -> 4`.

This is one of the three places [How we will know it worked](#how-we-will-know-it-worked) predicted
the defects would cluster: *memory or handle growth over a session longer than any probe has ever
run*.

**2. The gun fired blanks under load.** `muzzle-bursts=2602` against `fired=1894` with
`refused=708`, and `1894 + 708 = 2602` exactly. Every shot the per-frame spawn cap turned away
still made a noise and a muzzle flash. Visible only because the two counters were kept separately —
either one alone looks fine. Fixed by moving the sound and the burst *after* the spawn succeeds;
the same run now reads `muzzle-bursts = fired = 1965` with `refused=448` beside it.

**3. `Audio.PlayOneShot` could not be given a volume.** `AudioEngine::PlayOneShot` has always taken
one, and its own comment says it exists "for footsteps and impacts" — the binding passed a
hardcoded `1.0`. A footstep at the volume of a gunshot is not a footstep, so the footsteps were
built on an `AudioSourceComponent` purely to reach `Volume`. Fixed on `master`: position and volume
are both optional now. The footsteps stayed on the component anyway, because the two routes are
different tests.

`Audio.GetVoiceCount()` and `GetOneShotCount()` were bound in the same change. Both existed in C++
and were called by **nothing**, so "no leaked voices" was not a claim any test could make — which
is why defect 1 had survived this long.

#### Not verified by eye

Every number above is from the log. The HUD **loads** — RmlUi reports the document, the fonts and
the compositing target, and parses the RCSS without a single warning — and `ui-health` tracks
gameplay, so the binding is live. Whether the bar, the score and the crosshair are actually in the
right places, and whether the muzzle flash looks like anything, has not been looked at. Nor has the
mix: four sounds at authored volumes with no one listening to them is a guess.

#### What P6 could not express

The HUD data model is **fixed at two variables**, `health` and `score`, declared in C++ before any
document loads. The Proving Ground tracks a weapon level, a damage level and an enemy count, and
none of them can reach the HUD. [ui.md](../engine/ui.md) already said a general `UI.Set(name,
value)` was "worth doing when a second HUD needs it — not before"; this is the second HUD.
Recorded in [cross-cutting.md](cross-cutting.md).

### P7 — Ship it — **PASSED**

Run the whole thing in `GanymedRuntime`, Dist configuration, from a copied asset tree.

- **Tests:** the read-only asset path, `.meta` sidecar completeness, registry portability, the
  `WindowedApp` / `mainCRTStartup` Dist configuration, and boot with no editor present.
- **This phase is where I expect the most bugs.**
- **Gate (defined here):** an install containing nothing but the executable, its DLLs and an
  `assets/` tree boots, scans every asset from a sidecar with **nothing minted and nothing
  written**, uses the `.compiled` cache without recompiling, and plays a full autopilot-plus-
  autofire circuit with zero errors.

#### The install

```
ship/
  GanymedRuntime.exe                 9.7 MB, Dist          (Debug is 42 MB)
  msvcp140.dll vcruntime140.dll vcruntime140_1.dll
  assets/                            Game/assets entire, .meta and .compiled included
    runtime.yaml                     AssetRoot: assets
    fonts/                           ENGINE-owned
    shaders/compiled/                ENGINE-owned
```

The two engine-owned directories are the part worth knowing. `UIEngine` loads its faces from
`assets/fonts/...` and `Shader::Create` from `assets/shaders/compiled/<profile>/...`, both
**relative to the working directory** rather than to the project root — by design, so the editor's
chrome survives opening someone else's project. In a shipped layout the working directory *is* the
install, so engine chrome and game content share one tree. It works, and the asset scan ignores
`.ttf` and `.bin` so nothing is minted or quarantined by it, but it is a surprise and it pins
`AssetRoot` to `assets`.

#### Gate run — 150 s in the shipped install, 0 errors

```
Asset scan: 31 files, 31 adopted from sidecars, 0 minted, 0 written, 0 quarantined, 0 collisions
AssetManager initialized (31 assets indexed, assets read-only)
Scene 'assets/scenes/ProvingGround.ganymede' loaded (52 entities)
UI document 'assets/ui/hud.rml' loaded

fired=7975  refused=7376  despawned=7975  live=0
muzzle-bursts=7975   impacts=7605  impacts-despawned=7605  live-impacts=0
voices=6  one-shots=0   grounded=100%  steps=325
```

No recompile lines at all: the shipped `.compiled` tree was used as shipped. No
`AssetRegistry.gr` in the game's tree and nothing adopted from a legacy registry — **sidecars alone
are sufficient identity for a shipped game**, which is what that migration was for.

**Dist is roughly four times the throughput of Debug.** The same 145 s of script time fired 7,975
rounds against Debug's 1,894, and ran at wall-clock speed instead of stalling through the burst
phase. The spawn cap absorbed the difference: 7,376 refusals, every one counted, nothing leaked.

#### Three defects, all of them invisible from the editor

**1. A shipped asset with no `.meta`, and nothing said so.** The first shipped boot read
`31 files, 30 adopted from sidecars, 1 handles minted` — and that line reads as normal at a glance.
The asset was `prefabs/Impact.gprefab`, created in P6 after the editor run that would have minted
its sidecar. It happened to still work, because `Scene.Spawn` resolves by path; anything naming it
by handle would have been broken with no message anywhere.

A mint on a *writable* install is ordinary. A mint on a **read-only** install is a shipping defect:
the sidecar cannot be written, so the handle is different on every boot. `ScanAssets` now warns and
names each path, the same shape as the orphaned-sidecar report beside it. Verified in both
directions — with the sidecar restored the scan reads `31 adopted, 0 minted`; with it removed the
log says `no sidecar: prefabs/Impact.gprefab`.

**2. The shipped build wrote a 1.7 MB trace log in 150 seconds.** 15,463 lines, one per spawned
entity, each flushed to disk **synchronously inside a frame**, because `Log::Init` set level
`trace` and `flush_on(trace)` in every configuration. Right with a debugger attached; absurd in a
shipped game, where nobody reads it and the lines carry build-machine paths. Dist now logs `info`
and above and flushes on `warn`: the same run writes 799 lines and 96 KB, and still carries the
boot banner, the renderer table, the asset scan, every warning and every error — which is what a
user's bug report actually needs.

**3. The executable is not self-contained.** `staticruntime "off"` in every configuration, so the
import table names `MSVCP140.dll`, `VCRUNTIME140.dll` and `VCRUNTIME140_1.dll`. A machine without
the redistributable fails with a Windows dialog before any of our code runs — no log, nothing to
report. **Not fixable as written**: every static library must agree on the CRT, and the third-party
ones build from premake files inside `extern/`, which is not ours to edit. Recorded in
[cross-cutting.md](cross-cutting.md) with the two real options; P7's install ships the three DLLs.

#### What held

- **`WindowedApp` / `mainCRTStartup` works.** No console window, `stdout` empty, and the log file
  still written.
- **The read-only path is correct.** Nothing was written into `assets/` during any run; the guard
  held at the one place identity is decided.
- **Boot with no editor present.** The install has no editor, no source, no registry, no project
  file. It boots in about a second.

#### One thing that is not the engine's fault

Every MSBuild build on this machine ends with

```
pwsh.exe -ExecutionPolicy Bypass ... applocal.ps1 ...
'pwsh.exe' is not recognized as an internal or external command
```

That is vcpkg's global MSBuild integration (`vcpkg integrate install`) running its AppLocal DLL
copier, which needs PowerShell 7. This project uses no vcpkg packages, so the step has nothing to
do and the link has already succeeded by the time it runs — but it reports an error on every build,
which is exactly the kind of noise that trains people to ignore build output. `vcpkg integrate
remove`, or installing `pwsh`, is the fix, and neither is a change to this repository.

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

## How we will know it worked — checked

Not "the game is fun". The milestone succeeds if it produces a list of **engine** defects that the
probe-per-change method never found — and the prediction, recorded before any of it was built, was
that they would cluster in three places: P7's read-only asset path, the animator under many
simultaneous instances, and memory or handle growth over a session longer than any probe has ever
run.

**Two of the three landed. One did not. And the largest group was not predicted at all.**

| Predicted | Outcome |
|---|---|
| P7's read-only asset path | **Hit.** A shipped asset with no `.meta` mints a fresh handle on every boot and said nothing about it; the count sat in the middle of an INFO line that reads as normal |
| The animator under many simultaneous instances | **Missed.** P4 drove six instances across three clips, switching independently in the same frames, and nothing broke. The animation milestone's work held |
| Memory or handle growth over a long session | **Hit, and it was audible.** A destroyed entity's audio voice was never released — it kept playing, at its last position, for the rest of the session |

The group nobody predicted is **the script binding surface**, and it produced the same bug twice:
`UI.SetScore` declared `int` and `Audio.PlayOneShot` hardcoded its volume, in a codebase where
`EmitBurst` already carried a comment explaining why a binding must take a `double`. Both were
found by gameplay doing the ordinary thing; neither could have been found by a probe, because a
probe passes the literal the author had in mind.

The second-largest group is **character controllers**, which is less a list of defects than one
missing decision: a `CharacterVirtual` had no presence in the world at all, and everything downstream
of that — no raycast could see the player, no projectile could hit it, no trigger could notice it,
and once it had presence nothing could refuse a push — followed from never having had a game ask.

### The defects, by phase

| Phase | Found | Fixed |
|---|---|---|
| P0/P1 | velocity-driven capsules cannot slide along a wall | `CharacterVirtual` |
| P2 | importer invented tangents; albedo decoded as linear; textures loaded upside down; no mip chain | all four |
| P4 | a raycast cannot see a character controller | documented, then fixed in P5 |
| P5 | `UI.SetScore` refused a float; a character could be shoved out of the world; a static box is invisible to a character | binding fixed, sensors added, push recorded |
| P6 | a destroyed entity's voice was never released; `PlayOneShot` had no volume; voice counters were unreachable | all three |
| P7 | a read-only install mints silently; Dist wrote a 1.7 MB trace log; the executable is not self-contained | first two fixed, third recorded |

Eleven fixed, five recorded in [cross-cutting.md](cross-cutting.md) as decisions rather than
oversights. **None of them were found by a probe**, and every one was found by the game doing
something ordinary for longer than a probe runs.

The honest caveat: this milestone verified almost everything through the log. The renderer output,
the HUD layout and the audio mix have been **measured and not looked at**. Whatever is wrong with
how any of it looks or sounds is still there.
