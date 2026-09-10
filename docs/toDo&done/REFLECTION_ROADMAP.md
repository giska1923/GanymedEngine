# Reflection Milestone — Scope Sketch (`entt::meta`)

Status: **scope sketch, not a phase plan.** Written 2026-09-08 against `master` at `ee4148f`. No
phase is broken down to the step-and-verification level that
[`PARTICLE_ROADMAP.md`](PARTICLE_ROADMAP.md) or
[`ASSET_PIPELINE_ROADMAP.md`](ASSET_PIPELINE_ROADMAP.md) use, deliberately: this milestone is
sequenced *after* asset Phases 1–3 (see [`ASSET_PIPELINE_ROADMAP.md`](ASSET_PIPELINE_ROADMAP.md)
decision 14), and two of its four phases are shaped by what those phases produce. Detailing them now
would be designing against requirements that do not exist yet. What this document fixes is the
**library choice, the scope boundary, the attribute vocabulary, and the risks** — the decisions that
are expensive to change later.

The milestone thesis: Ganymed already reflects **types** and uses it well — `ComponentList`,
`ForEachType` and `ComponentTraits<T>` in
[`ECS/ComponentTraits.h:69`](../../GanymedEngine/source/GanymedE/ECS/ComponentTraits.h#L69) drive
`EditorUndo`'s snapshot tuple, whose header correctly claims *"a new component type joins undo for
free."* What it does not reflect is **members**. Every consumer that needs to know a component's
fields — the serializer, the inspector, the Lua bindings — hand-lists them, and the two lists that
must agree (save and load) have no compile-time enforcement. This milestone closes the member gap and
nothing else.

---

## Decision 1 — `entt::meta`, not RTTR

entt is vendored at **3.16.0** with the complete `meta` module (14 headers in
`extern/entt/src/entt/meta/`). Nothing in the engine uses it yet.

Adopting RTTR would mean running a second reflection system beside one already shipped, by a
different author, with different idioms, that knows nothing about entt's component storage.
`entt::meta` interops with it, which is what "add component by type name", copy/paste-a-component and
prefab property diffing actually require — each needs to get from a reflected type back to real
storage. There is also a maintenance question about RTTR (my recollection is that 0.9.6 from ~2018 is
still its last tagged release; **verify before acting on that**), but it is not the deciding factor.
"Second reflection library in a monolithic static-lib build" is.

What RTTR would buy that `entt::meta` does not: reflection across a DLL boundary, and tooling that
enumerates types it was not compiled against. Both matter for plugin architectures. Ganymed is one
static library in one build; neither applies.

The API this milestone builds on, all verified present in 3.16.0:

| Registration (`entt::meta_factory<T>`, `factory.hpp`) | Read-back (`meta.hpp`) |
| --- | --- |
| `.type(name)` / `.type(id, name)` | `resolve<T>()`, `resolve(id)`, `resolve()` → range of all types |
| `.data<&T::member>(name)` | `meta_type::data()` → range of `meta_data` |
| `.func<&T::fn>(name)` | `meta_data::name()`, `::type()`, `::get(instance)`, `::set(instance, value)` |
| `.base<B>()`, `.conv<To>()`, `.ctor<Args...>()` | `meta_any`, `meta_handle`, `meta_type::func()` |
| `.traits(Value, bool unset = false)` | `meta_data::traits<UserEnum>()` |
| `.custom<Value>(args...)` | `meta_data::custom()` → `meta_custom`, type-hash-checked cast |

---

## Decision 2 — the attribute vocabulary is two-tier, because entt already is

Every engine that auto-generates an inspector converges on attribute metadata, because the last 20%
of fields need a widget the type alone cannot imply: UE has `UPROPERTY(EditAnywhere,
meta=(ClampMin="0.0"))` plus `IPropertyTypeCustomization`; Unity has `[Range]` / `[ColorUsage]` plus
`CustomPropertyDrawer`; Godot has `_get_property_list` plus `PROPERTY_HINT_*`. RTTR's answer is
string-keyed `metadata(key, variant)` — a map lookup and a dynamic variant per query.

entt 3.16 offers something better shaped, and the design should use both tiers rather than inventing
a third:

- **`.traits(Flags)`** — a user bitflag enum packed into the node itself. Free to read, no
  allocation, no lookup. This is where `Hidden`, `ReadOnly`, `NotSerialized`, `Transient`,
  `AdvancedOnly` go. **Hard constraint: `meta_traits::_user_defined_traits = 0xFFFF`
  (`meta/node.hpp:43`), so a user enum gets exactly 16 flags**, shifted into the upper half of a
  `uint32_t`. Budget them; do not spend flags on things that want a value.
- **`.custom<Payload>(args...)`** — an arbitrary struct held by `shared_ptr`, retrieved with a
  type-hash-checked cast (`meta.hpp:767`). This is where valued attributes go: `Range{min, max}`,
  `AssetSlot{AssetType}`, `Tooltip{text}`, `EnumNames{...}`, `CurveBounds{minY, maxY}`. One
  allocation per registration, none per query.

Consequence: prefer a flag over a payload whenever the attribute has no value, and keep the payload
structs small and POD-ish. A single `Attributes` payload struct per field, rather than one payload
per attribute kind, is likely the right shape — `meta_custom` holds one payload, not a list.

---

## Decision 3 — never derive a serialized key from a C++ identifier

The tempting macro is `#define GE_FIELD(f) .data<&Type::f>(#f)`. It is fine for editor display names
and actively dangerous for serialization keys: it couples the YAML key in every saved scene to the
C++ member identifier, so renaming a member silently invalidates every scene and prefab on disk. UE
needs a whole `CoreRedirects` system because of exactly this coupling.

entt handles it correctly by construction — `.data<&T::member>(name)` requires the name explicitly,
so the registration *is* the stable contract. The rule is just to not defeat that with a
stringifying macro on serialized types. Editor-only display names may use one.

Corollary worth stating: the hand-written `SceneSerializer` has an accidental virtue — a member
rename forces a compile error in the serializer, which forces a human to decide what happens to the
key. Auto-generation removes that forcing function. The replacement is a test that round-trips every
component through reflection and compares against a committed golden YAML file, so a key change
fails CI rather than corrupting content silently.

---

## Decision 4 — reflection is for editor, serialization and tooling paths only. Never per-frame.

`meta_data::get(instance)` returns `meta_any` **by value**. entt's `basic_any` has small-buffer
optimization, so a `float` or a `bool` will not allocate, but a `std::string`, a `glm::mat4` or a
`std::vector` will. For an inspector that is a handful of components on one selected entity — a few
dozen `meta_any` per frame, irrelevant. For a scene serializer it is thousands of allocations per
save, which is fine because saving is not a frame-budget operation.

For anything a system does per entity per frame it is disqualifying. The discipline line: **no engine
system may read a component through `entt::meta`.** Systems use `ComponentList`/`ForEachType` and
direct member access, as they do today. Write this into
[`ecs.md`](../engine/ecs.md) when the milestone lands, because it is the constraint most likely to be
violated by someone who sees a convenient generic path.

---

## Decision 5 — one `ComponentReflection.cpp`, not per-component blocks

All 23 registration blocks live in a single `Reflection/ComponentReflection.cpp`, with
`Reflection::Init()` in it naming every one of them.

The argument for the alternative is real: a registration block next to its `struct` in
`Components.h` is easier to read and harder to forget when adding a field. The argument against it is
that it multiplies the linker-stripping risk by 23 — the failure mode from the Risks section, where a
Release build or the runtime silently loses a type because nothing referenced its TU. One file with
one explicit `Init()` that enumerates its own contents cannot half-register. It also gives one place
to keep in sync with `ComponentList`, one place to audit how the attribute vocabulary is actually
being used, and one TU to watch if compile time becomes a problem (split by component group then, per
the Risks section — not before).

**The cost this incurs, stated plainly:** field knowledge now lives in two files, so adding a member
to a component in `Components.h` leaves it unreflected until someone remembers to edit
`ComponentReflection.cpp`. Nothing catches that automatically. The R1 test (every `ComponentList`
type resolves, every `meta_data` has a name) proves types are registered; it *cannot* prove a type's
member list is complete, because the true member set is exactly what is not reflected.

The one forcing function that does work is a per-component size sentinel in the registration file:

```cpp
// Bump deliberately when adding a field, after reflecting it. Catches the
// "added a member, forgot to register it" case that no test can detect.
static_assert(sizeof(PointLightComponent) == 24, "PointLightComponent changed - reflect the new field");
```

Honest limits: padding makes it imperfect (a `bool` dropped into existing padding will not move
`sizeof`), and it is one more hand-maintained number. It nonetheless converts the common case —
adding a `float`, a `glm::vec3`, an `AssetRef` — from a silent omission into a compile error, which is
the same trade `AssetType`'s append-only comment already makes. Recommended, not mandatory; decide at
R1 whether the noise is worth it, and if it is, apply it to every component rather than a chosen few.

---

## The measured surface (what this actually replaces)

The naive case for reflection is ~2,800 lines of consumers over 489 lines of component declarations
across 23 components — `SceneSerializer.cpp` 1,050, `SceneHierarchyPanel.cpp` 1,787,
`ScriptEngine.cpp` 571. That ratio overstates the win considerably.

Measured against `ee4148f`:

- **Inspector: ~660 reflectable lines, not 1,787.** The per-component drawing runs
  [`SceneHierarchyPanel.cpp:1127-1790`](../../GanymedEditor/source/Panels/SceneHierarchyPanel.cpp#L1127),
  19 `DrawComponent<T>` blocks. The other ~1,120 lines are the hierarchy tree, drag-drop
  reparenting, context menus, the add-component menu, the `DrawComponent` template itself
  ([`:616`](../../GanymedEditor/source/Panels/SceneHierarchyPanel.cpp#L616)) and the undo
  commit-boundary protocol. Reflection touches none of it.
- **Of those 660, `ParticleEmitterComponent` alone is 142**
  ([`:1595-1737`](../../GanymedEditor/source/Panels/SceneHierarchyPanel.cpp#L1595)) — curve and
  gradient editors, i.e. 21% of the reflectable surface and exactly what a generic inspector handles
  worst. It will need a custom drawer, which is a registration plus a lambda, not free.
- **Widget density is low: 77 total widget calls across 23 components**, ~3.3 each. This is not a
  wall of `DragFloat` waiting to be collapsed.
- **Undo needs nothing from this milestone.**
  [`EditorUndo.h`](../../GanymedEditor/source/EditorUndo.h) snapshots whole component values, which is
  *better* than a property-path system: lossless (it round-trips `AnimatorComponent::Time`, which the
  serializer deliberately drops), it distinguishes an absent field override from one equal to the
  default, and it puts no string parsing on the undo path. Its own header argues this. Do not convert
  it.

Honest expected outcome: **~400–500 inspector lines and ~600–700 serializer lines replaced**, against
~200 lines of custom-drawer registrations plus the generic consumers themselves. The real return is
not deletion, it is that a new component type joins the inspector, the serializer and the Lua
bindings by registering once — the same property `ComponentList` already gives undo.

---

## Sketched phases

Deliberately coarse. Phases R3 and R4 depend on asset Phase 3 having landed.

**R1 — Registration and the attribute vocabulary.** `Reflection/` under `GanymedE/`: the user trait
enum (≤16 flags), the attribute payload struct, a `GE_REFLECT_COMPONENT` registration entry point, and
all 23 registration blocks in one `ComponentReflection.cpp` (decision 5) with `Reflection::Init()`
naming each. Plus a test asserting every type in `ComponentList` resolves and every reflected
`meta_data` has a name, and a call on the size sentinels if they are adopted. No consumer yet — this
phase is verifiable on its own and produces no behaviour change.

**R2 — Generic inspector with a custom-drawer registry.** A `PropertyDrawer` dispatch keyed on
`meta_type`, default drawers for the primitives and `glm` vector types, custom drawers registered for
`FloatCurve`, `ColorGradient`, enums, and `AssetRef<T>` slots. `DrawComponent<T>` bodies collapse to
the generic path where they can; the particle emitter and anything with cross-field logic keep a
custom drawer. **The undo commit-boundary protocol must survive intact** — it is 7 call sites and it
is the subtlest thing in the panel; the generic drawer has to own `ActiveId` across a drag exactly as
the hand-written path does. Verification is behavioural: the inspector looks and behaves identically,
and one drag still produces one undo entry.

**R3 — Generic serializer.** `SceneSerializer` save and load driven by `meta_type::data()`, with
`NotSerialized` honoured and the golden-YAML round-trip test from decision 3 as the gate. The
byte-identical round-trip check that asset Phase 3 uses is the right verification here too. Note the
serializer has deliberate asymmetries (`AnimatorComponent::Time` dropped, `.gmat` texture references
as paths) that must be expressed as attributes rather than lost.

**R4 — What the milestone is actually for: prefab per-property overrides and multi-entity editing.**
These are the two items that are *impossible* today rather than tedious, and they are why the
milestone exists at all. Per-property overrides need a stable property path per field and a diff
against the prefab source — Unity's model. Multi-entity editing needs "iterate the properties common
to the selection, show a mixed-value state where they differ." Both are pure `meta_data` consumers on
top of R1–R3. Scope them properly when they are next, not now.

---

## Risks

- **Static-init registration stripped by the linker.** The classic failure for this pattern in a
  static library: if nothing in the program references the TU holding a registration block, the
  linker drops it and the type silently is not reflected. It will work in a Debug build and fail in
  Release, or work in the editor and fail in the runtime. Do not rely on file-scope static
  registration objects. Use an explicit `Reflection::Init()` that names every registration function,
  and assert at the end of it that every `ComponentList` entry resolves. This is the single most
  likely way this milestone wastes a day.
- **`meta_ctx` is a `locator` singleton.** Registration order across TUs and the interaction with the
  editor/runtime having separate entry points both need a deliberate answer, not an accident.
- **The 16-flag trait ceiling** (`meta/node.hpp:43`). Reachable if flags are spent carelessly.
  Anything with a value belongs in `.custom<>`.
- **The inspector's undo protocol is the real integration risk**, not the reflection. The generic
  drawer must preserve the "one command per drag, capture-before on grab, capture-after on release"
  contract that `ComponentEditCommandBase::CaptureAfter` depends on. Getting this wrong produces one
  undo entry per frame of a drag, which is worse than no undo.
- **Compile time.** `entt::meta` is header-heavy and template-heavy. Measure before and after; if the
  registration TU becomes a bottleneck, split it per component group rather than reaching for
  precompiled headers reflexively.
- **Scope creep into "reflect everything."** Reflecting assets, render state, settings and physics
  configs is not in scope. Components only, plus the `.meta` import configs when asset Phase 4 needs
  them.

---

## Explicitly out of scope

- **Converting `EditorUndo` to property paths.** It is better as it is — see the measured-surface
  section.
- **Reflection-driven Lua bindings.** Tempting (571 lines in `ScriptEngine.cpp`), but sol2 already
  does usertype binding well, the bindings encode deliberate API decisions rather than raw field
  access, and a generic bridge would go through `meta_any` on a path scripts hit frequently. Revisit
  only if the binding list becomes a maintenance problem.
- **Reflection in engine systems.** Decision 4. Not a scope question, a correctness one.
- **Reflection across a DLL boundary, and plugin type discovery.** The features that would justify
  RTTR. Ganymed has no plugin architecture and this milestone does not create the need for one.
- **A code generator / custom build step.** UE's UHT-style approach buys `UPROPERTY` on the member
  itself and no separate registration block. It also buys a build-tool dependency, a parse step, and
  generated headers to keep in sync. Hand-written registration blocks for 23 components are cheaper
  than a generator by a wide margin at this scale, and decision 3 shows the explicit name is a feature
  rather than a cost.

---

## Open questions to settle before R1

1. Does `Reflection::Init()` live in the engine, with the editor registering additional
   editor-only attributes on top? Probably yes — the serializer is engine-side and needs the same
   registration — but the attribute *vocabulary* is mostly editor-shaped, so the layering needs a
   decision.
2. Which asset Phase 3 shape does the `AssetRef<T>` drawer key on — the concrete `AssetRef<Material>`
   type, or a reflected base plus an `AssetSlot{AssetType}` attribute? Cannot answer until
   `AssetRef` exists, which is why R2 waits.
3. Are the per-component `sizeof` sentinels of decision 5 worth their noise? Decide at R1, all-or-none.

## R1 — executed (2026-09-08)

Landed: [`Reflection/Reflection.h`](../../GanymedEngine/source/GanymedE/Reflection/Reflection.h) and
[`Reflection/ComponentReflection.cpp`](../../GanymedEngine/source/GanymedE/Reflection/ComponentReflection.cpp),
`Reflection::Init()` wired into `Application`'s constructor second, after `JobSystem::Init()`. 32 types
and 119 members registered; no consumer, no behaviour change. Documented in
[scene.md § Member reflection](../engine/scene.md#member-reflection), with the decision-4 discipline
line added as rule 9 of [ecs.md § Rules that must never break](../engine/ecs.md#rules-that-must-never-break).

It lives in `scene.md` rather than a `reflection.md` of its own because the layer is currently 2 files
with no consumers, and `scene.md` already owns components and serialization. **Worth promoting to its
own doc when R2 or R3 lands** — at that point the drawer registry, the property-path scheme and the
serializer contract are more than a section.

**Sequencing corrected.** The milestone was sketched as following asset Phases 1–3. That holds for
R2–R4 (the `AssetRef<T>` drawer and the `.gmat`-as-path asymmetry both need Phase 3) but not R1, which
registers today's types under today's names and verifies itself. R1 was therefore executed first.

Open questions, answered:

1. **Registration is engine-side only; the editor adds no second pass.** The deciding API fact is that
   `.custom<>` holds exactly one payload per meta object — a second registration pass would *overwrite*
   the engine's attributes rather than add to them. One payload slot means one owner. Editor-only
   knowledge (drawer function pointers) goes in the editor's own `meta_type`-keyed map in R2.
2. Deferred to R2 by construction, as intended.
3. **Sentinels adopted, but not all-or-none — that instruction is not implementable as written.**
   `sizeof(std::string)` is 40 with MSVC's STL and 32 with libstdc++; vector and unordered_map differ
   likewise. A hard `static_assert` on the 6 components with a library container would break the Linux
   and macOS builds or degenerate into a per-platform table. Rule adopted instead: *library container
   member ⇒ no sentinel*, which is mechanical rather than a judgement call and covers 16 of the 21
   `ComponentList` entries.

Two corrections to the plan as sketched:

- **"A test asserting every type in `ComponentList` resolves" would pass vacuously.**
  `internal::resolve<Type>` returns a node built from a function-local static when the type is absent
  from the meta context, so `entt::resolve<T>()` is *always* truthy — the test would report an entirely
  empty registration file as healthy. The honest signal is `resolve<T>().name() != nullptr`; `name` is
  only ever assigned by `.type(id, name)`.
- **The vocabulary was pruned against the measured code, not the sketch.** `AdvancedOnly` dropped
  (nothing in the panel has an advanced section, and no `BeginDisabled` exists). `EnumNames` dropped
  (entt reflects values on the enum *type*, so names live once beside the enum). `Color`, `Radians`,
  `OmitIfDefault`, `Flatten`, `SerializeByName`, `Component` and `Custom` added, each forced by a
  specific measured fact — see scene.md. `Transient` folded into `Runtime = Hidden | NotSerialized`
  rather than spending a flag. Ten of the sixteen used.

Risks, measured rather than assumed:

- **Compile time is a non-issue.** `<entt/entt.hpp>` is amalgamated (meta included) and was already
  included by 8 engine headers, so the only new cost is instantiation in one TU. Touch-and-rebuild of
  `ComponentReflection.cpp` is 7.8 s wall clock; the same measurement on `Core/JobSystem.cpp` as a
  control is 3.7 s. ~4 s for one TU nobody edits often. No need to split it per component group.
- **`meta_ctx` singleton**: `meta_factory<T>{}` delegates to `locator<meta_ctx>::value_or()`, and the
  engine is one static library linked into one binary, so there is one context. A plugin DLL would have
  to be passed the context explicitly; Ganymed has no plugin architecture.
- **Linker stripping**: avoided as planned — one explicit `Init()` naming eleven registration functions,
  and the boot log prints the type and member counts so a dropped block is visible.

Verified: x64 Debug builds clean (engine and full solution, 0 warnings), the editor boots and logs
`Reflection initialised: 32 types, 119 members` / `Reflection validation passed`. `Validate()` was
negative-tested by deliberately marking `AnimatorComponent::Speed` as `Color` with a texture asset
slot — both violations were reported and the assert fired; the injection was then reverted and the
clean run repeated. Two source files added, so premake regeneration was required.

---

## R2 — executed (2026-09-10)

Landed: [`EditorInspector.h/.cpp`](../../GanymedEditor/source/EditorInspector.h) - a
`meta_type`-keyed `PropertyDrawer` registry, default drawers, and `DrawReflectedComponent<T>`.
Eight of the panel's twenty sections converted. Two source files added, so premake regeneration was
required. Written up in
[editor.md](../editor/editor.md#the-generic-reflected-inspector) and
[scene.md](../engine/scene.md#current-state).

**The phase's headline risk did not exist.** The plan said "the generic drawer has to own `ActiveId`
across a drag exactly as the hand-written path does", and called the undo protocol "the real
integration risk". It is not: the commit boundary lives in `SceneHierarchyPanel::DrawComponent<T>`,
which wraps the whole *section* - it copies the component, reads `GetActiveID()` either side of the
body, and hands both to `TrackCommitBoundary<T>`. The body's entire obligation is the house rule
already written on that boundary: mutate only on a reported edit, return true when it does. Every
drawer obeys it, so **`EditorUndo` was not touched at all** and one-command-per-gesture survives by
construction rather than by re-implementation.

**Two entt facts established by measuring, not by reading.**

- **`meta_type::data()` iterates in registration order.** Checked first, because field order is
  user-visible and a hash-ordered container would have scrambled every converted section and forced
  an ordering attribute. It does not. Registration order in `ComponentReflection.cpp` is now a real
  contract.
- **`meta_data::get` may return a copy or a reference depending on policy**, so every drawer reads
  into a concrete local, edits that, and writes back with `set`. Editing through the `meta_any`
  would work by accident today and break on a policy change.

**Two gaps the conversion exposed, both fixed rather than worked around.**

- `BoxColliderComponent::HalfExtents` is drawn with `resetValue = 0.5`, which no attribute could
  express. Added `Attr::Reset`. One field uses it, which is the same bar every other attribute in
  the vocabulary had to clear.
- **`Speed` was inert on every vec3 field.** `DrawVec3Control` hardcoded `0.1f`, so the
  `.Speed(0.05f)` registered on the collider offsets did nothing. Threaded the speed through the
  widget and corrected those registrations to `0.1f` - the value that actually ships. Declaring an
  attribute nothing reads is worse than not declaring it, and this was only visible because
  something finally tried to consume it.

`DrawVec3Control` moved from a file-static in the panel to `EditorWidgets`, so the reflected drawer
produces the same widget the hand-written sections do rather than a `DragFloat3` that looks nothing
like it.

**Converted:** Sprite Renderer, Directional Light, Point Light, Audio Listener, Rigid Body, Box /
Sphere / Capsule Collider. Between them they exercise colours, ranged floats, bools, a registered
enum, type-level notes, the X/Y/Z widget and a `Flatten`ed nested struct.

**Still hand-written**, each for a reason stated at its own site: Transform, Prefab Instance, Camera,
Static Mesh, Animator, Script, Spot Light, Sky Light, Audio Source, Particle Emitter. R1's
registration comments had already predicted the Spot Light and Sky Light cases.

**Verification.**

| Check | Result |
| --- | --- |
| Looks identical | An entity carrying all seven convertible components, inspector open, backbuffer screenshot against `HEAD`: the Properties panel is **pixel-identical**. The only differing pixels in the frame are the Stats panel's own live counters and a content-browser icon |
| No phantom undo entries | Inspector open on those seven sections, ~2 s idle, no input: `UndoDepth` stays **0**. A drawer that reported `edited` every frame would push one command per frame, which is the failure mode worse than no undo |
| Every field has a drawer | No `no property drawer` warnings for any converted component |
| Builds | Debug and Release clean, 0 errors. Editor boots with `Reflection initialised: 32 types, 119 members` / `validation passed`; runtime boots with 0 errors, 0 warnings |

**Not done, deliberately.** A scripted drag to prove one-gesture-one-command end to end: driving
ImGui input programmatically is a test harness this editor does not have, and the property is
already guaranteed by the boundary being untouched. The idle-`UndoDepth` check covers the failure
mode a generic drawer could actually introduce.

**Left as adjacent work.** `Attr::Section` (the particle emitter's grouping) is unread by the
generic path, because the only component using it stays hand-written - it will matter the moment
anything else groups fields. The R1 note's suggestion to promote reflection to its own
`docs/engine/reflection.md` was **not** taken: the drawer registry is editor-side and belongs in
`editor.md`, the registration stays in `scene.md`, and neither is yet large enough on its own. Worth
revisiting when R3 puts the serializer contract in the same place.

---

## R3 — executed (2026-09-10)

Landed: `WriteReflected` / `ReadReflected` in
[`SceneYaml.h`](../../GanymedEngine/source/GanymedE/Scene/SceneYaml.h) plus a new `SceneYaml.cpp`
(one new file, premake regeneration required). Nine components converted on **both** save and load:
Sprite Renderer, Directional / Spot / Point Light, Audio Listener, Rigid Body, Box / Sphere /
Capsule Collider. Written up in [scene.md](../engine/scene.md#the-reflected-path).

**The gate was byte-identical output**, as the plan asked, and it is worth being precise about how
that was measured, because a naive round-trip would have passed while hiding a real difference.
The method: run the *same four committed scenes* through both writers and diff the two outputs
directly. Result:

| Scene | Hand-written vs reflected |
| --- | --- |
| `BoxesPhysicsExample` (rigid bodies + colliders) | **byte-identical** |
| `Phase5Test` (42 entities) | **byte-identical** |
| `3DExample`, `Example` | field-for-field identical; differ only in entity UUIDs |

Those last two needed a control before the result meant anything: **the unmodified binary produces
different bytes for them across two runs of itself**, because entities in those files mint fresh
UUIDs on load. Pre-existing non-determinism, unrelated to this change - and exactly the kind of
thing that would otherwise have been read as "the new serializer broke something". Every writer is
also a fixpoint: pass 2 is byte-identical to pass 1 in all four.

**Two deliberate differences from the code replaced.**

- **Reading is more tolerant, and had to be.** The hand-written loader did
  `c.Field = node["Field"].as<T>()` unguarded, so a missing key threw. The generic reader leaves the
  constructed value alone. This is not a nicety: `OmitIfDefault` means a field can legitimately be
  absent, and it has to read back as the default.
- **`OmitIfDefault` is not implemented, and asserts rather than being ignored.** It needs an
  equality comparison entt cannot supply without a registration nothing has made. No converted
  component carries the flag, so the generic writer refuses it loudly and those components stay
  hand-written. Half-implementing it would have silently dropped a field from a saved scene.

**The two consumers convert independently**, which R3 demonstrated rather than assumed:
`SpotLightComponent` is now serialized generically while its inspector section is still
hand-written. The cross-field clamp that blocks it from the panel says nothing about how it is
stored. "Reflected" is per-consumer, not a property of a component - worth knowing before R4.

**Enums persist by ordinal via one registration line each** (`RegisterReflectedEnumCodec<E>()`).
Getting from a `meta_any` holding an enum to its underlying integer needs the concrete type, and an
explicit line beside the component that uses it is cheaper and more readable than a conversion
registration nobody would find. `RigidBodyType` is the only one so far.

**Verification.** Debug, Release and Dist build clean. The byte comparison above. The scene
regression is unchanged - 40 meshes / 32 culled / 5 instanced draws / 7 draw calls across four hot
reload rounds, 48 reloads, 0 errors - and the runtime boots with 0 errors, 0 warnings, 10 entities,
which exercises the *read* path on a scene containing converted components.

**Left as adjacent work.** `PrefabSerializer` was not converted; it reads the same component blocks
out of a different container and is the obvious next user of `ReadReflectedComponent`. The
per-run UUID churn in `3DExample` and `Example` is a real diff-noise problem for committed scenes
and predates all of this.

---

## R4a - executed (2026-09-10): per-property prefab overrides

R4 as sketched bundles two independent features and says to "scope them properly when they are
next". Scoped, and split: **R4a is per-property prefab overrides** (this section), **R4b is
multi-entity editing** (not started). They share nothing but the reflection layer, and R4a needed a
design decision that R4b does not.

Landed: `PrefabMemberComponent`, [`EditorPrefabOverrides.h/.cpp`](../../GanymedEditor/source/EditorPrefabOverrides.h),
`EmitReflectedValue` split into [`ReflectedValue.h`](../../GanymedEngine/source/GanymedE/Scene/ReflectedValue.h),
and an override hook threaded through the property drawer. Three new files, premake regeneration
required. Written up in [editor.md](../editor/editor.md#per-property-overrides) and
[scene.md](../engine/scene.md#prefab-member-links).

**The design question the plan flagged - "a stable property path per field" - turned out to be the
wrong half of the problem.** The property path is trivial: R1 made the registered field name the
serialized key, so `meta_data::name()` *is* the path. What is hard is the **entity** path: an
instance's entities get fresh UUIDs, so nothing says which prefab object a given instance entity
came from. Investigating found the answer already half-built - `PrefabSerializer` writes prefab
entities with canonical ids 1..N in DFS order - so the fix was to stop throwing that pairing away.
`PrefabMemberComponent::CanonicalID`, recorded inside `Instantiate`, is the whole mechanism.

**Overrides are computed, not stored, and that is the load-bearing decision.** Unity records an
override list on the instance. A recorded list is a second source of truth: it goes stale when the
prefab changes, needs migrating when a field is renamed, and every edit path has to remember to
maintain it. A diff cannot be stale, and it needs no format change beyond the canonical link. The
cost is recomputation, which was measured rather than assumed - see below.

**The comparison reuses R3.** `EmitReflectedValue`: *a field that would serialize identically is not
an override.* That keeps "overridden" and "would be written differently" the same statement, where a
hand-written `operator==` per type would eventually stop being. A type with no YAML codec reports
*not* overridden - "cannot tell" must not become a claim. R3 paying off inside R4a was not planned;
it is a good sign the phases were ordered right.

**Queries are templates on the component type, not `meta_type`-keyed.** Going from a `meta_type` to
a component's storage needs raw `entt::registry` access that neither `Entity` nor `Scene` exposes,
and every call site knows the concrete type anyway. Adding a by-type-id accessor to the engine for
an editor feature was the alternative and was not worth it.

**Verification.** Debug, Release and Dist build clean.

| Check | Result |
| --- | --- |
| A fresh instance has no overrides | `RateOverTime overridden=false` |
| An edited field is detected | `overridden=true`, and the section marker agrees |
| **An untouched field in the same component is not** | `Looping overridden=false` - the precision check a coarse implementation fails |
| Revert restores the prefab value | `RateOverTime=0 overridden=false` |
| The link survives a save/load round trip | After `Serialize` + `OpenScene`: `canonicalID=1`, value 7 preserved, still reported overridden |
| Cost | **0.136 ms/frame** worst case - a selected prefab instance with a particle emitter (46 fields), section marker plus every per-field query, Release. Zero when nothing selected is a prefab member, so no caching was added |
| No regression | Scene render unchanged (40 meshes / 32 culled / 5 instanced / 7 draws, 48 reloads, 0 errors); runtime boots clean with `33 types, 120 members` |

**Limitations, stated rather than discovered later.**

- **Prefab instances already in committed scenes have no canonical link** - they were instantiated
  before it existed - so they report no overrides until re-instantiated. *Revert Instance* does
  that, since it rebuilds the subtree from the file. Backfilling by DFS order was considered and
  rejected: structural edits are allowed, so position in the tree is not identity.
- **The per-field affordance only appears in reflected sections**, because the hook lives in the
  property drawer. Hand-written sections get the section-level `*` and nothing finer. That boundary
  moves on its own as more sections convert.
- **The template cache cannot notice a `.gprefab` edited on disk**; it is dropped on scene change.
  Hooking `AssetWatcher` would fix it, but prefabs are path-resolved and have no asset manager, so
  `OnAssetModified` currently returns false for them.
- **Apply-to-prefab still writes the whole instance.** Per-property *apply* ("push just this field
  to the prefab") is the natural next step now that the diff exists, and is not done.

**R4b - multi-entity editing - is executed below.**

---

## R4b - executed (2026-09-10): multi-entity editing

Ctrl+click builds a selection; the inspector shows the components common to it, marks fields the
selection disagrees about, and applies an edit to all of them as one undo entry. Written up in
[editor.md](../editor/editor.md#multi-entity-editing).

**The selection model change cost six call sites, not thirty.** The plan estimated ~30 because it
assumed `Entity m_SelectionContext` becomes a set. It does not: `m_SelectionContext` stays as the
**primary** - the entity clicked last - and `m_Selection` is added beside it, primary first.
`GetSelectedEntity()` returns what it always did, so gizmos, the tag field and every prefab action
were untouched. Only the six `Get/SetSelectedEntity` uses in `EditorLayer` were even read.

**Edits propagate after the fact rather than driving N widgets**, which is what keeps every property
drawer single-entity and unaware multi-edit exists. The widgets drive the primary; on a frame a
widget reports an edit, the primary's new value is copied to the rest. Selecting several entities
therefore never flattens their differing values - only an actual edit does. This is the same shape
as R4a's override hook, and it is why the two features compose without knowing about each other:
`MultiEditHook` and `OverrideHook` are separate optional hooks on the same drawer, and mixed wins
over overridden when a field is both.

**The undo work is where the care went**, as the milestone's risk section predicted - though not for
the reason it gave. The commit boundary already collapsed a gesture into one command; what it could
not do was span entities. `PendingEdit` gained a `Secondary` vector, captured at the same instant as
the primary's command, and `CommitPendingEdit` folds them into one `CompositeCommand`. Pushing N
commands would have made Ctrl+Z walk back through the selection one entity at a time - the same
class of wrongness as one command per frame of a drag.

**Verification.** Debug, Release and Dist build clean. Three entities with intensities 1 / 2 / 1,
only one of which also has a box collider:

| Check | Result |
| --- | --- |
| Mixed detection | `Intensity mixed=true` (1/2/1), `Radius mixed=false` (all default) |
| Common components | `PointLight=true`, `BoxCollider=false` - the section only one entity has is not drawn |
| Propagation | `A=9 B=9 C=9`, and the field stops reporting mixed |
| **One gesture = one undo entry** | `UndoDepth == 1` after a 3-entity gesture, not 3 |
| **Undo restores per-entity values** | One Ctrl+Z gives back `1 / 2 / 1` - the individual prior values, not a flattened one. This is the check that would catch capturing the primary's before-value N times |
| No regression | Scene render unchanged (40 meshes / 32 culled / 5 instanced / 7 draws, 48 reloads, 0 errors); runtime clean |

**Left undone, deliberately.**

- **Shift-range selection.** It needs a flattened view of a tree the panel draws recursively and
  does not keep. Ctrl covers the case multi-edit exists for.
- **The gizmo still moves the primary only.** Transform composition across a selection is a
  viewport feature, not an inspector one.
- **The no-active-phase edit path stays single-entity for undo** (a drag-drop onto a section, a
  popup that closes in the same frame). It has no gesture to wait for, so the other entities'
  before-values are gone by the time it runs; fixing it means moving the per-frame pre-copy up
  into `DrawComponent`, which costs a copy per section per frame for a case that currently has no
  multi-entity consumer.
- **Hand-written sections get no mixed-value marking**, for the same reason they get no per-field
  override marking: the hook lives in the reflected property drawer. That boundary moves on its own
  as more sections convert.

---

## Milestone status

R1-R4 are done. What the milestone set out to make possible - per-property prefab overrides and
multi-entity editing - both exist, and both are what R1's registration was shaped for.

The two consumers are converted unevenly and on purpose: **15 of 20** inspector sections and 9 of 21
serialized components go through reflection, with each remaining one held back by something
specific rather than by effort. The reflected/hand-written boundary is where the per-field
affordances stop.

Still out of scope by decision: reflection-driven Lua bindings, reflection in engine systems
(decision 4), reflection across a DLL boundary, and a code generator.

---

## Follow-up - converting the remaining inspector sections (2026-09-10)

Converted **Transform** and **Spot Light**, taking the inspector to 10 of 20 sections. Two drawer
capabilities were added for them, both of which R1's registration had already anticipated:
`Trait::Radians` on a vec3 (Transform's rotation) and a bare `AssetHandle` drawer keyed on
`Attr::Slot` - the exact case the two-tier vocabulary was introduced for, since `AssetHandle` is an
alias for `UUID` and the type alone cannot say a field is an asset reference.

**The pattern that unlocked both: handle what reflection cannot express *around* the generic call,
not inside it.**

- Transform runs `MarkChanged<TransformComponent>` after `DrawReflectedComponent` returns true. A
  side effect is not expressible as an attribute, but nothing stops the section from running one.
- Spot Light clamps outer >= inner afterwards. A generic drawer sees one field at a time and cannot
  express a cross-field invariant, but the section can fix up what the drawer just wrote.

That distinction is what separates the sections that could convert from the ones that could not:
**a rule you can apply after the fact is fine; one that changes what gets drawn is not.**

**Verification, and a result better than "identical".** The screenshot comparison against the
hand-written version showed one small difference - a scrollbar thumb, meaning the content height had
changed. Rather than accept or hand-wave it, the hypothesis (the type-level note string) was tested
by temporarily aligning the registered text to the panel's and re-running: **pixel-identical**. So
the note string was the entire delta, proven rather than assumed.

The alignment was then reverted, leaving the registered prose in place. Two components now display
the registration's wording rather than the panel's older string (`DirectionalLightComponent`, which
changed silently back in R2 because that phase's probe did not include it, and now
`SpotLightComponent`). That is the registration being the single source of truth doing its job; the
shorter wording is one line away in `ComponentReflection.cpp` if it is preferred.

Also checked, because the degrees round-trip is where a generic drawer would quietly corrupt data:
after ~2 s of drawing with no input, Transform's rotation reads exactly 30 / 45 / -60 and
`UndoDepth` is 0. A drawer that wrote back unconditionally would drift and mint a phantom command
per frame.

**Where the boundary now is, and why it is structural.** Six sections stay hand-written:

| Section | Blocked by |
| --- | --- |
| Camera, Sky Light | Field **visibility** depends on another field's value. Unlike a clamp, this cannot be applied after the fact - you cannot un-draw a field |
| Static Mesh, Animator, Script | The UI is driven by **asset** data (a mesh's material slots, its clip names, a Lua class's fields), not by component members. There is nothing to reflect |
| Particle Emitter | Paired min/max fields whose clamp *direction* depends on which of the pair moved, which post-hoc fixup cannot reconstruct; plus Play/Stop/Restart, which are actions rather than fields |

**Audio Source and Prefab Instance were then converted too**, on an explicit decision that the layout
change is acceptable - the one call in this milestone that was the owner's to make rather than mine,
since every other conversion preserved the shipped UI exactly. Audio Source's checkboxes no longer
share lines and `Group` moved to the end, because field order is now registration order. Taking the
inspector to **12 of 20** sections.

That conversion surfaced a real correctness point rather than only a cosmetic one: `ReadOnly` on an
asset slot has to suppress the **drop target** explicitly. `BeginDisabled` blocks item clicks, but a
drag-drop payload is not an item click, so without the guard a field the registration marks
non-editable would silently accept one. Verified: both sections drawn for ~2 s leave `UndoDepth` at
0, and no field reports a missing drawer.

**The Particle Emitter then converted too, via the ranged field type**, taking the inspector to
**13 of 20**.

`RangeF` is a min/max pair authored as one thing. It exists for exactly one reason: a drawer seeing
`LifetimeMin` and `LifetimeMax` as unrelated floats can draw them but cannot **clamp** them, because
which half to push depends on which half the author moved. One drawer owning both halves knows.

**It is invisible on disk and in Lua, deliberately.** `RangeF` is layout-identical to the two floats
it replaced, and two things were kept byte-for-byte:

- `SceneSerializer` still writes `LifetimeMin` / `LifetimeMax`, from `Lifetime.Min` / `.Max`.
  Verified by instantiating the SparkBurst prefab and dumping the scene: the keys and values are
  unchanged. No migration was needed and no committed scene moved.
- The Lua bindings gained an overload taking a `RangeF` member pointer plus a `float RangeF::*`, so
  `GetParticleLifetimeMin` still exists and still reads the same value. A C++ refactor must not
  silently rewrite a scripting API that shipped - that surface was the single biggest risk in this
  change and it cost 8 call sites to protect.

Everything else the emitter needed already existed as attributes R1 had written and nothing had yet
consumed: the four `CollapsingHeader` groups are `Attr::Section` (grouping added to
`DrawReflectedProperties`), and the curve and gradient editors are drawers keyed on `FloatCurve` and
`ColorGradient`, reusing the house widgets that own `ActiveId` for a whole drag. Play / Stop /
Restart stay hand-written because they are **actions, not fields**.

**A generic consumer exposed two wrong registrations**, which is the recurring pattern of this
milestone: `Playing` and `Time` were `ReadOnly | NotSerialized`. That was right while the section
drew its own status line and nothing else; as generic fields they became two disabled rows repeating
that line. They are `Runtime` now. Nothing was wrong until something tried to consume the
registration - the same way `Speed` on a vec3 was inert until a drawer read it.

**Verification.** Debug, Release and Dist clean. The four sections draw in registration order with
the right first field and type (`Initial -> Lifetime (RangeF)`, `Over Lifetime -> SizeCurve
(FloatCurve)`, `Rendering -> RenderMode` enum); no field reports a missing drawer; values read back
correctly from the prefab (`lifetime=0.25..0.5 speed=2..4 size=0.04..0.08`); `UndoDepth` stays 0
while the section is merely drawn. Scene regression unchanged, runtime clean.

**Stated honestly: the emitter section was not captured visually.** It is taller than the Properties
panel, and the one attempt to scroll it programmatically was wrong (`ImGui::Begin` from `OnUpdate` is
outside the ImGui frame and took the app down). The evidence above is structural and behavioural
rather than pixel-level, unlike every other conversion in this milestone.

**Camera and Sky Light then converted too, via a field filter**, taking the inspector to **15 of 20**.

Their blocker was field *visibility* depending on another field's value. A clamp can be applied after
the generic drawer runs; visibility cannot, because you cannot un-draw a field - it has to be decided
first. `EditorUI::FieldFilter` is a predicate asked per field before anything is submitted.

**Deliberately a predicate in editor C++ rather than an attribute.** "Show this when that other field
equals X" is a small expression language, and the vocabulary is not the place for one - a predicate
costs nothing, expresses anything, and keeps UI conditionals out of the engine's registration where
R1 said they do not belong.

Camera also needed a **nested-struct fallback**: a reflected struct with no drawer of its own is now
drawn inline, so `SceneCamera`'s fields become rows of the Camera section exactly as the hand-written
version had them. Worth being precise about why this is not `Trait::Flatten`: the camera really *is*
a nested map on disk, so Flatten would have been a false statement that the first generic writer to
touch CameraComponent would have believed. Inline-in-the-inspector and siblings-in-the-file are
different claims, and only the second is a serialization trait.

**Another registration corrected by a generic consumer** - the fourth time this milestone.
SkyLightComponent's Environment carried the Tip "Using HDR IBL (procedural colors are fallback)",
which was true only while an environment was assigned, because the hand-written section drew that
line only then. A field-level Tip is static text and shows either way, so with no environment
assigned the panel now asserted something false. Reworded to hold in both states.

**Verification.** Debug, Release and Dist clean. Camera: perspective shows Vertical FOV 52.000 and
Near 0.050, orthographic shows Size 10.000 and Near -1.000, the projection combo comes from the enum
registration, and the FOV survives a switch to orthographic and back. Sky Light: colours present with
no environment, gone once one is assigned. Both leave `UndoDepth` at 0 while merely drawn, and no
field reports a missing drawer. Scene regression unchanged; runtime clean at `34 types, 117 members`.

**Remaining hand-written (5 of 20):** Static Mesh, Animator and Script - the UI is driven by asset or
Lua data (a mesh's material slots, its clip names, a Lua class's fields), not by component members,
so there is nothing to reflect - plus the Tag field and the prefab action buttons, which are not
component sections at all.

That is the honest end of this line of work: every section whose shape comes from component *data*
is now generic, and the five that are left are driven by something else entirely.


---

## R5 execution notes - `OmitIfDefault`, and the rest of the serializer

**Every component is now written and read generically.** Three *fields* stay hand-written, each
marked `Trait::Custom`: `RelationshipComponent`'s two ends (the halves of a link must agree),
`StaticMeshComponent::MaterialOverrides` (a flow sequence whose index is the meaning) and
`ScriptComponent::Fields` (a `{Name, Type, Value}` sequence over a closed variant). `TagComponent`
is written generically but read by hand, because the tag is needed to *create* the entity.

**The blocker was larger than the flag.** R3 stopped at nine components and named `OmitIfDefault` as
the reason. Implementing it was necessary and not sufficient - three more things were in the way:

1. **`Custom` conflated two questions.** `AnimatorComponent::Clip`, `SizeCurve` and
   `ColorOverLifetime` were flagged `Custom` because they need a bespoke *widget* - a combo over the
   mesh's clip names, a curve editor, a gradient editor. On disk they are a string and two
   sequences, needing nothing bespoke at all. One flag meaning "both consumers skip this" locked
   them out of the generic writer over a fact about the inspector, which contradicts this
   milestone's own "the two consumers convert independently" line. Split into `CustomDrawer` and
   `CustomWriter`, with `Custom = CustomDrawer | CustomWriter` kept as the composite so every
   genuinely-both registration is unchanged. Cost: one trait bit, eleven of sixteen now spent.
2. **`Flatten` could not express `LifetimeMin`.** It emitted the nested type's field names verbatim,
   which is right for a collider's `PhysicsMaterial` (`Friction`, `Restitution`) and collides five
   ways for `RangeF` (five `Min` keys). Added `Attr::KeyPrefix` - a valued attribute, per field.
   Deliberately not derived from the field name even though the prefix equals it for all five today:
   that coincidence as a rule would make an on-disk key a function of a C++ member name, which
   decision 3 forbids.
3. **No nested sub-maps.** `CameraComponent::Camera` is a `Camera:` block on disk. A reflected struct
   with no codec now writes as a nested map, which is the *serialization* counterpart of the
   inspector's nested-struct fallback and, unlike it, a real statement about the file.

**How `OmitIfDefault` compares.** Against a default-constructed instance of the owning type, passed
*in* to `WriteReflected` rather than built by it: recovering a default-constructed `T` from an
`entt::meta_type` would need `.ctor<>()` on every component, where `WriteReflectedComponent<T>`
already knows `T` and cannot forget one. Equality lives on the YAML codec, because entt has none to
synthesize - `meta_any::operator==` compares the *addresses* two anys point at unless a comparison
function was registered, so the naive version would have been false for every field every time and
made the flag a silent no-op. A field whose type registered no equality is never omitted: a
redundant key costs a diff line, an omitted differing one is data loss.

**One regression found and fixed in the inspector.** Marking the `RangeF` fields `Flatten` broke the
paired range widget, because `DrawProperty` tested `Flatten` *before* looking up a drawer, so
`Lifetime` started drawing as two loose rows. The two branches turned out to do the same thing, so
the fix removed the `Flatten` special case entirely and let the drawer lookup come first -
`PhysicsMaterial` has no drawer and still falls through to the inline path, `RangeF` has one and
keeps it. The inspector now reads no serialization trait at all, which is what it should always have
been doing.

**Verification.**

- **Byte-identity, measured rather than assumed.** All seven committed scenes and prefabs were
  loaded and re-saved by both writers and the outputs diffed. Five byte-identical; `3DExample` and
  `Example` match block-for-block once the pre-existing per-run UUID churn is accounted for, and a
  control run of the *unmodified* binary reproduces that churn against itself.
- **A finding worth recording: the committed fixtures are stale.** Both writers reformat them - the
  curve emitters changed style after those files were last saved. So `git diff` is not a valid
  identity test here, which is why the comparison is between two runs.
- **Round-trip fixed point** over one entity carrying every component at non-default values:
  save -> load -> save is byte-identical, covering the four components no fixture exercises
  (Animator, Point Light, Spot Light, sphere and capsule colliders), the by-name `AudioGroup`, the
  flattened `RangeF` and `PhysicsMaterial`, the nested camera map, all four `AssetRef<T>` slots, both
  curve types, and an empty `Clip` correctly omitted.
- **Inspector**, by screenshot: `Lifetime` still one paired row, a collider's `Friction` and
  `Restitution` still inline rows, Audio Source's Group combo now the second row. No rotation drift
  and `UndoDepth` 0 while merely drawing.
- Debug, Release and Dist clean.

**One layout change, pre-approved.** `AudioSourceComponent::Group` moved to second in the
registration, because registration order *is* on-disk key order and `Clip, Group, Volume, ...` is the
order every saved scene already has. The inspector draws it second now as well; the two orders were
free to differ only while the writer was hand-written.

**Adjacent, not done.** `PrefabSerializer::Save` copies `PrefabMemberComponent` into the prefab file
when the source subtree is itself part of an instance - it strips `PrefabInstanceComponent` for
exactly that reason and should strip this too. Pre-existing, found by the round-trip probe, and out
of scope here.
