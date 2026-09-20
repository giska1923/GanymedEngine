# Milestone — Skeletal attachments (bone sockets)

**Status: A2 implemented. A1 (carry clips) and A3 (Proving Ground wiring) remain.** A1 is
content — Meshy credits, clip pick, post-process — and is still the cheap disproof that current
unarmed clips will look worse with a rifle in the hand. A3 lives on the game branch.

The engine can pin an entity to a joint (`BoneAttachmentComponent` + `BoneAttachmentSystem`). A
character still cannot *look* armed until A1 ships carry clips and A3 wires `Rifle.glb` to
`RightHand`. That remaining gap is content and wiring, not a missing capability.

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
> splits its write-ups that way; see [README.md](README.md). The facts this plan depends on are
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
- **Not IK, not look-at, not procedural aim.** Attachment reads the pose; it never writes to it.
- **Not clip blending.** There is still no crossfade — `PlayAnimation` restarts on a clip change.
  A weapon on a socket does not need blending; it only makes the lack more visible.
- **Not the separate clip asset.** That is its own item in [cross-cutting.md](cross-cutting.md)
  and is independent of this one.

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

`Scene.cpp` registers `AnimationSystem` before `TransformSystem`, with the comment *"after anything
that moves entities... and before anything that reads world space"*. The attachment system belongs
**immediately after `TransformSystem` and before `CameraSystem`**: the palette is fresh, the
target's world matrix has been published, and nothing has read world space yet. A camera socketed
to a head joint therefore works, which it would not if this ran any later.

It writes `WorldTransformComponent::World` directly:

```
world = targetWorld * jointGlobal(joint) * offsetTransform
```

### Why not write the local transform instead

Running *before* `TransformSystem` and writing `TransformComponent` was the first sketch, and it is
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
- **Cost:** 3 credits per action, re-issued as one `action_ids` set (clips must ship in one file;
  see [cross-cutting.md](cross-cutting.md)).
- **Post-processing is not optional.** Every download so far has needed the same two fixes, and
  both recurred on the second generation: a constant scale artifact on `Hips` in at least one
  clip, and real root motion that has to be detrended *and* re-centred. Measure before installing.
- **Gate:** the clip set plays on the existing player with head height, hips height and forward
  offset within ~5 cm of each other across all clips, and zero net root drift. **No engine change
  has happened at this point** — the character simply mimes holding a weapon that is not there.

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

### A2 follow-up — one item left

The socket frame left out the skinned submesh's `LocalTransform`, which `Renderer3D` applies and
the socket did not — so a socket on a rig whose joints are centimetres and whose vertices are
metres landed at 141 **metres** rather than 1.41. A correctly sized prop that far away looks tiny,
which invites a compensating `Scale` and produces an 86 m rifle the moment the socket fails to
resolve. `OverrideWorld` also discarded the attached entity's `Scale`, so that compensation had
nowhere honest to live. Both are **fixed**: `LocalTransform` is folded in, the bind pose's basis
scale is divided back out, and local `Scale` is composed. See `docs/engine/scene.md`.

Still open:

- **Hide an entity whose socket does not resolve**, rather than leaving it at its parent transform.
  The window is short — the frames before a skinned mesh finishes streaming, on every load — and
  now that scale is no longer compensated it is only a brief pop rather than a wrong-sized prop.
  There is no visibility flag on any component today, so this is new surface, not a tweak: either
  an `Enabled`/`Visible` bit that `RenderSystem` honours (useful well beyond sockets) or a
  socket-local suppression. Worth deciding which before building either.

### A3 — Wiring, on the game branch — **done**

The rifle is socketed to the player's `RightHand`: one entity, `Scale: 0.45`, `Offset` in metres.
Recorded with the derivation and its measured gate in
[PROVING_GROUND.md](PROVING_GROUND.md), which is where content work belongs.

Two things from it worth keeping here, because they are about the feature rather than the game:

- **A rig's joints and its vertices need not share a unit.** This one has joints in centimetres
  and vertices in metres. That is what the `LocalTransform` fold-in above exists for, and it is
  invisible until something rigid is pinned to a joint.
- **A wrong premise costs as much as wrong maths.** The first attempt derived the barrel axis from
  `LeftHand - RightHand`, assuming the support hand rides the handguard. At idle this rig's hands
  are 0.937 m apart. The maths was right and the answer was 40 deg off.

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
- **The editor needs the *target's* skeleton to populate a joint dropdown.** The clip combo in
  `SceneHierarchyPanel` reads `mesh->GetClips()` off the same entity; this reads
  `mesh->GetSkeleton().JointNames` off a different one, which the inspector does not currently do
  for any component.
- **`Scene::Copy` runs on play.** `Resolved` is a runtime index and must reset, the same way
  `AnimatorComponent::Time` and `Palette` already do.

## Files it touches

Taken from the set of files that currently mention `AnimatorComponent` — the closest existing
analogue, being a serialized, reflected, editor-visible, script-reachable component read by a
system:

| File | Why |
|---|---|
| `Scene/Components.h` | the struct |
| `ECS/ComponentTraits.h`, `Reflection/ComponentReflection.cpp` | registration and reflection |
| `Scene/SceneSerializer.cpp`, `Scene/SceneYaml.cpp` | read and write |
| `Scene/Scene.cpp` | system registration, in the slot named above |
| `Scene/Systems/BoneAttachmentSystem.{h,cpp}` | **new** — regeneration required |
| `Scene/Systems/TransformSystem.h` | `OverrideWorld` (chosen) |
| `GanymedEditor/source/Panels/SceneHierarchyPanel.cpp`, `EditorUndo.h` | inspector |
| `Scripting/ScriptBindings.cpp` | `AttachToBone` / `DetachFromBone` |
| `docs/engine/scene.md`, `docs/engine/ecs.md` | **in the same change** |

## Branch policy

Unchanged from [PROVING_GROUND.md](PROVING_GROUND.md): A2 is engine work and lands on `master`
first; A1 and A3 are content and wiring and live on the game branch. The check stays mechanical:

```
git diff --stat master.. -- GanymedEngine/source GanymedEditor/source GanymedRuntime/source
```
