# Milestone — Aim offset

**Status: A1, A2 and A4 implemented on `master`. A3 implemented on `first-game`. A5 not started.**

A1 execution notes are at the bottom of [Phase A1](#phase-a1--the-component-and-the-pose-pass).
A2's are at the bottom of [Phase A2](#phase-a2--inspector-section-with-a-preview-scrub).
A4's are at the bottom of [Phase A4](#phase-a4--viewport-aim-handle).

> **Engine and editor milestone.** A1, A2 and A4 touch `GanymedEngine/source/` or
> `GanymedEditor/source/`, which the [branch policy](PROVING_GROUND.md#branch-policy) puts on
> `master`. A3 is game content and lands on `first-game` after `master` is merged into it — never
> the other way.

Tilt and twist a character's upper body toward where the player is aiming, on top of whatever clip
is playing. The rifle is socketed to `RightHand`, so it follows the torso for free.

---

## Why now

The Proving Ground player carries a socketed rifle and fires at the crosshair, and three things
about that still read wrong on screen. All three are the pose, not the shot:

1. **Pitch never reaches the body.** Look up or down and the rounds follow the crosshair (they
   converge on the camera ray's hit point, `Player:AimPoint`), but the torso and the gun stay at
   whatever elevation the clip authored. Shooting at something on a roof fires out of a level rifle.
2. **Aiming while moving has no clip.** In aim mode the body faces the camera's yaw, so strafing
   slides sideways and backpedalling plays the forward stride at negative speed — the moon-walk
   `PROVING_GROUND.md` rejected once and then accepted as the lesser evil. The clips are a
   weapon-carry set with no strafe or backpedal variants.
3. **The rifle often isn't pointing at the target**, so `Player:BarrelPoint` falls back to the chest
   point, because it only fires from the barrel when the barrel is within ~35° of the aim point.

A procedural aim offset fixes (1) outright and fixes most of (2): the legs keep facing where they run
and the upper body twists up to ±90° toward the aim, which covers strafing. It helps (3) whenever the
clip holds the rifle roughly level — both shooting clips do, measured at −2.7° to +1.7° of barrel
elevation in `PROVING_GROUND.md`'s A3 table.

It does **not** fix the lowered idle. `Lower_Weapon_Look_Raise` swings the barrel down as far as
−78°; no spine pitch turns that into an aim pose, and it should not try. **An aim-idle clip is still
content to download**, and it is worth doing before or alongside A3 so the standing case has
something to offset from.

**The expensive prerequisites already exist:**

| Capability | Where |
|---|---|
| Pose evaluation in edit mode, without advancing the clock | `AnimationSystem::OnUpdateEditor` → `Evaluate(ts, false)` |
| Joint globals per frame, with the palette built from them | `SampleClipGlobals` / `BuildSkinningPalette` (`Animation.h`) |
| A socket that reads the palette, so an attached rifle follows any pose change | `BoneAttachmentSystem` → `TryGetJointFrame` |
| The pose a rig is drawn with, animator or not | `ResolvePosePalette` (`Mesh.h`) |
| Joint space → mesh space | `Mesh::GetSkinTransform()` |
| Scripts run before `AnimationSystem`, so a Lua write lands on this frame's pose | system registration order ([scene.md](../engine/scene.md)) |
| A joint-name combo off the target's skeleton | `BoneAttachmentComponent` inspector section |

## How production engines do this, and where Ganymed diverges

**Unreal** has two routes. The *Aim Offset* asset is a 2D blend space of **authored additive poses**
(typically nine: up/centre/down × left/centre/right), driven by pitch and yaw inputs and applied as
a mesh-space additive layer over locomotion. It looks best because an animator posed every angle,
and it needs pose blending, additive layers and nine authored poses per weapon stance. The procedural
route is AnimGraph nodes (*Transform (Modify) Bone*, *Look At*) that rotate named bones.

**Unity** offers the Animation Rigging package's *Multi-Aim Constraint*: procedural, a weighted chain
of joints, **target-driven** (you give it a world target and an aim axis and it solves the angles).
Mecanim's humanoid IK has `SetLookAtWeight` with separate body/head/eye weights.

**Godot 4** does it with skeleton modifiers applied after the animation pass (recent versions ship a
look-at modifier). I have not checked which 4.x release introduced it.

**Ganymed takes the procedural route, angle-driven.** Procedural, because authored aim poses need
pose blending, and Ganymed has none: `PlayAnimation` hard-cuts, and there is no crossfade and no
additive layer. Building that is a far larger milestone than this one. Angle-driven (pitch and yaw
in, like Unreal's Aim Offset inputs) rather than target-driven (like Unity's Multi-Aim), because
the game already has both angles. And a target-driven solve needs the weapon's aim axis, which lives
on the socket, which is resolved *after* animation — see the design tensions.

## What it deliberately is not

- **Not IK.** The left hand does not reach for the rifle. Two-bone IK onto a grip socket needs the
  socket resolved mid-animation, and without finger joints the hand still cannot close on the grip.
  That is a separate milestone if it is ever wanted; authored clips that hold a rifle are the cheaper
  answer.
- **Not pose blending, additive layers or body masks.** No crossfade, no upper/lower split of
  *clips*. The aim offset is a rotation applied after sampling; it does not mix two clips.
- **Not closed-loop.** It does not measure the barrel and correct the residual. The barrel ends up
  at clip elevation + pitch. See the design tensions.
- **Not a head look-at.** The head rides the chain. A separate head/eye constraint is follow-up work.
- **Not per-clip.** One chain and one set of limits per entity, whichever clip plays.
- **Not retargeting, and not a rig editor.** Joints are referenced by name, as sockets are.

## Phases

| Phase | What | Branch | Size |
|---|---|---|---|
| **A1** | `AimOffsetComponent`, the pose pass, serialization, Lua | `master` | ~1 day |
| **A2** | Inspector section with a Pitch/Yaw preview scrub | `master` | ~0.5 day |
| **A3** | Proving Ground wiring: pitch from the aim point, yaw twist replacing the moon-walk | `first-game` | ~0.5 day |
| **A4** | Viewport aim handle | `master` | ~1 day |
| **A5** | Docs close, history record | both | ~0.25 day |

~3.25 days. **A4 is the phase to cut** if the budget tightens — A2's scrub already makes the pose
checkable without Play, and the handle edits only a preview value (see the design tensions).

---

## Phase A1 — the component and the pose pass

### Goal

A character with an `AimOffsetComponent` bends a named joint chain by `Pitch` and `Yaw` on top of
the clip, the palette carries the result, and everything that reads the palette — the draw, sockets,
the skeleton overlay, joint picking — follows it with no change of its own.

### Steps

1. **`AimOffsetComponent`** in `Scene/Components.h`, plain data:

   | Field | Serialized | Meaning |
   |---|---|---|
   | `Joints` | yes | Up to 4 joint names, root-most first — for the Meshy rig `Spine02`, `Spine01`, `Spine` |
   | `Weights` | yes | One per joint, the share of the total angle each takes; normalised at evaluation so they need not sum to 1 exactly |
   | `PitchLimit` / `YawLimit` | yes | Clamp, radians. Defaults ±1.0 pitch (57°), ±1.57 yaw (90°) |
   | `ModelForward` | yes | Which mesh-space axis the rig faces: `+Z` (Meshy), `-Z`, `+X`, `-X`. Up is `+Y` |
   | `Enabled` | yes | Off = the clip exactly as authored |
   | `Pitch` / `Yaw` | **no** | Radians, the live inputs Lua writes each frame (and the editor preview) |
   | `Resolved` | **no** | Cached joint indices, −1 unresolved — the same pattern as `BoneAttachmentComponent::Resolved` |

   Fixed-size `std::array` of 4, not `std::vector`. Four is enough for a spine; a rig that wants more
   is out of scope. The names are `std::string`, so the struct is not trivially copyable and gets
   **no** `sizeof` sentinel — the mechanical rule in `ComponentReflection.cpp` (a library container
   means no sentinel), the same as `BoneAttachmentComponent`. `YawLimit`'s default is the full
   `π/2` (`1.5707963267948966f`); a truncated `1.57` would fail omit-if-default and leak into every
   scene.

2. **The component checklist** ([ecs.md](../engine/ecs.md), "Adding a component type"):
   `ComponentList`, `GE_REFLECT_COMPONENT` (no `sizeof` sentinel, above), `SceneSerializer` both halves (omit-when-default
   like the particle emitter, so scenes without it stay byte-identical), and a **`Scene::Copy`
   fixup sweep** that resets `Pitch`, `Yaw` and `Resolved` — a fifth sweep beside the four
   [scene.md](../engine/scene.md) lists. Add Component entry in the hierarchy panel's list (the full
   section is A2).

3. **The pass**, a free function beside `SampleClipGlobals` in `Animation.h` / `AnimationSystem.cpp`:

   ```cpp
   // Rotates each chain joint - and everything below it - about the joint's own pivot, in skin
   // space, by its weighted share of yaw then pitch. Operates on globals, after sampling.
   void ApplyAimOffset(const Skeleton& skeleton, const AimOffsetChain& chain,
       const glm::vec3& upSkin, const glm::vec3& rightSkin, float pitch, float yaw,
       std::vector<glm::mat4>& globals);
   ```

   Per chain joint `j`, parent-first: `M = T(p_j) * R_j * T(-p_j)`, where `p_j` is the joint's
   current origin, then `G_k = M * G_k` for `j` and every descendant `k`. `R_j` is the yaw by
   `w_j * yaw` about `up`, then the pitch by `w_j * pitch` about `right` **turned by the full yaw**,
   so pitching while twisted tilts along the aim, not along the hips. Descendant lists are computed
   once per (mesh, chain) and cached beside `Resolved`. Cost is chain × subtree matrix multiplies:
   3 × ~15 on this rig.

4. **Axes in skin space, not joint space.** `right` and `up` come from `ModelForward` in *mesh*
   space, brought into skin space with the rotation part of `inverse(Mesh::GetSkinTransform())`.
   Joint-local axes are **not** used — see the decision below.

5. **Hook into `AnimationSystem::Evaluate`.** `AnimView` gains `OptRW<AimOffsetComponent>`. After
   sampling, before `Global * InverseBind`: split `BuildPalette` so it samples into `m_Globals`,
   calls `ApplyAimOffset` when the component is present, enabled and resolved, then multiplies.
   `SampleClipGlobals` itself stays untouched, so the clip inspector keeps measuring the clip, not
   the clip plus whatever aim the entity last had.

6. **Resolution** by name against the mesh's skeleton, warn-once per entity for a name that does not
   exist, the same shape as `BoneAttachmentSystem`. An empty joint slot ends the chain.

7. **Lua:** `Entity:SetAimOffset(pitch, yaw)` and `Entity:GetAimOffset()` (a table `{pitch, yaw}`),
   no-op / zeroes without the component, as the animation bindings do. Clamping happens in the pass,
   not the binding, so the editor preview and Lua share one rule. Typings in `ganymed.d.ts`.

### Decisions, with reasoning

**Rotate about a character-space axis through each joint's pivot, never about a joint-local axis.**
Measured on `ArmoredHumanoid.glb` at rest, in armature space: `Spine02`, `Spine01` and `Spine` have
local axes within ~2° of the world axes (X ≈ +X, Y ≈ up, Z ≈ forward) — but `Hips` is rotated
arbitrarily (its X is `(-0.20, +0.54, +0.82)`), and Mixamo and Blender rigs use their own
conventions. "Pitch about local X" would be right on this spine by coincidence and wrong on the next
rig. A skin-space axis is correct on all of them, and this rig's aligned spine becomes a free
cross-check: on it, the skin-space `right` (−X, because the rig faces +Z) matches the joints' −X.

**A post-pass over globals, not a hook inside the sampler.** Applying rotations in the local forward
pass is marginally cheaper and does not need descendant lists, but it puts entity state into
`SampleClipGlobals`, the function the clip inspector calls to measure the clip. The post-pass keeps
the sampler pure.

**Additive on the clip, zero means "as authored".** Barrel elevation ends up at clip elevation +
pitch. On the two shooting clips that error is the clip's own −2.7° to +1.7°, which is inside what
the crosshair convergence already hides — the round goes to the crosshair either way.

### Risks

- **Descendants include the arms, head and rifle socket.** That is the point, but a chain weighted
  too high at `Spine02` pivots the whole upper body from the waist. Tune the weights, and give the
  default weights some thought rather than an even split.
- **Limits interact with twist.** ±90° yaw on the spine alone looks broken past ~60° on most rigs.
  The defaults are a starting point, and A3 is where they get measured.
- **Scaled joints.** The pivot rotation is rigid. A clip that scales a spine joint (the constant-Hips
  scale Meshy used to emit) still composes, because scale lives in `G` and `M` is applied on the left.

### Verification

| Probe | Pass condition |
|---|---|
| `Enabled`, `Pitch = Yaw = 0` | Palette **bit-identical** to the same entity without the component (max abs diff 0) |
| `Pitch = +0.3` on the rest pose | The `Muzzle`'s `GetWorldForward().y` rises; the neck's global rotation differs from rest by 0.3 rad ± 1e-4 (weights sum to 1) |
| Sign | Positive pitch looks **up**, positive yaw turns toward the character's left (same convention as `Yaw` in `Player.lua`) — written down once and asserted |
| `Pitch = 2.0` with `PitchLimit = 1.0` | Neck delta is 1.0 rad |
| Twisted pitch (`Yaw = 1.0`, `Pitch = 0.3`) | The pitch axis is `right` turned by 1.0 about up, not the hips' `right` |
| Socket same-frame | `Rifle:GetWorldPosition()` one frame after a pitch change equals the socket frame from the new palette |
| Clip inspector | Head Y / Hips Y figures for `ArmoredHumanoid` unchanged with the component present |
| Serialization | Scene without the component: load → save byte-identical. With it: load → save → load → save identical; `Pitch`/`Yaw`/`Resolved` never written |
| `Scene::Copy` | An editor preview `Pitch` does not survive into Play |
| Unresolvable joint name | One warning per entity per distinct name; pose unchanged |
| Cost | Profile scope on the pass; one character, Release. Expectation: single-digit microseconds, measured rather than claimed |

### Execution (2026-09-23, on `master`)

The component, the pass, the Lua bindings, the fifth `Scene::Copy` sweep, and the Add Component
menu entry are in. There is still no inspector section — that is A2, and until it lands a component
added from the menu has no header and no Remove.

Boot probes in `AnimationSystem.cpp`, Debug only, on a 4-joint rest pose (`Hips` / `SpineA` /
`SpineB` / `Neck`). All of these asserted clean on the Debug editor:

- Pitch = yaw = 0 leaves every global bit-identical (early-out; a `glm::rotate(0)` multiply is not
  the test).
- Pitch 0.3, weights 0.25 / 0.75: hips unchanged, neck rotation delta 0.3 ± 1e-4, neck +Z rises.
- Pitch 2.0 with limit 1.0: neck delta 1.0.
- Yaw 0.5 on one joint: neck +Z gains +X (the character's left when forward is +Z and right is −X).
- Yaw 1.0 and pitch 0.3 on joint 2 matches `angleAxis(0.3, yawedRight) * angleAxis(1, up)` within
  1e-4, and differs from pitching about the unturned right.
- Identity skin and a uniform 0.01 skin both yield up = +Y, right = −X.

Cost, same function, 2000 calls, a 32-joint line, chain joints 4/5/6 (those subtrees are longer
than a real spine of ~15, so a character should be cheaper, not dearer):

| Config | µs / call |
|---|---|
| Debug (MSVC, unoptimized) | 164.0546 |
| Release | 3.92345 |

The Release figure is the one the cost row asked for, and it is single-digit. It is not a
measurement of `ArmoredHumanoid`. The 2000-iteration loop is compiled out of Release; leaving it
in would hitch the first animated frame of every session.

Not run, and not claimed: a scene load/save byte compare, `Scene::Copy` into Play, a socket
same-frame read, the clip inspector's Head Y / Hips Y on `ArmoredHumanoid`, and a warn-once against
a real misspelled joint. The socket row follows from system order (scripts, then
`AnimationSystem`, then `TransformSystem`, then `BoneAttachmentSystem`) and was not executed. `SampleClipGlobals` is
unchanged, which is what keeps the clip inspector off the aim pass.

---

## Phase A2 — inspector section with a preview scrub

### Goal

Author the chain, weights and limits in the Properties panel, and see the result in edit mode
without pressing Play.

### Steps

1. `DrawComponent<AimOffsetComponent>` in `SceneHierarchyPanel.cpp`. Joint slots are combos off the
   entity's own skeleton — the `BoneAttachmentComponent` Joint combo, pointed at `self` instead of a
   target. Weights as sliders, limits in degrees in the UI (radians stored), `ModelForward` as a
   combo, `Enabled` checkbox.
2. **Preview `Pitch` / `Yaw` sliders**, edit mode only, clamped to the limits. They write the
   non-serialized inputs; `AnimationSystem::OnUpdateEditor` already evaluates poses, so the pass
   runs and the overlay, socket and rifle all move with the slider.
3. Undo for the authored fields through `ComponentEditCommand<AimOffsetComponent>`, as every other
   section does. **No undo for the preview sliders** — they are not scene state.
4. A readout under the sliders: resolved joint names, and the neck's actual delta angle, so a
   clamped or unresolved chain says so instead of silently doing less.

### Decisions, with reasoning

**Preview values live on the component, not in the editor.** An editor-side preview map would need
its own way into `AnimationSystem`. The component already is that way, the `Scene::Copy` sweep from
A1 keeps a preview out of Play, and the serializer never writes it.

**Undo snapshots the whole struct.** `ComponentEditCommand<AimOffsetComponent>` restores every
field, including `Pitch`, `Yaw` and `Resolved`. A weight undo must copy the live values of those
three back onto the restored component, or Ctrl+Z silently rewinds the preview.

### Verification

| Probe | Pass condition |
|---|---|
| Drag preview Pitch in edit mode | Skeleton overlay, rifle and muzzle move live; `Time` does not advance |
| Save, reload | Chain/weights/limits persist; preview reads 0 |
| Ctrl+Z after a weight edit | Restores the weight; preview untouched |
| Unresolvable joint in a slot | Readout names it; pose unchanged |

### Execution (2026-09-23, on `master`)

The section is in `SceneHierarchyPanel`, between Animator and Bone Attachment. Joint combos and
weight sliders are hand-drawn off this entity's skeleton. Pitch limit, yaw limit, model forward
and enabled go through `DrawReflected`, so the limits stay degrees in the row and radians in the
component, and a multi-selection propagates those fields the same way every other reflected row
does. Joint and weight edits propagate by hand.

Preview Pitch / Preview Yaw are drawn only when `Recording()` is true (edit mode; Play passes a
null undo stack). They write `Pitch` / `Yaw` and return false, so the commit boundary never
pushes a command for the scrub. `ComponentEditCommand::Apply` copies the live `Pitch`, `Yaw` and
`Resolved` onto the snapshot it is about to write, which is what keeps Ctrl+Z of a weight from
rewinding a preview moved afterwards.

The readout resolves names itself, rather than trusting `Resolved`, so a combo click is not one
frame of "missing" while the system cache catches up. A name the mesh does not have is printed,
and the bend is described as unchanged. Past-limit live angles are printed as clamped. The tip
joint's number is the angle between a fresh `SampleClipGlobals` at the animator's time and the
global reconstructed from `Palette * inverse(InverseBind)`.

Not run in the editor: dragging the preview against a skeleton overlay, a save/reload, and Ctrl+Z.
The commit-boundary and the `Apply` copy are what those rows depend on, and they were not exercised
through the window. Debug and Release were built after the section landed.

---

## Phase A3 — Proving Ground wiring (`first-game`)

### Goal

The player's torso and rifle follow the crosshair in pitch, and aiming while strafing twists the
upper body instead of sliding.

### Steps

1. `AimOffsetComponent` on the player's `Body`: `Spine02` / `Spine01` / `Spine`, weights tuned by eye
   with the A2 scrub and recorded here with the reason.
2. **Pitch** in `Player:Animate` from the direction chest → `Player:AimPoint()`, not raw camera
   pitch. The camera sits 5 m behind and 1.6 m above the capsule, so its pitch is not the gun's
   pitch at short range — the same parallax argument that made the shot converge on the aim point.
3. **Yaw: split legs and torso.** While aiming and moving, `meshYaw` keeps following the velocity
   (the legs run forward) and the aim offset takes `wrap(yaw − meshYaw)`, clamped. Past the yaw
   limit — a true backpedal — the body turns to the aim and the reversed-clip fallback stays. So
   the moon-walk survives only for moving straight away from the camera while aiming.
4. Remove the "aiming while moving has no right clip" caveat from `Player:Animate`'s comments
   wherever the twist now covers it.
5. `Player:BarrelPoint`'s ~35° check stays: it is what keeps the idle's lowered rifle on the chest
   fallback until an aim-idle exists.

### Verification

| Probe | Pass condition |
|---|---|
| Stand, pitch +0.4 / −0.3 with the running clip frozen | Muzzle forward elevation within ~3° of the chest → aim-point elevation |
| Strafe right while aiming | Legs face velocity; torso and rifle face the aim; `BarrelPoint` returns the barrel |
| Backpedal while aiming | Past the yaw limit: body turns, reversed clip plays (unchanged fallback) |
| Gate modes (`autofire`, `p5gate`) | Fire counts, hits and every P1–P7 gate number unchanged — they never capture the cursor |
| Share of shots from the barrel vs the chest | Logged over a scripted strafe-and-shoot run, before and after — the number this milestone exists to move |

---

## Phase A4 — viewport aim handle

### Goal

In edit mode, drag a point in the viewport and have the selected character aim at it.

### Steps

1. With an `AimOffsetComponent` entity selected and a new **Aim** toggle on (beside the socket gizmo
   modes), show an ImGuizmo translate handle on a virtual target point, initialised 3 m ahead of the
   chain root at chain-root height.
2. Each drag converts the target to `(pitch, yaw)` relative to the chain root's world position and
   the entity's world forward (from `ModelForward`), clamps to the limits, and writes the **preview**
   inputs A2 already uses.
3. The target point is editor state (`EditorLayer`), like the joint selection: not serialized, not
   undoable, cleared on scene change and on Play.
4. Draw a line from the muzzle (if the entity has one under its hierarchy) to the target, so the
   residual between barrel and target is visible — the closed-loop error this milestone chose not to
   correct.

### Decisions, with reasoning

**Built because it was asked for, and recorded as the cuttable phase.** It edits nothing that is
saved: weights and limits — the values worth tuning — are not what the handle changes, and the
preview it writes is also reachable from A2's sliders. Its real value is the muzzle-to-target line in
step 4, which shows how far the clip-plus-pitch pose is from actually pointing at a thing.

### Verification

| Probe | Pass condition |
|---|---|
| Drag the target up | Preview Pitch rises; torso and rifle follow |
| Drag past the limit | Preview clamps; the handle keeps moving; the readout says clamped |
| Press Play | Handle gone; preview reset (A1's sweep) |
| Muzzle line | Visible, from the barrel, updates with the drag |

### Execution notes (A4)

The handle lives in `EditorLayer`, next to the transform gizmo. No new source file, so no premake
regeneration. The toolbar crosshair is a peer of W/E/R: arming it hides the entity and socket
gizmos for an entity that has `AimOffsetComponent`; Q/W/E/R clear it. There is no new hotkey.

The drag writes the **clamped** angle. The plan's step text says clamp-then-write, and "Preview
clamps" means Preview Pitch/Yaw stop at the limit. The point is the unconstrained target, so it
keeps moving. The A2 readout only said "clamped" when the stored angle was *past* the limit; a
value sitting on the limit would have stayed silent. The readout now also says "Pitch clamped at
N deg" when the stored angle is within 1e-3 rad of a positive limit. A slider parked on that same
stop shows the same line.

Init does not write. Pitch and yaw near zero place the point 3 m ahead of the chain root along
the entity's horizontal world forward, at the root's height. A surviving preview (Stop does not
zero the editor scene; `Scene::Copy` zeros the play copy) places it 3 m along that aim. The chain
root is the first named joint: `WorldTransformComponent::World * TryGetJointFrame`, the same frame
the skeleton overlay uses. An empty or unresolved chain draws no handle.

Forward is `ModelForward` through the entity world matrix, Y flattened. The pose pass builds its
axis from the skin transform in mesh space. A uniform skin scale does not change that direction,
which is the Meshy rigs. A skin *rotation* would make the handle and the pass disagree; nothing
in tree has one.

The muzzle line walks `RelationshipComponent::Children` for the tag `Muzzle` and draws with
`ImDrawList::AddLine` through the same view/projection as `ImGuizmo::Manipulate`. A point with
`clip.w <= 0` is skipped. The line is one frame behind the pose: the drag writes `Pitch`/`Yaw`
after `AnimationSystem` has already evaluated.

Play hides the handle (`editing` is false) and clears the point. The preview reset is A1's copy
sweep, not a second zero of the editor scene. After Stop, selecting the entity again inits from
whatever preview survived.

Not dragged in the editor. The Proving Ground scene with a `Muzzle` child is on `first-game`, not
on this branch, so the four verification rows above are unrun. Debug editor
(`GanymedEditor.vcxproj`, x64) built after the change.

---

## Phase A5 — docs and close

Live docs updated in each phase's own change, per [AGENTS.md](../../AGENTS.md). A5 audits them,
moves this file to `docs/history/AIM_OFFSET.md` with execution notes and measured evidence, links it
from [docs/README.md](../README.md), and removes its row from [ToDo/README.md](README.md).

## Docs this milestone must update

| Phase | Doc |
|---|---|
| A1 | [scene.md](../engine/scene.md) — component catalog, AnimationSystem pass, the fifth `Scene::Copy` sweep, serialization; [scripting.md](../engine/scripting.md) — `SetAimOffset` / `GetAimOffset`; [ecs.md](../engine/ecs.md) only if the checklist changes |
| A2, A4 | [editor.md](../editor/editor.md) — the section, preview semantics, the Aim handle |
| A3 | `PROVING_GROUND.md` on `first-game` — the facing/aim paragraphs and the barrel-vs-chest numbers |

**New source files:** none planned — the component goes in `Components.h`, the pass beside the
sampler, the section in `SceneHierarchyPanel.cpp`. If A4 grows its own file, that means premake
regeneration.

## Design tensions, recorded

**Angle-driven vs target-driven.** Unity's Multi-Aim takes a world target and solves; this takes
angles. Target-driven is the better API for AI ("aim at that enemy") and it could close the loop on
the barrel — but the barrel's direction is the rifle's socket frame, and `BoneAttachmentSystem` runs
*after* `AnimationSystem`, so solving for it inside the pass means evaluating the socket
mid-animation. That is the same ordering problem left-hand IK has, and it belongs with that
milestone. The enemy AI can compute angles in Lua in the meantime.

**Open-loop barrel error.** The barrel points at clip elevation + pitch, not at the target. On the
shooting clips that is within ~3°. On the idle it is up to 78°, and no spine rotation should fix
that — the fix is content.

**A fifth `Scene::Copy` sweep.** Nothing enforces these sweeps. Putting the live inputs in a
system-owned map instead (the audio pattern) avoids the sweep but gives Lua and the editor preview no
place to write through declared access. The Palette precedent wins: runtime state on the component,
swept on copy.

**The handle edits a preview.** See A4.

**The aim-idle clip is the bigger visual win for standing.** This milestone makes moving-and-aiming
right and adds pitch everywhere; standing still with the current idle stays wrong until a clip is
downloaded. Worth sequencing the download before A3 so the tuning in A3 is done against the pose
that will ship.
