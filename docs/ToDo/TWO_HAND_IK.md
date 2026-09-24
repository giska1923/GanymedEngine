# Milestone — Two-hand weapon IK

**Status: planned. Nothing built.** Phases H1–H6 below.

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
| **H1** | Analytic two-bone IK over globals, with boot self-tests | `master` | ~0.75 day |
| **H2** | `TwoHandIKComponent`, weapon frame resolution, the pass, serialization, Lua | `master` | ~1 day |
| **H3** | Inspector section, reach readouts, overlay | `master` | ~0.75 day |
| **H4** | Aim lock: the barrel onto the aim direction | `master` | ~0.5 day |
| **H5** | Proving Ground: rifle onto `Spine`, `Grip`/`Support` markers, measured | `first-game` | ~0.75 day |
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

---

## Phase H3 — inspector, readouts, overlay

1. Inspector section: weapon (drop or auto), chain joint combos off this entity's skeleton, marker
   names, weights. **Readout per hand**: reach as a fraction of arm length at the current frame, and
   "clamped" when the target is out of reach — the number that makes a weapon pose tunable.
2. **Overlay** (Skeletons visualizer on): a small cross at each marker, a line wrist→marker in the
   accent colour when not reached.
3. The rifle's weapon pose is placed with the existing **socket gizmo** on the rifle
   (`BoneAttachmentComponent` on `Spine`); markers with the ordinary gizmo. No new gizmo.

| Probe | Pass condition |
|---|---|
| Drag the rifle with the socket gizmo | Hands follow live; reach readout updates |
| Drag a marker past reach | Readout says clamped; overlay line appears |
| Scrub the animator | Hands stay on the markers through the clip |

---

## Phase H4 — aim lock

**Goal:** the barrel points at the aim direction exactly, not "chest orientation + weapon pose".

1. The aim direction in mesh space comes from the entity's `AimOffsetComponent` inputs (yaw about up,
   then pitch about the yawed right — the same construction `ApplyAimOffset` uses).
2. After the weapon frame is computed and before the hands are solved, rotate the weapon frame about
   its `Grip` marker so its forward axis (a per-weapon `Forward` marker or the weapon's −Z) matches
   the aim direction, blended by an `AimLock` weight.
3. **The weapon entity must be drawn where the hands are**: `BoneAttachmentSystem` gains the same
   correction for a weapon referenced by a `TwoHandIKComponent`, or the pass writes the corrected
   frame somewhere the socket system reads. Decide in H4; do not let the two diverge.

| Probe | Pass condition |
|---|---|
| Aim pitch/yaw sweep, idle clip frozen | Muzzle forward vs aim direction within 0.5° |
| AimLock 0 | Identical to H3 |

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
