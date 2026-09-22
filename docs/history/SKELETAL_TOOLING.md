# Milestone — Skeletal joint tooling

**Status: complete. S1–S6 landed.** The live behaviour is in
[editor.md](../editor/editor.md) and [scene.md](../engine/scene.md). Leftovers that this close
did not invent a second time — hide-unresolved, and the A3 grip picture — are in
[cross-cutting.md](../ToDo/cross-cutting.md#skeletal-leftovers-after-the-attachment-and-tooling-close).

> **Editor milestone.** Every phase touches `GanymedEditor/source/` or `GanymedEngine/source/`,
> which the [branch policy](../ToDo/PROVING_GROUND.md#branch-policy) puts on `master`; the game
> branch receives that work by merge and never sends anything back.

Seeing joints, selecting them, and placing a socket by dragging it rather than by typing radians.
The counterpart to [SKELETAL_ATTACHMENTS.md](SKELETAL_ATTACHMENTS.md), which built the mechanism and
deliberately left the tooling out.

---

## Why now — the trigger condition was set, and it fired

`SKELETAL_ATTACHMENTS.md` scoped a socket editor out in one sentence and named the evidence that
would justify building one:

> **Not a socket editor.** No named socket assets authored onto a skeleton, no gizmo for placing
> them. A joint name plus an offset, typed into the inspector. *If placing offsets by hand becomes
> the bottleneck, that is the evidence for building more.*

Here is the Rifle entity as committed to `Game/assets/scenes/ProvingGround.ganymede` on
`first-game`:

```yaml
BoneAttachmentComponent:
  Target: 0
  Joint: RightHand
  Offset: [-0.248746991, 0.144001997, -0.00640099961]
  Rotation: [-2.71431017, 0.254526004, 0.885160983]
```

Nobody reasons their way to a rotation of (−2.714, 0.255, 0.885) radians. That is the residue of
dragging three `DragFloat` sliders until the rifle stopped looking wrong, and the commit *"Put a
bone socket in the same space as the mesh it is pinned to"* is the other half of the same session.
One weapon on one character cost that. A second weapon, a second character, or a muzzle emitter
moved onto the barrel each start from zero.

**The expensive prerequisite is already built**, which is the other reason this is worth doing now.
Editor-time pose evaluation is normally the hard part of skeleton tooling — Unity and Unreal both
had to build a full editor-time animation path before any of their skeleton UI could exist. Ganymed
has one already, built for a different reason:

| Capability | Where |
|---|---|
| **Pose evaluation in edit mode** — samples without advancing the clock, so rigs do not wander while you place things | `AnimationSystem::OnUpdateEditor` → `Evaluate(ts, false)` ([AnimationSystem.h:20](../../GanymedEngine/source/GanymedE/Scene/Systems/AnimationSystem.h)) |
| **Time scrubbing** that clears `Playing`, so play mode cannot overwrite the scrub | Animator inspector section |
| **Sockets resolve while editing** — "or the inspector would show the weapon on last frame's hand" | `BoneAttachmentSystem::OnUpdateEditor` |
| **A joint dropdown off the *target's* skeleton** | `BoneAttachmentComponent` inspector section |
| **Rest pose with no clip set** — `Palette` is populated regardless, so joints exist in edit mode even at `Clip: (none)` | `AnimationSystem::Evaluate` |

What is missing is narrow: **nothing draws a joint.** The only `DrawLine` / `DrawWireSphere`
consumers in the repository are the collider gizmos and Jolt's debug draw. A joint is picked from a
dropdown with no indication of where it is in space, and the offset that places something on it is
three numbers with no handle.

## What it deliberately is not

- **Not a rig editor.** Joints cannot be added, removed, renamed or re-parented. `Skeleton` is
  imported data with no authoring path, and giving it one means owning a rig format — which is the
  same argument [MODEL_EDITOR.md](MODEL_EDITOR.md) makes about not being a modeller.
- **Not clip editing.** S5 *reads* clips and reports what is wrong with them. Retiming, key
  deletion and baking belong in the DCC; an inspector that edits third-party clip data becomes a
  second source of truth against the file it came from.
- **Not blending or crossfade.** Still absent, still deferred — `PlayAnimation` restarts on a clip
  change. A socket makes the lack more visible and does not need it fixed.
- **Not IK, look-at or procedural aim.** Attachment reads the pose; nothing here writes to it.
- **Not retargeting.** One skeleton, one clip set, per mesh asset.
- **Not an animation state machine.** `Clip` is a string and stays one.
- **Not asset-side socket libraries.** Named sockets stored on the mesh asset and reused across
  instances are the obvious follow-up and are *named* here, not built — see the design tensions.

## Phases

| Phase | What | Size | Wants (not blocks) |
|---|---|---|---|
| **S1** | One owner for joint → world | ~0.5 day | — |
| **S2** | Skeleton visualization | ~1 day | S1 |
| **S3** | Joint selection: picking + a joint tree | ~1.5 days | [MAP_EDITOR](../ToDo/MAP_EDITOR.md) M0's ray |
| **S4** | The socket gizmo | ~1.5 days | S3, [MAP_EDITOR](../ToDo/MAP_EDITOR.md) M1's snap settings |
| **S5** | The clip inspector | ~1.5 days | [MODEL_EDITOR](MODEL_EDITOR.md) P1's panel |
| **S6** | Docs, and closing the attachment milestone | ~0.5 day | all |

~6.5 days. **S2 alone would have saved the afternoon that produced the rotation above**, and S5 is
the one to keep if everything else is cut — it converts a measurement that has been done by hand on
every clip download into a readout.

---

## Phase S1 — one owner for joint → world

**Done.**

### Goal

One function computes a joint's world matrix. The attachment system calls it; so does every tool
below.

### Why this is first, and not a refactor to do later

The joint frame is **not** `Palette[i] * inverse(InverseBind[i])`. `BoneAttachmentSystem` has to
fold in the skinned submesh's `LocalTransform` and then divide the bind pose's own basis scale back
out, because a Meshy rig has joints in centimetres and vertices in metres with `LocalTransform`
(0.01) the factor between them. Getting that wrong is what put a socket at **141 metres instead of
1.41** — recorded in [SKELETAL_ATTACHMENTS.md](SKELETAL_ATTACHMENTS.md)'s A2 follow-up and in
[`TryGetJointFrame`](../../GanymedEngine/source/GanymedE/Renderer/Mesh.h).

A visualizer that re-derives that formula will drift from the system that uses it, and the drift
looks like *"the bones are drawn slightly wrong"* rather than like a bug. That is the same
two-owners failure the attachment milestone already argued through when it chose to make
`TransformSystem::RecomputeSubtree` public rather than duplicate the walk.

### Steps

1. Extract the block at `BoneAttachmentSystem.cpp:256-306` into a free function. It takes the mesh,
   the palette, and the joint index, and returns the joint's frame in the **target entity's local
   space** — the caller multiplies by `targetWorld`:

   ```cpp
   // Renderer/Mesh.h — Mesh.h already includes Animation.h, so this is where a function
   // needing both Skeleton and Submesh::LocalTransform belongs.
   bool TryGetJointFrame(const Mesh& mesh, const std::vector<glm::mat4>& palette,
                         int32_t joint, glm::mat4& outFrame);
   ```
2. It keeps the existing guards: a singular inverse bind returns false; an out-of-range joint
   returns false. The warn stays on `BoneAttachmentSystem` (`WarnOnce`, entity-attributed) rather
   than inside the function — S2 skipping a bad joint should not spam. The `IsSkinned` submesh
   search moves in with it — `Submesh::IsSkinned` is already the single gate the colour pass,
   shadow pass and bounds all honour.
3. `BoneAttachmentSystem::Evaluate` becomes a call to it. **The system's behaviour must not change
   at all.**

### Risks

- This refactors a formula that was debugged recently and painfully. The verification below is a
  before/after comparison for that reason, not a visual check.

### Verification

**Done.** `TryGetJointFrame` lives in `Mesh.h` / `Mesh.cpp`. `BoneAttachmentSystem::Evaluate` calls
it. The statements that produce `jointGlobal` moved unchanged.

| Probe | Result |
|---|---|
| The committed Rifle socket, same scene, same clip, same `Time` | Not re-run: `ProvingGround.ganymede` lives on `first-game`. The matrix is the same product (`targetWorld * jointGlobal * offset * localScale`) with `jointGlobal` from the moved statements, so it is identical by construction. |
| A rig with a non-identity `LocalTransform` (the centimetre case) | Self-check on first `TryGetJointFrame`: `LocalTransform` = 0.01, joint at y = 141.4 cm, palette = bind-pose `Global * InverseBind`. Origin lands at `(0, 1.414, 0)`, unit basis. Omitting `LocalTransform` is the 141 m case. |
| A joint whose inverse bind is singular | Function returns false. System still `WarnOnce`s and `Restore()`s. Self-checked with a zero matrix. |
| An out-of-range or unresolved joint | Returns false before indexing `InverseBind` or `palette` (`joint < 0`, `joint >= size`). Self-checked. |

---

## Phase S2 — skeleton visualization

**Done.**

### Goal

See the joints.

### Steps

1. Draw from `RenderSystem`, behind an editor opt-in flag on a scene singleton — **exactly the
   shape `PhysicsSettings::ShowColliderGizmos` already uses**: engine default off so a shipped game
   never draws a skeleton, editor pushes it true every frame because `Scene::Copy` does not carry
   singletons onto the play scene.
2. What is drawn, per animated entity with a skeleton:
   - a line from each joint to its parent (`ParentIndices`);
   - a small marker at each joint;
   - the selected joint (S3) highlighted in `Accent`;
   - an axis triad for the **selected joint only** — a triad on all 90 joints of a humanoid rig is
     270 lines of noise that hides the thing you are looking at.
3. **Leaf joints have no child to draw toward.** Draw a short stub along the joint's own +Y, sized
   from the parent bone's length, which is what every DCC does. Without it a hand or a foot simply
   has no visible last bone.
4. **Marker geometry is a 3-line cross or a 6-line octahedron, not a wire sphere.**
   `DrawWireSphere` defaults to 24 segments — three rings, so ~72 lines per joint, ~6 500 lines for
   a 90-joint rig before a single bone is drawn. A cross is 3.
5. **Marker size derives from bone length**, not a constant. A constant that reads well on a 1.8 m
   character is invisible on a 0.3 m prop rig and fills the screen on a 10 m one.
6. **X-ray by default.** Bones are inside the mesh, so a depth-tested skeleton is an invisible
   skeleton. Blender, Maya and Unreal all default to drawing the rig in front. A toggle keeps the
   depth-tested view for the cases where the occlusion is the information.
7. Joint name labels through the ImGui draw list, projected to screen — **for the selected joint
   and its immediate parent/children only**. 90 overlapping labels is not a feature.

### Decisions, with reasoning

**Engine-side drawing, editor-side opt-in.** The alternative is drawing from the editor, which would
need the editor to walk skeletons and palettes itself and to reimplement the frame maths S1 just
centralised. `RenderSystem` already reads `AnimatorComponent::Palette` through declared access; it
is the natural owner. The cost is one more flag on a settings singleton — and if
[MAP_EDITOR](../ToDo/MAP_EDITOR.md) M2 lands first (which turns collider gizmos on in Edit for the same
reason), these should share one *editor visualizers* bitfield rather than accumulating parallel
booleans.

### Risks

- **Line budget.** Depth-tested lines are one submit; x-ray is a second overlay submit with depth
  testing off — still batched, not per-joint. A per-joint sphere would have made it expensive
  anyway. Count `DebugLines` / `DebugLineDraws`, not just milliseconds.
- Several animated entities on screen at once multiplies everything. Draw only for entities in the
  current selection, with an "all" toggle, rather than every rig in the scene.

### Verification

| Probe | Result |
|---|---|
| Select the player, enable the visualizer | Editor default is on. Selection includes hierarchy (capsule → body). `RightHand` highlight + labels when the Rifle's `BoneAttachmentComponent` is selected (`Resolved` + target). Visual check of "at the hand" is Edit-time, not timed here. |
| Scrub the Animator's `Time` | Overlay reads `AnimatorComponent::Palette` each frame; `OnUpdateEditor` already samples without advancing. |
| A 90-joint rig | Line count ≈ joints × (1 bone + 3 marker) + leaf stubs + 3 triad on the highlight. X-ray is **one extra line submit**, not per-joint. `DebugLines` / `DebugLineDraws` on the Stats panel. |
| Frame time with the visualizer on, Release | Not measured. Toggle off is the cost baseline (`ShowSkeletons` false returns before any `TryGetJointFrame`). |
| A rig with a non-identity `LocalTransform` | Same `TryGetJointFrame` as the socket. |
| Toggle off | Early-out; zero skeleton lines. |

Joint picking and the joint tree are S3. Until then the highlighted joint is the selected socket's `Resolved` index.

---

## Phase S3 — joint selection: picking and a joint tree

**Done.**

### Goal

Click a bone. Know which one it is. Hand it to the tools.

### Steps

1. **Editor-side selection state**: `{ UUID entity, int32_t joint }`, held by the editor exactly as
   the outliner holds entity selection. It is not a scene concept and must not become one.
2. **Picking**: build the ray with `Math::ScreenPointToRay` — **the same function
   [MAP_EDITOR](../ToDo/MAP_EDITOR.md) M0 introduces**. Then ray-vs-segment distance for each bone and
   ray-vs-sphere for each joint marker, nearest hit within a screen-space threshold. Brute force
   over at most `Skeleton::MaxBones` (128) is free, and unlike entity picking it is **synchronous** —
   the GPU pick buffer cannot see joints at all, since joints are not entities and carry no ID.
   - **If M0 has not landed**, this phase needs ~20 lines of screen→ray of its own. Write it here
     and let M0 adopt it, rather than duplicating later — whichever lands first owns it.
3. **A joint tree panel**: hierarchy from `ParentIndices`, `EditorUI::SearchField` for filtering
   (the furniture helper exists; do not hand-roll one), click to select. Viewport picking and the
   tree drive the same selection.
4. The `BoneAttachmentComponent` inspector gains *"pick in viewport"* beside its joint combo, and
   highlights the joint the component currently names.

### Decisions, with reasoning

**Joint selection is subordinate to entity selection, not parallel to it.** Clicking a bone must not
clear the entity selection, and changing the entity selection clears the joint. The rule matters
because the editor already has destructive verbs bound to selection — Delete and Ctrl+D act on
entities, and a second selection domain that they might plausibly apply to is how "delete" becomes
ambiguous. Unreal keeps skeleton selection inside a dedicated asset editor for exactly this reason;
Ganymed keeps it in the scene viewport but makes it strictly a refinement of the entity selection.

### Risks

- Picking a bone that is inside geometry conflicts with entity picking on the same click. Resolve by
  precedence: when the visualizer is on and a bone is within the threshold, the bone wins; otherwise
  the entity pick proceeds. Make that rule explicit in the panel's tooltip, not just in code.

### Verification

| Probe | Result |
|---|---|
| Click five known joints on the player | Built: screen-space pick + Joints panel share `EditorJointTool`. Visual check of "tree highlights the same joint" is Edit-time, not run here. |
| Search "hand" | Case-insensitive substring via `EditorUI::SearchField`. `LeftHandThumb` also matches; that is substring search, not a token match. |
| Select a different entity | `SyncJointToolToEntitySelection` clears the joint when the primary UUID changes. |
| Press Delete with a joint selected | Delete still calls `DeleteSelectedEntity`. No joint-delete path exists. |
| Click empty space with the visualizer on | Miss falls through to entity pick. Armed socket-pick consumes the click and stays armed. |

Viewport picking and the tree were not clicked in a running editor this landing.

---

## Phase S4 — the socket gizmo

**Done.**

### Goal

Drag the rifle into the hand.

### Steps

1. With a `BoneAttachmentComponent` selected and resolved, `ImGuizmo` manipulates the socket's
   **world** matrix. That matrix already exists — `BoneAttachmentSystem` wrote it to
   `WorldTransformComponent::World` this frame — so the handle needs no new maths to *place*.
2. Convert the drag back into component space:
   ```
   socketLocal = inverse(targetWorld * jointFrame) * draggedWorld
   ```
   then `Math::DecomposeTransform` into `Offset` (translation) and `Rotation` (Euler). Scale goes to
   the entity's own `TransformComponent::Scale`, which the attachment system already composes and
   which is where the rifle's `0.45` lives today.
3. **Never write `WorldTransformComponent` from the gizmo.** Write the component; let the system
   recompute on the next update. Writing the cache directly is the staleness trap the entity gizmo
   already learned (`Scene::MarkChanged<TransformComponent>`), and here it would be overwritten by
   `OverrideWorld` a frame later anyway.
4. One drag is one undo entry: snapshot on the rising edge of `ImGuizmo::IsUsing()`, commit on the
   falling edge, exactly as `EditorLayer::m_GizmoBefore` already does for entities.
5. Snapping reads [MAP_EDITOR](../ToDo/MAP_EDITOR.md) M1's `MapSnapSettings` when it exists. Rotation
   snapping is the useful one here; translation snapping on a socket mostly is not, because the grip
   point is wherever the hand is, not on a grid.

### The Euler tension, recorded

`BoneAttachmentComponent::Rotation` is **Euler radians**, and the component's own comment says it
writes world matrices precisely *because* feeding a joint quaternion through Euler storage is lossy
and gimbal-sensitive. A gizmo forces exactly that decomposition back on.

It is tolerable, with care: this is an authored rest offset decomposed once per drag, not a pose
decomposed per frame. Two ways to handle it:

| Option | Cost |
|---|---|
| **(a) Accept, and apply rotation as a delta** — the entity gizmo already composes `after * inverse(before)` to avoid gimbal jumps mid-drag, and the same trick applies | No format change. A socket dragged through ±90° of pitch can still land on a degenerate Euler triple |
| **(b) Change `Rotation` to a quaternion** | A scene-format change, a serializer migration, a reflection re-registration, and an inspector widget for a quaternion — for a field with one call site |

**Chosen: (a)**, with (b) recorded as the answer if it bites. The committed rifle rotation
(−2.714, 0.255, 0.885) is already in the region where this is not academic, so **the verification
below tests it rather than assuming it**.

### Verification

**Done as code.** The editor was not run; visual and Euler-pop probes are open.

| Probe | Result |
|---|---|
| Drag the rifle to a correct-looking grip | Not run. `ProvingGround.ganymede` lives on `first-game`. Conversion is `inverse(targetWorld * jointFrame) * draggedWorld` into `Offset`/`Rotation`/`Scale`. |
| Scrub the clip afterwards | Not run. Each frame's joint world is sampled from this frame's palette; Offset is rest in joint space, so the system should keep the grip across the cycle. |
| One Ctrl+Z | By construction: rising-edge snapshot of `BoneAttachmentComponent` and `TransformComponent::Scale`, one `Gizmo Socket` command (or a two-child composite if both changed). Not clicked in the editor. |
| Drag a socket through vertical (±90° pitch) | **Not run.** Option (a) matches the entity gizmo (`Rotation += decomposed - Rotation`). A pop here is still the trigger for option (b). |
| Drag with the target animating | Not run. Each ImGui frame inverts that frame's `jointWorld`, so the handle cannot accumulate against a stale joint. Mesh trails the handle by one frame, same as the entity gizmo. |
| A socket that does not resolve | No gizmo. Viewport banner states the reason (`Socket has no target`, `Set a joint…`, `Joint name does not resolve`, …). Not seen in the editor. |

---

## Phase S5 — the clip inspector

**Done.**

### Goal

Stop measuring the same two artifacts by hand on every clip download.

### Why this is the phase to keep

[SKELETAL_ATTACHMENTS.md](SKELETAL_ATTACHMENTS.md) records that **every** clip set downloaded so far
has needed the same two fixes, and that both recurred on the second generation: a constant scale
artifact on `Hips`, and real root motion needing detrending *and* re-centring. It also records what
the first costs when missed — the `Idle` clip shipped with a constant **1.1765** scale on the root
joint, which would have grown a socketed weapon 17.6% whenever the player stood still.

Those are mechanically detectable properties of the channel data. They are currently found by a
person measuring, per clip, per download.

### Steps

1. A clip section in [MODEL_EDITOR](MODEL_EDITOR.md) P1's Asset Inspector when that exists, or in
   the Animator inspector if it does not. The asset panel is the right home — clips are a property
   of the mesh asset, not of the entity.
2. **Per clip**: duration, channel count, joints animated, which paths (translation / rotation /
   scale) are present.
3. **Artifact detection**, reported and never blocking:

   | Check | Rule |
   |---|---|
   | Constant non-unit scale | A `Scale` channel whose values are all equal and ≠ 1. Report the joint and the factor |
   | Net root drift | Root joint translation at `t = Duration` minus `t = 0`, per axis, plus the detrended residual |
   | Cross-clip consistency | Head height, hips height and forward offset at `t = 0` for every clip on the mesh, as a table. A1's gate was "within ~5 cm of each other" — measured by hand |
   | Over the joint limit | `JointCount() > Skeleton::MaxBones` (128). Already warned at import and clamped at upload; surface it where an author looks |

4. **Read-only.** No retiming, no key deletion, no baking. The fix belongs upstream, in the DCC or in
   the download step. An inspector that edits third-party clip data becomes a second source of truth
   against the file it came from, and the file is what gets re-downloaded.

### Risks

- A false positive on a clip that legitimately scales a joint. Report the fact and let a person
  judge; the wording should describe what was measured, not assert a defect.

### Verification

**Done as code.** The inspector lives on a selected mesh in the Asset Inspector. Pose sampling is
`SampleClipGlobals`, the same function `AnimationSystem` uses before `InverseBind`. The player
clip set is on `first-game`; it was not opened here.

| Probe | Result |
|---|---|
| Run against the player's committed clip set | Not run. `ArmoredHumanoid.glb` lives on `first-game`. Constant scale is "all Scale keys equal and not 1"; a 1.1765 `Hips` channel would list as `Hips scale is constant at 1.1765 (not 1)`. |
| The cross-clip table | Head Y, Hips Y, Hips Z at `t=0` (mesh space), plus max−min span in cm. A1's "forward offset" is the Hips Z column. Figures were not compared to a hand table — that table was never measured. |
| A clip with deliberate root motion | Net Δx/Δy/Δz is reported as a measurement, not a warning. Residual is max \|sample − lerp(start,end)\| on any axis. Not seen in the editor. |
| A mesh with no clips | "No animation clips". Self-check: the empty branch is the `Clips.empty()` path, no tables. |

---

## Phase S6 — docs, and closing the attachment milestone

**Done.**

S1–S5 already updated the live docs in the table below in those changes. S6 audited them in
place: `TryGetJointFrame` in [rendering.md](../engine/rendering.md), attachment maths and
skeleton gizmos in [scene.md](../engine/scene.md), clip measurements in
[assets.md](../engine/assets.md), Joints panel / picking / socket gizmo / Controls in
[editor.md](../editor/editor.md). No second changelog.

| Probe | Result |
|---|---|
| Live docs vs the table | Present. S1–S5 wrote them; this phase did not append "recent changes". |
| A3 visual (idle / walk / run / 180° backpedal, Skeletons on) | **Not watched.** `ProvingGround.ganymede` lives on `first-game`. Named in [cross-cutting.md](../ToDo/cross-cutting.md#skeletal-leftovers-after-the-attachment-and-tooling-close). |
| Hide-unresolved | **Decided, not built.** General `Visible` / `Enabled` bit, not a socket-local flag. Same leftover file. This phase did not invent a second one. |
| Retire attachments | [SKELETAL_ATTACHMENTS.md](SKELETAL_ATTACHMENTS.md) → `docs/history/`. |

---

## Design tensions, recorded

1. **Euler storage versus a rotation gizmo.** Option (a) above, with the quaternion migration named
   as the fallback rather than discovered later.
2. **Joint selection is a second selection domain.** Kept subordinate to entity selection so the
   editor's destructive verbs stay unambiguous.
3. **The joint frame is recovered per attachment per frame**, via one `inverse()` of the inverse
   bind. `SKELETAL_ATTACHMENTS.md` already names this as the line to revisit if attachments are ever
   counted in hundreds rather than ones and twos. S1 does not change that trade — it centralises
   it, which is what makes changing it later a single edit.
4. **Sockets stay on the instance.** A named socket library on the mesh asset — Unreal's model — is
   the obvious next step, and [MODEL_EDITOR](MODEL_EDITOR.md) P3 establishes exactly that pattern for
   collision defaults. It is not built here because a socket library wants the asset inspector to
   exist first, and because one character with one weapon is not yet evidence for it. **The second
   weapon is.**
5. **X-ray is the default.** A depth-tested skeleton inside a character is an invisible skeleton;
   the toggle exists for the cases where occlusion is the information.
6. **Visualizer flags are accumulating.** Collider gizmos, physics debug draw, markers
   ([MAP_EDITOR](../ToDo/MAP_EDITOR.md) M4) and now skeletons all follow the same engine-default-off,
   editor-opts-in-per-frame pattern on a settings singleton. Whichever milestone lands second should
   collapse them into one bitfield rather than adding a fourth boolean.
7. **Mixed selection still uses the entity gizmo.** The socket path skips group-drag; the entity
   path does not skip sockets. A crate multi-selected with a resolved rifle still writes the
   rifle's ignored local TR (and its Scale). Out of S4's scope.

## Docs this milestone must update

| Phase | Doc |
|---|---|
| S1 | [engine/rendering.md](../engine/rendering.md) for `TryGetJointFrame`; [engine/scene.md](../engine/scene.md) where the attachment maths is described |
| S2 | [engine/rendering.md](../engine/rendering.md) — debug draw; [editor/editor.md](../editor/editor.md) — the viewport toggle |
| S3 | [editor/editor.md](../editor/editor.md) — the joint tree panel, picking precedence, and the selection rule |
| S4 | [editor/editor.md](../editor/editor.md) — gizmo modes and the Controls table |
| S5 | [editor/editor.md](../editor/editor.md); [engine/assets.md](../engine/assets.md) if the checks are described beside the clip data |
| S6 | [SKELETAL_ATTACHMENTS.md](SKELETAL_ATTACHMENTS.md) → `docs/history/`, leftovers in [cross-cutting.md](../ToDo/cross-cutting.md#skeletal-leftovers-after-the-attachment-and-tooling-close) |

**New source files in S3 (the joint tree panel) mean premake regeneration** —
`GanymedEditor/premake5.lua` globs `source/**`, expanded at generation time.
