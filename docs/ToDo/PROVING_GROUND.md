# Milestone — Proving Ground (the test game)

**Status: complete.** Phase 0 and P1–P7 are all built, gated and written up below. The game
runs in `GanymedRuntime`, Dist, from a shipped install. What is left of this document is a
record rather than a plan, and it belongs in `docs/history/` once the branch is merged — see
[ToDo/README.md](README.md) for the one thing missing from it first.

A small third-person shooter, built to find out what is wrong with the engine. The game is the
instrument, not the goal: every phase below is chosen for the engine surface it puts under load,
and a phase that would be fun but tests nothing already built is cut.

The premise is that this engine has never had a _consumer_ that runs for more than a minute. Every
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

| Where                 | What                                                                                                          | Rule                                                                         |
| --------------------- | ------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------- |
| `master`              | Everything under `GanymedEngine/source/`, `GanymedEditor/source/`, `GanymedRuntime/source/`, premake, scripts | Engine features the game needs are built, verified and merged **here first** |
| `game/proving-ground` | The game's assets, scenes, prefabs and Lua                                                                    | **No change under `GanymedEngine/source/`**, ever                            |

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

These are known _now_. Doing them first collapses most of the branch-juggling that would otherwise
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
  its checkerboard and HUD from disk. That is only true of moving the _working directory_, which
  the actual fix does not do. It moves the project root and leaves engine- and editor-owned assets
  resolving against CWD, so the chrome was never at risk.

What it did turn up was a real latent bug: `ContentBrowserPanel.cpp` defined
`extern const std::filesystem::path g_AssetPath = GetAssetRoot();` at namespace scope, a
static-initialisation-time snapshot taken before `main`. It would have frozen the default root
forever. Removed, and the reason recorded in `AssetPaths.cpp` so it is not reintroduced.

_Original text follows._

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
disk-loaded assets resolved against the _editor's_ install location rather than the project's.
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
  P0.1 buys is that the copy stops being a _per-iteration_ step and becomes a _packaging_ step.
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
first time it ran, so nothing was pushed and the test _passed cleanly for the wrong reason_. Any
gameplay timer will misfire the same way. Recorded in
[cross-cutting.md](cross-cutting.md#the-first-frames-timestep-is-over-a-second); it will be met
again in P1.

_Original text follows._

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

_Original text follows._

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

**This entry was wrong, and worth keeping wrong here.** It said pickups and heal spots _work
without this_ and would merely feel bad — true when it was written, and false by the time P5
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

| Failure mode | Result                                                                                                                                                       |
| ------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| tipping      | **PASS** - `maxTilt = 0.0000` for the whole run; the rotation lock never slipped                                                                             |
| sinking      | **PASS** - y range 0.940-1.380 against a settled 0.950. The low is 12 mm of solver penetration, the high is the capsule climbing the step. No downward drift |
| sticking     | **FAIL** - jams on Block B's corner at `(-8.0, 7.0)` on **every lap**, ten times in 130 s, always the same spot                                              |

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

**This fires Decision 1's own trigger.** The plan said: _if P1's gate needs more than one attempt,
stop and do `CharacterVirtual`._ It took three. Two of those were route bugs of mine - an arc that
drifted off the map, then waypoints authored inside solid blocks - but the third failure belongs to
the engine, and it is precisely the one Decision 1 named as most likely to bite.

Wall sliding is the one thing `CharacterVirtual` gives that this needs. Recommended: build it on
`master` before P2, rather than letting every later phase inherit a controller that stops dead on
contact.

### P2 — The map

Several buildings from box colliders, enterable, with interiors. Lighting.

- **Tests:** static mesh rendering at scene scale, box colliders, the four light types, shadows,
  and the asset pipeline under a real content load rather than the eight committed fixtures.
- **Gate:** walk inside and out of every building that has a door; no tunnelling through a wall
  at full speed. **PASSED (M6).** 130 s of Debug `GanymedRuntime` autopilot: `inWall=0`,
  `inWH=0`; `inBH` reached 261 frames across two visits through the `+Z` door. The Warehouse is a
  sealed shell — the honest form of this gate is walk the Blockhouse and bounce off the Warehouse.

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

| Requirement                                        | Why                                                                                                                                                    |
| -------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------ |
| **GLB (glTF 2.0)**                                 | `cgltf` is the only importer                                                                                                                           |
| **Albedo + Normal + _combined_ MetallicRoughness** | The only three map handles `Material` has. A separate AO or emissive map is imported and then ignored                                                  |
| **1 unit = 1 metre**                               | The player capsule is 1.9 m tall: radius 0.35, half-height 0.6                                                                                         |
| **Y-up, -Z forward**                               | What `Player.lua` assumes when it derives forward from yaw                                                                                             |
| **Box-composable silhouette**                      | **There are no mesh colliders.** Every collider is a box, sphere or capsule placed by hand, so a curved wall looks right and collides wrong            |
| **Low poly**                                       | Not for framerate. A cold mesh apply is 5.50 ms and two thirds of that is `GenerateSidecars` writing files on the main thread ([assets.md](assets.md)) |

#### Importing one

1. Drop the `.glb` into the right folder. The content browser's tree refreshes every 0.25 s, so it
   appears on its own - but appearing is not importing.
2. **Content Browser → Rescan assets/.** This is the step that mints the handle and writes the
   `.meta`. The asset watcher does _not_ do it: it tracks known assets for modification and has no
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

#### Generating one through Meshy's API instead

The two characters were generated through the REST API rather than the web app, and it is worth
recording why that mattered: **the settings that were wrong on every hand-imported asset are
request parameters, not toggles you can forget.** The first five assets all arrived with 4096
textures, no normal map and `doubleSided` set; the API run got 2048 and a normal map first time.

The chain is four billed calls per character — `text-to-3d` preview (20 credits), `refine` (10),
`rigging` (5), `animations` (3 per action). What each one taught:

- **`action_ids`, not `action_id`.** The plural form returns ONE glb carrying one clip per
  action. That is the only shape this engine can use: `AnimationSystem::ResolveClip` looks a
  clip up on the mesh asset via `Mesh::FindClip`, so a clip in a separate file can never reach a
  skin. The `basic_animations` that come free with rigging are one-clip-per-file and useless here.
- **`height_meters` fixes scale AND origin.** Every web-app export normalised to a 1.9 unit box
  centred on the origin; `height_meters: 1.8` produced exactly 1.800 m with `y[0, 1.8]`, feet at
  the origin, which is the convention the prefabs already assume. No scale factor, no offset.
- **The rig step strips the normal and metallicRoughness maps.** `enable_pbr: true` works — the
  _refine_ output carries all three — but the rigged output has base colour only. They can be
  grafted back: the UV _sets_ of the refined and rigged meshes overlap 100% (rigging reorders
  vertices for skinning but does not re-unwrap), so the refine task's maps still apply. That is
  what `<Name>_textures/` holds, and why those `.gmat` files are hand-authored rather than
  generated — `GenerateSidecars` only writes when the `.gmat` does not already exist, so
  authoring it up front both supplies the grafted maps and sets `TwoSided: false`.
- **Prompt text cannot set a pose, but it can ruin one.** Asking for a rifle "slung flat against
  the back" produced one held across the chest in a bent-arm ready stance — which auto-rig would
  have weighted across both arms and the chest, stretching the rifle on every walk cycle. Naming
  a weapon at all was the mistake. Re-rolled unarmed, 20 credits.
- **Verify the download, not the render.** Every claim above was measured off the glb before it
  entered the tree: skin present, joint count, clip names, interpolation mode, per-clip root
  drift, ankle-to-toe facing, weight sums, texture dimensions. All free, and all faster than
  noticing in the editor.

The same strip was applied to the three props and the three buildings once their overrides were
in — 122.3 MB of glb across the eight models became 3.7 MB, with `GroundTile1x1x01` going from
**18.67 MB to 0.01 MB** for its 102 triangles. Each map was sha256-compared against its extracted
sidecar first, because the strip is only safe if the sidecar the `.gmat` points at is the same
bytes. All 15 matched.

Not done, deliberately: capping the buildings' maps with `MaxSize`. Their set is 4096 albedo,
4096 normal, 2048 metallicRoughness, which is a defensible choice for architecture you walk up
to — unlike a 4096 two-channel metallicRoughness on a prop, which was pure waste. Capping them
is a visual-quality decision, and it saves no disk at all, only VRAM.

Once the maps come from the `.gmat`, the copy Meshy bakes into the glb is dead weight, and it
costs more than disk: embedded maps bypass the texture manager (`TextureImporter::Upload`, no
handle) and are decoded and uploaded at mesh apply whether or not anything draws with them — see
[assets.md](assets.md). Both characters had theirs removed, which is a real glb rewrite (deleting
an image deletes its bufferView, which renumbers every later one, so accessors get remapped and
the BIN chunk rebuilt). **5.87 -> 0.93 MB and 6.49 -> 1.15 MB**, with every byte of vertex, index,
node, skin and animation data diffed identical before and after.

Two things the API does not fix, both corrected on import: the characters face **+Z** where this
engine's forward is -Z (a pi rotation on the `Body` child), and `doubleSided` is still set.

#### Every `.gmat` in this tree was decorative until now

`StaticMeshComponent::MaterialOverrides` is empty by default, and empty means _use the material
that came with the mesh_. **No scene in this repository had ever filled a slot**, so every `.gmat`
— the three buildings' included, which predate all of this — was written, committed, and never
read by anything.

It is invisible because the generated sidecar is a faithful copy of the imported material: same
albedo, same maps, same flags. It only starts to matter the moment you _edit_ one, and then it
fails silently — the change is simply ignored, with no warning, because nothing is wrong.

That is exactly what happened here. The grafted normal and metallicRoughness maps and
`TwoSided: false` sat on disk doing nothing until the eleven scene slots and the prefab's one were
filled with `MaterialOverrides: [<handle>]`. Confirmed by a cold boot compiling
`ArmoredHumanoid_textures/albedo_0.jpg` for the first time, and by the orks visibly gaining
surface relief.

All fourteen mesh entities now carry a `MaterialOverrides` slot, buildings included.

Worth considering on the engine side: a scan-time note when a `.gmat` exists for a mesh that no
entity overrides. It is the kind of thing that is obvious once seen and invisible before.

#### `doubleSided` is not always Meshy being careless

Three times in a row — the buildings, the characters, the props — the advice here was "Meshy sets
`doubleSided` on everything, flip it". **That is wrong for the buildings, and the way it is wrong
is invisible from outside.**

Set `TwoSided: false` on the Warehouse and stand inside it: the walls vanish. Only the window
frames survive, because they are the only part with thickness. These buildings are hollow shells
whose walls are single-sided, so `doubleSided` is load-bearing geometry information, not an
exporter default to tidy away. Reverted, and they stay `true`.

The characters and the props are closed solids, so culling is correct there and they keep
`TwoSided: false` — verified by looking at each one with culling actually active, which is not
the same as looking at it before the `.gmat` was wired up.

The rule is about the mesh, not the exporter: **cull a solid, keep both faces on a shell.** The
test costs one screenshot from inside.

#### The placeholder boxes are gone, and two things went with them

`BoxTextured.glb` was doing four unrelated jobs. All four references are out of the scene:

- **Block A and Block B** were P1-era cover, superseded by P2's buildings. `ROUTE`'s waypoints
  were authored against them (_"along Block A's east face"_), so the labels are now marked as
  history — the path is what the gate measures, not what it passes.
- **Step** (8 x 0.2 x 1) is deleted on request. It was the thing in the scene deliberately
  exercising `CharacterControllerComponent::StepHeight` (0.4), and nothing was authored to
  replace it. ~~Step-up has no coverage now.~~ **Corrected (P2):** the `GroundTile` pad does it by
  accident - 16 x 16 m at `x [-8, 8] z [2, 18]`, collider top at `y = 0.19`. **M6** authored
  `StepUp Ledge` at `(12, 0.15, 4)`, a 4×4×0.30 m box, in `(0.2, 0.4)`. The P2 autopilot lifted
  to `y=1.21` at the pad (baseline grounded `y=0.95`), so 0.30 m is walked. The cliff at
  `StepHeight` still needs a box taller than 0.4 m.
- **The projectile's 0.15 m cube** is now a particle bolt (below).
- **The Ground** keeps its 100x1x100 collider and its mesh, but wears a new flat
  `materials/Ground.gmat` — no maps at all, just a matte albedo.

**Why the ground is a flat colour rather than the tile texture.** `GroundTile1x1x01` cannot tile:
its top face spans `x[-0.62, 0.82] z[-0.18, 0.49]` out of a +-1 footprint, i.e. it is an
irregular 15-vertex sculpted pad, and at the scale a floor needs that irregularity becomes a
metres-wide seam pattern — tried, screenshotted, reverted. The box's own UVs are no better: they
run `u 0..6`, six faces in a strip, so its top face can only ever wear a sixth of any image,
which at 4096 is 6.8 px/m across 100 m. With no map the UV layout stops mattering.

There is no UV tiling parameter anywhere — not on `Material`, not in `MaterialSerializer`, not in
the shaders. That is what a proper tiled floor would need, and it is the cheapest of the three
ways to get one (the others being a seamless ground texture, or a real terrain/plane asset).

#### Bullets are particles, not sprites and not meshes

A projectile keeps its rigid body and sphere collider untouched — P3's collision behaviour and
gate are unaffected — and only what draws it changed.

`SpriteRendererComponent` was the obvious answer and it is the wrong one. `RenderSystem::
SubmitSprites` draws the quad at the entity's own transform, so it is **not billboarded**; and a
projectile is a dynamic body whose rotation `SyncTransforms` overwrites from Jolt every frame, so
a script could not aim it at the camera either — the same trap that puts the player's facing on a
child entity.

Particles are billboarded, unlit and support additive blending, and **additive + bloom is the only
thing in this engine that can read as a glow**, because there is no emissive channel.
`WorldSpace: true` is what makes it a trail: particles spawn at the emitter's world position and
stay there while the bolt moves on, and their sizes are metres rather than being multiplied by
the projectile's 0.15 scale.

Two things worth knowing before touching the numbers:

- **Additive stacks before bloom sees it.** The first attempt (220/s, colour at 1.0) rendered as
  a solid white pillar under `autofire`, because 220/s at 60 fps is ~3.7 particles a frame
  emitted within 0.47 m of each other. Rate and starting colour both had to come well down.
- **One draw call per live emitter.** Normal fire at a 0.12 s interval keeps roughly 8 alive;
  the `autofire` gate measured **`live=67`**. That is the load to watch if projectiles ever get
  cheaper to spawn, and it is worth a frame capture before assuming it is free.

#### A1: the player carries a weapon (clips only, no socket yet)

The player's clip set was regenerated as a **weapon-carry set**, which is step A1 of
`SKELETAL_ATTACHMENTS.md` on `master`. No engine change is involved and no weapon exists yet: the
character mimes holding one, which is deliberately the cheap disproof before the socket is wired.

| Role | Library action                    | Clip name in the glb          |
| ---- | --------------------------------- | ----------------------------- |
| idle | `334` Lower Weapon, Look, Raise   | `Lower_Weapon_Look_Raise`     |
| walk | `234` Walk Forward While Shooting | `Walk_Forward_While_Shooting` |
| run  | `98` Run and Shoot                | `Run_and_Shoot`               |

**The selection criterion was grip consistency, not individual quality.** A socket offset is fixed
relative to the hand joint, so one offset has to work across all three clips. `511` Rifle Charge
was rejected despite its name because its hands sit low and back, where the other two hold at
chest height — a rifle placed correctly for those two would be wrong in that one.

Measured on the installed file:

| clip                          | duration | head y | hips y | head z | right hand y |
| ----------------------------- | -------- | ------ | ------ | ------ | ------------ |
| `Lower_Weapon_Look_Raise`     | 5.20 s   | 1.501  | 0.965  | +0.070 | 1.149        |
| `Walk_Forward_While_Shooting` | 3.27 s   | 1.526  | 0.970  | -0.023 | 1.432        |
| `Run_and_Shoot`               | 0.67 s   | 1.469  | 0.989  | +0.225 | 1.312        |

**Gate: two of three criteria met, and the third was the wrong criterion.** Head height spread is
5.6 cm and hip height spread 2.4 cm, both at or inside the 5 cm the plan asked for. Forward offset
spread is 24.7 cm, which the plan would call a failure — but that number is the _head's_ lean,
and a run leaning 22 cm further forward than a walk is what running looks like. What the criterion
was actually protecting against is the root sitting off-centre, and the root is centred: the
detrended clip's mean hips XZ is (-1.0, -2.9) cm against a rest pose of (-1.0, -2.9) cm, with
0.000 m net drift. The plan's wording was written against the previous set, which happened to be
unusually upright. **Recorded rather than quietly passed.**

Post-processing, from the plan's "not optional" list:

- **No scale artifact this time.** Both previous generations carried a constant `Hips` scale; this
  one does not. The installer asserts that rather than assuming it, so a silent recurrence fails
  the build instead of shipping a resizing character.
- **`Run_and_Shoot` travelled 1.713 m in 0.67 s** and was detrended and re-centred, same as before.

**The character is now visibly shorter than it was.** Head height went from ~1.65 m across the
unarmed set to ~1.50 m here. That is the braced weapon stance, not a defect — no clip drives a
scale channel off 1.0, asserted above.

**One number that did not fit, and is worth the argument.** `Run_and_Shoot` was authored at
**2.57 m/s**, measured from its own root motion before that motion was stripped. The player moves
at **6.0 m/s**. Matching the feet to the ground exactly would need a 2.34x multiplier, which turns
a tactical jog into fast-forward; it plays at **1.7** instead, so the feet run at ~4.4 m/s and some
slide remains. The principled fix is the other direction: 6 m/s is 21.6 km/h, sprint pace for
someone carrying a rifle, and dropping `Player.Properties.speed` to ~4.0 would let this clip play
at 1.55 with almost no slide. That moves every P1-P7 gate number, so it is **not** done here.

#### A3 — the rifle is in the player's hand

Entity `3000000000000000017`, a child of the player's `Body`, mesh `Rifle.glb` at `Scale: 0.45`
(0.856 m), `BoneAttachmentComponent` on `RightHand` with `Target: 0` (= my parent). One entity —
no compensating child — which is only possible because of the engine fix recorded in
[SKELETAL_ATTACHMENTS.md](SKELETAL_ATTACHMENTS.md): before it, the socket frame was 100x out and a
`Scale: 45` workaround produced an 86 m rifle with a level-sized shadow whenever the socket failed
to resolve.

**How the numbers were derived, because the first two attempts were both wrong on screen.**

The barrel is the rifle's **-X**, established from the vertex profile rather than assumed: a
uniform tube from x -0.95 to -0.24 with nothing hanging below it, the magazine at x -0.21..0.00,
the trigger gap, then the pistol grip at x +0.21..+0.55. Up is +Y.

The first attempt took the barrel axis to be `LeftHand - RightHand`, on the reasoning that the
support hand sits on the handguard. **Measured, that is false for this animation library** — at
idle the hands are 0.937 m apart. The rifle came out 40 deg across the chest, which is exactly
what rendered. The lesson is not about matrices: a premise about what a pose _means_ is as much a
thing to measure as the maths is.

What is true is that the two shooting clips hold the right hand at a stable orientation relative
to the character (14 deg and 22 deg of spread, and 16 deg between the clips). So the barrel is
anchored to **mesh-space +Z** — the direction the rig faces — averaged over those two clips only,
with `Lower_Weapon_Look_Raise` deliberately excluded: that clip swings the hand up to 108 deg,
because lowering and raising the weapon is what it is _for_, and a socketed gun should follow it.

Verified by sweeping every clip rather than one frame:

| Clip                          | barrel elevation | yaw off forward                      |
| ----------------------------- | ---------------- | ------------------------------------ |
| `Run_and_Shoot`               | -0.8 to -2.7 deg | 6.9-8.5 deg                          |
| `Walk_Forward_While_Shooting` | +0.5 to +1.7 deg | 7.6-8.2 deg                          |
| `Lower_Weapon_Look_Raise`     | +16 to -78 deg   | swings — the weapon is being lowered |

And in the engine, from a temporary probe: joint basis `(1.0000, 1.0000, 1.0000)`, joint
translation `(0.015, 1.043, -0.422)` **metres**, socket basis `(0.45, 0.45, 0.45)`.

**`Offset` is the two points that have to coincide, both measured — neither estimated.** The first
pass guessed them off a slice profile (grip at `[0.34, -0.19, 0]`) and a rule of thumb (the hand
4.5 cm past the wrist along the finger direction). Both were wrong — by 4 cm and 7.7 cm — and
compounded into a rifle floating a hand's width clear of the grip, correctly aimed the whole time,
which is what makes that kind of error easy to accept. What they should be:

|               | measured                                                         | how                                                                                                                |
| ------------- | ---------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------ |
| hand centre   | `(-0.0553, +0.0777, +0.0206)` joint space, 9.8 cm from the wrist | weighted centroid of the vertices bound to `RightHand` — only meaningful once the mis-bound hip vertices were gone |
| grip centroid | `(+0.3664, -0.2754, -0.0037)` rifle local                        | centroid of the geometry below the receiver, between the trigger gap and the butt plate                            |

`Offset = handCentre - R * (Scale * grip)`, asserted to land the grip on the hand centre
(err 2e-17). That moved the rifle 8.2 cm to `[-0.2487, 0.1440, -0.0064]`. The muzzle then sits
0.606 m from the hand centre and the butt 0.291 m — consistent with an 0.856 m rifle held at its
grip.

- **Gate: met.** The rifle stays in the hand through idle, walk, run and the backpedal turn, at the
  right size, aimed where the player faces while shooting and lowered while idle, with the grip in
  contact with the hand rather than near it. 0 errors and 0 warnings in the log.

**Not done, deliberately:** the hovering rifle at the Weapon Crate is untouched. Now that the
player always carries a rifle, that pickup wants either retiring or converting to attach-on-collect
— a gameplay decision, not part of wiring the socket.

**Known rough edge** (overtaken: the rifle left the hand in the two-hand IK section below, and the
idle no longer lowers it): during the deepest part of `Lower_Weapon_Look_Raise` the barrel points down
~78 deg and the rifle can clip the thigh. No constant offset fixes that without breaking the
shooting pose; it is a property of socketing a weapon onto generic library animation.

#### The sheet of geometry on the player's leg — a rig defect, not a socket one

Visible while running **and** standing: a flat sheet fanning from the right hip out to the knee.
It survived three wrong diagnoses of mine (the oversized rifle, then the correctly-sized rifle
clipping edge-on, then a specular highlight) before being measured properly.

**Cause.** Meshy rigged this model in an A-pose, where the hands hang beside the thighs. A
proximity-based weight solver cannot tell a knuckle from a hip there, and it bound ~850 hip and
upper-thigh vertices to `RightHand`. They are dragged to the hand whenever the arm raises. The
rig has been like this since it was generated; nothing about sockets was involved.

**Why the obvious checks missed it.** Weights sum to exactly 1.0, every joint index is in range,
there is one influence set and no vertex exceeds four influences — the data is _well-formed_, just
wrong. Nor does distance from the bound joint catch it: in the bind pose the hand really is 27 cm
from the hip, and some offenders sit only 12 cm from the wrist along the forearm axis, inside any
envelope a real hand also has to fit in.

**What does separate them is connectivity.** Reaching a mis-bound hip vertex from the forearm
means walking up the arm, across the shoulder and down the torso. With the mesh welded by position
(UV seams duplicate vertices and would cut the graph) and a BFS out from each forearm, the two
sides separate cleanly — and the **left hand, which this same rig got right, is the control**:

|             | welded verts | hops from forearm (median / 99th / max) |
| ----------- | ------------ | --------------------------------------- |
| `LeftHand`  | 209          | 5 / 8 / **8**                           |
| `RightHand` | 272          | 5 / 28 / **28**                         |

A hop limit of 12 — generous against the left hand's own maximum of 8 — re-weighted 847 vertices.
Weight goes back to each vertex's other influences, which are already `RightUpLeg` and `Hips`;
the 10 with no other influence were assigned by nearest bone _segment_, not nearest joint origin,
which on a limb picks the wrong end.

**Measured result**, by skinning the mesh in Python and comparing triangle areas against bind:

|        | triangles >100 cm² | worst blow-up         | total skinned area (bind 2.825 m²) |
| ------ | ------------------ | --------------------- | ---------------------------------- |
| before | 7                  | 144 cm², **47x** bind | 3.26 m² (+15%)                     |
| after  | **0**              | —                     | 2.885 m² (+2%, normal deformation) |

`ArmoredHumanoid.glb` is edited in place, handle unchanged, original kept at
`archive_unrigged/ArmoredHumanoid.glb.preweights`. The left hand has a milder version of the same
contamination which is below the threshold and currently invisible; if a left-hand sheet ever
appears, lower the hop limit rather than re-rigging.

#### Two more rifle clips, retargeted from a new rig (2026-09-24)

`ArmoredHumanoid.glb` now carries five clips: the three above plus **`Walk_Backward_While_Shooting`**
(library `233`) and **`Walk_Left_with_Gun`** (`528`). The backpedal is wired (see the facing
paragraph); the strafe clip is asset content only. Original kept at
`archive_unrigged/ArmoredHumanoid.glb.preclips`; handle unchanged.

**What was asked for and what exists.** The goal was a two-handed rifle set — aim idle, walk, run,
strafe both ways, backpedal — to fix the lowered idle, the strafe slide past `TORSO_TWIST`, the
reversed-clip backpedal and the left hand that never reaches the rifle (see
[TWO_HAND_IK.md](../history/TWO_HAND_IK.md)). Mixamo is Adobe's and has no API, so it cannot come through Meshy. Meshy's
library (678 actions, listed free at `GET /openapi/v1/animations/library`) has **no rifle set, no
rifle aim idle and no right strafe**; the rifle-adjacent clips are `233` / `529` / `541` backpedals,
`528` / `527` left strafes, and turn-in-place clips.

**The original rig task was gone**, and so was every other task on the account — Meshy deletes
tasks and their assets after a few days (`expires_at`; 3 days for Text-to-Motion). Animations need
a live `rig_task_id`, so the installed glb was re-rigged by upload (`model_url` as a data URI,
`height_meters: 1.8`, 5 credits). Two findings from that:

- **The re-rig decimated the mesh**: 925 vertices back for 14 523 sent. Switching the character to
  the new rig was never an option.
- **The skeleton is the same template but not the same rest pose**: 24 joints, same names, same
  hierarchy, same 0.01 `Armature`, but rest rotations differ by up to 23.8° (`LeftHand`), 17.7°
  (`Spine02`), 15.2° (`Hips`) and offsets by up to 5.1 cm. Keys are relative to each joint's rest,
  so a direct transplant would distort every pose.

**So the clips were retargeted onto the installed skeleton**, offline: per key, each joint's
world-space rotation away from the new rig's rest is applied on top of the installed rest, then
brought back to parent-relative; non-root joints keep the installed bone offsets, and `Hips`
takes the source's offset from rest scaled by the hip-height ratio (1.016). **Validated against a
control** — `234` bought again on the new rig (3 credits) and retargeted, against the installed
`234` from the original rig, joint positions relative to `Hips` over all 99 keys: spine and head
0.3–0.4 cm, hands 3.1–3.7 cm, left foot 7.8 cm worst, 1.4 cm mean, 4.2° worst world rotation. That
is an upper bound on the method's error: each library clip is already Meshy's own fit to a rig.

Then, same as A1: root motion detrended and re-centred on the rest pose (`233` travelled 1.22 m,
`528` 1.31 m; 0.000 after), scale channels dropped (all within 1e-6 of 1), and the clips appended
to the existing glb — rotation on all 24 joints plus `Hips` translation, 25 channels each. The mesh,
indices, inverse binds, materials and the three original clips are byte-identical; the new keys
read back within 0.03°. In the engine: the importer lists all five clips with no warning, and
playing them puts the rifle where the offline numbers said.

**What each clip is worth, measured with the rifle placed as the socket places it:**

| Clip                           | Hands apart | Right→left hand line   | Barrel vs forward (current socket) | Verdict                                                                                                                                                                                    |
| ------------------------------ | ----------- | ---------------------- | ---------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `Walk_Backward_While_Shooting` | 42 cm       | 27–31° across the body | +4..+7° yaw, +3..+6° up            | **Same grip family as the forward walk** — the socket fits. The backpedal clip                                                                                                             |
| `Walk_Left_with_Gun`           | 38–39 cm    | **3–5° off forward**   | **−26° yaw**                       | **A real forward two-handed hold**, hands exactly the rifle's grip-to-handguard length apart — but crouched (head 1.20 m vs ~1.5) and a different grip, so the current socket skews it 26° |
| Text-to-Motion aim idle        | 31–32 cm    | 35–38° across          | +90° yaw, +39° up                  | **Rejected**: hands crossed at the chest, not a forward aim; not imported                                                                                                                  |

The aim idle came from Text-to-Motion (`prime`, 4 s): the first prompt timed out server-side
(0 credits), the shorter retry succeeded (10 + 3 to apply) but produced the same across-the-chest
hold the library has. Prompting harder may or may not fix that; it was not retried.

**Spent: 27 credits** (rig 5, library clips 9 including the `234` control, Text-to-Motion 13);
balance 623 → 596. The tooling is kept next to the API key in `D:\Projects\C++\meshy\`
(`meshy_rig.py`, `meshy_animate.py`, `meshy_retarget.py vet|install`), with this run's rig task
and downloads in `work\2026-09-24\`; `install` reproduces the installed asset byte for byte.

**Open, and each needs a decision rather than more download:**

- **Backpedal speed.** The clip is wired, capped at 2x playback against a 6 m/s backpedal. Most
  shooters backpedal at a fraction of the forward speed; ~2 m/s would let this clip play at ~2x
  with almost no slide. A movement change, so it is a gameplay decision, not done here.
- **Strafe wiring.** `Walk_Left_with_Gun` wants a strafe mode that faces the aim and plays it,
  reversed for right, and either accepts the crouch or is not used.
- **`528`'s hands sit on a forward rifle line**, so a clip-specific rifle placement along it
  would put the left hand on the handguard with no IK. IK was chosen instead, so the grip stops
  depending on how each clip and each future model happens to be authored — see
  [TWO_HAND_IK.md](../history/TWO_HAND_IK.md).
- **Still no aim idle.**

#### Two-hand IK — the rifle on the chest, both hands on it (H5 of [TWO_HAND_IK.md](../history/TWO_HAND_IK.md), 2026-09-24)

**What changed in the scene.**

- **The `Rifle` moved from `RightHand` to `Spine`:** `Offset [0.173818, 0.000528, 0.397881]`,
  `Rotation [3.039984, 0.884929, -2.91941]`, `Scale` unchanged at 0.45.
- **Two markers were added beside `Muzzle`:** `Grip` (`3000000000000000018`) and `Support`
  (`…019`).
- **`Body` gained `TwoHandIKComponent` with `AimLock: 1`**, taking the default chains and marker
  names.
- **`Player.lua` changed only in comments.** Two of them described the hand socket.

Nothing else in the game changed.

**How the numbers were derived, all measured.** The engine dumped the joint frames of all five
clips every 1/30 s (`TryGetJointFrame`, the socket's own path), and the pose was solved offline
over that dump.

- **`Grip`** is the right wrist where A3's measured grip-to-hand fit put it. That is the inverse
  of the old socket offset, so the grip A3 verified is kept exactly.
- **`Support`** puts the left hand's centre on the rear of the handguard: local x −0.30, on the
  `meshy_retarget.py` handguard line just under the bore, forward of the magazine.
  - The centre is the weight-weighted centroid of the vertices bound to `LeftHand`:
    (−0.0103, 0.1034, 0.0075) in the wrist frame. The same method on the right hand gives
    (−0.0529, 0.0720, 0.0198), within 6 mm of A3's independent measurement.
  - The support hand's orientation comes from `Walk_Left_with_Gun`, the one clip with a real
    forward two-handed hold. It is the left wrist relative to that clip's right-to-left hand line,
    taken at the medoid frame; the frames spread 1.5°.
- **The weapon pose.** Under the aim lock the rifle's orientation is the aim's, so only where the
  socket puts the `Grip` pivot matters, which is three numbers in the `Spine` frame. Reach does
  not depend on the aim: the shoulders and the rifle both ride `Spine`, which carries the aim
  offset's whole rotation. The H4 sweep had already shown clamped counts that did not change
  across 49 aims.

**The finding: a textbook shouldered hold does not fit this rig.** The arms are 0.534 m (right)
and 0.521 m (left), and the rifle is 0.856 m.

- **Butt pinned in the right shoulder pocket, support hand mid-handguard:** the left hand clamps
  on 65–99 of the forward walk's frames and on all 39 of the backpedal's.
- **Minimising reach alone** puts the rifle on the sternum at whatever forward limit it is given,
  which is a hug, not a hold.
- **The resolution came from the support hand, not the rifle.** With the palm at the rear of the
  handguard, a shouldered pose reaches everything. The butt is 6 cm medial of the right shoulder
  joint, 1 cm below it and 8 cm in front of it. Of 424 poses that kept every shooting frame at or
  under 93% reach, this one was taken for a worst case of 87%. The socket's own rotation is the
  lock's orientation relative to `Spine` at the shooting clips' medoid frame.

**Measured in the engine, over every clip at 1/30 s through the real pass, aim at 0.** These
reproduce the offline model to three decimals:

| Clip                             | Right reach | Left reach | Clamped (L)  | Wrist → marker, reached frames | Barrel vs aim | Lock turns the rifle |
| -------------------------------- | ----------- | ---------- | ------------ | ------------------------------ | ------------- | -------------------- |
| `Walk_Forward_While_Shooting`    | 53–54%      | 74–79%     | 0 / 99       | ≤ 3.9e-7 m, 3.8e-7 rad         | 7.5e-5°       | 0–6.8°               |
| `Run_and_Shoot`                  | 50–51%      | 62–66%     | 0 / 21       | ≤ 2.8e-7 m                     | 6.5e-5°       | up to 44.7°          |
| `Walk_Backward_While_Shooting`   | 60–61%      | 78–87%     | 0 / 39       | ≤ 3.0e-7 m                     | 3.0e-5°       | 8.9–11.9°            |
| `Walk_Left_with_Gun`             | 57%         | 52–55%     | 0 / 38       | ≤ 3.6e-7 m                     | 3.3e-5°       | up to 54.8°          |
| `Lower_Weapon_Look_Raise` (idle) | 52–61%      | 34–112%    | **43 / 157** | ≤ 5.1e-7 m                     | 7.9e-5°       | up to 74.7°          |

- **No clamped frame in any shooting clip.** That is the plan's gate.
- **The idle is held up at the chest, as the plan wanted.** The lock also keeps it on the aim,
  and the clip's lowering is gone.
- **The idle's left hand clamps for 1.4 s of its 5.2 s loop (3.07–4.47 s), up to 12% of an
  arm (about 6 cm short).** That is when the idle's chest looks around (±70°) while the lock
  holds the rifle on the aim. No weapon pose fixes it; **an aim idle does.** The "still no aim
  idle" item above now has a second reason.
- **Run and strafe need the lock.** Unlocked, a rifle riding their forward-pitched chest points
  37–42° down. Locked, it is level.

**Seen**, in four frames from the editor (walk, run, idle at 2.7 s, idle at its worst clamp): butt
at the shoulder, right hand on the pistol grip, support hand forward of the magazine, barrel on the
aim.

- No candy-wrapper twist is visible at the wrists, despite the lock turning the rifle up to 75°.
- The worst idle frame's miss is hidden behind the body from that angle.
- The left hand's milder weight contamination (above) did not show in those four frames. They
  were not a sweep.

**P5 gate, as the representative of P1–P7.** There were three runs of 180 s each, in Debug
`GanymedRuntime`, with `p5gate` flipped for the run and restored after:

- **Before:** hand socket, no IK.
- **After:** Spine socket, IK and lock. Run twice.

Every check the P5 gate defines matched in all three runs:

- the heal, weapon and upgrade triggers, within 0.2 s of each other;
- the upgrade bought, and `ui-mismatch=0`;
- `fired=142`, `despawned=142`, `live=0`;
- `inWall=0`;
- route complete, and 0 errors.

The counters after the route (damage taken, times downed, final position) differ. They differ just
as much _between the two identical "after" runs_: health after probe 1 read 64 in one and 82 in
the other. So that difference is run-to-run variance, not the IK.

The rest of P1–P7 was not re-run, on this argument: IK changes only the skinning palette and where
the rifle is drawn. Physics reads neither, and the gate modes never capture the cursor, so they
fire from the chest rather than the barrel.

**Not measured:**

- **The barrel-versus-chest shot count.** `BarrelPoint` only runs under mouse aim, which no gate
  mode drives.
- **`BarrelPoint`'s ~35° check was kept, as a guard.** With the lock at 1 the barrel is on the aim
  offset's angles to 1e-4°. The check still catches the aim fade (`AIM_BLEND_TIME`) and parallax
  at close range, because the lock aims along the chest's line to the aim point and the muzzle is
  half a metre off the chest. The comment in `Player.lua` now says that.

#### What the character import left open

- **Three facing/animation defects, three unrelated causes** — all fixed, all worth remembering
  because none of them was visible in the asset and each failed differently:
  - _The player did not turn with the camera._ `Body` hung off the capsule, which is
    `LockRotation`, so it inherited no yaw. Reparented under `Yaw`. The caution recorded here
    earlier — that this would disturb what the gates measured — was wrong: the gates measure the
    capsule's position and its raycasts, and reparenting a **mesh** changes neither.
  - _The orks ran backwards._ `Enemy.lua` writes `SetRotation(0, facing, 0)` onto the Body every
    frame, which **overwrote** the pi baked into the scene rather than composing with it, so the
    correction was erased on the first update. The pi belongs in the script. `self.facing` itself
    must not change, because `Sense()` raycasts along it.
  - _The player looked smaller when running._ It was the opposite: the **Idle** clip drove a
    constant scale of **1.1765 on `Hips`**, the root joint, and nothing else — so the player was
    17.6%% too large while standing still. A Meshy retarget artifact (1.1765 = 1/0.85). Removing
    the 24 scale channels from that clip is enough, because `AnimationSystem::BuildPalette`
    starts from the rest pose and a joint with no channel keeps its authored transform.

  The general lesson: **a mesh whose rig faces +Z needs its correction wherever the rotation is
  last written.** In the scene for anything static, in the script for anything a script turns.

- **The run clip was the wrong clip, and library clips need checking before they are trusted.**
  Removing the Idle scale did not fix "the player looks smaller when running", because the run
  was a second, unrelated problem: library action 16 "Run Fast" is a head-down sprinting lunge.
  Measured against Idle, its hips sat 16 cm lower and its head 45 cm further forward, so the
  character really was shorter and hunched. Regenerated with action **532 "Run Fast 4"**, chosen
  by fetching the library's `preview_url` GIFs and comparing mid-stride frames — free, and far
  better than guessing from names. Head height across the three clips is now 1.646 / 1.649 /
  1.604, and hips 1.102 / 1.090 / 1.063.

  Two things the new download needed before it could be used, neither of which the first
  generation had:
  - **Real root motion.** `run_fast_4` travelled **3.759 m in 0.67 s**. Nothing extracts root
    motion, so it would have slid forward and snapped back every loop. Fixed by subtracting the
    straight line from first key to last on the Hips' X and Z — which removes the travel while
    keeping the sway and bob that make a run read as a run, and leaves the last key equal to the
    first, which is what a looping clip wants. Y is untouched.
  - **A 1.53 m authoring offset.** Detrending alone was not enough: the clip _starts_ 153 cm
    forward of the origin, so the mesh rendered a metre and a half in front of its own entity.
    The keys are re-centred on the rest pose afterwards. Idle and Casual_Walk sit within 3 cm of
    it, which is how the offset was spotted.

  That clip's own root motion also sets its playback speed: 3.759 m / 0.67 s is 5.6 m/s against
  the player's 6.0, so it plays at **1.0** rather than the 0.6 the old lunge needed.

- **The mesh faces its velocity, not the camera.** The first attempt at "running backwards"
  negated the animation speed, which plays the stride in reverse — a moon-walk. What was wanted
  was the character turning round and running forwards. `Player:Animate` now turns the `Body`
  toward the velocity heading, eased over a couple of frames so tapping S does not pop.

  This also closed the strafe gap recorded here earlier: A and D used to slide sideways facing
  forwards, because there is no strafe clip. Facing the movement direction means the forward run
  is always the right clip, whichever way the stick is pushed.

  **That stopped being safe once the rifle was socketed** (A3). The gun points wherever the body
  points, so a body facing its velocity shot sideways on every strafe, and standing still it did
  not follow the mouse at all. Facing then grew a third mode, and the aim offset is what splits
  the legs from the torso:
  - **Aiming and moving, outside the backpedal** — `meshYaw` follows the velocity, turned toward
    the aim only as far as keeps the torso within `TORSO_TWIST` (60°): the legs target
    `camera yaw − clamp(twist, ±60°)`. `AimOffsetComponent` on `Body` takes
    `wrap(camera yaw − meshYaw)` and the elevation from the chest (`capsule.y + 0.5`, the same
    point `Fire` uses) to `AimPoint`. The chain is `Spine02` / `Spine01` / `Spine`, root-most
    first (`Spine02` is the child of `Hips` in `ArmoredHumanoid.glb`). Weights are the component
    defaults 0.10 / 0.20 / 0.30, which normalise to a sixth, a third and a half, least on the
    waist. An even split was rejected because the waist would swing the arms and the rifle.
    These were not scrubbed against the overlay.
  - **Backpedal** — aiming and moving more than 115° off the aim, left again under 100°: the body
    faces the camera's yaw, eased at 20/s, and plays `Walk_Backward_While_Shooting` (below) at
    `speed / 0.96` capped at 2x — the clip was authored at 0.96 m/s and the backpedal runs at the
    full 6 m/s, so the feet slide the difference. The reversed forward clip is gone. Standing and
    aiming also faces the camera's yaw.
  - **Not aiming** — the velocity rule above, unchanged.

  **Two things the first wiring got wrong, both found by runtime probes after A3.** It switched
  modes at exactly 90° off the velocity, which is where a pure A/D strafe lands, so float
  rounding picked the branch: at a camera yaw of 0.7 the twist came out one ulp past π/2 on all
  586 frames, the strafe mode never engaged, and a 90° twist was the spine's whole job whenever
  it did. And the offset was written as `(0, 0)` the frame aiming ended — the chest twist went
  from 45° to 0 in one 22 ms frame. Now the inputs are held while aiming and faded by a 0.25 s
  smoothstep weight, so the chest tracks the mouse directly while aiming and eases only in and
  out. Re-probed at the camera yaw that failed: a pure strafe holds strafe mode on every frame
  with zero flips, legs 30.0° off the velocity, torso 60.0°; moving straight at the camera holds
  the backpedal on every frame; the largest one-frame change of the offset is 0.111 rad on release
  and 0.136 rad on fade-in, against 0.785 before.

  **Shots converge on the crosshair** when a human is aiming (`Player:AimPoint`). The camera ray
  is cast from the player's depth along it — an Enemy standing between camera and player put the
  first aim point behind the gun — steps past `PG.passThrough` sensors, and the round flies from
  the chest point toward what it hit, pitch included. The gate modes never capture the cursor and
  still fire flat along yaw, so their numbers do not move. Verified with a temporary probe in the
  runtime: pitch −0.15 hit Ground 12 m ahead (dir y −0.132), +0.25 went to the 200 m far point
  (dir y +0.260), −0.35 hit Ground 2.2 m ahead, yaw 1.2 hit an Enemy; the body reached the aim yaw
  within ~0.1 s and `aimHold` drained to 0. Not yet watched by a human with the mouse, and the
  aim-mode backpedal was not exercised.

  **Rounds and the flash now leave the barrel.** `Muzzle` is a child of the Rifle at rifle-local
  `(-0.97, 0.19, 0)` — 2 cm past the bore, whose tip face was measured off the glb's vertices
  (x -0.952, y 0.12–0.26, z ±0.06) — rotated `(0, pi/2, 0)` so its -Z runs down the barrel (the
  rifle's -X) and its +Y, the particle cone, stays the rifle's up. `Player:BarrelPoint` reads it
  with `GetWorldPosition` / `GetWorldForward` and falls back to the chest point unless the barrel
  is clear of the capsule, ahead along the yaw, **and** pointing within ~35 deg of the aim point.

  The third condition is the one a probe forced. With only the first two, a shot while standing
  left the idle clip's lowered barrel at knee height (y 0.36) and climbed to the crosshair. A
  second probe run: the idle's floor-pointing barrel and a body still turning toward the aim both
  fell back to the chest; shots while running left the barrel at shoulder height (y ~1.67) along
  the shot line, and hit an Enemy charging down it ~9 m out (Impacts spawned; none at the player).

  **Still open, and content:** standing fire always takes the chest fallback, because
  `Lower_Weapon_Look_Raise` never holds the rifle on target. An aim-idle with a stable grip,
  chosen by the same grip-consistency rule as A1, is what makes standing shots come from the gun.
  The share of shots that leave the barrel rather than the chest, over a scripted strafe-and-shoot
  run, was not re-logged after the aim offset. The strafe case is what that number should move;
  it has not been measured.

- **`Fox.glb` is now unreferenced** and still in the tree. It is the only asset with clips that
  were not generated by Meshy, which makes it the one independent check that the animator is not
  merely agreeing with one exporter. Keep it until there is a second source.
- **The three props' oversized maps are capped, not fixed.** `MaxSize` in each `.meta` bounds
  what `TextureCompiler` encodes and uploads, so the 4096 metallicRoughness maps cost 1024 in
  VRAM and in `.compiled/`. **The source pngs on disk are untouched** — ~91 MB across the three,
  counting both the sidecars and the copies embedded in the glbs. Only a re-export shrinks that.
  The `retexture` endpoint would do it at 10 credits each, at 2k with `enable_pbr`, and would
  also give them the normal maps none of them have.
- **The health station's status light does not glow.** There is no emissive channel anywhere in
  the engine, so the green dot is albedo lit like any other surface. A `PointLightComponent`
  child with a small radius is the cheap fix, at the cost of one light slot.
- **Nothing here is in git-lfs.** `.gitattributes` has no LFS rules and `.git` is already 1.6 GB,
  so every one of these binaries goes in as a plain blob, permanently. The characters are ~6 MB
  each now rather than ~30 MB, which buys time rather than fixing it.

#### The colliders did not match the meshes, on either building

Nothing keeps a hand-placed box set honest. The editor does not compare the boxes to the mesh they
were eyeballed against, there is no warning when they disagree, and the disagreement is invisible
from outside - a wall you walk through and a wall you walk into look identical until you touch
them. Both buildings were wrong, in opposite directions.

The audit is worth recording because it is cheap and repeats on any box-composed building.
Rasterize the glb's triangles into a 5 cm grid over the horizontal plane, keeping only what falls
inside the capsule's height band (floor+0.15 m to floor+1.85 m). Do the same for the box set. Diff
them: a cell with mesh and no box is a wall you walk through, a cell with a box and no mesh is an
invisible wall. Then flood-fill the grid inward from outside the footprint with the capsule's
0.35 m radius, once for the mesh and once for the colliders - "is the interior reachable" is the
only question either representation has to answer, and the two answers have to agree.

**The two meshes have exactly one walkable opening between them.** The Blockhouse has a single
doorway in its `+Z` wall, local `x [-0.47, 1.40]`, 1.87 m wide, open from 0.12 m above the floor
(a threshold) to 3.11 m. Nothing else on either building is open at walking height: the
Blockhouse's two other apertures are windows whose sills sit at 2.5 m, and the Warehouse has no
aperture at all, at any height, on any face. **It is a sealed shell and was always meant to be
one** - `doubleSided` is what lets you see its interior, not a way in.

The box sets said something else entirely:

| Building   | Wall | Mesh                   | Collider set                                                        |
| ---------- | ---- | ---------------------- | ------------------------------------------------------------------- |
| Warehouse  | `X-` | solid                  | 0.90 m gap at `z [-2.35, -1.45]`                                    |
| Blockhouse | `X+` | solid                  | 1.40 m gap at `z [-0.71, 0.69]`                                     |
| Blockhouse | `Z-` | solid                  | 1.80 m gap at `x [1.68, 3.48]`                                      |
| Blockhouse | `Z+` | **the 1.87 m doorway** | 1.00 m gap at `x [-3.67, -2.67]`, and the doorway itself walled off |

The Blockhouse had three holes, not one of them where its door is, and its one real door was solid
wall. The Warehouse had a 0.90 m hole against a 0.70 m capsule - wide enough, and only just, which
is why it reads as an intermittent bug rather than an open gate. Every gap is the right _sort_ of
gap, roughly door-sized and centred on a wall face, so this looks less like a series of slips than
like one door plan applied to the wrong faces.

The corrected sets, local to each building's parent, are scene data only - no engine change is
implied and none was made:

| Entity                     | Translation            | Half extents                     |
| -------------------------- | ---------------------- | -------------------------------- |
| `Blockhouse Wall X-`       | `[-3.85, 0, 0]`        | `[0.15, 2.28, 3.58]` (unchanged) |
| `Blockhouse Wall X+`       | `[3.85, 0, 0]`         | `[0.15, 2.28, 3.58]`             |
| `Blockhouse Wall Z-`       | `[0, 0, -3.43]`        | `[4, 2.28, 0.15]`                |
| `Blockhouse Wall Z+ Left`  | `[-2.235, 0, 3.43]`    | `[1.765, 2.28, 0.15]`            |
| `Blockhouse Wall Z+ Right` | `[2.7, 0, 3.43]`       | `[1.3, 2.28, 0.15]`              |
| `Blockhouse Door Lintel`   | `[0.465, 1.555, 3.43]` | `[0.935, 0.725, 0.15]`           |
| `Warehouse Wall X-`        | `[-9.85, 0, 0]`        | `[0.15, 4.23, 5.775]`            |

Two entities went away with the merges, so the Blockhouse carries eight children and the
Warehouse six. The jamb pieces are named `Left`/`Right` rather than duplicating one tag, because the tag is what
`Enemy.lua` prints in `blocked-by=` and two walls with the same name make that line ambiguous.

The lintel is the one box that is not a wall segment. The doorway is 2.99 m tall in a 4.56 m wall,
so leaving the gap full height would have left a metre and a half of open air above the door that
the mesh draws as solid. It costs one box and it keeps the shell's silhouette true to the mesh for
raycasts, which is the only thing that can tell the difference.

**The 0.12 m threshold in the doorway is deliberately not collided.** The player would walk over it

- `CharacterControllerComponent::StepHeight` is 0.4 - but P4 measured the other half of that: the
  enemies are flat-bottomed boxes, and a box stops dead at a kerb a capsule rides over. Collide the
  threshold and the door becomes passable for the player and impassable for everything chasing them,
  which is worse than a capsule clipping 12 cm of doorstep. A 0.12 m threshold would not have added
  much anyway - the `GroundTile` pad's 0.19 m lip already covers step-up at that scale, and what is
  missing is a ledge near the 0.4 m limit, which is a [ToDo](README.md) item rather than a doorstep.

**What this costs P4.** Its occlusion probe was authored against the Blockhouse's `-Z` hole, in the
belief that it was the doorway, and it is now solid; the Sentry at `(18.4, -6)` is still inside,
but the only way to it is the `+Z` door at world `x [15.53, 17.40]`, `z = -2.57`. The gate's
numbers were never wrong - acquisition at `px = 17.2` against a predicted 17.11 is exactly what a
collider edge at `x = 17.66` produces - and its conclusion that buildings occlude where their
colliders are still holds. The sentence after it does not: _"which also says the colliders
hand-placed in P2 line up with the mesh they were measured from."_ They did not, and that gate
could not have found out, because **a raycast gate against colliders can only ever prove the
collider set self-consistent.** Nothing that queries physics can see a box that disagrees with the
mesh drawn over it; that needs the mesh, which is what the audit above reads. Re-running the probe
means new positions on the `+Z` side.

#### M6 — walk the corrected set, 130 s Debug `GanymedRuntime`

`Player.lua` `FOOTPRINTS` still treated the old holes as doors. M6 removed them: the Blockhouse
keeps only the `+Z` doorway, the Warehouse none. Without that, a clip through a now-solid wall at
an old gap would not have incremented `inWall`.

```
GATE t=10s  pos=(17.5, 0.95, -2.0)  inWall=0 inBH=0   inWH=0
GATE t=15s  pos=(10.8, 1.21,  3.9)  inWall=0 inBH=96  inWH=0
GATE t=100s pos=(16.9, 0.95, -3.8)  inWall=0 inBH=109 inWH=0
GATE t=105s pos=( 8.1, 1.12,  4.8)  inWall=0 inBH=261 inWH=0
GATE t=130s pos=(20.2, 0.95, -2.1)  inWall=0 inBH=261 inWH=0
```

Two visits through the `+Z` door, never inside the Warehouse, never inside a wall. Stuck-escapes
at `(-32.4, -8.6)` are the capsule leaning on the sealed `-X` face, which is the slide test the
route was written for.

The Warehouse was not rebuilt from nothing in the map tool. The `1df1654` boxes already match the
glb POSITION AABB × scale 10 to < 3 mm. Generate-from-mesh on the hollow shell would fill the
interior. Recorded in `docs/ToDo/MAP_EDITOR.md` on `map-editor`.

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

### P4 — Enemies with eyes — **PASSED**, including the `+Z` re-run

Waypoint patrol, line-of-sight acquisition via raycast, charge. Animation on the enemies.

- **Tests:** P0.3's raycast, the animator under many simultaneous instances, and whether buildings
  actually occlude.
- **Gate:** an enemy behind a building does not acquire the player; stepping into the doorway does.
  **PASSED** on the original `-Z` hole, and **PASSED** on the real `+Z` door in M6.

#### What was built

Seven enemies, each a two-entity rig: a dynamic, rotation-locked body carrying `Enemy.lua` and the
box collider, and a `Body` child carrying the mesh, the `AnimatorComponent` and the facing. The
child exists for the same reason the player's `Yaw` does — the body is `LockRotation`, so Jolt
hands `SyncTransforms` the same orientation every frame and a yaw written on the parent is erased
before anything can see it.

Six patrol a two-point leg authored per instance as script `Fields` (`label`, `patrolTo`); the
seventh, the **Sentry**, stands still inside the Blockhouse and exists for the gate.

`Fox.glb` **was** the enemy mesh, copied out of the editor's sample models, superseded by
`Ork.glb` once the Meshy pipeline produced a rigged humanoid. It was the wrong shape for one
— 2.8 m long against a 0.7 m collider.
It was chosen anyway because it is the only asset in either tree with **more than one clip**
(`Survey` 3.417 s, `Walk` 0.708 s, `Run` 1.158 s, 24 joints), and "the animator under many
simultaneous instances" is not tested by six copies of the same clip.

#### The premise this phase overturned

`Enemy.lua`'s P3 header said: _P4 makes them move, and that is when they become character
controllers._ **Wrong, and wrong in a way that would have deleted P3's result.**

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
doorway between them. **That gap was not a doorway.** The `-Z` wall is unbroken in the mesh, and
the 1.8 m was a hole in the box set - see
[P2](#the-colliders-did-not-match-the-meshes-on-either-building). It is closed now, the real door
is on `+Z`, and the run below is not reproducible as written. What it measured is unaffected: the
probe only ever queried colliders, and every number in it is a correct measurement of the
colliders that existed at the time. The Sentry stands at `(18.4, -6)` and never turns. A sight
line from there to a player at `(px, -12)` crosses `z = -9.43` at `x = 18.4 + 0.5717 * (px - 18.4)`, so the wall's
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

#### Gate run 1b — the `+Z` re-run (M6)

`LOS_ROUTE` already stood on the `+Z` side. The first M6 losgate acquired nothing: the Sentry
still faced `-Z`, so the door was `blocked-by=behind` and no transition was logged. Vacuous.
`facing` is now a script property; the Sentry is authored at yaw π, looking through the door.

```
LOS Sentry t=8.2s  ACQUIRED player=(14.5, -1.0)   # crossed the door walking to probe 2
LOS Sentry t=8.6s  lost     player=(16.7, -0.2)   blocked-by=Blockhouse Wall Z+ Right
LOSGATE probe 2 at (19.70, 0.89), 8.0s            # behind the east jamb — no acquire
LOS Sentry t=21.8s ACQUIRED player=(15.8,  2.4)   # doorway sight line
LOSGATE probe 3 at (15.58, 1.33), 8.0s            # held
LOS Sentry t=31.3s lost     player=(17.1, -1.5)   blocked-by=Blockhouse Wall Z+ Right
LOSGATE probe 4 at (19.88, 0.68), 6.0s            # stayed lost
LOSGATE route complete
```

The occluder named is the east jamb of the real door. Probe 2 and probe 4 both sit in the Sentry's
view cone now — the wall, not the FOV, is doing the blocking.

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
without a character controller is the _shape_, not the body type, and P1's finding should be read
as being about capsules specifically.

**The player was pinned for 40 s, and it is not an engine bug.** From `t=40s` to `t=80s` the
capsule sat at `x = -12.7` with `z` sliding between `-8.9` and `-10.8` - **inside the Warehouse**,
which it had entered through the `X-` hole P2 describes above, and which is sealed now. The `X+` wall
is at world `x = -12.15` with a 0.15 m half-extent and the capsule's radius is 0.35, which puts a
body pressed against its inner face at exactly `-12.65`. The controller was working — it slid along
the wall the whole time. What failed is the autopilot's stuck escape: it advances to the _next
waypoint_, with no notion of whether that waypoint is reachable, so once it was inside a building
with every remaining target outside it, it simply leaned on the nearest wall until a later waypoint
happened to line up with the door. **Decision 5's cost, arriving where Decision 5 said it would.**
The building it was trapped in has no door, so "a later waypoint lined up" was the autopilot leaving
through the same hole it came in by. The failure that demonstrates - escaping to the next waypoint
with no notion of whether it is reachable - is real and unchanged; the Blockhouse is now the only
building that can stage it.

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
  about what it costs, and it cost the _autopilot_, not the enemies.

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

**And a static box is still invisible to it.** The inner body is _Kinematic_, and Jolt refuses to
pair two non-dynamic bodies — with exactly one exemption, which is sensors. So P0.4, filed as
"optional, do it if P5 feels bad", turned out to be **required**: without `IsSensor` a pickup is not
merely solid-when-it-should-be-walkthrough, it is undetectable. P0.4's own text said pickups "work
without this", and that was true when it was written and false by the time P5 arrived.

#### What was built

`Pickup.lua`, one script for all three kinds, each a static **sensor** box with a cube child so
there is something to see:

| Tag               | Kind    | Behaviour                                                              |
| ----------------- | ------- | ---------------------------------------------------------------------- |
| `Heal Spot`       | heal    | permanent; heals 14/s while you stand in it, using enter/exit to count |
| `Weapon Crate`    | weapon  | consumed on touch, destroys itself, halves the fire interval           |
| `Upgrade Station` | upgrade | permanent; spends 2 score for +1 projectile damage, refuses if poor    |

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
_through_ the pickups rather than dying on them.

#### Three defects this phase found, all of them real

**1. `UI.SetScore` took an `int`, and every script property is a Lua float.** Buying an upgrade
computed `score - cost` where the cost came from a property, so the result was a float, and sol2
with `SOL_ALL_SAFETIES_ON` refused it — "not a numeric type that fits exactly an integer". The
throw escaped into the frame and took the rest of `OnUpdate` with it, so the run continued looking
healthy while the player stopped updating. Fixed on `master`: the binding takes a `double` and
truncates, which is what `EmitBurst` and `SetParticleMaxParticles` already did. **The precedent
existed and this binding had not followed it.**

**2. An enemy shoved the player through the ground plane.** Giving the player presence made a
charging enemy able to move it, and a `CharacterVirtual` resolves an overlap by moving _itself_,
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
  the spawn point" does not exist. Death recovers the player _where it fell_ instead, which is not
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
  by a _moving_ player for the first time, and `EmitBurst` under load.
- **Gate (defined here):** one run of autopilot plus autofire, the whole circuit. Every shot that
  is actually fired flashes and bangs and nothing else does; every impact emitter is reaped;
  component-owned voices track what is alive; one-shots drain to zero; the HUD reads back what
  gameplay wrote. Zero errors.

#### What was built

| Piece                     | What it exercises                                                                       |
| ------------------------- | --------------------------------------------------------------------------------------- |
| `ui/hud.rml` + `hud.rcss` | the data model P5 drives, live, plus a crosshair                                        |
| Footsteps on the player   | `AudioSourceComponent` and the `StopSound`-then-`PlaySound` retrigger                   |
| Weapon report             | unspatialised `Audio.PlayOneShot`, at the listener by definition                        |
| Impact sound              | **positional** one-shot — the only sound with real work for the 3D listener             |
| A hum on every enemy      | seven spatialised _moving_ looping sources, which the listener had never been driven by |
| Music bed                 | streamed, looping, unspatialised, on the `Music` group                                  |
| Muzzle flash              | `EmitBurst` on a local-space emitter parented to `Yaw`, so it points where you look     |
| `Impact.gprefab`          | one emitter entity per hit, spawned at the contact point, self-destructing              |

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
the defects would cluster: _memory or handle growth over a session longer than any probe has ever
run_.

**2. The gun fired blanks under load.** `muzzle-bursts=2602` against `fired=1894` with
`refused=708`, and `1894 + 708 = 2602` exactly. Every shot the per-frame spawn cap turned away
still made a noise and a muzzle flash. Visible only because the two counters were kept separately —
either one alone looks fine. Fixed by moving the sound and the burst _after_ the spawn succeeds;
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
chrome survives opening someone else's project. In a shipped layout the working directory _is_ the
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

A mint on a _writable_ install is ordinary. A mint on a **read-only** install is a shipping defect:
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

| Predicted                                      | Outcome                                                                                                                                                                 |
| ---------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| P7's read-only asset path                      | **Hit.** A shipped asset with no `.meta` mints a fresh handle on every boot and said nothing about it; the count sat in the middle of an INFO line that reads as normal |
| The animator under many simultaneous instances | **Missed.** P4 drove six instances across three clips, switching independently in the same frames, and nothing broke. The animation milestone's work held               |
| Memory or handle growth over a long session    | **Hit, and it was audible.** A destroyed entity's audio voice was never released — it kept playing, at its last position, for the rest of the session                   |

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

| Phase | Found                                                                                                                 | Fixed                                       |
| ----- | --------------------------------------------------------------------------------------------------------------------- | ------------------------------------------- |
| P0/P1 | velocity-driven capsules cannot slide along a wall                                                                    | `CharacterVirtual`                          |
| P2    | importer invented tangents; albedo decoded as linear; textures loaded upside down; no mip chain                       | all four                                    |
| P4    | a raycast cannot see a character controller                                                                           | documented, then fixed in P5                |
| P5    | `UI.SetScore` refused a float; a character could be shoved out of the world; a static box is invisible to a character | binding fixed, sensors added, push recorded |
| P6    | a destroyed entity's voice was never released; `PlayOneShot` had no volume; voice counters were unreachable           | all three                                   |
| P7    | a read-only install mints silently; Dist wrote a 1.7 MB trace log; the executable is not self-contained               | first two fixed, third recorded             |

Eleven fixed, five recorded in [cross-cutting.md](cross-cutting.md) as decisions rather than
oversights. **None of them were found by a probe**, and every one was found by the game doing
something ordinary for longer than a probe runs.

The honest caveat: this milestone verified almost everything through the log. The renderer output,
the HUD layout and the audio mix have been **measured and not looked at**. Whatever is wrong with
how any of it looks or sounds is still there.
