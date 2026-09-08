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
