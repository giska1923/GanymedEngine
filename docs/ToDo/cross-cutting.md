# ToDo — Cross-cutting

Gaps that belong to no single subsystem.

---

## macOS is unbuilt and unverified

**Linux is done** — built (Debug, Release, Dist) and run, with the status table and the conformance
findings in [build-and-tooling.md](../engine/build-and-tooling.md#platform-status).

macOS has still never been compiled. Every verification claim in [`docs/history/`](../history/) is
Windows, and now Linux; nothing is clang, Xcode, Metal or arm64. The places most likely to break,
narrowed by what Linux actually hit:

- **The shader profile set.** `ProfileDirectory()` in `Shader.cpp` asks for `compiled/metal/` on
  macOS; if the profile folder ↔ backend mapping and the compile script disagree, every shader
  silently fails to load. Linux did not test this — it shares the `glsl` profile with Windows'
  OpenGL path, and those shaders loaded unchanged.
- **The `angledIncludeDirs` / `xcode4` machinery** in the workspace premake exists solely for
  Xcode and has never been exercised. Same for the `.app`-bundle avoidance in `GanymedEditor`'s
  `kind`.
- **arm64.** The workspace pins `x86_64` and every platform repeats the AVX2 flag set. A native
  Apple-silicon build needs those dropped and `JPH_USE_*` swapped for Jolt's NEON path — that is a
  port, not a build fix.
- **The link lists.** Linux proved these go stale silently: `enkiTS` and `TextureEncode` were in
  the engine's `links` and nowhere else, and only GNU ld noticed. The macOS lists were fixed in the
  same change, but by inspection, not by linking.

Not scheduled, for the reason the Linux entry carried: a platform you do not build is a platform you
do not support, and deciding to support one is a bigger call than a ToDo entry.

## The Linux build is verified in WSL2, not on a real Linux machine

Distinct from the above, and smaller. The Linux build and run happened under WSL2 (Ubuntu 22.04,
gcc 11.4). Native hardware has now been tried for Vulkan; GL and audio still have not.

- **Vulkan / Intel ANV.** WSL has no Vulkan loader, so bgfx fell through to OpenGL. On a native
  Linux box Vulkan is the backend bgfx picks first. A native Ubuntu run on Intel UHD (TGL GT1)
  brought Vulkan up, created the swapchain, loaded shaders, submitted the IBL bake, then died in
  the first `bgfx::frame()` with Mesa ANV `GPU hung on one of our command buffers` /
  `VK_ERROR_DEVICE_LOST`. Surface creation was not the problem. The bake shaders now use
  integer-bounded loops, and `Environment::Bake` splits stages with `bgfx::frame()` when the
  live device is Intel + Vulkan — see [rendering.md](../engine/rendering.md#environment--ibl).
  **Diagnosed and fixed.** The cause was `bgfx::Attachment::init`'s default last parameter,
  `BGFX_RESOLVE_AUTO_GEN_MIPS`: the bake asked bgfx to generate mips for a cubemap whose mips it
  renders by hand, and bgfx's Vulkan mip-gen builds that blit with `baseArrayLayer = face` and
  `layerCount = 6`, which is out of bounds for every face but 0. ANV hangs on it; D3D11 and NVIDIA
  do not. Passing `BGFX_RESOLVE_NONE` is the whole fix - see
  [rendering.md](../engine/rendering.md#environment--ibl).

  **What actually found it was the Khronos validation layer**, which had never been loaded: every
  earlier log shows `Enabled instance layers:` empty while `VK_EXT_debug_report` and
  `VK_EXT_debug_utils` were both enabled, so bgfx had a callback waiting with nothing feeding it.
  Four changes were made before that on inference from timings and hang locations, and all four
  were aimed at the wrong thing. On a machine that reproduces a GPU hang, install
  `vulkan-validationlayers` and run with `VK_LOADER_LAYERS_ENABLE='*validation*'` **first**.

  **Verified on the machine that hung**: editor and runtime both boot and run. The staged bake and
  the inter-stage GPU drain that were added while chasing this are gone again - they were never the
  fix, and a second code path for one vendor is not worth carrying for a theory that turned out to
  be wrong. Native Linux Vulkan is closed.

  *The history of the four wrong turns follows, because the reasoning is the part worth keeping.*

  Four attempts in it was **not diagnosed**. Integer-bounded loops, an 8x sample-count cut, a
  clamp on an out-of-range prefilter LOD and splitting the bake across command buffers all left it
  hanging. With the GPU drained between stages so the timing means something, it dies **before the
  first stage reports** - and that stage is the panorama blit, one `texture2D` fetch per pixel with
  no loop in it. The bake also runs from `EditorLayer::OnAttach`, so the submission that hangs is
  the first real rendering the process ever does.

  Two things are worth knowing before anyone picks this up again. `GANYMED_SKIP_IBL_BAKE=1` creates
  the IBL targets without rendering into them, which answers "is it the bake at all, or is it the
  first frame" in one run. And **the Vulkan validation layer has never been loaded** on that
  machine - `Enabled instance layers:` is empty in every log - while `VK_EXT_debug_report` and
  `VK_EXT_debug_utils` are both enabled, so bgfx already has a callback waiting for it. Installing
  `vulkan-validationlayers` and forcing the layer in through the loader costs no code change and is
  the most likely thing to name the actual fault.

  Workaround meanwhile: `--renderer=opengl`.
- **The GL driver stack.** WSLg's Mesa served GL through its **d3d12 gallium driver**
  (`D3D12 (Intel(R) UHD Graphics)`). Hardware-accelerated, but not what a native user runs.
- **Audio.** miniaudio selected its Null device because WSL exposes none, so ALSA and PulseAudio
  were never opened. It degraded cleanly, which is worth something, but it is not a test.

## A frame profiler (Tracy) is still worth considering

The frame loop **is** instrumented now, and the `Instrumentor` behind it was rewritten to afford it
(per-thread buffers, ~72 ns a scope against ~3–7 us before) — see
[build-and-tooling.md](../engine/build-and-tooling.md#profiling--debug-tooling). What is closed is
"there is no frame breakdown". What stays open is the question T2 kept separate, now with the cheap
option actually taken rather than assumed:

Tracy would add, and the current writer will not grow cheaply:

- **A live view while the app runs**, instead of open-the-app, close-it, load a JSON. This is the
  big one, and it is most of why people adopt it.
- **GPU zones.** The engine's GPU time comes from the bgfx stats overlay (F1) and sits in a
  different window from the CPU trace, so correlating a slow frame to a slow pass is done by eye.
- **Lock contention and allocation tracking**, neither of which this writer can see at all.
- **Statistics across frames** — the current workflow answers "what happened in this capture" by
  loading it into Python, which is how the numbers in the build doc were produced.

Against: it is a new third-party dependency plus premake integration plus a viewer binary to build,
and the chrome://tracing pipeline now does the thing it was failing to do. Nothing is blocked on it.

The honest trigger for revisiting: the first time a frame problem needs GPU and CPU on one timeline,
or the first time the capture-then-load loop is the slow part of an investigation.

## Removing a project leaves its generated files behind

`GanymedEngine/extern/Glad/` still exists in working trees that predate the bgfx migration: a
`Glad.vcxproj`, its `.filters`, and `bin/`/`temp/` Debug folders. The Glad *project* was deleted
correctly — [`BGFX_MIGRATION.md`](../history/BGFX_MIGRATION.md) records it, nothing in
`premake5.lua` includes it, and the loader's headers are gone. What survived is generated output.

**A fresh clone never sees it**, which is why it went unnoticed for so long: `.gitignore` excludes
`**/extern/**/*.vcxproj`, `**/extern/**/bin/` and `*.make`, so none of it was ever tracked.
`git ls-files GanymedEngine/extern/Glad` returns nothing.

So this is not a repository bug and the fix is not a commit. The general shape is that **premake
never deletes project files for a project it stops generating, and git cannot clean what it does
not track** — so every project removed from the workspace leaves that residue on every machine
that built it before the removal. Glad is just the one that has sat there longest.

Checked at the same time, since it is the question the above raises: the twenty `.vcxproj`/`.make`
files at `extern/`'s root all correspond to live projects — `bx`, `bimg` and `bgfx` from
`bgfx.lua`, the other seven from their own `.lua`. Glad is the only orphan.

Options, in ascending order of effort:

- Delete the directory by hand on any machine that has it. No repo change, and it does not come
  back.
- `git clean -xdf` also does it, but it destroys every build output and the `.compiled/` asset
  caches with it — a full rebuild plus a texture recompile to remove one stale folder.
- Teach the generate scripts in `scripts/` to reap `extern/*.vcxproj` whose `.lua` no longer
  exists. Recorded for completeness, not recommended: projects leave this workspace rarely, and a
  script that deletes build files by pattern is a worse failure mode than the folder it cleans.

## Nothing installs the Linux system packages, and `setup_dependencies.sh` is stale

Found while verifying the Sandbox removal: a Linux build in the project's own WSL image now fails in
bgfx with

```
bgfx/3rdparty/khronos/vulkan-local/vulkan.h:52:10: fatal error: xcb/xcb.h: No such file or directory
```

`renderer_vk.h:15` defines `VK_USE_PLATFORM_XCB_KHR` unconditionally on Linux, so bgfx's Vulkan
path needs the xcb headers. Both `/usr/include/xcb/xcb.h` and `/usr/include/X11/Xlib.h` are absent
from that image — only the *runtime* libraries (`libx11-6`, `libx11-xcb1`) are installed, not the
`-dev` packages. GLFW's X11 backend needs `Xlib.h` for the same reason.

**No script in this repository installs them.** `scripts/setup_dependencies.sh` is named as though
it would, but it only runs `git submodule update --init --recursive` and then checks that submodule
files exist. The system-package step does not exist anywhere, so the Linux instructions are
incomplete by exactly the amount that stops a fresh machine from building — which is the same gap
recorded above under WSL-vs-native, seen from the other side.

While reading it, three separate staleness bugs in that one script:

- It checks for `GanymedEngine/extern/Glad/premake5.lua` and reports a **failure** if it is missing.
  Glad was deleted by the bgfx migration. On a correct fresh clone this script now says
  `❌ Glad premake5.lua missing` and prints the "some dependencies are still missing" path.
- It points at `./scripts/setup_premake_Unix.sh` and `./scripts/GenerateProjects_Unix.sh`. Neither
  exists; the real names are `setup_premake.sh` and `Linux_GenerateProjects.sh`.
- It never mentions the system packages above.

Unlike the untracked `extern/Glad/` folder in the entry above, **this one is committed**, so every
clone has it. Worth fixing together: add the apt/dnf package list, drop the Glad check, correct the
two script names.

## Nothing prunes stale objects out of the Linux static archives

The sharper-toothed relative of the two entries above, found while verifying the Sandbox removal.
A clean-looking Linux Debug build failed to link `GanymedEditor` with 22 undefined references to
`glad_gl*`, raised from `Platform/OpenGL/OpenGLTexture.cpp` — **a file the bgfx migration deleted**.
It is not on disk, not tracked, and no premake file mentions glad. What survived is nine object
files still sitting inside `bin/Debug-linux-x86_64/GanymedEngine/libGanymedEngine.a`:

```
OpenGLBuffer.o  OpenGLContext.o  OpenGLFramebuffer.o  OpenGLRendererAPI.o  OpenGLShader.o
OpenGLTexture.o OpenGLVertexArray.o  OpenGLEnvironment.o  OpenGLUniformBuffer.o
```

Cause: `ar -rcs` merges into an existing archive rather than rebuilding it, so a deleted source's
object is never evicted. Release and Dist were unaffected only because those object trees were
created after the migration. Deleting the three `bin`/`temp` Linux trees and rebuilding gives a
clean result in all three configurations, so **there is nothing to fix in the source** — the
question is whether the build should defend against it.

Options, none of them obviously right:

- **Nothing.** Document the trap (done — see
  [build-and-tooling.md](../engine/build-and-tooling.md)) and delete the tree when it bites. It has
  bitten once in the project's life.
- **Have the generate scripts `make clean` when the source list changes.** Correct, and it throws
  away a full dependency rebuild every time a file is added — minutes, for a problem measured in
  years.
- **Switch the archive rule to delete the `.a` first.** One line in the gmake template, but that
  template is premake's, not ours, so it means carrying a customisation in `vendor/premake`.

**What this finding actually costs is the confidence in an earlier claim.** `docs/history/` records
the Linux bring-up as "Debug, Release and Dist all build clean". That run was reading this archive,
so it did not link Debug from scratch, and the Debug result in that record is weaker than it reads.
The history file is immutable and correct as written, so the correction is recorded here instead.

## The first frame's timestep is over a second

Measured while verifying P0.2's rotation lock, from a Lua script logging its own `ts`:

```
f1 ts=1.3764    f2 ts=0.0472    f3 ts=0.0076    f4 ts=0.0039    f5 ts=0.0033
```

Frame 1 covers boot — asset scan, shader loads, the first mesh applies — and that whole interval
is delivered to gameplay as one timestep. Frame 2 is still 10x a normal frame.

Physics survives it: `PhysicsSystem` accumulates and clamps to `MaxStepsPerFrame`, so the
spiral-of-death guard absorbs the spike. **Gameplay does not.** Any script that accumulates time
sees more than a second elapse before it has run once, which silently broke this probe's own
`if t < 0.5 then push() end` gate - the gate was already false on the frame it first ran, so the
push never happened and the test reported a clean pass for the wrong reason. A cooldown, a spawn
timer, or a "wait half a second then do X" in the game will all misfire the same way.

The usual fix is to clamp the delta handed to layers - most engines cap it around 0.1-0.25 s - and
optionally to discard the first frame outright. Neither is done here.

Not scheduled, because the right shape of the fix is a real decision: clamping changes what `ts`
means for everyone, and a game that legitimately hitches wants to know. Recorded because it will be
met again in [PROVING_GROUND.md](PROVING_GROUND.md)'s P1, and the failure looks like a gameplay bug
rather than a frame-timing one.

## The three per-platform Input files are one file three times

`Platform/Windows/WindowsInput.cpp`, `Platform/Linux/LinuxInput.cpp` and
`Platform/macOS/macOSInput.cpp` contain the *same* GLFW code - `glfwGetKey`, `glfwGetMouseButton`,
`glfwGetCursorPos` - each wrapped in a `#ifdef GE_PLATFORM_*` so only one compiles. The split
predates GLFW being the only windowing backend; there has never been a second implementation for
them to differ in.

Cursor mode and the mouse delta were added in `Core/Input.cpp` instead, written once, rather than
pasting stateful logic into three files that would then have to be kept in step. That leaves
`Input` split across two ideas of where it lives, which is worse than either option on its own -
but only until the three are folded into the one.

The fold is mechanical: move the three function bodies into `Core/Input.cpp`, delete the platform
files, regenerate. It is left out of the cursor change because deleting three source files and
adding one is a project-regeneration change that has nothing to do with cursor capture, and mixing
them would make both harder to review.

## `CharacterVirtual::mMaxStrength` is not exposed, and a scripted body cannot be pushed anyway

The other half of this pair - that a character was invisible to every query and raised no
contact - is **done**: every character now carries an inner body, and
[physics.md](../engine/physics.md#presence-the-inner-body) describes it. What is left is the
strength knob and the velocity-write question below, which the inner body does not touch.

Jolt's character *does* push dynamic bodies: `CharacterVirtual::HandleContact` applies an impulse
to any dynamic body it touches, clamped to `mMaxStrength * dt`. `CreateCharacters` never sets
`mMaxStrength`, so every character in the engine runs on Jolt's default of 100 N, and
`CharacterControllerComponent` gives nobody a way to change it.

That is a small gap on its own. It is a larger one in combination with how gameplay drives an NPC:
the game branch's `Enemy.lua` writes `SetLinearVelocity` every frame, which overwrites whatever
impulse the character imparted before the next step ever sees it, so **an impulse cannot move a
script-driven body at all**.

How much that still matters is now an open question rather than a measured one. The inner body
is Kinematic, so it also displaces dynamic bodies through ordinary penetration resolution, which
a velocity write does not undo - a character may already shove a scripted NPC out of a doorway
without any impulse surviving. **Measure before fixing**: the earlier claim here, that such an
NPC is an impassable door, was written when characters had no presence at all and has not been
re-checked since.

Both halves are worth fixing and they are separate: exposing `MaxStrength` is a field, while
"pushes should survive a velocity write" is a question about whether the velocity API should set
or should target - an `AddVelocity`/`SetDesiredVelocity` distinction, which is what controllers in
most engines end up with.

## Nothing can refuse a push on a character, so an NPC can shove the player out of the world

A `CharacterVirtual` resolves an overlap by moving **itself**, with no mass and no resistance. Now
that characters have presence, any dynamic body that drives into one moves it, for as long as it
keeps driving. The Proving Ground's P5 met this immediately: an enemy charging at 4.2 m/s pushed
the player a steady 1 m/s for over a minute and then through the ground plane -

```
GATE t=60s pos=(18.4, 0.95,  8.4)
GATE t=80s pos=(35.9, 0.95, 44.2)
GATE t=85s pos=(36.6, -14.47, 45.1)   <- inside the plane's +-50 extent, so through it, not off it
GATE t=95s pos=(10.3, -678.57, 2.4)
```

Jolt has the control: `CharacterContactSettings::mCanPushCharacter`, delivered per contact through
`CharacterContactListener::OnContactAdded`. **The engine installs no `CharacterContactListener at
all`**, so that setting cannot be reached, and neither can any of the others on that interface
(`mCanReceiveImpulses`, contact velocity overrides for moving platforms).

Installing one is the single change that unlocks all of them, and it is the same shape as the
`ContactListener` that already exists for bodies. The policy question underneath it is worth
deciding once rather than per-game: *what is allowed to move a character?* Unity's answer is
nothing, unless the script asks; Unreal's is mass-weighted. Either is a defensible default, and
having no answer is not.

Worked around in the game for now by having enemies back off after landing a touch, which bounded
the drift during a 14 s hold from 21 m to 0.07 m - a fix in the AI for a gap in the engine.

## There is no `OnCollisionStay`

`OnCollisionEnter` fires once when a contact is made and `OnCollisionExit` once when it breaks.
Nothing reports the frames in between, so **"something is touching me right now" is not a question
a script can ask.** Every trigger volume that acts continuously - standing in fire, standing in a
heal spot, an enemy leaning on you - has to reconstruct it by counting enter/exit pairs.

That counter leaks, and the leak is not hypothetical: when an entity is *destroyed* while touching,
its contact-removed event arrives after the entity is gone and cannot be resolved back to it, so
the exit never lands and the count stays high forever. P5's contact damage is a bounded budget of
ticks per touch rather than a "while touching" flag for exactly this reason - a budget cannot leak.

Jolt reports persisting contacts through `ContactListener::OnContactPersisted`, which
`PhysicsContactListener` does not override. The cost is one more virtual and a third event kind on
the way to scripts; the question worth thinking about first is whether gameplay wants a per-frame
event at all, or a queryable "who am I touching" set, which is what most engines settle on.

## `CharacterControllerComponent` is not in `ComponentList`

Found while adding `BoneAttachmentComponent` to that list. The controller is reflected, has a
hand-written serializer block, and is a real component on entities, but `Scene::Copy` and
`DuplicateEntity` iterate `ComponentList` — so a character in the editor scene loses the controller
on play, and a duplicated character is not a character. The constructor's change-buffer hookup
skips it too; that is harmless today because the type is untracked.

The fix is adding it to the list. There is no runtime field to reset on copy.

## A character cannot be teleported

Nothing moves a character except its own velocity. Its `TransformComponent` is overwritten from the
controller every frame by `SyncTransforms`, so writing it does nothing, and `CharacterVirtual`'s
own `SetPosition` is not exposed through `PhysicsScene` or the script bindings.

So **there is no way to respawn**. P5's player recovers where it fell, which is not a thing anyone
would ship, and the same gap blocks checkpoints, teleporters, level transitions and cutscene
placement. It is also the smallest item in this file: `SetPosition` already exists on the Jolt
object and already keeps the inner body in step (`UpdateInnerBodyTransform` runs inside it) - what
it needs is a `PhysicsScene::SetPosition` that routes to the character or the body interface, the
same way `SetLinearVelocity` already routes to either.

## A script cannot tell whether a contact was with a sensor

`OnCollisionEnter(other)` hands over the other entity and nothing else. A sensor causes no collision
response, but its contact event is indistinguishable from a solid hit, so a projectile that flies
*through* a trigger volume still reports hitting something and despawns in mid-air over it.

P5 worked around it by having the pickups publish their own names into a shared table for the
projectiles to check, which is the kind of thing a script should never have to arrange. The fix is
to carry the flag on the event: `PhysicsCollisionEvent` already exists and the sensor bit is known
at dispatch time (`Body::IsSensor`), so this is a field, a parameter, and a line in the `.d.ts`.

## The HUD data model is two variables, declared in C++

`UIEngine` binds exactly `health` and `score` into the `hud` data model, before any document loads,
because RmlUi binds to real C++ addresses rather than to a bag of names. So **a game cannot add a
third**. The Proving Ground tracks a weapon level, a projectile damage level and how many enemies
are left, and none of them can reach its HUD.

[ui.md](../engine/ui.md) already records the shape of the answer and the condition for doing it:
*"Fixed setters rather than a general UI.Set(name, value)... Worth doing when a second HUD needs
it - not before."* P6 is the second HUD, so the condition is met.

Two ways to do it, and the choice is the whole of the work:

- **A bound map.** RmlUi can bind a container, so one `Rml::Vector`/`Map` of variants reaches every
  name a document asks for. Cheapest, and it gives up compile-time knowledge of what exists: a
  typo'd `{{helth}}` renders empty rather than failing.
- **`BindFunc` per name, registered at document load.** Keeps the addresses real, costs a
  registration step and a place to put it.

Either way `UI.SetHealth`/`UI.SetScore` should stay as they are - a HUD that every game has wants
the short call, and the general path is for the rest.

## A shipped Dist build is not self-contained

`staticruntime "off"` in both `GanymedEngine/premake5.lua` and `GanymedRuntime/premake5.lua`, in
every configuration, so the Dist executable imports `MSVCP140.dll`, `VCRUNTIME140.dll` and
`VCRUNTIME140_1.dll`. A machine without the Visual C++ redistributable cannot start it, and the
failure is a Windows dialog before any of our code runs - so there is no log, and no way for the
person to tell you what happened.

**This is invisible on any development machine**, because installing Visual Studio installs the
redistributable. The Proving Ground's P7 only found it by reading the import table.

The obvious fix - `staticruntime "on"` under the Dist filter - **is not available**: every static
library the executable links must agree on the CRT, and the third-party projects are built from
premake files inside `GanymedEngine/extern/`, which is not ours to edit. Switching only the engine
and the runtime produces a link error, not a smaller problem.

So the choice is between:

- **Ship the three DLLs beside the executable.** What P7 did by hand, and what a packaging step
  should do. Microsoft permits redistributing them, and it keeps the install self-contained.
- **Require the redistributable** and say so in an installer. Normal for a large game, absurd for
  a demo.

Either way it belongs in a packaging step rather than in a person's memory.

## There is no packaging step

P7 assembled a shipped install by hand, and the list is not obvious enough to keep re-deriving:

```
ship/
  GanymedRuntime.exe                 from bin/Dist-windows-x86_64/GanymedRuntime/
  msvcp140.dll vcruntime140.dll vcruntime140_1.dll
  assets/                            the whole project root, .meta and .compiled included
    runtime.yaml                     with AssetRoot: assets
    fonts/                           ENGINE-owned, from GanymedRuntime/assets/fonts
    shaders/compiled/                ENGINE-owned, from GanymedRuntime/assets/shaders
```

The two engine-owned directories are the part that surprises. `UIEngine` loads its faces from
`assets/fonts/...` and `Shader::Create` loads from `assets/shaders/compiled/<profile>/...`, both
**relative to the working directory** rather than to the project root - by design, so the editor's
own chrome keeps working when it opens someone else's project. In a shipped layout the working
directory *is* the install and the project root is `assets/`, so engine chrome and game content
end up in the same tree. It works - the asset scan ignores `.ttf` and `.bin`, so nothing is minted
or quarantined - but a game's asset tree containing the engine's fonts is a surprise, and it means
`AssetRoot` cannot be renamed to anything other than `assets` without splitting them.

Two things would make this repeatable, and they are separable:

- **A packaging script** under `scripts/`, taking a configuration and an output directory. Cheap,
  and it can fail loudly on the mistakes P7 actually made: a missing `.meta`, an absent
  `.compiled` tree, a `runtime.yaml` still pointing at a development path.
- **Resolving engine chrome against the executable** rather than the working directory, which
  would let a shipped game's `assets/` hold only the game. Bigger, and it touches every
  `Shader::Create` call site.

## Skeletal leftovers after the attachment and tooling close

[SKELETAL_ATTACHMENTS.md](../history/SKELETAL_ATTACHMENTS.md) and
[SKELETAL_TOOLING.md](../history/SKELETAL_TOOLING.md) are in history. Two items they named were
deliberately not built, so they live here rather than vanishing with the plans. A third was found
later.

**A socket warns "no rigged mesh" while the mesh is only loading.** Opening the unmodified
`ProvingGround.ganymede` logs, once on the first frame: "BoneAttachment on 'Rifle' targets
'Body', which has no rigged mesh". `Body`'s mesh is not `Ready()` yet, and
`BoneAttachmentSystem` uses one branch for "no `StaticMeshComponent`", "not loaded" and "not
rigged". The socket recovers on the next frame, and the warning is cleared once the socket
resolves. It is wrong rather than harmful: it names a real failure that is not happening, on
every load of every socketed scene. The fix is to split out `!Mesh.Ready()` and stay quiet (or
say "still loading") while the handle is pending. Found in H2 of
[TWO_HAND_IK.md](TWO_HAND_IK.md).

**A general `Visible` / `Enabled` bit `RenderSystem` honours.** Decided in the attachments A2
follow-up: hide an unresolved socket during the frames a skinned mesh is still streaming. Not a
socket-local flag — that would be a second visibility system the day anything else needs to hide.
Unity's renderer enabled / Unreal's hidden-in-game. Useful for cutscenes, inventory, pooling;
sockets are one client. Do not add a socket-only suppression in the meantime. The pop is brief
now that scale is no longer compensated on fail-to-resolve. The outliner eye is the wrong hook:
`EditorViewFilter` is cleared on Play, so reusing it would hide the rifle in the editor and show
it in the game.

**The A3 visual gate was not watched from master.** Wiring is recorded (Rifle child of Body,
`Joint: RightHand`, hand-tuned Offset/Rotation, Scale 0.45). The picture — rifle stays in the
hand through idle / walk / run and the 180° backpedal turn, with Skeletons on so the wrist is
visible — needs `ProvingGround.ganymede` on `first-game`. The other half of that gate is content:
`Muzzle` is still parented to `Yaw`; moving it onto the barrel was not done. The engine half now
exists — `Entity:GetWorldPosition` / `GetWorldForward` read a socketed entity's drawn world (see
[scripting.md](../engine/scripting.md#world-transform)) — so what is left is content on
`first-game`: a `Muzzle` under the Rifle, and `Player:Fire` spawning from it.

## Aim-offset leftovers

[AIM_OFFSET.md](../history/AIM_OFFSET.md) is in history. A1, A2 and A4 are on `master`; A3 is on
`first-game` (`e7085d4`). These were named in that close and are still open.

**Nothing in the milestone was watched on the character.** A2's preview drag, save/reload and
Ctrl+Z were not done in the window. A4's four rows — drag up, drag past the limit, Play, the
muzzle line — were not done either; the Debug editor was built. A3's frozen-clip elevation,
strafe, backpedal, gate re-run and barrel-versus-chest count were not done. The scene is
`ProvingGround.ganymede` on `first-game`.

**The spine weights are the A1 defaults.** `Spine02` / `Spine01` / `Spine` at 0.10 / 0.20 / 0.30,
omitted from the scene so the defaults apply. They were not tuned against the overlay. An even
share at the waist is the thing those numbers exist to avoid; whether a sixth / a third / a half
looks right is still a look, not a measurement.

**The lowered idle is still the wrong pose for standing still.** `Lower_Weapon_Look_Raise` points
the barrel as far as −78°. Spine pitch does not turn that into an aim, and the milestone did not
download a clip that would. `BarrelPoint`'s ~35° check is what keeps those shots on the chest
fallback until that clip exists.

**The viewport line does not search upwards.** It walks `RelationshipComponent` children of the
selected entity for the tag `Muzzle`. The skeletal leftover above still has `Muzzle` parented
to `Yaw`, so selecting `Body` draws no line until `Muzzle` moves under the rifle. That move is
the same content change, not a new one. The line is also one frame behind the pose, because the
drag writes `Pitch` / `Yaw` after `AnimationSystem` has evaluated. Closing it so the barrel meets
the point is closed-loop aiming, which the milestone left alone; it is H4 of
[TWO_HAND_IK.md](TWO_HAND_IK.md), along with the left hand on the rifle.

**Two of the aim probes' rotation checks are quantised.** The twisted-pitch probe and the two-joint
yaw+pitch probe in `RunAimOffsetProbes` measure rotation error as `2·acos(|dot|)` against a 1e-4 rad
tolerance. `RotationDelta` does not have this problem: `glm::angle` switches to an asin form near
zero. In float, the first `|dot|` below 1.0 is already about
7e-4 rad, so the check passes only while the dot rounds to exactly 1. It fails spuriously the moment
it does not, and it cannot see an error between 0 and 7e-4 rad. That is too strict and blind at the
same time. The two-bone probes (H1 of [TWO_HAND_IK.md](TWO_HAND_IK.md)) use
`2·atan2(|v|, |w|)` of the delta quaternion (`RotationError` in `AnimationSystem.cpp`). The fix is
to switch those two checks to `RotationError`.
