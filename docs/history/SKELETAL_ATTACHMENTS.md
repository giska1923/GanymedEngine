# Milestone — Skeletal attachments (bone sockets)

**Status: A1, A2 and A3 have landed as mechanism and wiring. S6 retired this file to
`docs/history/`.** Mechanical verification is written up below. The visual A3 gate (rifle stays
in the hand through the carry clips) and a visibility bit were named rather than deferred by
silence; they now live in
[cross-cutting.md](../ToDo/cross-cutting.md#skeletal-leftovers-after-the-attachment-and-tooling-close).
The engine can pin an entity to a joint (`BoneAttachmentComponent` + `BoneAttachmentSystem`), the
character runs a weapon-carry clip, and the rifle is socketed to `RightHand`.

Evidence, on `first-game`, in `Game/assets/scenes/ProvingGround.ganymede`: the player body carries
`AnimatorComponent { Clip: Lower_Weapon_Look_Raise }` and its `Rifle` child carries a
`BoneAttachmentComponent` naming `RightHand` with a hand-tuned offset and rotation.

The offsets were placed by hand, which is the bottleneck this plan said to watch for. That
evidence became [SKELETAL_TOOLING.md](SKELETAL_TOOLING.md).

---

## Why this is not solvable in content

Two alternatives were tried on the way here. Both failed, for reasons worth recording, because
they are the two things anyone would try first.

**Model the weapon into the character.** Meshy's first player generation came back holding a rifle
across the chest. Auto-rigging weights that geometry to whichever joints are nearest, so the
rifle's right end binds to `RightHand`, its left end to `LeftHand`, and its middle to the chest.
The moment a walk cycle swings the arms independently, the rifle stretches between them like
rubber. That generation was discarded and re-rolled unarmed.

> The detailed record of that — the prompts, the measurements, the re-roll — is in
> `PROVING_GROUND.md` **on the game branch**, not in this copy. The milestone's own branch policy
> splits its write-ups that way; see [ToDo README](../ToDo/README.md). The facts this plan depends on are
> restated here rather than linked, so nothing below relies on reading the other branch.

**Parent the weapon to the character entity.** It then hangs at a fixed offset from the capsule and
ignores every arm animation, which reads as a bug rather than as a weapon.

Neither is a near miss. A joint-space attachment is the actual mechanism, and it is what every
engine of this kind provides: Unreal's sockets, Unity's bone transforms, Godot's
`BoneAttachment3D`.

## What it deliberately is not

- **Not a socket editor.** No named socket assets authored onto a skeleton, no gizmo for placing
  them. A joint name plus an offset, typed into the inspector. If placing offsets by hand becomes
  the bottleneck, that is the evidence for building more.
  — **It did.** The rifle's offset and rotation were placed by dragging sliders, and the committed
  values say so. That evidence is [SKELETAL_TOOLING.md](SKELETAL_TOOLING.md); this bullet stands as
  the scope decision it was, not as a claim about what should exist now.
- **Not IK, not look-at, not procedural aim.** Attachment reads the pose; it never writes to it.
- **Not clip blending.** There is still no crossfade — `PlayAnimation` restarts on a clip change.
  A weapon on a socket does not need blending; it only makes the lack more visible.
- **Not a separate clip asset.** A rigged `.glb` ships mesh, skin and clips in one file, and the
  engine keeps them there on purpose — separate clip assets would buy identity surgery and nothing
  else (see [assets.md](../engine/assets.md#skinning-data)). Clips therefore have to be generated
  as one `action_ids` set rather than downloaded one at a time. _(This bullet and the note under A1
  both used to cite an item in `cross-cutting.md`; no such item exists there, so the constraint is
  stated here against the doc that actually records it.)_

---

## What already exists, and what is thrown away

`AnimationSystem::BuildPalette` composes the pose every frame:

```cpp
m_Globals[i]  = base * m_Locals[i].ToMatrix();      // the joint's transform in skin space
outPalette[i] = m_Globals[i] * skeleton.InverseBind[i];
```

**The joint transform this feature needs is already computed.** It is immediately multiplied by the
inverse bind into a skinning matrix, and `m_Globals` is a system-owned scratch vector reused for
the next entity, so nothing survives the frame except the palette.

That is the whole gap: one value per joint, computed and then discarded.

### Recovering it rather than storing it

```
jointGlobal(i) = Palette[i] * inverse(InverseBind[i])
```

One 4x4 inverse per attachment per frame. The alternative — keeping `m_Globals` on the component
beside `Palette` — costs another 24 to 128 `mat4`, i.e. **2 to 8 KB per animated entity**, for a
feature most entities will never use, and it widens the struct `Scene::Copy` fixes up on every
play. Recovery is the right trade while attachments are counted in ones and twos. If they are ever
counted in hundreds, this is the line to revisit.

---

## The design

```cpp
struct BoneAttachmentComponent
{
    // The entity carrying the skinned mesh and its AnimatorComponent. Zero means "my parent",
    // which is the common case and keeps simple setups from having to name anything.
    UUID        Target;
    // By name, like AnimatorComponent::Clip - and for the same reason: a rename in the DCC should
    // fail loudly rather than silently attach to whatever joint 7 happens to be.
    std::string Joint;
    glm::vec3   Offset{ 0.0f };
    glm::vec3   Rotation{ 0.0f };

    // Runtime. Not serialized; re-resolved when the target's mesh changes.
    int32_t     Resolved = -1;
};
```

### Where it runs

`Scene.cpp` registers `AnimationSystem` before `TransformSystem`, with the comment _"after anything
that moves entities... and before anything that reads world space"_. The attachment system belongs
**immediately after `TransformSystem` and before `CameraSystem`**: the palette is fresh, the
target's world matrix has been published, and nothing has read world space yet. A camera socketed
to a head joint therefore works, which it would not if this ran any later.

It writes `WorldTransformComponent::World` directly:

```
world = targetWorld * jointGlobal(joint) * offsetTransform
```

### Why not write the local transform instead

Running _before_ `TransformSystem` and writing `TransformComponent` was the first sketch, and it is
wrong for a reason that is easy to miss: **`TransformComponent` stores rotation as Euler angles.**
Feeding a joint's orientation through Euler decomposition is lossy and gimbal-sensitive, and a
socket passing through ±90° of pitch would pop. Writing the world matrix keeps the whole path in
matrices and never decomposes anything.

### Descendants — chosen: `OverrideWorld`

Writing a world matrix after `TransformSystem` has run leaves the attached entity's **children**
holding matrices composed from the pre-attachment value. A muzzle-flash emitter parented to the
gun is exactly that case, so this is not hypothetical.

`TransformSystem::RecomputeSubtree(Entity, const glm::mat4&)` already does precisely the right
thing — writes the cache, then walks the children with the freshly computed matrix, idempotent
within a pass — but it is **private**. Three ways out, in order of preference:

1. **Make it public**, or add a narrow `OverrideWorld(Entity, const glm::mat4&)` that calls it.
   One traversal, one owner, smallest diff. The risk is that it invites anyone to stomp the
   transform cache.
2. **Duplicate the walk** in the attachment system. About fifteen lines and no coupling, but now
   two places know how the cache is maintained, and they will drift.
3. **Fold attachment into `TransformSystem`** as a second pass. No ordering question at all, but it
   widens that system's declared access from transforms to animators, meshes and skeletons — a
   real cost in a codebase where `ValidateOrdering()` depends on those declarations meaning
   something.

**Chosen: (1)** — `TransformSystem::OverrideWorld(Entity, const glm::mat4&)`. It clears
`m_Visited` (the dirty pass has already marked every touched entity, so calling
`RecomputeSubtree` without that would no-op) and pushes the matrix down the subtree. The
cache-stomp risk is accepted; `BoneAttachmentSystem` is the one caller.

---

## Phases

### A1 — Rifle-carry clips, before any engine work

**Done — the clip set shipped.** The player body runs `Lower_Weapon_Look_Raise`. Verification
below: the three clips are in the glb; the ~5 cm table was not measured.

Regenerate the player's clip set with weapon-carry animations rather than the unarmed idle, walk
and run it has now.

- **Why first:** it is the cheap disproof. A socket with the current clips gives a character
  running with a rifle swinging loose in one hand while both arms pump — **worse than carrying
  nothing.** Building the feature first would deliver a working capability that demonstrates a bad
  result, and the clips cost a few credits against a day of engine work.
- **Candidates in Meshy's library:** `511` Rifle Charge, `573` Rifle Turn Left, `585` Rifle Aim
  Turn Right, `686` Walk Backward with Gun 1, `425` Vault with Rifle. Pick by fetching each
  `preview_url` GIF and comparing mid-stride frames — that is free, and it is how the current run
  clip was chosen after the first one turned out to be a head-down lunge.
- **Cost:** 3 credits per action, re-issued as one `action_ids` set — clips must ship in one file,
  because they live inside the `Mesh` asset ([assets.md](../engine/assets.md#skinning-data)).
- **Post-processing is not optional.** Every download so far has needed the same two fixes, and
  both recurred on the second generation: a constant scale artifact on `Hips` in at least one
  clip, and real root motion that has to be detrended _and_ re-centred. Measure before installing.
- **Gate:** the clip set plays on the existing player with head height, hips height and forward
  offset within ~5 cm of each other across all clips, and zero net root drift. **No engine change
  has happened at this point** — the character simply mimes holding a weapon that is not there.

**Verification (A1).** The clip set shipped inside `ArmoredHumanoid.glb` on `first-game`. The glTF
skin has **three** animations, named exactly as `Player.lua` selects them:

| Speed       | Clip                          | `animSpeed` |
| ----------- | ----------------------------- | ----------- |
| `< 0.5` m/s | `Lower_Weapon_Look_Raise`     | 1.0         |
| `< 4` m/s   | `Walk_Forward_While_Shooting` | 1.0         |
| else        | `Run_and_Shoot`               | 1.7         |

There is no dedicated backpedal clip. Holding S turns the mesh 180° and plays walk/run forward —
that is the "backpedal turn" the A3 gate names, not Meshy's `Walk Backward with Gun 1`.

The **~5 cm cross-clip table was never measured.** Reconstructing it from a transcript would be
inventing the gate. [SKELETAL_TOOLING.md](SKELETAL_TOOLING.md) S5 is the readout; it reports Head Y /
Hips Y / Hips Z at t=0 per clip. Those figures were not compared to a hand table on this branch.

### A2 — The engine feature, on `master`

**Done on `skeletal-attachments`.** `OverrideWorld` is public; `BoneAttachmentSystem` runs after
`TransformSystem` and before `CameraSystem`; `Resolved` resets on `Scene::Copy`; a bad joint
name warns once and restores parent-relative world. New files: `BoneAttachmentSystem.{h,cpp}` —
**project regeneration is required.**

- **Tests still to run against a character:** a rig with no finger joints; a socket on a moving,
  animating character; a socket whose target's mesh is swapped at runtime (does `Resolved`
  re-resolve?); a socket naming a joint that does not exist; a socket with children of its own.
- **Gate (needs A1 clips + a scene):** an entity parented to `RightHand` tracks the hand through
  a full run cycle with no visible lag and no drift over several minutes; its own children track
  it; a bad joint name warns once and leaves the entity at its parent's transform rather than at
  the origin.

**Verification (A2).** Mechanical probes, against the committed player mesh and the system as it
stands. Visual lag / multi-minute drift were not timed — that is the A3 picture, and S2 is what
makes the hand a thing you can see.

| Probe                                   | Result                                                                                                                                                                                                                                                                                                           |
| --------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Rig with no finger joints               | `ArmoredHumanoid.glb` skin: **24 joints**. `LeftHand` / `RightHand` are terminals. No finger, thumb, or toe-beyond-`ToeBase` joints. A weapon attaches at the wrist, as the hazard below said.                                                                                                                   |
| Socket on a moving, animating character | `BoneAttachmentSystem` runs every frame in play _and_ edit, after `AnimationSystem` has written `Palette` and `TransformSystem` has published `targetWorld`. Same-frame pose; no extra delay by construction.                                                                                                    |
| Target mesh swapped at runtime          | `ResolveJointIndex` keeps `Resolved` only while `JointNames[Resolved] == Joint`; otherwise it walks the name list. `Scene::Copy` / duplicate / deserialize reset `Resolved` to −1 so a stale index cannot attach to whichever joint now occupies that slot. Not exercised by swapping the player's mesh in play. |
| Joint name the skeleton does not have   | Warns once per distinct failure (`WarnOnce`) and `Restore()`s parent-cache × local. Never the origin. Empty `Joint` is quiet. Not planted as a typo in the committed scene (`RightHand` is present).                                                                                                             |
| Socket with children of its own         | `TransformSystem::OverrideWorld` clears `m_Visited` and walks the subtree. The committed `Rifle` has **no children**. `Muzzle` is still parented to `Yaw`. The walk is untested with a live child.                                                                                                               |
| Bad inverse bind                        | Singular `InverseBind` returns false from `TryGetJointFrame`, warns once, `Restore()`. Self-checked with a zero matrix.                                                                                                                                                                                          |

The run-cycle gate (tracks the hand, no visible lag, no drift over minutes) is the A3 picture. It
was not watched from this branch.

### A2 follow-up — LocalTransform (fixed) and visibility (decided)

The socket frame left out the skinned submesh's `LocalTransform`, which `Renderer3D` applies and
the socket did not — so a socket on a rig whose joints are centimetres and whose vertices are
metres landed at 141 **metres** rather than 1.41. A correctly sized prop that far away looks tiny,
which invites a compensating `Scale` and produces an 86 m rifle the moment the socket fails to
resolve. `OverrideWorld` also discarded the attached entity's `Scale`, so that compensation had
nowhere honest to live. Both are **fixed**: `LocalTransform` is folded in, the bind pose's basis
scale is divided back out, and local `Scale` is composed. See `docs/engine/scene.md`.

**Decided, not built — hide an unresolved socket.**

The window is short — the frames before a skinned mesh finishes streaming, on every load — and
now that scale is no longer compensated it is only a brief pop rather than a wrong-sized prop.

There is no runtime visibility flag on any component today. The outliner eye is
`EditorViewFilter::HiddenEntities`, an editor filter that `OnUpdate` clears so Play and the
runtime draw everything. Reusing it would hide the rifle in the editor and show it in the game,
which is the opposite of the streaming pop.

| Option                                                          | Why not / why                                                                                                                                |
| --------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------- |
| Socket-local suppression                                        | A second visibility system the day anything else needs to hide. This note exists so that does not happen.                                    |
| **`Visible` / `Enabled` on a component `RenderSystem` honours** | The engine-shaped answer (Unity renderer enabled, Unreal hidden-in-game). Useful for cutscenes, inventory, pooling — sockets are one client. |

**Chosen: the general bit.** Not built here: inventing a visibility component as a side quest of
joint-frame extraction is the over-build, and the pop is brief. The next piece of work that
actually needs to hide something implements it, and sockets piggy-back. Do not add a socket-only
flag in the meantime.

### A3 — Wiring, on the game branch — **done**

**Done — the rifle is socketed.** `Rifle` is a child of the player body with
`BoneAttachmentComponent { Target: 0, Joint: RightHand }` and a `0.45` local scale. Verification
below: wiring confirmed; the grip was not watched; `Muzzle` is still on `Yaw`.

Attach `Rifle.glb` to the player's `RightHand`, offset by hand against the new clips. Either retire
the hovering rifle pickup at the Weapon Crate or keep it and attach on collect — the latter is more
interesting and costs nothing extra, since `Pickup.lua` already knows when the crate is consumed.

Two things from it worth keeping here, because they are about the feature rather than the game:

- **Gate:** the rifle stays in the hand through idle, walk, run and the backpedal turn, and the
  muzzle particle emitter can be moved from `Yaw` onto the gun's barrel without changing where
  shots go.

**Verification (A3).** Wiring, on `first-game` `ProvingGround.ganymede`, as committed:

| Piece                 | State                                                                                                                                                                                                                                                                 |
| --------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `Rifle` parent        | Body (`3000000000000000013`), `Target: 0` → parent                                                                                                                                                                                                                    |
| `Joint`               | `RightHand`                                                                                                                                                                                                                                                           |
| `Offset` / `Rotation` | `[-0.2487, 0.1440, -0.0064]`, `[-2.7143, 0.2545, 0.8852]` — the slider residue that triggered [SKELETAL_TOOLING.md](SKELETAL_TOOLING.md)                                                                                                                              |
| Local `Scale`         | `0.45` on the rifle entity, not a compensating child                                                                                                                                                                                                                  |
| `Muzzle`              | **Still parented to `Yaw`**, translation `[0, 0.5, -1]`. The "move onto the barrel" half of the gate was not done. Shots still leave from Yaw-space; moving the emitter would change where they appear, not yet where they go, until fire is re-derived from the gun. |
| Idle / walk / run     | The three A1 clips, selected by capsule speed in `Player.lua`                                                                                                                                                                                                         |
| Backpedal turn        | Mesh yaw eased 180° when moving opposite the camera; same walk/run clips, not a backward cycle                                                                                                                                                                        |

The "stays in the hand" picture was not watched from this branch. S2 draws the joints; S6 did not
re-run the gate (`ProvingGround` lives on `first-game`). Until then this section records the
wiring, not the grip. Leftover:
[cross-cutting.md](../ToDo/cross-cutting.md#skeletal-leftovers-after-the-attachment-and-tooling-close).

---

## Hazards found while scoping

- **A socket inherits whatever the clip does to the joint chain, including scale.** The player's
  `Idle` clip shipped with a constant 1.1765 scale on `Hips`, the root joint; had a socket existed
  then, the weapon would have grown 17.6% whenever the player stood still. Clips are data from a
  third party, so this is a class of bug rather than a one-off. The feature should not paper over
  it, but whoever debugs "my weapon changes size" should find this paragraph.
- **This rig has no finger joints.** `LeftHand` and `RightHand` are the terminal arm joints, so a
  weapon attaches at the wrist and the hand cannot close around it. With a carry clip the grip pose
  is baked into the animation, which is why A1 comes first. Without one, no socket offset will make
  it look held.
- **The editor needs the _target's_ skeleton to populate a joint dropdown.** **Built.** The
  `BoneAttachmentComponent` section resolves the target entity (drop from the outliner, or the
  hierarchy parent when `Target` is zero) and fills a combo from
  `mesh->GetSkeleton().JointNames` on _that_ entity — the first inspector section to read a
  component off a different entity than the one selected. [SKELETAL_TOOLING.md](SKELETAL_TOOLING.md)
  S2 draws where those joints are; S3 picks them.
- **`Scene::Copy` runs on play.** `Resolved` is a runtime index and must reset, the same way
  `AnimatorComponent::Time` and `Palette` already do.

## Files it touches

Taken from the set of files that currently mention `AnimatorComponent` — the closest existing
analogue, being a serialized, reflected, editor-visible, script-reachable component read by a
system:

| File                                                                  | Why                                          |
| --------------------------------------------------------------------- | -------------------------------------------- |
| `Scene/Components.h`                                                  | the struct                                   |
| `ECS/ComponentTraits.h`, `Reflection/ComponentReflection.cpp`         | registration and reflection                  |
| `Scene/SceneSerializer.cpp`, `Scene/SceneYaml.cpp`                    | read and write                               |
| `Scene/Scene.cpp`                                                     | system registration, in the slot named above |
| `Scene/Systems/BoneAttachmentSystem.{h,cpp}`                          | **new** — regeneration required              |
| `Scene/Systems/TransformSystem.h`                                     | `OverrideWorld` (chosen)                     |
| `GanymedEditor/source/Panels/SceneHierarchyPanel.cpp`, `EditorUndo.h` | inspector                                    |
| `Scripting/ScriptBindings.cpp`                                        | `AttachToBone` / `DetachFromBone`            |
| `docs/engine/scene.md`, `docs/engine/ecs.md`                          | **in the same change**                       |

## Branch policy

Unchanged from [PROVING_GROUND.md](../ToDo/PROVING_GROUND.md): A2 is engine work and lands on `master`
first; A1 and A3 are content and wiring and live on the game branch. The check stays mechanical:

```
git diff --stat master.. -- GanymedEngine/source GanymedEditor/source GanymedRuntime/source
```
