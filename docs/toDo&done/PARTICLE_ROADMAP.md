# GanymedEngine — Particle System Roadmap

Status: **Phase 1 executed.** Written 2026-09-03, against the
post-content-authoring engine (branch point: the undo/prefab/`.gmat` milestone, complete; HEAD
`2e74671`). Follows the format of [`CONTENT_AUTHORING_ROADMAP.md`](CONTENT_AUTHORING_ROADMAP.md):
each phase carries goal, steps, decisions with rationale, risks, and a verification table;
execution notes get appended as phases run. Read the decision notes even if you skip the code
sketches.

This is the plan of record for the eighth milestone: a **particle system** — emitters authored as
components, simulated CPU-side, rendered as camera-facing billboards (alpha and additive) and as
instanced mesh debris, with keyframed over-lifetime curves edited in a custom ImGui curve editor.
After this milestone an author can drop an emitter on an entity, shape a smoke plume or spark
burst with curves and a gradient, scrub it live in the editor with full undo, save it
byte-deterministically, reuse it through prefabs, and trigger bursts from Lua at runtime.

## Decisions of record

Settled up front, before any code:

1. **The plan lives here**, indexed from `docs/README.md`. No new doc files: the particle pass
   lands in `docs/engine/rendering.md`, the component + system chain in `docs/engine/scene.md`,
   the curve editor + inspector in `docs/editor/editor.md`, bindings in
   `docs/engine/scripting.md` + `GanymedEditor/scripts-src/types/ganymed.d.ts`.
2. **Component-only authoring.** Every parameter lives on `ParticleEmitterComponent`, serialized
   in the scene; there is **no `.gparticle` asset type**. Production norms split: Unity's particle
   system is itself a component (Shuriken), Unreal's Niagara and Godot's
   `ParticleProcessMaterial` are assets/resources. Ganymed takes the Unity shape because the
   previous milestone already built the reuse vehicle — a tuned emitter entity becomes a
   `.gprefab` — and an asset type would buy nothing but a second serializer, a registry surface,
   and an inspector editing a shared object with the scoping problems the content-authoring
   milestone's Decision 2 just spent a phase avoiding. The upgrade path stays open: the
   component's YAML block is already the asset schema if one is ever wanted.
3. **Over-lifetime parameters are keyframed curves, not begin/end lerps** — `FloatCurve` for
   size, `ColorGradient` for color — with an in-house ImGui curve editor as the milestone's
   largest editor-UI item. Unity's `MinMaxCurve`/`Gradient` is the norm being followed; the
   divergence is **linear keys only, no tangents/Bezier** — the engine-wide interpolation
   precedent (`AnimationClip::Channel` is Linear/Step,
   `GanymedEngine/source/GanymedE/Renderer/Animation.h:58-79`; no Bezier exists anywhere in the
   tree), and a tangent-handle editor roughly doubles the widget for a v1 whose effects (smoke,
   sparks, debris) linear keys serve fine.
4. **Rendering v1**: unlit textured camera-facing billboards, alpha + additive blend per emitter,
   per-particle color/size/rotation, submitted into view 5's sequential transparent stream —
   **plus mesh particles** that ride the existing lit Phong opaque instanced path unchanged.
   Explicitly not v1: flipbook atlases, soft particles, lit billboards, GPU simulation
   ("Not doing").
5. **No collision.** Gravity + initial velocity only. Depth-buffer or physics-scene collision is
   a feature with its own correctness surface; nothing in the v1 content set needs it.
6. **Simulation is deterministic from a serialized per-emitter seed.** This is not a gameplay
   feature — it is the milestone's verification instrument. The house culture proves things by
   byte-identity and exact counts; a seeded emitter ticked N frames at a fixed `Timestep` has an
   exactly assertable state, which makes Phase 2 verifiable *before* a single pixel exists.
7. **Phase order: foundations → simulation → rendering → curve editor → integration.** The curve
   editor is the biggest single item but goes fourth, not first — an inversion of the
   fail-fast-on-riskiest-unknown precedent, argued: the editor's genuinely risky part (undo
   commit-boundary participation) was de-risked at planning time by verifying `InvisibleButton`
   holds `ActiveId` for a full drag in the vendored ImGui 1.91.9b source, and a curve editor with
   no live particles behind it cannot be *authored against*, only unit-tested. Meanwhile
   simulation before rendering keeps Phase 2's verification purely numeric (Decision 6's
   instrument), and Phase 2 ships the curves in the component with a numeric-only inspector, so
   curve sampling and serialization are exercised for two phases before the widget that edits
   them exists.

## Where the engine is today (facts this plan is built on)

Verified against the tree at planning time (file:line refs current at `2e74671`); re-verify
anything load-bearing before executing a later phase.

- **Renderer2D is the wrong host for billboards.** It owns a 20k-quad dynamic VB with a
  full-range `SetData` per flush, submits to view 6 (`SceneTransparent`) whose view transform is
  `(identity, viewProj)` — `u_view` is degenerate there — and has no blend-state control of its
  own. Its corner-construction CPU billboarding pattern is worth copying; the renderer itself is
  not.
- **The right seam exists in view 5.** bgfx executes views in ID order; view 5 (`SceneHDR`,
  Sequential mode, real `setViewTransform`) runs 3D opaque *and* 3D transparent into the HDR MRT
  `{RGBA16F, RED_INTEGER entityID, Depth}`. The transparent loop sets Alpha blend + depth-test
  LESS + depth-write OFF at `Renderer3D.cpp:863-864` and restores state at `:897-902`.
  Submitting particle draws between the transparent loop and the restore inherits the correct
  view transform, free depth-test against opaques, and
  CPU-sort-order-equals-execution-order (Sequential mode). View 23 is the only free view below
  tonemap if a separate pass were preferred; 26 is declared-dead, 27 is picking, 29-31 sit
  post-tonemap.
- **Every fragment shader targeting views 5/6 must write `gl_FragData[1]`** (entity ID) — the
  MRT obligation, verified across all six existing scene shaders. Transparent meshes already
  write their ID across whole triangles regardless of alpha.
- **Blend state is one global sticky singleton** (`RenderState`, `RenderCommand.cpp:11`),
  applied per submit; mutations persist across frames and renderers. Every pass that changes it
  restores it manually — the bloom Additive→Alpha restore at `SceneRenderer.cpp:169/195` is the
  precedent. `RenderState::BlendMode` has Alpha and Additive only; no premultiplied mode.
  `RenderState.h` packs `BGFX_STATE_*` — it is a renderer header and must not leak into
  `Components.h` (design principle 6, "engine types firewalled").
- **The per-frame dynamic geometry template is the ImGui backend**
  (`ImGuiRendererBgfx.cpp:142-208`): transient VB+IB with `getAvailTransient*` guard, memcpy,
  setState, submit. `RenderCommand` and `Geometry` know nothing of transient buffers; a particle
  renderer calls `bgfx::` directly plus a manual `FrameUniforms::Apply()` (which re-uploads
  `u_Lights` — 2KB per submit; fine at a-few-draws scale, noted).
- **Camera basis for CPU billboarding**: the `RenderContext` singleton carries
  `{const Camera*, mat4 CameraTransform, EditorCamera*}`; right/up are `CameraTransform`
  columns. `Renderer3D`'s own camera state is file-static with no accessor — passing the basis
  from `RenderSystem` is less invasive than adding one.
- **Mesh-particle batching is confirmed on the opaque path only.** Opaque sort is Material ptr →
  Mesh ptr → SubmeshIndex → SortKey; N same-mesh same-material commands are contiguous and
  become **one** `DrawInstancedGroup` (chunked at 1024). The transparent path merges only
  distance-sort-adjacent runs — transparent mesh particles are one draw *each*. Costs at scale:
  one `DrawCommand` push per particle (two `Ref` refcount bumps), per-particle frustum cull, and
  shadow casters pushed unculled ×4 cascades.
- **Per-instance data**: `MeshInstanceData` is `{mat4, vec4 EntityID}` = 80B (static_assert is
  `%16`, not `==80`); 5 of 16 `i_data` slots used. A wider particle-only instance struct with
  tint is possible but needs its own vertex shader — deferred (tension 3).
- **Shader toolchain**: `vs_Particle.sc`/`fs_Particle.sc` dropped into `assets/shaders/src/` are
  picked up by `scripts/compile_shaders.{bat,sh}` (3 profiles × 3 app trees). `varying.def.sc`
  already has `a_color0`/`v_color0`/`a_texcoord0`; entity ID as a float attrib follows the
  Renderer2D precedent (`Buffer.h` maps Int→Float). A per-shader `varying.Particle.def.sc`
  escape hatch exists. Texture slot 0 is free per-draw for an unlit shader.
- **The only keyframe precedent** is `AnimationClip::Channel` (parallel Times/Values arrays,
  glTF-shaped) with the `FindKeys` sampler (`AnimationSystem.cpp:21-40`): `upper_bound` binary
  search → `{Lower, Upper, Alpha}`. No curve or easing utility exists in `Math/` (only
  `DecomposeTransform` + `BoundingVolumes`).
- **No engine RNG exists.** `UUID` owns a file-static `mt19937_64`, not exported.
- **Component runtime-state precedent fork**: `AnimatorComponent::Palette` is component-owned
  runtime state — `Scene::Copy` clears it (`Scene.cpp:219-225`), the serializer skips it — vs
  the `AudioSystem` voice map, system-owned because it holds live foreign resources. A particle
  pool is pure data (Palette-shaped) but persists across frames (not Palette-shaped); argued in
  Phase 2.
- **System chain is 8** (`Scene.cpp:52-70`): Physics → NativeScript → LuaScript → Animation →
  Transform → Camera → Audio → Render. `RenderSystem` declaring `RO<ParticleEmitterComponent>`
  makes `ValidateOrdering` enforce particle-before-render — the documented palette lesson at
  `RenderSystem.h:24-27`.
- **Editor preview postures**, both documented as deliberate: `AnimationSystem`
  samples-without-advancing; `AudioSystem` is silent in edit. Particles can do neither
  trivially — a stateful spawn/age simulation has no closed form to sample. Argued in Phase 2.
- **Undo commit-boundary protocol**: `DrawComponent<T>` copies `T` before each frame
  (`SceneHierarchyPanel.cpp:653`), tracks `ActiveId` before/after the section, with
  pending-edit / instant-edit branches; the `bool(T&)` lambda contract returns true only on real
  widget edits. A custom canvas widget participates correctly **iff** it submits real ImGui
  items — pure `ImDrawList` + manual hit-testing pushes one command per frame, the documented
  failure. The per-frame-copy comment ("components are handle/POD/small-string sized") becomes
  false for a component holding keyframe vectors; amend it.
- **`ImCurveEdit`/`ImGradient` exist in `extern/ImGuizmo/src` but are not compiled** (only
  `ImGuizmo.cpp` is in the premake file list). Adding them is two premake lines — declined
  (tension 6).
- **`EntitySnapshot`/undo machinery is copy-assignment over `ComponentList`** — vector members
  already round-trip (Children, MaterialOverrides, Palette, Fields). A new component with
  keyframe vectors needs zero new undo machinery, only `ComponentList` registration.
- **Serialization of vector fields**: YAML Flow sequences guarded by `IsSequence`, emitted only
  when non-default (`MaterialOverrides` precedent, `SceneSerializer.cpp:115-142/625-632`) —
  protects the canonical saves. The shared YAML dialect lives in `Scene/SceneYaml.h`.
- **Lua**: the two-route split is documented at `ScriptBindings.cpp:241-253` (live-state →
  system-routed; authored fields → component-direct, untracked). The play-idempotency trap is
  documented at the `PlayAnimation` binding (:158-178). `ganymed.d.ts` is a hand-mirror.
- **Editor GUI cannot be driven programmatically**; verification = scripted probes + logged
  evidence + screenshots; interactive checks named as such. Byte-identity saves are the house
  instrument.

---

## Phase 1 — Curve + RNG foundations

**Goal:** `FloatCurve` and `ColorGradient` exist as engine math types with a shared, tested
sampler; a seedable engine RNG exists; both serialize to YAML deterministically. Small,
dependency-free, and everything later builds on it.

### 1.1 `Math/Curve.h/.cpp` — `FloatCurve` + `ColorGradient`

New files `GanymedEngine/source/GanymedE/Math/Curve.h/.cpp` (premake regen after adding files).

```cpp
struct FloatKey  { float Time; float Value; };
struct ColorKey  { float Time; glm::vec4 Value; };

class FloatCurve
{
public:
	FloatCurve();                       // one key {0, 1} — identity multiplier
	float Sample(float t) const;        // clamped at ends; linear between keys
	// Sorted-invariant editing API: AddKey (insert-sorted), RemoveKey,
	// SetKey (re-sorts on Time change). The keys vector is never exposed mutably.
	const std::vector<FloatKey>& Keys() const;
	bool IsDefault() const;             // exactly the constructed state — serializer guard
private:
	std::vector<FloatKey> m_Keys;       // invariant: sorted by Time, size >= 1
};
// ColorGradient: same shape over ColorKey; default one key {0, white}.
```

**Decision: array-of-structs keyframes, `FindKeys`-shaped sampler.** The sampler is the
`AnimationSystem::FindKeys` algorithm (`upper_bound` → lerp) reused in shape; the *storage*
diverges from `AnimationClip::Channel`'s parallel Times/Values arrays, which are glTF-shaped — an
authoring type with no import constraint reads and edits more naturally as `vector<Key>`.
Unity's `AnimationCurve` is the norm (keys with tangents); Ganymed's divergence is linear-only
(Decision 3). Not unified with `Channel` — the animation sampler serves vec3/quat channels with
Step mode and a glTF pipeline behind it; forcing one template over both would couple an import
format to an authoring type for ~30 shared lines. One comment in `Curve.h` names the sibling.

**Decision: never empty, default is identity.** Default-constructed curves carry one key
(`{0,1}` / `{0,white}`), so sampling never branches on emptiness and `RemoveKey` refuses to
delete the last key. `IsDefault()` gates serialization — a component whose curves were never
touched emits nothing, and existing canonical saves stay byte-identical when the component ships
in Phase 2.

### 1.2 `Core/Random.h` — the engine RNG

New header-only `GanymedEngine/source/GanymedE/Core/Random.h`, beside the `UUID` precedent (Core
is where the engine's one existing RNG lives; Math/ holds geometry, not entropy):

```cpp
class Random
{
public:
	explicit Random(uint32_t seed);     // PCG32 (splitmix-seeded); state is two uint64s
	float    Float01();                 // [0,1)
	float    Range(float min, float max);
	uint32_t UInt();
private:
	uint64_t m_State, m_Inc;
};
```

**Decision: small hand-rolled PCG32, not `std::mt19937`.** The requirement is reproducibility of
the *sequence* given a seed, across runs and (best-effort) platforms, with copyable state — a
particle pool copied by `Scene::Copy` must not alias the source's stream. `mt19937` works but is
2.5KB of state per emitter for no benefit; PCG32 is 16 bytes, seedable, and its float derivation
is ours to pin (divide by 2^32 — no `std::` distribution objects, whose sequences the standard
does not pin across implementations). Direction-in-cone and other domain math stays in
`ParticleSystem`, not in `Random` — the RNG hands out numbers, the system gives them meaning.

### 1.3 YAML dialect

`Scene/SceneYaml.h` gains `YAML::convert<>` + emitter support for `FloatCurve` and
`ColorGradient`: a Flow sequence of `[t, v]` / `[t, r, g, b, a]` pairs, read guarded by
`IsSequence` per element (the `MaterialOverrides` hardening posture — malformed input warns and
yields the default, never throws out of a scene load). Keys emit in stored (sorted) order →
write→read→write is byte-identical by construction.

### Phase 1 risks

- The sorted invariant is the one thing every consumer trusts; the editing API must be the only
  mutation path (keys vector returned const). A widget reaching in and reordering raw storage is
  the bug class to close at the type boundary.
- Cross-platform float determinism is best-effort, not promised — the deterministic-sim checks
  run on one platform per run; state that in the probe, don't claim more.

### Phase 1 verification

Scripted probe block (temporary, removed after), x64 Debug.

| Check | Evidence |
|---|---|
| Sampler correctness | Probe: curve `{0:0, 0.5:2, 1:1}` sampled at −1, 0, 0.25, 0.5, 0.75, 1, 2 → exact expected values (clamp ends, lerp middle); single-key curve constant everywhere |
| Sorted-invariant editing | AddKey out of order → Keys() sorted; SetKey moving a key past a neighbor → re-sorted, sample continuous; RemoveKey on the last key → refused + warning |
| RNG determinism | Two `Random(42)` instances → first 1000 outputs identical; `Random(42)` vs `Random(43)` diverge within 4 draws; a copied `Random` continues the identical sequence from the copy point |
| YAML round-trip | Curve → emit → parse → emit → byte-identical strings; malformed node (scalar where sequence expected) → warning + default curve, no throw |
| Canonical-save immunity | All committed scenes: load → save → byte-identical to pre-phase canonical saves (nothing serializes yet; this pins the baseline the Phase 2 guard must preserve) |

### Phase 1 execution notes

Executed 2026-09-04. x64 Debug, MSBuild; engine and editor build clean after `premake5 vs2022`
(new `Curve.cpp`). Verification ran from a temporary `RunParticlePhase1Probe` (engine TU, called
from `EditorLayer::OnAttach`, `Application::Close()` at the end); probe removed.
**37/37 pass.** Sampler, sorted-invariant editing, RNG, YAML round-trip, and canonical-save
immunity as amended below. Float determinism is this platform, this run — not a cross-compile
promise.

**YAML decode is stronger than the glm conversions on purpose.** `convert<FloatCurve>` /
`convert<ColorGradient>` never return false: a scalar where a sequence was expected warns and
yields the identity default, so `as<T>()` cannot throw a bad particle block out of a scene load.
glm `vec3`/`vec4` still return false and throw through `as<T>()`; `Deserialize`'s try/catch stays
the backstop for those. Per-element `IsSequence` skips a malformed key and keeps the rest. Measured:
`YAML::Load("not-a-sequence").as<FloatCurve>()` and `YAML::Load("42").as<ColorGradient>()` both
return the default with one `GE_CORE_WARN` and no exception.

**`ReplaceKeys` is the deserialize path, not an authoring API.** The editing surface is
`AddKey`/`RemoveKey`/`SetKey`; `Keys()` is const. `RemoveKey` on the last key warns and refuses
(logged: `FloatCurve::RemoveKey: refusing to delete the last key`). `SetKey` moving a middle key
past its neighbor re-sorts; samples after the move are the new polyline (`t=0.5 → 0.5`,
`t=1.25 → 1.5` on `{0:0, 1:1, 1.5:2}`).

**The committed scenes are not a serializer fixed point, and this phase did not make them one.**
`SceneSerializer::Serialize` writes `Scene: Untitled` unconditionally, so `Phase5Test` and
`Demo.ganymede` (`Runtime Demo`) already diverge on load→save. `BoxesPhysicsExample.ganymede` is
not UUID-sorted (box3, UUID `1842…`, is first in the file; canonical order would lead with floor).
`Example.ganymede` / `3DExample.ganymede` still remap the thrice-repeated UUID `12837192831273`.
`SceneSerializer.cpp` was not touched. The check that actually pins Phase 2's omit-guard: load →
save → load → save is byte-identical for all five committed scenes, and none of those saves contain
`SizeCurve` / `ColorOverLifetime` / `FloatCurve`.

**Out of scope, flagged:** `3DExample.ganymede` stores `Scene: UntitledAdd commentMore actions` — a
GitHub UI artifact in the file, not an engine bug. Do not "fix" by re-saving; that is a content
commit.

**`Random` is 16 bytes** (`static_assert`). Two `Random(42)` streams matched for 1000 `UInt` +
1000 `Float01` draws; seed 43 diverged within 4; a copy continues from the copy point without
aliasing the source. PCG32 + splitmix64 seeding, float via `2^-32`, no `std::` distributions.

**Docs:** `core.md` (`Random`), `architecture.md` (Core + Math lines), `scene.md` (YAML dialect +
curve types), this file, `docs/README.md`. No new doc files.


---

## Phase 2 — `ParticleEmitterComponent` + `ParticleSystem` simulation

**Goal:** emitters exist as components, simulate deterministically from their seed, serialize
with byte-identity, copy correctly into play mode, are covered by undo with zero new machinery,
and are editable through a numeric-only inspector section — all before any particle is drawn.

### 2.1 The component, trimmed

`Scene/Components.h`. Every field is serializer + inspector + undo + d.ts surface, so the sketch
was cut down; each trim is named because someone will want it back:

```cpp
// Component-local, firewalled: RenderState.h packs BGFX_STATE_* and must not
// reach Components.h (the AudioTypes.h split is the precedent). The renderer
// maps this to RenderState::BlendMode at draw time.
enum class ParticleBlend : uint8_t { Alpha = 0, Additive = 1 };

struct ParticleEmitterComponent
{
	// Emission
	float    RateOverTime = 10.0f;      // particles/sec
	uint32_t MaxParticles = 1000;       // hard cap; spawn stalls at cap
	bool     Looping = true;
	float    Duration = 5.0f;           // emission window when !Looping
	bool     PlayOnStart = true;        // runtime + play mode

	// Initial state (cone axis = entity local +Y; rotate the entity to aim)
	float    LifetimeMin = 1.0f,  LifetimeMax = 1.0f;
	float    SpeedMin = 1.0f,     SpeedMax = 1.0f;
	float    ConeAngle = 25.0f;         // degrees, 0 = beam, 180 = sphere
	float    StartSizeMin = 0.1f, StartSizeMax = 0.1f;
	float    StartRotationMin = 0.0f, StartRotationMax = 0.0f;   // degrees
	float    RotationSpeedMin = 0.0f, RotationSpeedMax = 0.0f;   // deg/sec, constant per particle
	float    GravityModifier = 0.0f;    // × 9.81 world-down
	bool     WorldSpace = false;        // simulation space; false = local (Unity default)
	uint32_t Seed = 0;                  // 0 = derive from entity UUID at Play

	// Over lifetime (x-axis = age/lifetime, 0..1)
	FloatCurve    SizeCurve;            // multiplier on StartSize
	ColorGradient ColorOverLifetime;    // THE particle color (no separate StartColor)

	// Rendering
	enum class Mode : uint8_t { Billboard = 0, Mesh = 1 };
	Mode          RenderMode = Mode::Billboard;
	AssetHandle   Texture  = InvalidAssetHandle;  // Billboard; unset = white
	ParticleBlend Blend    = ParticleBlend::Alpha;
	AssetHandle   Mesh     = InvalidAssetHandle;  // Mesh mode
	AssetHandle   Material = InvalidAssetHandle;  // Mesh mode, single .gmat; unset = mesh default

	// Runtime-only (serializer skips; Scene::Copy resets — the Palette posture)
	bool Playing = false; float Time = 0.0f; float EmitAccumulator = 0.0f;
	Random Rng{0};
	std::vector<Particle> Pool;         // Particle: pos, vel, rotation, rotSpeed, age, lifetime, startSize
};
```

Trims, with rationale: **no `Direction` vector** — the entity transform aims the cone (the Unity
shape-module norm: you rotate the system, you don't type a vector; a redundant authored direction
is two sources of truth for one arrow). **No `StartColor`** — the gradient *is* the color; a tint
times a gradient is two knobs for one channel. **No rotation-over-lifetime curve** — constant
per-particle rotation speed covers tumbling debris and swirling smoke; a third curve widget
instance is pure surface. **No emission-rate curve and no authored burst list** — `RateOverTime`
plus a Lua `EmitBurst(n)` (Phase 5) covers the demo and most effects; Unity's Bursts array is the
norm, deferred until an effect actually needs authored bursts. **No per-particle stored color** —
color is `ColorOverLifetime.Sample(age/lifetime)` computed at render, zero pool bytes.

**Decision: `Seed = 0` means "derive from entity UUID at Play".** Two prefab instances of one
emitter must not visibly march in lockstep, which a fixed authored seed causes; but the
deterministic-replay instrument needs a pinned seed. Default 0 = per-instance variation for free;
the probe authors an explicit seed. Both behaviors are one field.

### 2.2 The pool lives on the component

**Decision: component-owned pool + `Scene::Copy` reset + serializer skip — the Palette posture,
extended honestly.** The pool is pure data (no foreign resource, unlike the audio voice map), so
system-owned storage would buy only a UUID-keyed map with its own lifetime bugs (entity
destroyed, map entry leaks — the exact class of problem declared-access components don't have).
The honest difference from Palette: Palette rebuilds every frame, a pool persists across frames —
so `Scene::Copy`'s reset means **play mode starts with empty pools and emitters warm up on
play**. Accepted: a looping smoke column takes `LifetimeMax` seconds to reach steady state after
pressing Play, exactly like Unity without prewarm; a `Prewarm` flag is deferred ("Not doing").
The component header documents the reset the same way `AnimatorComponent` does. Unreal/Unity
keep particle state in renderer-owned scene proxies — Ganymed diverges because component-owned
state is what its `Scene::Copy` + declared-access architecture already knows how to snapshot,
reset, and order-enforce.

### 2.3 `ParticleSystem`

New `Scene/Systems/ParticleSystem.h/.cpp`. `IterView` `RW<ParticleEmitterComponent>` +
`RO<WorldTransformComponent>`; no reactive views, so no editor drain obligation. Registered
**between Audio and Render** (`Scene.cpp:52-70`, chain becomes 9) — it needs only WorldTransform
(post-TransformSystem) and must precede RenderSystem; Render-last is preserved. `RenderSystem`
gains `RO<ParticleEmitterComponent>` **already in Phase 2** — at that point `ValidateOrdering`
*enforces* the ordering rather than leaving it conventional (the palette lesson,
`RenderSystem.h:24-27`); adding the declaration early costs nothing and locks the slot.

Per-emitter tick, in fixed order (order is part of the determinism contract; comment it):

1. **State machine**: `PlayOnStart` fires once on first tick (idempotent flag); `Time += dt`;
   non-looping emitters stop *emitting* at `Duration` but keep simulating until the pool drains.
2. **Age + retire**: `age += dt`; retire `age >= lifetime` via **stable compaction**
   (`std::remove_if`), not swap-erase — swap-erase reorders the pool by retirement pattern,
   which destroys the exact-replay instrument for one line's saving. At v1 scale (≤ a few
   thousand particles) the copy cost is noise.
3. **Integrate**: `vel += gravity * GravityModifier * dt; pos += vel * dt;
   rotation += rotSpeed * dt`. In local-space mode gravity is world-down rotated into emitter
   space per frame (one inverse rotation per emitter — without it a tilted local-space
   fountain's gravity tilts too, which is wrong and visibly so).
4. **Spawn**: `EmitAccumulator += RateOverTime * dt`; spawn `floor`, keep the fraction; per
   particle draw lifetime/speed/cone direction/size/rotation from `Rng` **in a fixed documented
   order** (reordering draws silently changes every effect — the trap to comment). Spawn
   position/direction from WorldTransform when `WorldSpace`, identity when local. Cap at
   `MaxParticles`.
5. **Bounds**: accumulate a conservative world-space AABB over live particles (+ max size
   margin) into a runtime field — Phase 3's frustum-cull input, computed here where positions
   are hot.

**Decision: editor preview ticks all emitters, always — `OnUpdateEditor` runs the same tick.**
Against both precedents, deliberately, and the header comment must say so against each:
`AnimationSystem` samples-without-advancing — impossible here, a spawn/age sim has no closed
form to sample at time T. `AudioSystem` is silent in edit — defensible for sound, wrong for the
thing being *visually authored*: a curve editor over an invisible effect authors blind, and the
entire point of the milestone's editor investment is tune-while-watching. Unity ticks particles
in the scene view (selected systems, with a playback widget), Godot's emitting particles run
in-editor; tick-all is the cheap end of that norm. A per-emitter inspector Stop and the
`Playing` flag give authors quiet when they want it; a selected-only refinement is deferred
("Not doing").

### 2.4 Checklist integration

The 8-step new-component checklist: `Components.h` → `ComponentList` (buys `Scene::Copy`,
duplication, prefab copying, and undo snapshots in one line) → serializer both sides (every
authored field guarded to emit only when non-default — canonical saves of existing scenes must
stay byte-identical) → `DrawAddComponentEntry` line → `DrawComponent` section with the
`bool(T&)` contract (Phase 2 ships numeric fields, enum combos, and asset-slot drops only;
curve fields render as a read-only key count until Phase 4) → runtime-state fixups
(`Scene::Copy` reset at the Palette site `Scene.cpp:219-225` — pool, accumulator, timer, **and
RNG together**; serializer skips runtime fields) → system registration + `ValidateOrdering`
silent → Lua/d.ts deferred to Phase 5.

**Amend the `DrawComponent` per-frame-copy comment** (`SceneHierarchyPanel.cpp:653`):
"handle/POD/small-string sized" is now false — an open ParticleEmitter section heap-copies two
keyframe vectors per frame. Measured cost is two small allocs per frame while that one section
is open; acceptable, but the comment must stop claiming otherwise. `ComponentEditCommand<T>`
storing before/after by value doubles it per command — also fine, also noted.

### Phase 2 risks

- The determinism contract is fragile by construction: RNG draw order, compaction strategy, and
  tick sub-step order are all silently breakable by innocent refactors. The probe is the
  tripwire; the comments name the contract.
- Guarded serialization is the canonical-save protection; one unguarded default-emitting field
  breaks byte-identity for every committed scene. Review each `<<` against its default.
- `Rng` participates in copy-assignment (undo snapshots, `Scene::Copy` before the reset lands) —
  a copied-then-reset pool with a *not*-reset RNG double-plays the stream. The Copy fixup resets
  pool, accumulator, timer, and RNG together.

### Phase 2 verification

Scripted probes, x64 Debug; fixed `Timestep(1/60)` driven by hand, not wall clock.

| Check | Evidence |
|---|---|
| Deterministic replay | Emitter with Seed=42, rate 100, tick 300 fixed steps → record pool size + FNV hash over all particle positions; reset, replay → **identical size and hash**; run twice across process restarts → identical |
| Seed independence | Seed 42 vs 43, 300 ticks → different hashes; Seed=0 on two entities → different hashes (UUID-derived) |
| Lifecycle exactness | Rate 10, lifetime exactly 1.0, looping, 60Hz → pool size settles at 10 ±1 and stays; non-looping Duration 1.0 → emission stops at tick 60, pool drains to 0 by tick 120 |
| Cap honored | Rate 10000, MaxParticles 100 → pool never exceeds 100 (assert every tick) |
| Byte-identity save | Scene with a default-curve emitter: save → load → save byte-identical; edit SizeCurve (3 keys) + gradient (2 keys) → save → load → save byte-identical; **all pre-existing committed scenes still byte-match their canonical saves** |
| Play/stop hygiene | Editor scene emitter with 50 live particles → play → active-scene copy starts at pool 0 (warm-up measured), editor scene's pool untouched; stop → editor pool still 50, ticking continues |
| Undo free-coverage | Scripted `ComponentEditCommand<ParticleEmitterComponent>` with before/after differing in curve keys → undo/redo → curves byte-equal through a save (the EntitySnapshot vector-member claim, measured on this component) |
| Ordering enforced | `ValidateOrdering` silent with the 9-system chain; probe: reorder ParticleSystem after Render in a scratch scene → validation trips (the enforcement is real, not assumed) |

### Phase 2 execution notes

*(appended at execution)*

---

## Phase 3 — Rendering: billboards in view 5 + instanced mesh debris

**Goal:** emitters draw — billboards as one draw per emitter in view 5's sequential stream with
per-emitter blend mode, mesh particles as instanced opaque draws through the existing Phong
path — with draw counts measured, blend state proven restored, and the MRT contract honored.

### 3.1 `ParticleRenderer` + the view-5 seam

New `GanymedEngine/source/GanymedE/Renderer/ParticleRenderer.h/.cpp`: owns the particle shader,
transient-buffer construction, and the per-frame emitter list; `Renderer3D`'s flush calls
`ParticleRenderer::Flush()` at exactly one point — **after the transparent loop, before the
state restore** (the `Renderer3D.cpp:863-902` bracket). `RenderSystem` submits visible emitters,
passing the camera basis pulled from `RenderContext` (`Renderer3D`'s camera statics have no
accessor, and adding one is more invasive than passing three vec3s).

**Decision: submit into view 5's sequential stream, not a dedicated view 23.** In the seam,
billboards inherit the real `setViewTransform` (view 6's is degenerate — the Renderer2D lesson),
depth-test LESS against opaques with depth-write already off, the entity-ID MRT wiring, and
Sequential mode's submit-order-is-execution-order, which makes CPU sorting sufficient. A
separate view needs its own view transform, framebuffer binding, and clear/discard configuration
to say the same thing twice. Cost of the seam: particles always draw **after** all transparent
meshes rather than depth-interleaved with them — a smoke plume behind a glass pane composites
over it. Unity sorts particles and transparent meshes in one queue; Ganymed diverges (accepted
v1 artifact, recorded in `rendering.md`) because interleaving means injecting particle draws
into the transparent mesh sort — restructuring out of proportion to the artifact.

Ordering within the particle stream: emitters sorted back-to-front by distance (the
transparent-mesh posture); within an Alpha emitter, particles sorted back-to-front (`std::sort`
over a scratch index array on view depth — **the pool itself stays untouched; its order is the
determinism instrument**); Additive emitters skip the sort (order-independent by construction).

### 3.2 The billboard draw

Per emitter: CPU corner construction (the Renderer2D pattern — right/up basis scaled and rotated
per particle; works in any view, handles per-particle rotation naturally, no reliance on
`u_view` rows), vertex `{pos3, color4 (gradient sample), uv2, entityID float}` into transient
VB+IB (`getAvailTransient*` guard → drop excess with a one-per-session warning, the ImGui
backend posture); state = current stream state with blend swapped per emitter through the
`RenderState` singleton (mapping `ParticleBlend` → `RenderState::BlendMode`) and **restored
unconditionally after the particle block** (the bloom precedent — the singleton is sticky across
frames); texture slot 0 (`GetAsset<Texture2D>`, white fallback); manual
`FrameUniforms::Apply()` per submit (the u_Lights 2KB cost, fine at per-emitter draw counts);
one `bgfx::submit` per emitter. Target: **one draw call per visible emitter**, measured in the
verification table.

Shaders: `assets/shaders/src/vs_Particle.sc` + `fs_Particle.sc` (compile_shaders picks them up;
3 profiles × 3 app trees ship them). `fs_Particle` is unlit texture × vertex color — and
**writes `gl_FragData[1]` = the emitter's entity ID unconditionally** (the MRT obligation).
Consequence, stated not discovered: transparent texels of a particle quad still write the ID, so
hovering "empty" quad area picks the emitter — identical to existing transparent-mesh behavior,
so this is consistency, not regression. Picking a particle selects its **emitter** (the Unity
norm; particles are not entities and there is nothing else to select).

### 3.3 Mesh particles

**Decision: mesh particles ride the existing lit Phong opaque instanced path unchanged — no new
shader, no per-particle tint in v1.** `RenderSystem` submits each live particle as a normal
`SubmitMesh` with the particle's transform (position/rotation/uniform scale composed with the
emitter transform in local-space mode) and the emitter's entity ID, resolving the component's
`Material` handle via `GetAsset<Material>` (shared `Ref` → batches; unset = mesh defaults). The
opaque sort (Material → Mesh → Submesh) makes N same-mesh same-material particles contiguous =
**one `DrawInstancedGroup`** — measured below. The user's "unlit" decision was about billboards;
debris chunks are world geometry and want the scene's light, and this path gives `.gmat`
authoring, batching, and shadow casting for zero renderer work. The alternative (unlit instanced
shader + a 96B instance struct with tint in `i_data5`) is the recorded upgrade path (tension 3).
**Hard authoring rule, stated in `rendering.md` and the inspector tooltip: mesh particles must
use opaque materials** — the transparent path merges only sort-adjacent runs, so a transparent
debris material is one draw *per particle*.

Costs stated, not discovered: one `DrawCommand` push (two Ref bumps) + one frustum cull per
particle, and shadow casters pushed unculled ×4 cascades — mesh debris casts shadows (a feature,
and a cost: `DrawCalls` includes the shadow pass, so the stats will show it). Budget guidance in
docs: hundreds of mesh particles, not tens of thousands; billboards carry the high counts.

### 3.4 Culling + stats

Per-emitter frustum cull against the Phase 2 AABB (skip the whole emitter — no per-billboard
cull); `Renderer3D::Statistics` gains `ParticleEmitters`, `ParticleBillboards`,
`ParticleDrawCalls` (ResetStats is a memset — free), surfaced in the editor stats panel
(`EditorLayer.cpp:340-353`).

### Phase 3 risks

- The sticky `RenderState` singleton is the milestone's leak hazard: an early-out between the
  blend swap and the restore (the transient-buffer exhaustion path!) leaves Additive set for the
  next frame's 2D/grid/lines. Structure the flush so the restore is unconditional (scope guard
  or single-exit), and probe it.
- The MRT write is unverifiable by eye — a missing `gl_FragData[1]` shows up only as broken
  picking. Scripted readback probe required.
- The transient-buffer guard path truncates silently under load; the warning must be loud once,
  and the stats counter honest about dropped billboards.

### Phase 3 verification

Probes read `Renderer3D::GetStats()` and the picking readback after real frames; screenshots
interactive.

| Check | Evidence |
|---|---|
| One draw per emitter | 3 billboard emitters, 500 particles each → `ParticleDrawCalls == 3`; stats particle count 1500 |
| Mesh batching measured | 1 mesh emitter, 50 particles, one opaque `.gmat` → InstancedDraws +1 vs baseline, DrawCalls +1 (+shadow-cascade deltas recorded, not surprising); switch the material to `Transparent: true` → TransparentMeshes +50 (the hard rule, demonstrated) |
| Blend restore | Alpha + Additive emitters both live; probe compares the `RenderState` blend mode before frame N and after frame N across 100 frames → identical every frame; grid/2D render correctly after (screenshot, interactive) |
| Blend grouping | Additive emitter in front of Alpha emitter → both composite correctly (screenshot, interactive); log shows emitters sorted back-to-front |
| MRT / picking | Scripted: hover-pick readback over a dense particle cloud → the emitter's entity ID; over empty background → -1 unchanged |
| Cull | Emitter behind the camera → culled-emitter count +1, zero draws, sim still ticking (pool size unchanged) |
| Transient exhaustion | Probe forces a pathological particle count → truncated draw, one warning, no crash, state restored (the early-out leak case, explicitly) |
| Determinism unaffected | Phase 2's 300-tick hash probe re-run with rendering live → identical hash (render reads, never writes, sim state — the sort-scratch-array claim, measured) |
| Runtime parity | GanymedRuntime boots a particle scene: renders both modes, clean logs, no editor dependencies |
| Visual evidence | Screenshots: alpha smoke, additive sparks, mesh debris — committed to the execution notes (interactive) |

### Phase 3 execution notes

*(appended at execution)*

---

## Phase 4 — Curve editor + inspector polish

**Goal:** the curves become authorable — an in-house canvas curve editor and gradient editor
that participate correctly in the undo commit-boundary protocol — and the ParticleEmitter
inspector section becomes the full, mode-aware authoring surface.

### 4.1 `EditorWidgets` — the canvas curve editor

New `GanymedEditor/source/EditorWidgets.h/.cpp` (namespace `GanymedE::EditorUI`; premake regen)
— the codebase's first custom-drawn widget, which is why it gets its own home rather than
another static beside `DrawVec3Control`:

```cpp
namespace GanymedE::EditorUI {
	bool CurveEditor(const char* label, FloatCurve& curve, float minY, float maxY,
	                 ImVec2 size = {0, 80});
	bool GradientEditor(const char* label, ColorGradient& gradient);
}
// Both honor the bool(T&) contract: return true ONLY on frames a key actually changed.
```

**Decision: in-house ~200-line widget, not the vendored `ImCurveEdit`/`ImGradient`.** Those
files sit uncompiled in `extern/ImGuizmo/src`; adding them is two premake lines — and then the
editor inherits a Delegate-interface API, an internal edit-state model, its own keyframe
container to convert to and from, and **unverified `ActiveId` participation**, which is the one
property the undo protocol cannot live without. The house protocol (`TrackCommitBoundary`
reading `ActiveId` before/after each section) turns any widget that doesn't hold `ActiveId`
across a drag into a command-per-frame spam generator — the documented failure mode. An owned
widget makes the obligation structural: **one `InvisibleButton` spans the canvas** (verified in
the vendored ImGui 1.91.9b source: a held InvisibleButton owns `ActiveId` for the entire drag),
hit-testing against key positions decides what the drag moves, `ImDrawList` draws
grid/polyline/keys. Interactions: drag a key (clamped to neighbors' times or re-sorted through
the `SetKey` API — never raw vector writes), double-click empty canvas adds a key at that
position (instant edit → same-frame command via the instant-edit branch), right-click a key
deletes it (refused on the last key, matching the type invariant). `edited` is true only on
frames a key's value actually changed — a drag that hovers without moving commits nothing (the
pending-edit-dropped rule does the rest).

`GradientEditor`: a horizontal color bar, keys as draggable markers (same InvisibleButton
ownership), color assignment through `ColorEdit4`'s popup — the content-authoring milestone's
execution notes already established popups fall out of the boundary protocol correctly
(active-no-edit frames drop the pending edit; the eventual edit commits).

### 4.2 The full inspector section

`Panels/SceneHierarchyPanel.cpp`, replacing Phase 2's numeric-only section: grouped headers
(Emission / Initial / Over Lifetime / Rendering), mode-aware — Billboard shows the Texture slot
(`AcceptAssetDrop(AssetType::Texture)`) + Blend combo; Mesh shows Mesh + Material slots (the
`.gmat` slot reuses the materials-milestone drop pattern) and the opaque-material rule as a
tooltip. Min/Max pairs as paired DragFloats with a max≥min clamp on edit. Edit-mode preview
controls — Play / Stop / Restart buttons driving the runtime `Playing`/`Time` state (not
undoable, not serialized: preview state, not authored state — the same line the material
editor's live-preview drew). All of it under the existing `bool(T&)` contract; curve/gradient
rows are just two more widget calls whose returns join the OR.

### Phase 4 risks

- Undo-protocol participation is the phase's core risk and it is concentrated in two rules:
  ActiveId held for the whole drag (structural, via InvisibleButton) and `edited` honest per
  frame (reviewable in ~10 lines). The failure modes are command-spam and lost edits; the log
  probe reads directly in `GanymedE.log` (`EditorUndoStack::Push` traces every command).
- Canvas coordinate math (curve-space ↔ screen-space with a min/max Y range) is fiddly and
  purely visual; wrong math is caught by eyes, not probes — budget an interactive pass.
- The widget edits the curve through the sorted-invariant API only; a "fast path" raw write is
  the regression to reject in review.

### Phase 4 verification

Interactive-heavy by nature (established: ImGui cannot be driven programmatically); the log
evidence is permanent.

| Check | Evidence |
|---|---|
| Drag granularity | Interactive: one key dragged for ~2 s → `GanymedE.log` shows exactly one `Undo: pushed 'Edit ParticleEmitter'` line; Ctrl+Z restores the pre-drag curve exactly |
| Instant edits | Interactive: double-click-add → one command that frame; right-click-delete → one command; delete on the last key → refused, no command |
| Hover-no-move | Interactive: grab a key, release without moving → zero commands (the pending-dropped rule) |
| Scripted undo round-trip | Phase 2's `ComponentEditCommand` probe extended with multi-key curve + gradient diffs → undo-all/save byte-identical to the pre-edit save; redo-all/save identical to the post-edit save |
| Sorted invariant under UI | Interactive: drag a key past its neighbor → keys re-sort, the polyline never crosses itself, the sim samples continuously (no pop in the live preview) |
| Mode-aware section | Interactive: switching RenderMode swaps the asset slots; each switch is one undoable command |
| Live authoring loop | Interactive: edit SizeCurve while the emitter plays in-edit → particles respond next frame (the Phase-2 preview posture paying off, on camera — screenshot) |
| Save integrity | After an interactive authoring session: save → load → save byte-identical |

### Phase 4 execution notes

*(appended at execution)*

---

## Phase 5 — Lua bindings + runtime demo + docs sweep

**Goal:** scripts drive emitters idempotently, the runtime ships a demo effect, and the docs
match the tree.

### 5.1 Lua bindings

`Scripting/ScriptBindings.cpp` + the `ganymed.d.ts` hand-mirror. Following the documented
two-route split (`ScriptBindings.cpp:241-253`): the pool is component-owned, so
**Play/Stop/EmitBurst are component-direct** (flags + a burst count consumed by
`ParticleSystem` next tick), and authored numeric fields are component-direct untracked (no
reactive consumers exist for this component — `RenderSystem`'s view is plain RO, not reactive;
state that in the binding comment). Surface, kept minimal: `PlayParticles()`,
`StopParticles()`, `EmitBurst(count)`, `IsParticlesPlaying()`, plus read/write of the scalar
authored fields. **`PlayParticles` on an already-playing emitter is a no-op, not a restart** —
the documented `PlayAnimation` idempotency trap (:158-178): script systems run before
`ParticleSystem`, and scripts call from per-frame branches; a restart semantic would reset the
pool 60×/sec. `EmitBurst` accumulates (two calls in one frame = one bigger burst). Curves are
not scriptable in v1 ("Not doing").

### 5.2 Runtime demo

The audio milestone's demo shape, mirrored: the GanymedRuntime demo scene gains a
physics-impact spark burst — a Lua script's collision callback calls `EmitBurst` on a child
emitter entity (authored as a prefab, exercising Decision 2's reuse story end to end). This is
the milestone exit proof that component + sim + rendering + prefab + Lua compose.

### 5.3 Docs sweep

Each item belongs to the phase that caused it; this is the audit. `docs/engine/rendering.md` —
the particle pass (the view-5 seam, sequential ordering, blend swap/restore, MRT +
transparent-texel picking note, the after-transparent-meshes artifact, the opaque-mesh-particle
batching rule, budget guidance); `docs/engine/scene.md` — `ParticleEmitterComponent`, the
9-system chain, the Copy-reset posture and warm-up consequence, the determinism contract;
`docs/engine/ecs.md` — only if the checklist surface moved; `docs/editor/editor.md` — the
curve/gradient editors, their undo-protocol obligations (the InvisibleButton rule, recorded as
the house pattern for future custom widgets), the preview-tick posture against the
Animation/Audio precedents; `docs/engine/scripting.md` + `ganymed.d.ts` — the new bindings and
the idempotency note; this file gains execution notes per phase, per house rule.

### Phase 5 verification

| Check | Evidence |
|---|---|
| Idempotency | A probe script calls `PlayParticles()` every frame for 120 frames → pool grows monotonically to steady state, zero restarts (the pool-size log is monotone until first retirements) |
| Burst accumulation | Two `EmitBurst(10)` in one frame → exactly +20 next tick |
| Demo | GanymedRuntime boots the demo scene: impact → burst renders, clean logs, `assets/` byte-identical after the run (the runtime write posture, re-verified) |
| d.ts parity | Every binding registered in `ScriptBindings.cpp` appears in `ganymed.d.ts` (grep audit) |
| Doc audit | Every claim in the touched docs spot-checked against code; stale-claims grep for the rewritten sections |
| Full regression | Editor smoke (open/author/play/stop/save), Sandbox boot, runtime demo boot — clean logs; all committed scenes still byte-match their canonical saves |

### Phase 5 execution notes

*(appended at execution)*

---

## Explicitly not doing (v1)

Named so nobody half-builds them in passing:

- **Flipbook/atlas animation, soft particles, lit billboards** — Decision 4's line; each is a
  shader-feature axis with its own uniforms and editor surface.
- **GPU simulation** (compute or transform feedback) — CPU sim at v1 scale is the entire
  determinism story; GPU sim forfeits the replay instrument.
- **Collision** (Decision 5) — no depth-buffer or physics-query interaction.
- **Trails/ribbons, sub-emitters, velocity-over-lifetime / noise / force fields** — module
  families, not fields.
- **`.gparticle` asset type** (Decision 2) — prefabs are the reuse vehicle.
- **`ImCurveEdit`/`ImGradient` adoption** — argued in Phase 4; the files stay uncompiled.
- **Curve tangents/Bezier, Step interpolation on particle curves** — linear only.
- **Per-particle mesh tint** (the wider instance struct + unlit instanced shader) — the recorded
  upgrade path of tension 3.
- **Emitter shapes beyond the cone** (box/edge/mesh/donut) — `ConeAngle` 0→180 spans beam to
  sphere; that is the v1 parameterization.
- **Prewarm** — play mode warms up from empty (Phase 2's honest cost of the Copy-reset posture).
- **Authored burst lists** — `EmitBurst` from Lua covers the demo; Unity's Bursts array waits
  for a need.
- **Premultiplied-alpha blend mode** — would let alpha + additive mix in one sorted stream
  (tension 2); wait for an effect that needs it.
- **Interleaving particles with transparent meshes** in one depth sort — the accepted
  view-5-seam artifact.
- **Selected-only editor preview / a playback scrub widget** — tick-all is v1; refinement is
  cheap later.
- **Curve editing from Lua** — scalar fields only.
- **StartColor separate from the gradient, rotation-over-lifetime curve, emission-rate curve,
  Direction vector** — the Phase 2 trims, listed so their absence reads as decided, not
  forgotten.

## Design tensions, recorded

1. **Authoring home: component (Unity) vs asset (Unreal/Godot)** → component (fixed decision).
   Prefabs carry reuse; the YAML block is the future asset schema if ever needed.
2. **Blend handling: per-emitter Alpha/Additive modes vs one premultiplied stream** →
   per-emitter modes. Premultiplied would sort alpha and additive particles together correctly,
   but adds a `RenderState` mode, an authoring convention (premultiplied textures), and shader
   work for a compositing edge v1 content won't hit. Deferred, named.
3. **Mesh particles: ride the lit Phong opaque instanced path vs a new unlit instanced shader
   with per-particle tint** → ride it. Zero renderer work, `.gmat` materials, batching and
   shadows for free; the "unlit" user decision was about billboards — debris is world geometry
   and wants scene light. Cost: no tint; the upgrade path (96B instance struct, `i_data5`, own
   vertex shader) is recorded.
4. **Pool ownership: component-owned (Palette shape) vs system-owned (Audio shape)** →
   component-owned. Pure data, and the snapshot/copy/ordering machinery already exists for it;
   the honest divergence from Palette (state persists across frames) surfaces as play-mode
   warm-up, accepted.
5. **Editor preview: tick-all vs silent (Audio) vs sample-without-advance (Animation)** →
   tick-all. Sampling is impossible for a stateful sim; silence makes visual authoring blind.
   The header comment names both precedents and this deviation, per house rule.
6. **Curve widget: in-house canvas vs vendored ImCurveEdit** → in-house. The undo protocol's
   ActiveId obligation is structural with an owned InvisibleButton and unverifiable in a
   Delegate API someone else wrote. ~200 lines buys the first house custom widget and its
   documented pattern.
7. **Curve storage: AoS `vector<Key>` vs Animation.h's parallel arrays** → AoS. No glTF
   constraint on an authoring type; the sampler algorithm is shared in shape, the storage is
   not — one comment in `Curve.h` names the sibling.
8. **Billboard pass: the view-5 sequential seam vs a dedicated view 23** → the seam. Inherits
   transform, depth, MRT, and ordering; the cost is particles-after-transparent-meshes,
   accepted and documented.
9. **Retirement: stable compaction vs swap-erase** → stable. Pool order is part of the
   determinism instrument; the copy cost at v1 scale is noise.
10. **Entity ID in transparent texels** → write unconditionally; pick selects the emitter.
    Consistent with existing transparent meshes; a discard threshold would break additive
    accumulation.
11. **Seed semantics: fixed authored seed vs per-instance variation** → `Seed=0` derives from
    the entity UUID, nonzero pins. Both behaviors, one field; the probe authors a pinned seed.
12. **RNG home: `Core/Random.h` vs `Math/`** → Core, beside the UUID precedent; and PCG32 over
    `mt19937` for copyable 16-byte state and a pinned float derivation.
13. **Per-frame component copies with keyframe vectors (undo protocol)** → accepted; the
    "POD-sized" comment is amended rather than the protocol redesigned for one component.
14. **Blend enum on the component: reuse `RenderState::BlendMode` vs a firewalled
    component-local enum** → component-local `ParticleBlend` (the `AudioTypes.h` split
    precedent). `RenderState.h` packs `BGFX_STATE_*`; including it from `Components.h` would
    leak bgfx defines into every scene TU, against design principle 6. The renderer maps the
    enum at draw time.
15. **Gravity in local-space sim: apply world-down naively vs rotate into emitter space** →
    rotate. One inverse rotation per emitter per frame; the naive version is visibly wrong on
    any tilted emitter.
