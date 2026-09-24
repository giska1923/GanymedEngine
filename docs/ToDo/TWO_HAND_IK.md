# Milestone — Two-hand weapon IK

**Status: H1–H4 done on `master` (the solver; the component, the pass, serialization and Lua;
the inspector section and the overlay; the aim lock). H5 done on `first-game` (the Proving Ground
content), and its notes exist only in this branch's copy of this file. H6 planned.** H3's three
interactive checks (drag the rifle, drag a marker, scrub) have not been done by hand yet; see
H3's notes.

> **Engine and editor milestone.** H1–H4 touch `GanymedEngine/source/` or `GanymedEditor/source/`,
> which the [branch policy](PROVING_GROUND.md#branch-policy) puts on `master`. H5 is game content
> and lands on `first-game` after `master` is merged into it — never the other way.

The rifle is posed from the character's chest and the aim; both hands are then solved onto grip
points authored on the rifle. The clip drives the body and legs. The arms follow the weapon.

---

## Why now, and why this shape

Today the rifle hangs off `RightHand` through a `BoneAttachmentComponent` with one hand-tuned
offset, and the left hand does whatever the clip does. On the Proving Ground that means the left
hand is never on the rifle. Two measurements (2026-09-24, `ArmoredHumanoid.glb`, rifle placed exactly
as the socket places it; method and tooling in `PROVING_GROUND.md` on `first-game`, "Two more
rifle clips") decide the design:

| Clip | Left wrist to handguard | Shoulder to grip point (arm 52.1 cm) | Right→left hand line vs forward |
|---|---|---|---|
| `Walk_Forward_While_Shooting` | 22–25 cm | 54–57 cm | 32–34° across |
| `Run_and_Shoot` | 35–36 cm | 61–64 cm | 36–38° across |
| `Walk_Backward_While_Shooting` | 23–24 cm | 53–54 cm | 27–31° across |
| `Lower_Weapon_Look_Raise` | 78–106 cm | 66–109 cm | — |

1. **Left-hand IK on the existing socket cannot work here.** The grip point is past the left arm's
   reach in every clip, because the rifle's placement comes from the right hand and these library
   clips hold an imaginary rifle ~35° across the body. IK would lock the elbow straight and still
   stop short.
2. **The fix is to stop letting the right hand decide where the rifle is.** Place the rifle
   relative to the chest, where both hands can reach, and solve *both* arms onto it.

**The decision is made for robustness, not for this one clip set.** Every future model, rig and clip
set would otherwise need its rifle re-measured and re-placed per clip, and the left hand would still
be wherever the clip author left it. With a weapon pose on the chest and grip points on the weapon,
a new character needs one weapon pose; a new weapon needs two markers; new clips need nothing.

**It also closes the aim loop.** The aim offset bends the spine but leaves the barrel at "clip
elevation + pitch", up to 78° off in the idle. A weapon posed from the chest, optionally locked to
the aim direction (H4), points where the player aims whatever the clip does.

**Prerequisites that already exist:**

| Capability | Where |
|---|---|
| Joint globals after sampling and the aim pass, before the palette | `AnimationSystem::Evaluate` → `ApplyAim` → `Global * InverseBind` |
| A subtree-rotation helper over globals, with a per-entity cache keyed on topology | `ApplyAimOffset`, `AimSubtreeCache` |
| Joint frame in mesh space (unit basis, metres) — the one owner | `TryGetJointFrame` (`Mesh.h`) |
| Placing an entity relative to a joint, with an editor gizmo | `BoneAttachmentComponent`, the socket gizmo |
| Marker children that follow a socketed parent | `TransformSystem::OverrideWorld` → `RecomputeSubtree`; the `Muzzle` under the Rifle |
| Reading a socketed entity's drawn world from Lua | `Entity:GetWorldPosition` / `GetWorldForward` |
| The pose a rig is drawn with, animator or not | `ResolvePosePalette` |

## How production engines do it, and where Ganymed diverges

**Unreal**'s mannequin skeleton ships IK bones for this (`ik_hand_gun`, `ik_hand_l`, `ik_hand_r`):
the weapon follows `ik_hand_gun`, and a *Two Bone IK* node in the Animation Blueprint pulls the left
hand onto the weapon. Heavier setups drive the weapon from an aim pose and IK *both* hands, which is
what shooters with procedural recoil and aim-down-sights do. **Unity**'s Animation Rigging does the same with
a *Two Bone IK Constraint* per arm, targets parented to the weapon, and a *Multi-Parent* or
*Multi-Aim* constraint posing the weapon. Both solve after the animation graph, before skinning.

**Ganymed follows the "weapon posed from the aim, both hands IK'd" shape**, as a pass inside
`AnimationSystem` after the aim offset, with two divergences:

- **The weapon pose is a `BoneAttachmentComponent` on a chest joint**, not a new transform type.
  Sockets, the socket gizmo and marker children already exist; a weapon pose is a socket whose joint
  is `Spine` instead of `RightHand`.
- **Grip targets are marker entities under the weapon**, not IK bones in the rig. Ganymed's rigs
  come from Meshy and are not authored here; adding bones means owning a rig format, which
  [SKELETAL_TOOLING.md](../history/SKELETAL_TOOLING.md) already declined.

## What it deliberately is not

- **Not finger posing.** The Meshy rig has no finger joints; the hand is a mitten and will not close
  on the grip. It reads acceptably from a camera 5 m behind.
- **Not full-body IK or FABRIK.** Two analytic two-bone chains. No spine or leg solving.
- **Not foot IK.** Feet stay where the clip puts them.
- **Not recoil, sway or ADS.** The weapon inherits the chest's motion; procedural motion on top is
  follow-up work.
- **Not weapon switching or holstering.** One weapon entity per character, enabled or not.
- **Not per-clip offsets.** One weapon pose per character; that is the point.
- **Not IK in the clip inspector.** It keeps measuring clips.

## Phases

| Phase | What | Branch | Size |
|---|---|---|---|
| **H1** | Analytic two-bone IK over globals, with boot self-tests — **done** | `master` | ~0.75 day |
| **H2** | `TwoHandIKComponent`, weapon frame resolution, the pass, serialization, Lua — **done** | `master` | ~1 day |
| **H3** | Inspector section, reach readouts, overlay — **done** (interactive checks pending) | `master` | ~0.75 day |
| **H4** | Aim lock: the barrel onto the aim direction — **done** | `master` | ~0.5 day |
| **H5** | Proving Ground: rifle onto `Spine`, `Grip`/`Support` markers, measured — **done** | `first-game` | ~0.75 day |
| **H6** | Docs, close | both | ~0.25 day |

~4 days. **H4 is the phase to cut** if the budget tightens: H1–H3 and H5 give a two-handed grip;
H4 makes the barrel exact.

---

## Phase H1 — the solver

### Goal

A pure function that moves a shoulder–elbow–wrist chain so the wrist lands on a target position and
orientation, over the same joint-global arrays `ApplyAimOffset` works on.

### Steps

1. Beside `ApplyAimOffset` in `Animation.h` / `AnimationSystem.cpp`:

   ```cpp
   struct TwoBoneChain { int32_t Upper, Lower, End; };   // e.g. RightArm, RightForeArm, RightHand
   // Moves Upper and Lower (and their subtrees) so End's origin reaches `target` where it can,
   // with the elbow on the side of `pole`, then sets End's rotation to `targetRotation`.
   // Blended by `weight` in [0, 1]; weight 0 leaves `globals` bit-identical.
   TwoBoneResult SolveTwoBone(const Skeleton&, const TwoBoneChain&, const glm::vec3& target,
       const glm::quat& targetRotation, const glm::vec3& pole, float weight,
       std::vector<glm::mat4>& globals, subtree lists...);
   ```

2. **Law of cosines.** Bone lengths from the current globals (not the rest pose, so a clip that
   scales still works). Distance shoulder→target clamped to `[|L1 − L2| + ε, L1 + L2 − ε]`; the
   clamp is reported, not hidden — `TwoBoneResult{ Reached, Stretch }`.
3. **Pole.** The elbow bends in the plane of shoulder, target and pole. Default pole: **the clip's
   own elbow position**, so the arm keeps the clip's style and a walk still swings the elbow. When
   the clip's elbow is within a few degrees of the shoulder→target line (plane undefined), fall back
   to a per-chain hint in character space (down and out).
4. **Apply in globals**, the aim pass's way: rotate Upper's subtree about the shoulder so the elbow
   lands, rotate Lower's subtree about the elbow so the wrist lands, then set End's rotation (its
   children follow). Subtree lists come from the same topology cache.

### Decisions, with reasoning

**Analytic, not iterative.** Two bones have a closed form; CCD or FABRIK buys nothing on a chain of
two and costs iterations and jitter.

**The target is the wrist frame, not the palm.** A marker says "the hand joint goes here, rotated
like this". Authoring a palm point would need a per-rig hand-to-palm offset (measured once on this
rig: the right hand's centre is 9.8 cm from the wrist) that every new rig would need measured again.
With the skeleton overlay on, placing a wrist marker is direct.

### Verification

| Probe | Pass condition |
|---|---|
| Weight 0 | Globals bit-identical |
| Reachable target, 2-bone probe skeleton | Wrist at target within 1e-4 m; bone lengths unchanged within 1e-5 |
| Target past reach | `Reached == false`, arm straight toward the target, wrist on the line at full length |
| Pole on the left vs right | The elbow lands on the pole's side; the elbow angle is identical |
| Pole on the shoulder→target line | Fallback hint used; no NaN |
| End rotation | Wrist global rotation equals the target within 1e-4 rad |

Boot self-tests in Debug, like the aim offset's — no timing loop in them.

### Execution notes (2026-09-24)

Landed as `SolveTwoBone` in `Animation.h` / `AnimationSystem.cpp`, beside `ApplyAimOffset`. The
current behaviour is described in [scene.md](../engine/scene.md#animationsystem--systemsanimationsystemh).
These notes cover where the build departed from the steps above.

**The signature settled like this.** The subtree lists live on `TwoBoneChain` (`Subtrees[3]`,
`SubtreeCounts[3]`, caller-owned), as they do on `AimOffsetChain`, and the chain carries its own
`PoleHint`. The plan left both as "subtree lists..." and "a per-chain hint".
`TwoBoneResult` gained a third field, **`Valid`**. Without it, a chain the solver refuses (for
example `Lower` not under `Upper`, or a zero-length bone) would read the same as "out of reach",
and those are different failures for H3's readout.

**Five decisions the plan did not make:**

- **What the weight means.** The weight blends the *goal*: the position lerps and the rotation
  slerps from the current wrist frame to the target, and then the solver solves fully. This is how
  Unity Animation Rigging's two-bone constraint blends. The alternative was scaling each solved
  delta rotation, but the forearm's delta is computed after the upper arm has already moved, so
  that does not blend cleanly. With the clip's elbow as the pole, a partial weight is continuous
  from the clip pose.
- **What `Reached` and `Stretch` describe.** Both describe the *full-weight* target. H3's readout
  should show how far the marker is from the arm, not how far a faded solve landed.
- **ε is relative:** 1e-5 of the chain length, not a fixed distance. The same arm measures 0.55 on
  a metre rig and 55 on a centimetre one, so a fixed ε would be wrong on one of them.
- **The pole threshold is 3°.** Past it the solver falls back to the hint, then to any
  perpendicular if the hint is also on the reach line. The switch is hard; see the risk below.
- **Chain validity is checked every call.** The solver refuses the chain unless `Lower` is in
  `Upper`'s subtree and `End` is in `Lower`'s. The check is a linear scan of an arm's subtree
  (four or five joints on the Meshy rig).

**Verification,** measured with the Debug editor's boot self-test. Each case ran on a metre rig and
on a centimetre rig (`RootTransform` = scale 100). Errors are in metres. The measurements came from
one temporary logging build, and the log lines were removed afterwards:

| Probe | Pass condition | Metre rig | Centimetre rig |
|---|---|---|---|
| Weight 0 | Globals bit-identical | identical | identical |
| Reachable target | Wrist within 1e-4 m | 1.5e-8 | 7.6e-8 |
| | Bone lengths within 1e-5 m (upper / lower / hand tip) | 6.0e-8 / 3.0e-8 / 7.5e-8 | 3.8e-8 / 3.8e-8 / 1.9e-7 |
| | Chest and head bit-identical, shoulder origin fixed | yes, 1.5e-8 | yes, 0 |
| End rotation | Within 1e-4 rad | 4.6e-8 rad | 1.4e-7 rad |
| Target past reach (Stretch 1/0.55) | `Reached == false`, wrist on the line at full length, arm straight within 1° | Stretch 1.81818, 5.6e-6 short, 0.513° | 1.81818, 5.5e-6, 0.513° |
| Pole +X vs −X | Elbow on the pole's side, same elbow angle | x = ±0.178, Δangle 7.2e-7 rad | ±0.178, 4.8e-7 rad |
| Pole on the reach line, and on the shoulder | Hint used (elbow below, the hint is down), no NaN | elbow y −0.187, wrist 1.6e-7 | −0.187, 1.7e-7 |
| Weight 0.5 (added) | Wrist on the halfway goal | 3.7e-8 | 8.5e-8 |
| Not one limb (added) | `Valid == false`, globals untouched | yes | yes |

The 5.5 µm shortfall and the 0.51° bend at full reach are the ε clamp, and both match the
prediction from ε = 1e-5. The rotation check is `2·atan2(|v|, |w|)` of the delta quaternion. The
two aim probes' inline `2·acos(|dot|)` cannot express an error between 0 and about 7e-4 rad in float, so it
cannot test a 1e-4 tolerance (see
[cross-cutting.md](cross-cutting.md#aim-offset-leftovers)).

Builds checked: Debug engine and editor, and Release engine, all with no warnings. No source files
were added, so premake does not need to regenerate the projects.

**What H2 inherits:**

- The pass builds each chain's three subtrees in a topology-keyed cache like `AimSubtreeCache`.
- It passes the clip's own elbow (`globals[Lower]` origin) as `pole`.
- It sets `PoleHint` to "down and out" in the space `SampleClipGlobals` writes, turned from mesh
  space the way `AimAxesInSkinSpace` turns the aim axes.

**Risks that only the rig will show** (H2/H5 should check for them on `ArmoredHumanoid`):

- **Candy-wrapper at the wrist.** The forearm swing is shortest-arc, and the hand takes its whole
  target rotation, so a marker rotated far about the forearm's axis twists the wrist's skinning.
  The Meshy rig has no twist joints. If H5 shows it, the usual fix is to give the forearm a share
  of the hand's twist about the forearm axis, which is a few lines in `SolveTwoBone`.
- **Elbow pop at the 3° switch.** This can happen if a clip's elbow crosses 3° of the reach line
  mid-clip. It is unlikely with the rifle clips, whose elbows are well bent.
- **Elbow speed near full reach.** Near full reach, `d(angle)/d(distance)` goes to infinity, so a
  marker that sits just inside reach will make the elbow flutter. H5's rule of no clamped frame in
  the shooting clips should keep the markers away from full reach. If it does not, the standard
  remedy is soft IK, as in Unreal's two-bone node with stretch limits. That remedy is not in H1.

---

## Phase H2 — the component and the pass

### Goal

A character with a `TwoHandIKComponent` has both arms solved onto its weapon's markers every
frame, after the aim offset and before the palette, in edit mode and in Play.

### Steps

1. **`TwoHandIKComponent`** on the rigged entity (`Body`):

   | Field | Serialized | Meaning |
   |---|---|---|
   | `Weapon` | yes | UUID of the weapon entity; 0 = the first child that has a `BoneAttachmentComponent` |
   | `RightChain` / `LeftChain` | yes | Three joint names each; defaults `RightArm/RightForeArm/RightHand`, `LeftArm/LeftForeArm/LeftHand` |
   | `RightMarker` / `LeftMarker` | yes | Child names on the weapon; defaults `Grip`, `Support` |
   | `RightWeight` / `LeftWeight` | yes | 0–1 |
   | `Enabled` | yes | |
   | `Resolved`, results | no | Joint indices, marker UUIDs, `Reached` / `Stretch` per hand — `Trait::Runtime` |

2. **Weapon frame, computed inside the pass.** The weapon's world is written by
   `BoneAttachmentSystem`, which runs *after* `AnimationSystem`. The pass therefore recomputes it:
   `anchorJointFrame(post-aim globals) × OffsetMatrix(weapon attachment, weapon scale)`, then
   `× marker local transform` for each hand. No cycle: the anchor (`Spine`) is not in either arm
   chain, so the weapon frame is final before the arms move. **`OffsetMatrix` moves out of
   `BoneAttachmentSystem.cpp` into a shared header**, and the frame goes through `TryGetJointFrame`,
   so the pass and the socket system compute the same matrix — one owner, as S1 argued.
3. **The pass**, in `Evaluate` after `ApplyAim`: resolve names, build targets, `SolveTwoBone` right
   then left (independent chains), then the palette. `AnimView` gains `OptRW<TwoHandIKComponent>`;
   the weapon and marker reads are declared `AccessView<RO<BoneAttachmentComponent>,
   RO<TransformComponent>, RO<RelationshipComponent>>`.
4. **Checklist**: `ComponentList`, reflection (runtime fields `Trait::Runtime`, so undo keeps them
   live for free), serializer (omit-if-default), `Scene::Copy` needs **no new sweep** if every
   runtime field is recomputed each frame — verify, do not assume.
5. **Lua**: `Entity:SetHandIKWeight(right, left)` for future reload or lowered-weapon states.
6. **Warn once** per entity for an unresolved joint or marker; skip that hand, never half-solve.

### Risks

- **The anchor joint's frame must match what the socket system uses** — same `TryGetJointFrame`,
  same `OffsetMatrix`, same scale handling — or the hands land a few centimetres off the rifle the
  player sees. Asserted in H2's verification, not assumed.
- **The left hand's weights.** `PROVING_GROUND.md` records mild contamination on the left hand below
  the visible threshold. IK moves the left arm further than any clip did and may expose it.
- **Declared access across entities.** `AnimationSystem` reading another entity's attachment and
  transform is new coupling. `ValidateOrdering` must still pass.

### Verification

| Probe | Pass condition |
|---|---|
| Both markers reachable | Each wrist within 0.5 cm of its marker (Python vet, reproduced in a runtime probe via `GetWorldPosition`) |
| Weapon frame agreement | Pass-computed weapon frame vs `WorldTransformComponent` of the weapon next frame: same within 1e-4 m |
| Weight 0 / disabled | Palette bit-identical to no component |
| A marker out of reach | Arm straight, `Reached == false`, readout says so |
| Missing marker | One warning, that hand untouched |
| Save / load | Round-trips; runtime fields never written |
| Cost | Profile scope, Release, one character |

### Execution notes (2026-09-24)

Landed on `master`. The current behaviour is in [scene.md](../engine/scene.md) (the component, the
pass and `BoneAttachmentSystem`), [ecs.md](../engine/ecs.md) (what `ValidateOrdering` now checks)
and [scripting.md](../engine/scripting.md) (`SetHandIKWeight`). These notes cover where the build
departed from the steps above.

**1. `ValidateOrdering` would have rejected the planned access.** `BoneAttachmentSystem` declared
`RW<BoneAttachmentComponent>` only because it wrote the component's runtime `Resolved` index.
`AnimationSystem`, which is registered earlier, reading that component therefore counted as a stale
read, and the `Scene` constructor asserts on any stale read. The validator works per component, not
per field. The pass never reads `Resolved`, but the validator cannot know that.

The options were:

- declare `RW` in `AnimationSystem`, which would be false;
- read through `Entity` without declaring, which hides the coupling the plan wanted visible;
- give the status to the system that computes it. **This is what was done.**

`Resolved` left the component. `BoneAttachmentSystem` rebuilds a per-frame entity→joint map and
answers `ResolvedJoint(entity)`, and it now declares the component `RO`. The editor's three readers
(the socket gizmo query, the joint highlight, and the Offset-versus-Translation gizmo switch) ask
the system instead. Several resets are gone:

- eleven `Resolved = -1` resets: `Scene::Copy`, `DuplicateEntity`, prefab save, scene load, two
  Lua bindings, and five editor edits;
- the reflected `Resolved` field itself.

The joint-index cache is gone too: the lookup is a linear search by name every frame. A side effect is
that `AnimationSystem`'s slot after the script systems is now enforced (see ecs.md).

**2. No `Weapon` UUID field.** The weapon is always "the first child whose socket targets this
rig". On the Proving Ground the rifle is a direct child of `Body` with `Target: 0`, which is the
only case there is. An explicit UUID needs a remap in the same three places `Target` has
(`DuplicateEntity`, prefab save, `ResolveHierarchy`) plus an entity picker in H3, all for a case
nobody has. Adding it later is additive: omit-if-default, with no migration. H3's inspector shows
which weapon was found instead of offering a picker.

**3. Other departures from the steps:**

- **`OffsetMatrix` is a member of `BoneAttachmentComponent`, not a new shared header.** It sits
  beside the data it reads, as `TransformComponent::GetLocalTransform` does. No new source file, so
  no premake regeneration.
- **Six strings, not two arrays.** There is a YAML codec for `std::array<std::string, 4>` only.
  Named fields also read better on disk.
- **Weight 0 still measures.** The pass resolves and runs the solver, which reports reach without
  moving anything, and rewrites no palette entry. H3's readout therefore works with the hands off.
  `Enabled` is the off switch.
- **The palette is built once, then patched.** The pass runs *after* the full palette loop. It
  reads the weapon's frame through `TryGetJointFrame` on that palette, which is the literal call and
  data `BoneAttachmentSystem` uses. It then rewrites only the solved arms' entries. The socket joint
  is refused if it lies inside the arm being solved; on the old `RightHand` setup that hand carries
  the weapon and the other still solves.
- **The `PoleHint` is derived from the rig.** It points away from the weapon's joint with the
  vertical removed, then down, in the globals' space. No authored field was needed.

**4. Verification.** The rig is `ArmoredHumanoid.glb` from `first-game`, opened by this `master`
build through `--project=` on a sparse scratch worktree. Nothing was merged or committed there. The
scratch scenes were copies of `ProvingGround.ganymede` with the clip set to
`Walk_Forward_While_Shooting`:

- the rifle moved to `Spine` at the placement that reproduces its `RightHand` pose at t = 0
  (Offset `[0.1286, 0.1461, 0.4825]`, Rotation `[3.0660, 0.7473, −2.9730]`);
- `Grip` 3.2 cm off that frame's right wrist;
- `Support` 12 cm nearer the barrel than the left wrist.

The measurements came from temporary logging, since removed:

| Probe | Pass condition | Result |
|---|---|---|
| Both markers reachable | Wrist within 0.5 cm | 1.5e-7 m / 8.4e-8 m, rotation 2.6e-7 / 1.7e-7 rad (stretch 0.61 / 0.94) |
| Weapon frame agreement (as drawn) | — | Marker `WorldTransformComponent` vs `bodyWorld × TryGetJointFrame(final palette, wrist)`, same frame: 1.3e-7 m / 7.5e-8 m. Stronger than the planned frame-vs-frame check: it also covers marker composition and both space conversions |
| Every clip, every 1/30 s, reached frames | — | Max 4.5e-7 m, 4.4e-7 rad across all five clips (352 frames) |
| Weight 0 / disabled | Palette bit-identical | Both bit-identical (`memcmp`) to no component |
| Right weight 0.75 | — | Wrist stops 0.80 cm from the marker: 25% of the 3.2 cm nudge |
| A marker out of reach (`Support` ~1.3 m further out) | `Reached == false` | `Reached` false, `Stretch` 2.07. Arm straightness is H1's probe |
| Missing marker (`SupportX`) | One warning, that hand untouched | One warning. Left `Valid` false, right solved |
| Save / load | Round-trips, runtime fields never written | Default writes `TwoHandIKComponent: {}`. `RightWeight: 0.75` survives; a hand-written default `LeftUpper` is dropped. The reloaded scene reproduces every number above |
| Cost | Release, one character | **2.8 µs** per call (mean of 352). Debug: 302 µs before the warning prefix stopped being built on every call |
| Boot | — | `ValidateOrdering` passes, `Reflection initialised: 41 types, 161 members`, validation passed |

Builds checked: Debug and Release editor, Debug runtime, all with no warnings. `tsc --noEmit` is
clean over `scripts-src`.

**5. What H5 inherits from the scratch placement.** This is information, not a failure; the
placement was a test fixture. With the rifle posed from the walk's t = 0:

- The left hand **clamps in 46 of 157 frames of `Lower_Weapon_Look_Raise`**, at stretch up to
  1.03, and reaches 0.997 in the backpedal.
- Every shooting clip reached on every frame. The right hand stays at 0.55–0.66.

The weapon pose H5 authors needs the `Support` side pulled nearer the left shoulder, to keep
clear of the H1 near-full-reach risk.

**6. Not in H2, so the next phase has to:**

- **No Add Component entry and no inspector section.** Both are explicit per component in
  `SceneHierarchyPanel.cpp`, and both are H3. Until then the component comes only from a scene file.
- **First-frame socket warning.** `BoneAttachmentSystem` warns "targets 'Body', which has no rigged
  mesh" on the first frame of the *unmodified* Proving Ground, while the mesh is still loading. The
  warning predates H2 and is misleading. See
  [cross-cutting.md](cross-cutting.md#skeletal-leftovers-after-the-attachment-and-tooling-close).

---

## Phase H3 — inspector, readouts, overlay

1. Inspector section, plus the Add Component entry (neither exists yet): the weapon that was found
   (read-only; H2 has no weapon field), chain joint combos off this entity's skeleton, marker
   names, weights. **Readout per hand** from the runtime `Valid` / `Reached` / `Stretch`: reach as
   a fraction of arm length at the current frame, and "clamped" when the target is out of reach —
   the number that makes a weapon pose tunable. `Valid` false should say which thing failed; the
   warning text in `ApplyTwoHandIK` already names it.
2. **Overlay** (Skeletons visualizer on): a small cross at each marker, a line wrist→marker in the
   accent colour when not reached.
3. The rifle's weapon pose is placed with the existing **socket gizmo** on the rifle
   (`BoneAttachmentComponent` on `Spine`); markers with the ordinary gizmo. No new gizmo.

| Probe | Pass condition |
|---|---|
| Drag the rifle with the socket gizmo | Hands follow live; reach readout updates |
| Drag a marker past reach | Readout says clamped; overlay line appears |
| Scrub the animator | Hands stay on the markers through the clip |

### Execution notes (2026-09-24)

Landed on `master`. The current behaviour is in [editor.md](../editor/editor.md) (the section) and
[scene.md](../engine/scene.md) (`HandStatus`, the overlay). These notes cover where the build
departed from the steps above, and what was and was not verified.

**1. The pass states its verdict; the editor only words it.** Step 1 asked for the readout to say
which thing failed. Re-deriving that in the editor would have duplicated the pass's rules, and
could drift from them: the weapon search, name resolution, the marker lookup, the
socket-inside-the-arm test. So the pass records it:

- `TwoHandIKComponent::HandStatus` per hand, set at each point where the pass gives up on a hand;
- the `Weapon` and `Markers` UUIDs it found.

`Status == Solved` replaces H2's `Valid` array, which it made redundant. All of these are
runtime, cleared every evaluation. The overlay reads the same fields. Reflection went from 161 to
163 members.

**2. The marker cross is a coloured X/Y/Z cross, not a plain one.** A marker is a wrist *frame*,
and lining its axes up against the hand joint's is half the authoring job. A single-colour cross
would show where the marker is but not how it is turned.

**3. The overlay lives in `RenderSystem::DrawSkeletonGizmos`, not the editor.** That is where the
skeleton overlay already is, so the markers draw in Play too and follow the same selection and
X-ray rules. `RenderSystem` declares `AccessView<RO<TwoHandIKComponent>>`, so `ValidateOrdering`
keeps it after `AnimationSystem`.

**4. Found, not fixed: the editor font has no em dash.** The inspector font is Inter, loaded with
ImGui's default glyph range (Latin-1), so U+2014 renders as "?". This affects eleven existing
UI strings across four panels, including the aim-offset readout. The new strings use ASCII. See
[cross-cutting.md](cross-cutting.md#editor-text-outside-latin-1-renders-as-).

**Verification.** The rig was `ArmoredHumanoid` from `first-game`, through the H2 scratch
worktree and scenes. A temporary startup hook selected `Body`, framed the camera and scrolled
the inspector; the hook has been removed. Evidence is screen captures of the running Debug editor
(PowerShell `CopyFromScreen`), read back as images:

| Probe | Result |
|---|---|
| Section and readout, both markers in reach | Weapon line "Rifle (socket on Spine)", the six combos and both markers correct. "Reach 61% of the arm" (right) and "Reach 87%" (left), matching H2's measured stretch of 0.608 |
| Marker past reach (`Support` 1.0 m further along the barrel) | Left readout, in the accent colour: "Clamped: marker at 276% of the arm". The viewport shows the left arm straight toward the muzzle and a wrist→marker line off-frame. Sampled colour (183, 175, 186) against (131, 130, 125) background: the accent, washed pale |
| Marker cross at the wrist | Pale red and green strokes crossing at the left wrist's joint star. `Grip` is occluded by the rifle when viewed from the left, as expected with depth test on |
| Missing marker (`SupportX`) | "The weapon has no child named 'Support': hand on the clip.", wrapped. The Marker combo lists the weapon's children, so the fix is one click |
| Boot | No ordering violation, no warnings. Debug and Release editor and Debug runtime build with no warnings |

**Not verified: the three interactive rows of the table above.** Dragging the rifle with the socket
gizmo, dragging a marker past reach, and scrubbing the animator all need hands on the editor. The
harness here can launch it and capture it, but cannot drive it. Also not exercised:

- undo of a combo edit;
- multi-select propagation;
- the `Disabled`, `NoWeapon`, `NoWeaponFrame`, `NoJoint`, `WeaponInArm` and `Unsolvable` wordings,
  whose statuses come from code paths H2 already measured, and which have not been seen on screen.

The live-follow behaviour is structural: the pass reads the socket and marker transforms every
frame in edit mode, and H2's clip sweep measured the hands on the markers through all five clips.
But nobody has watched it. **Before H5, check the three rows by hand.**

---

## Phase H4 — aim lock

**Goal:** the barrel points at the aim direction exactly, not "chest orientation + weapon pose".

1. The aim direction in mesh space comes from the entity's `AimOffsetComponent` inputs (yaw about up,
   then pitch about the yawed right — the same construction `ApplyAimOffset` uses).
2. After the weapon frame is computed and before the hands are solved, rotate the weapon frame about
   its `Grip` marker so its forward axis (a per-weapon `Forward` marker or the weapon's −Z) matches
   the aim direction, blended by an `AimLock` weight. **Measured in H2: the Proving Ground rifle's
   barrel is its local −X, not −Z** (its `Muzzle` sits at local `[−0.97, 0.19, 0]`), so a fixed
   axis convention is already wrong for the one weapon there is. Use a marker, such as the
   existing `Muzzle`.
3. **The weapon entity must be drawn where the hands are**: `BoneAttachmentSystem` gains the same
   correction for a weapon referenced by a `TwoHandIKComponent`, or the pass writes the corrected
   frame somewhere the socket system reads. Decide in H4; do not let the two diverge.

| Probe | Pass condition |
|---|---|
| Aim pitch/yaw sweep, idle clip frozen | Muzzle forward vs aim direction within 0.5° |
| AimLock 0 | Identical to H3 |

### Execution notes (2026-09-24)

Landed on `master`. The current behaviour is in [scene.md](../engine/scene.md) (the lock in the
pass, and the socket's locked branch), [editor.md](../editor/editor.md) and
[scripting.md](../engine/scripting.md) (`SetAimLock`).

**Step 3, decided: the pass owns the aimed frame.** It writes `LockedWeaponFrame` onto the
component, and `BoneAttachmentSystem` draws the weapon from it while `AimLockState == Locked`. It
reads the frame through a declared `OptRO<TwoHandIKComponent>`, so the ordering is checked.

The rejected option was giving `BoneAttachmentSystem` the same correction. That would have made
two computations of one fact, with the aim-offset inputs duplicated into a second system. Keeping
the old socket multiply as the only unlocked path is also what makes "AimLock 0 identical to H3"
true by construction rather than by float luck.

**Step 2, as built:**

- **Barrel = an `AimMarker` child's −Z**, default `Muzzle`. The plan's "the weapon's −Z" was
  already wrong for the rifle, whose barrel is its local −X. The existing `Muzzle`'s rotation of
  `(0, π/2, 0)` maps its −Z onto that barrel. So the forward is the engine's forward convention
  (`GetWorldForward`, lights), read off a marker the weapon already has.
- **A look-at with up, not a shortest arc.** Not a plan item. The barrel goes on the aim, and the
  muzzle's +Y goes as near mesh up as the aim allows, so a rolling chest does not cant the rifle.
  This is Unity Multi-Aim's world-up shape.

  The consequence: under a full lock the socket's rotation no longer matters, only where it puts
  the grip. The Bone Attachment section says so when a lock is active, because otherwise the
  Rotation rows and the socket gizmo look dead. **Author the weapon pose with Aim Lock at 0.**
- **The aim is the aim offset's `Pitch` / `Yaw`**, clamped by one shared function
  (`ClampAimAngle`) that `ApplyAimOffset` now also uses. The lock needs an
  `AimOffsetComponent`, and it ignores that component's `Enabled` flag, which only governs the
  spine bend.

**Also added:**

- `AimLock` defaults to 0, so a scene written before H4 is unchanged.
- The runtime `AimLockState` / `AimLockAngle` feed an inspector readout, and `SetAimLock(weight)`
  was added in Lua for a lowered weapon.
- Reflection went from 163 to 168 members.

**A bug I made and fixed before it shipped.** A lock that failed (no aim offset, or no aim marker)
while both hands solved let the pass count the frame as clean. That erased the warning set, so
the lock warning would have fired every frame. The lock failure now keeps the set armed. A scene
with `AimMarker: Barrel` logs exactly one warning over about 25 seconds.

**Verification.** The rig was `ArmoredHumanoid` from `first-game`, through the scratch worktree.
Temporary code, since removed:

- drove the aim offset's `Pitch` over {−1.2, −0.6, −0.3, 0, 0.3, 0.6, 1.2} and its `Yaw` over
  {−2, −1, −0.5, 0, 0.5, 1, 2}, so 49 cells including past both limits, each held 4 frames in edit
  mode;
- measured, at the end of `BoneAttachmentSystem`, the *drawn* `Muzzle` world −Z against an aim
  direction rebuilt independently from the component's fields and the body's world rotation;
- measured the drawn wrists against the markers' worlds.

| Probe | Pass condition | Result |
|---|---|---|
| Sweep, idle clip frozen (`Lower_Weapon_Look_Raise` t = 0), `AimLock 1` | Barrel within 0.5° | **6.6e-5°** max over 49 cells, all `Locked`. Wrists on markers within 4.9e-7 m, none clamped |
| Same sweep, walk clip frozen | Barrel within 0.5° | 6.4e-5° max. Right wrist 4.8e-7 m. **Left hand clamped on all 49**: see below |
| Signs | — | Pitch +1.2 (clamped to 1.0) aims world +Y 0.84. Yaw ±2 (clamped to ±π/2) swings the aim ±90° |
| `AimLock 0` | Identical to H3 | Weapon world **bit-identical** (`memcmp`) to `bodyWorld × jointFrame × OffsetMatrix`, state `Off`. Wrists 6e-7 m |
| Failing lock (`AimMarker: Barrel`) | — | One warning, weapon on its socket |
| Readout | — | "Barrel on the aim, 19 deg off the chest pose" in the inspector (screen capture) |

Builds checked: Debug and Release editor and Debug runtime, all with no warnings. `tsc --noEmit` is
clean. The aim-offset boot self-tests still pass on the shared clamp.

**What the sweep showed that the plan did not expect.** Without the lock, the barrel error was
18.54° **in every one of the 49 cells**. It did not grow with the aim. The rifle is socketed on
`Spine`, the top joint of the aim-offset chain, and each chain joint turns its whole subtree, so
`Spine` carries the whole aim rotation. The aim offset alone already makes the barrel follow every
*change* in the aim. The lock removes the constant the clip leaves: the chest pose at that frame,
plus its roll. That constant is 18.9° at the idle's t = 0 and 8.2° at the walk's, and it varies
over a clip (the plan's 78° was the lowered idle on the old hand socket).

So H4 is not what makes aiming work. It is what makes a clip's chest pose stop mattering to the
barrel. That is still the milestone's claim of "new clips need nothing", just a smaller correction
than the plan implied.

**What H5 inherits:**

- **Left-hand margin.** The lock turns the rifle about `Grip`, which moves `Support`. On the walk,
  8.2° about a ~0.42 m lever is about 6 cm, which took the left hand from 94% reach to clamped on
  every sample. The weapon pose H5 authors has to leave the left hand real margin once the lock
  is on, measured *with* the lock.
- **`Player:BarrelPoint`'s ~35° guard.** With the lock at 1 the barrel is on the aim to 1e-4°, so
  the guard can only trip while the lock is being faded. Decide in H5 whether to keep it as that
  guard.
- **A lowered weapon needs `SetAimLock(0)` as well as `SetHandIKWeight(0, 0)`.** Otherwise the
  rifle stays level on the aim while the hands drop away from it.

**Not verified by hand:**

- the Bone Attachment note under a live lock;
- a lock weight between 0 and 1 on screen (the slerp is exercised only by construction).

The three interactive H3 checks are still open.

---

## Phase H5 — Proving Ground (`first-game`)

1. The Rifle's `BoneAttachmentComponent` changes from `RightHand` to `Spine`; the weapon pose is
   placed with the socket gizmo so both grips are inside reach in every clip.
2. `Grip` (right wrist frame on the pistol grip) and `Support` (left wrist frame under the
   handguard) as children of the Rifle, beside the existing `Muzzle`.
3. `TwoHandIKComponent` on `Body`.
4. **Measured with `meshy_retarget.py vet`-style sampling and a runtime probe**: each wrist-to-marker
   error across all five clips; reach fractions; barrel-to-aim error with H4.
5. `Player:BarrelPoint`'s ~35° check becomes redundant once H4 lands and the barrel always points at
   the aim; decide whether to keep it as a guard.

| Probe | Pass condition |
|---|---|
| All five clips, both hands | Wrist within 0.5 cm of its marker on every sampled key; no clamped frame in the shooting clips |
| Idle (`Lower_Weapon_Look_Raise`) | The rifle is held up at the chest pose; the clip's lowering is overridden |
| Gate modes | P1–P7 numbers unchanged (they read the capsule) |
| Barrel vs chest shot count | Logged before and after |


### Execution notes (2026-09-24, `first-game`)

The full record, with the derivation, the per-clip table, the P5 A/B and the frames seen, is
[PROVING_GROUND.md](PROVING_GROUND.md), under "Two-hand IK — the rifle on the chest". This file has
the outcome and where the plan was wrong.

**Branch.** `master` was merged into `first-game` first (`8c4c0d1`), with two ToDo-doc conflicts
resolved by keeping both sides. The engine source is identical on both branches; H5 changed only
the scene, two comments in `Player.lua`, and docs.

**Outcome against the table above:**

| Probe | Pass condition | Result |
|---|---|---|
| All five clips, both hands | Wrist within 0.5 cm on every sampled key; no clamped frame in the shooting clips | **Met for the four shooting clips.** Max wrist error 5.1e-7 m on reached frames, 0 clamped. **The idle clamps the left hand on 43 of 157 frames** (1.4 s, up to 12% short) |
| Idle | Rifle held up at the chest pose; lowering overridden | Met. It is on the aim, too |
| Gate modes | P1–P7 numbers unchanged | P5 re-run before and after (twice after): every gate-defined check identical; the free-running counters vary as much between two identical runs. P1–P4, P6 and P7 were not re-run, by the construction argument |
| Barrel vs chest shot count | Logged before and after | **Not measured.** `BarrelPoint` runs only under mouse aim, and no gate mode drives it |

**Where the plan was wrong:**

- **"Placed with the socket gizmo so both grips are inside reach in every clip."** It was solved
  numerically instead, over an engine dump of every clip. The answer is not something a gizmo would
  find: a shouldered hold only fits this rig's 0.52 m arms with the support hand at the rear of
  the handguard. And "every clip" is not achievable with the lock on. The idle's chest looks
  around ±70° while the rifle stays on the aim.
- **The weapon pose is a function of the lock.** Under a full lock only the socket's position
  matters, and reach is then independent of the aim, so the solve needed no aim sweep. The
  socket's rotation was still set, to the lock's orientation at the shooting clips' medoid frame,
  so that `AimLock 0` is a sane pose rather than an arbitrary one.
- **`Player:BarrelPoint`'s ~35° check stays**, as a guard for the aim fade and for parallax at
  close range. It is no longer the thing that keeps a lowered rifle from firing.

**Open, for H6 or after:**

- **An aim idle.** It is the one fix for the idle clamps, and it was already open.
- **H3's three interactive checks.**
- **H6 has to assemble this milestone from two branches.** H1–H4 are recorded on `master` and H5
  here. The merge direction means these notes never reach `master` by themselves.

---

## Phase H6 — docs and close

Live docs updated in each phase's own change. H6 audits them, moves this file to
`docs/history/TWO_HAND_IK.md` with execution notes, links it from [docs/README.md](../README.md), and
removes its row from [ToDo/README.md](README.md).

| Phase | Doc |
|---|---|
| H1, H2, H4 | [scene.md](../engine/scene.md) — the component, the pass, its order after the aim offset, the shared `OffsetMatrix`; [scripting.md](../engine/scripting.md) — `SetHandIKWeight` |
| H3 | [editor.md](../editor/editor.md) — the section, readouts, overlay |
| H5 | `PROVING_GROUND.md` on `first-game` |

**New source files:** probably one shared header for `OffsetMatrix` (premake regeneration).

## Design tensions, recorded

**The pass reads other entities.** `AnimationSystem` has only ever read its own entity. Recomputing
the weapon frame inside it couples animation to the attachment data of a child. The alternative — a
separate IK system after `BoneAttachmentSystem` that rebuilds the palette — means building the
palette twice and a second write to `AnimatorComponent::Palette`. Coupling is the cheaper cost, and
the declared access makes it visible.

**The idle's lowering is overridden.** With the weapon on the chest, `Lower_Weapon_Look_Raise` no
longer lowers the rifle; its arms follow the weapon. That is what the game wants — the idle was the
wrong pose — but it means a clip can no longer choose to lower the weapon. `SetHandIKWeight` plus a
second weapon pose is how that returns, and it is not in scope.

**Wrist markers, not palm markers.** Portable across rigs at the cost of authoring the wrist, which
sits ~10 cm behind where the palm touches the weapon.

**H4 has two writers of one fact.** The weapon's aimed frame is needed by the pass (for the hands)
and by the socket system (to draw the weapon). H4 must pick one owner.
