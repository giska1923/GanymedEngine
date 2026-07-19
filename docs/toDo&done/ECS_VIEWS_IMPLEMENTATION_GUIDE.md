# GanymedEngine — ECS Views & Access Wrappers: Concrete Modification Plan

This document is a **file-by-file modification plan** for `D:\GanymedEngine`, showing exactly what
to add and change to bring the view/access-wrapper architecture (investigated in a production
engine's `engine/ecs` module) into your entt-based engine.

It was written after auditing your actual code:

- `GanymedEngine/source/GanymedE/Scene/Scene.h/.cpp` — `Scene` owns `entt::registry`; all game
  logic is hardcoded inline in `OnUpdateRuntime`/`OnUpdateEditor`.
- `GanymedEngine/source/GanymedE/Scene/Entity.h` — thin `entt::entity + Scene*` handle;
  `AddComponent`/`RemoveComponent` mutate the registry **immediately**.
- `GanymedEngine/source/GanymedE/Scene/Components.h` — plain structs, no registration, no flags.
- `GanymedEngine/source/GanymedE/Physics/PhysicsScene.cpp` — Jolt; iterates registry directly.
- `extern/entt` — **entt 3.16.0** (has `reactive_mixin`, `organizer`; `entt::observer` removed).
- Engine is **single-threaded** per scene update. Editor panels (`SceneHierarchyPanel`) are
  `friend`s of `Scene` and poke the registry directly.

---

## Table of contents

0. [The target design in one page](#0-the-target-design-in-one-page)
1. [Audit: current problems this refactor fixes](#1-audit)
2. [New module layout](#2-new-module-layout)
3. [Phase 0 — TypeList + component registration list](#3-phase-0)
4. [Phase 1 — Access wrappers + pack traits](#4-phase-1)
5. [Phase 2 — Change tracking (ChangeBuffer + accessors + `Modify()`)](#5-phase-2)
6. [Phase 3 — AccessView & IterView over entt](#6-phase-3)
7. [Phase 4 — ISystem + ViewHolder; decompose `Scene::OnUpdateRuntime`](#7-phase-4)
8. [Phase 5 — Deferred structural changes + graveyard](#8-phase-5)
9. [Phase 6 — Reactive views: InitView / FiniView / ChangeView](#9-phase-6)
10. [Phase 7 — Singletons over `registry.ctx()`](#10-phase-7)
11. [Phase 8 (optional) — ViewDesc + scheduling / `entt::organizer`](#11-phase-8)
12. [Worked example: fixing `GetWorldSpaceTransform` with a TransformSystem + ChangeView](#12-worked-example)
13. [Implementation order & checklist](#13-checklist)

---

## 0. The target design in one page

The reference engine's ECS is built on five rules:

1. **Views are throwaway stack objects.** `IterView<...> v = view<MyView>();` costs O(1).
2. **Persistent view data lives in a `State` struct owned by the system** (via a `ViewHolder` CRTP
   base). Expensive query data inside `State` is a `shared_ptr` deduplicated engine-wide.
3. **A view's template arguments declare *exactly* what it reads/writes/filters**, via wrappers:

   ```
   Has<T>    include, no access        No<T>     exclude, no access
   RO<T>     include, read-only        RW<T>     include, read-write
   OptRO<T>  optional, read-only       OptRW<T>  optional, read-write
   ReactHas / ReactRO / ReactRW / ReactOpt / ReactOptRO / ReactOptRW   (reactive views)
   bare T  == RO<T>;   ecs::Entity as pack element yields the entity id in the tuple
   ```

4. **Writes to change-tracked components are explicit**: views hand out an accessor proxy; reading
   is `operator*/->`, writing requires `.Modify()`, which logs the entity into that component
   type's change buffer exactly once. `ChangeView` replays those logs per-system ("what changed
   since *my* last read"). `InitView`/`FiniView` react to component add/remove one tick later, with
   removed components readable for one extra frame from a **graveyard**.
5. **Declared access doubles as scheduling metadata** (`ViewDesc`): systems whose write sets
   intersect other systems' read/write sets get ordering edges → automatic race-free parallelism.
   (Optional for you now — you're single-threaded — but the wrapper grammar makes it possible later.)

View family: `AccessView` (random access by entity), `IterView` (iterate matches),
`InitView`/`FiniView`/`ChangeView` (reactive), `SingletonAccessView`/`SingletonChangeView`,
`EntityChangedView`, plus type-erased dynamic views for editor code.

---

## 1. Audit

Concrete issues in GanymedEngine that the phases below fix directly:

| # | Where | Problem | Fixed in |
|---|---|---|---|
| A1 | `Scene.cpp:578` `FindEntityByUUID` | O(n) linear scan of *all* entities; called per parent hop, per entity, per frame by `GetWorldSpaceTransform` | Phase 0.3 (UUID map) |
| A2 | `Scene.cpp:589` `GetWorldSpaceTransform` | Recomputes full parent chain every call; render loops call it per entity → O(entities × depth × entities) per frame | Phase 12 (cached `WorldTransformComponent` + ChangeView) |
| A3 | `Scene.cpp:143-156` `Scene::Copy` | Hand-maintained list of 14 `CopyComponent<T>` calls — adding a component and forgetting one silently breaks Play mode | Phase 0.2 (`ComponentList` typelist) |
| A4 | `Scene.cpp:607-694` `OnComponentAdded<T>` | 16 empty explicit specializations that must be extended per component | Phase 0.2 |
| A5 | `Scene.cpp:272-386` `OnUpdateRuntime` | Physics, scripts, camera search, mesh submit, sprites, lights all inlined; no ownership, no ordering declaration, cannot ever be parallelized | Phase 4 |
| A6 | `Entity.h:17-51` | Structural changes are immediate; destroying/adding components mid-iteration is a latent crash (entt UB when done inside a view loop) | Phase 5 |
| A7 | everywhere | No way to know "what changed" — e.g. physics `SyncTransforms` writes every dynamic body's transform every frame whether it moved or not, renderer can't cache anything | Phases 2, 6 |
| A8 | `Components.h:137` `SkyLightComponent` | "First one wins" iteration to find scene-wide data (also `CameraComponent::Primary`) — these are singletons pretending to be components | Phase 7 |
| A9 | `Scene::OnRuntimeStop`, `InstantiateScripts` | init/teardown logic manually mirrored in multiple places — exactly what `InitView`/`FiniView` express declaratively | Phase 6 |

---

## 2. New module layout

Create a new folder **`GanymedEngine/source/GanymedE/ECS/`**:

```
GanymedE/ECS/
├── TypeList.h            // Phase 0 — minimal metaprogramming toolbox
├── ComponentTraits.h     // Phase 0 — per-component flags + ComponentList
├── AccessWrappers.h      // Phase 1 — Has/RO/RW/... + AccessorPackTraits
├── ChangeBuffer.h/.cpp   // Phase 2 — per-type change log with virtual indices
├── ComponentAccessor.h   // Phase 2 — read/write proxies with Modify()
├── Views.h               // Phase 3+6 — AccessView, IterView, Init/Fini/ChangeView
├── ViewHolder.h          // Phase 4 — per-system view-state storage
├── System.h              // Phase 4 — ISystem + SystemManager
├── CommandQueue.h/.cpp   // Phase 5 — deferred structural changes
├── Graveyard.h           // Phase 5 — one-frame storage for removed components
├── Singleton.h           // Phase 7 — ctx() wrapper + epochs + singleton views
└── ViewDesc.h            // Phase 8 — runtime access metadata (optional)
```

Add the folder to `premake5.lua` file globs (your `files { "source/**.h", "source/**.cpp" }`
pattern already picks it up if you use the standard Hazel premake layout — verify).

Everything below uses your conventions: `namespace GanymedE`, PascalCase methods, `m_` members,
`GE_CORE_ASSERT`, `Ref`/`Scope`.

---

## 3. Phase 0 — TypeList + component registration list

### 3.1 New file `ECS/TypeList.h`

You need ~120 lines of metaprogramming used by every later phase:

```cpp
#pragma once
#include <cstddef>
#include <type_traits>

namespace GanymedE {

	template<typename... Ts>
	struct TypeList
	{
		static constexpr size_t Size = sizeof...(Ts);
	};

	// Merge:   TypeListMerge<TypeList<A>, TypeList<B,C>>::Type == TypeList<A,B,C>
	template<typename... Ls> struct TypeListMerge;
	template<> struct TypeListMerge<> { using Type = TypeList<>; };
	template<typename... As> struct TypeListMerge<TypeList<As...>> { using Type = TypeList<As...>; };
	template<typename... As, typename... Bs, typename... Rest>
	struct TypeListMerge<TypeList<As...>, TypeList<Bs...>, Rest...>
		: TypeListMerge<TypeList<As..., Bs...>, Rest...> {};
	template<typename... Ls> using TypeListMergeT = typename TypeListMerge<Ls...>::Type;

	// IndexOf, Contains, Element<N>, Map, Filter — same pattern; implement:
	//   TypeListIndexOf<TL, T>::value          (static_assert if absent)
	//   TypeListContainsV<TL, T>
	//   TypeListElementT<TL, N>
	//   TypeListMapT<TL, template<typename> class F>
	//   TypeListFilterT<TL, template<typename> class Pred>
	//   ForEachType<TL>(lambda)  -> calls lambda(std::type_identity<T>{}) for each T
}
```

The runtime `ForEachType` helper is the workhorse for fixing A3/A4:

```cpp
	template<typename... Ts, typename F>
	void ForEachType(TypeList<Ts...>, F&& func)
	{
		(func(std::type_identity<Ts>{}), ...);
	}
```

### 3.2 New file `ECS/ComponentTraits.h` — the single source of component truth

This replaces the three hand-maintained lists in your codebase (`Scene::Copy`,
`OnComponentAdded` specializations, and later the editor's component menus):

```cpp
#pragma once
#include "TypeList.h"
#include "GanymedE/Scene/Components.h"

namespace GanymedE {

	// ---- opt-in flags, mirrors the reference engine's IComponent::Flags ----
	template<typename T>
	struct ComponentTraits
	{
		static constexpr bool TrackChanges = false;   // enables ChangeView on T
	};

	// specialize per tracked component (Phase 2+):
	// template<> struct ComponentTraits<TransformComponent> { static constexpr bool TrackChanges = true; };

	// ---- every user-facing component, in one place ----
	// NOTE: IDComponent and TagComponent are intentionally NOT here; they're identity,
	// created by CreateEntityWithUUID and never copied generically.
	using ComponentList = TypeList<
		TransformComponent,
		RelationshipComponent,
		SpriteRendererComponent,
		StaticMeshComponent,
		CameraComponent,
		DirectionalLightComponent,
		PointLightComponent,
		SpotLightComponent,
		SkyLightComponent,
		NativeScriptComponent,
		RigidBodyComponent,
		BoxColliderComponent,
		SphereColliderComponent,
		CapsuleColliderComponent
	>;
}
```

### 3.3 Modify `Scene/Scene.cpp` — use the list, add the UUID map

**(a) Replace the body of `Scene::Copy` (lines 143–156)** — the 14 `CopyComponent<...>` calls —
with:

```cpp
		ForEachType(ComponentList{}, [&](auto typeTag)
		{
			using T = typename decltype(typeTag)::type;
			CopyComponent<T>(dstRegistry, srcRegistry, enttMap);
		});
```

**(b) Delete all empty `OnComponentAdded<T>` specializations (lines 607–694)** and replace the
primary template + only the meaningful one:

```cpp
	template<typename T>
	void Scene::OnComponentAdded(Entity entity, T& component) { /* default: nothing */ }

	template<>
	void Scene::OnComponentAdded<CameraComponent>(Entity entity, CameraComponent& component)
	{
		if (m_ViewportWidth > 0 && m_ViewportHeight > 0)
			component.Camera.SetViewportSize(m_ViewportWidth, m_ViewportHeight);
	}

	// Explicitly instantiate for all known components so the default lives in the .cpp:
	template void Scene::OnComponentAdded<IDComponent>(Entity, IDComponent&);
	// ... or simply move the default template into Scene.h and drop this ceremony.
```

(Simplest correct option: move the primary template's empty body into `Scene.h` and keep only the
`CameraComponent` specialization in `Scene.cpp`.)

**(c) Fix A1 — add a UUID map.** In `Scene.h` add:

```cpp
	private:
		std::unordered_map<UUID, entt::entity> m_EntityMap;
```

Maintain it in `CreateEntityWithUUID` (after `AddComponent<IDComponent>`):

```cpp
		m_EntityMap[uuid] = (entt::entity)entity;
```

in `DestroyEntity` (before `m_Registry.destroy`):

```cpp
		m_EntityMap.erase(entityID);
```

and rewrite `FindEntityByUUID`:

```cpp
	Entity Scene::FindEntityByUUID(UUID uuid)
	{
		auto it = m_EntityMap.find(uuid);
		if (it != m_EntityMap.end())
			return Entity{ it->second, this };
		return {};
	}
```

Also populate the map inside `Scene::Copy`'s entity-creation loop (it already has the data) — and
in `SceneSerializer` deserialization if it creates entities with UUIDs (it goes through
`CreateEntityWithUUID`, so it's covered automatically).

> This one change alone turns `GetWorldSpaceTransform` from O(entities) per parent hop into O(1)
> per hop and is worth shipping before anything else.

---

## 4. Phase 1 — Access wrappers + pack traits

### 4.1 New file `ECS/AccessWrappers.h`

```cpp
#pragma once
#include "TypeList.h"

namespace GanymedE::ECS {

	// ---- the wrapper grammar (variadic: RW<A,B> == RW<A>, RW<B>) ----
	template<typename...> struct Has {};
	template<typename...> struct RO {};
	template<typename...> struct RW {};
	template<typename...> struct OptRO {};
	template<typename...> struct OptRW {};
	template<typename...> struct No {};
	template<typename...> struct ReactHas {};
	template<typename...> struct ReactRO {};
	template<typename...> struct ReactRW {};
	template<typename...> struct ReactOpt {};
	template<typename...> struct ReactOptRO {};
	template<typename...> struct ReactOptRW {};

	// Marker so views can yield the entity id in the tuple:
	// use GanymedE's Entity? No — views yield entt::entity; wrap into Entity{handle, scene}
	// only at the API surface (see Phase 3). The pack marker is:
	struct EntityId {};

	namespace Detail {

		enum class FilterLevel : uint8_t { Ignore, Include, Exclude };
		enum class AccessLevel : uint8_t { None, ReadOnly, ReadWrite };

		template<FilterLevel FL, AccessLevel AL, bool React, typename... Ts>
		struct ElementTraitsImpl
		{
			static_assert(((!std::is_const_v<Ts> && !std::is_volatile_v<Ts>) && ...),
				"Access wrappers take non-cv-qualified component types");
			static_assert(!(FL == FilterLevel::Exclude && AL != AccessLevel::None),
				"Cannot access excluded components");

			using AllTypes     = TypeList<Ts...>;
			template<typename TL, bool Cond>
			using If           = std::conditional_t<Cond, TL, TypeList<>>;

			using IncludeTypes = If<AllTypes, FL == FilterLevel::Include>;
			using ExcludeTypes = If<AllTypes, FL == FilterLevel::Exclude>;
			using ReadTypes    = If<AllTypes, AL != AccessLevel::None>;
			using WriteTypes   = If<AllTypes, AL == AccessLevel::ReadWrite>;
			using ReactTypes   = If<AllTypes, React>;

			// Slot<T>: what one result-tuple slot looks like (refined in Phase 2/3)
			static constexpr bool Optional  = FL == FilterLevel::Ignore;
			static constexpr bool Writeable = AL == AccessLevel::ReadWrite;
			using AccessedTypes = If<AllTypes, AL != AccessLevel::None>;
		};

		template<typename T>            struct ElementTraits
			: ElementTraitsImpl<FilterLevel::Include, AccessLevel::ReadOnly,  false, T> {};   // bare T == RO<T>
		template<typename... Ts> struct ElementTraits<Has<Ts...>>
			: ElementTraitsImpl<FilterLevel::Include, AccessLevel::None,      false, Ts...> {};
		template<typename... Ts> struct ElementTraits<RO<Ts...>>
			: ElementTraitsImpl<FilterLevel::Include, AccessLevel::ReadOnly,  false, Ts...> {};
		template<typename... Ts> struct ElementTraits<RW<Ts...>>
			: ElementTraitsImpl<FilterLevel::Include, AccessLevel::ReadWrite, false, Ts...> {};
		template<typename... Ts> struct ElementTraits<OptRO<Ts...>>
			: ElementTraitsImpl<FilterLevel::Ignore,  AccessLevel::ReadOnly,  false, Ts...> {};
		template<typename... Ts> struct ElementTraits<OptRW<Ts...>>
			: ElementTraitsImpl<FilterLevel::Ignore,  AccessLevel::ReadWrite, false, Ts...> {};
		template<typename... Ts> struct ElementTraits<No<Ts...>>
			: ElementTraitsImpl<FilterLevel::Exclude, AccessLevel::None,      false, Ts...> {};
		template<typename... Ts> struct ElementTraits<ReactHas<Ts...>>
			: ElementTraitsImpl<FilterLevel::Include, AccessLevel::None,      true,  Ts...> {};
		template<typename... Ts> struct ElementTraits<ReactRO<Ts...>>
			: ElementTraitsImpl<FilterLevel::Include, AccessLevel::ReadOnly,  true,  Ts...> {};
		template<typename... Ts> struct ElementTraits<ReactRW<Ts...>>
			: ElementTraitsImpl<FilterLevel::Include, AccessLevel::ReadWrite, true,  Ts...> {};
		template<typename... Ts> struct ElementTraits<ReactOpt<Ts...>>
			: ElementTraitsImpl<FilterLevel::Ignore,  AccessLevel::None,      true,  Ts...> {};
		template<typename... Ts> struct ElementTraits<ReactOptRO<Ts...>>
			: ElementTraitsImpl<FilterLevel::Ignore,  AccessLevel::ReadOnly,  true,  Ts...> {};
		template<typename... Ts> struct ElementTraits<ReactOptRW<Ts...>>
			: ElementTraitsImpl<FilterLevel::Ignore,  AccessLevel::ReadWrite, true,  Ts...> {};

		template<> struct ElementTraits<EntityId>
		{
			using AllTypes = TypeList<>; using IncludeTypes = TypeList<>;
			using ExcludeTypes = TypeList<>; using ReadTypes = TypeList<>;
			using WriteTypes = TypeList<>; using ReactTypes = TypeList<>;
			using AccessedTypes = TypeList<>; // slot handled specially
		};

		template<typename... Es>
		struct AccessorPackTraits
		{
			using AllTypes     = TypeListMergeT<typename ElementTraits<Es>::AllTypes...>;
			using IncludeTypes = TypeListMergeT<typename ElementTraits<Es>::IncludeTypes...>;
			using ExcludeTypes = TypeListMergeT<typename ElementTraits<Es>::ExcludeTypes...>;
			using ReadTypes    = TypeListMergeT<typename ElementTraits<Es>::ReadTypes...>;
			using WriteTypes   = TypeListMergeT<typename ElementTraits<Es>::WriteTypes...>;
			using ReactTypes   = TypeListMergeT<typename ElementTraits<Es>::ReactTypes...>;
			using PackElements = TypeList<Es...>;   // keep original order for tuple slots
		};
	}
}
```

**Deviation from the reference engine, on purpose:** it distinguishes component packs from
singleton packs via base-class checks (`IComponent`/`ISingleton`). Your components are plain
structs — don't add a base class. Instead, singleton views (Phase 7) are a *separate* small
template family, so no `AllComponents ^ AllSingletons` static_assert is needed.

---

## 5. Phase 2 — Change tracking

### 5.1 New file `ECS/ChangeBuffer.h/.cpp`

The reference engine's `ChangeList` keeps **two frames of history** with monotonically increasing
"virtual indices" so any number of consumers can each keep their own read cursor. Yours can be
drastically simpler because scene update is single-threaded (no per-thread contexts, no atomics):

```cpp
#pragma once
#include <entt/entt.hpp>
#include <vector>
#include <cstdint>

namespace GanymedE::ECS {

	class ChangeBuffer
	{
	public:
		using VirtualIndex = uint64_t;

		void Add(entt::entity e) { m_Current.push_back(e); }

		// Shift current -> previous. Call once per frame, BEFORE systems run.
		void NextFrame()
		{
			// CORRECTED (was `+= m_Previous.size()`, which is wrong whenever two consecutive
			// frames log different numbers of changes — it skews every virtual index from then on).
			// The invariant to preserve is:
			//     m_CurrFrameStart == m_PrevFrameStart + m_Previous.size()
			m_PrevFrameStart = m_CurrFrameStart;
			m_CurrFrameStart += m_Current.size();
			m_Previous.swap(m_Current);                // swap, not move, to recycle both buffers
			m_Current.clear();
		}

		// Append every change made after 'cursor' into out; returns new cursor.
		// History only covers [previous frame, now]; asserts if the cursor is older.
		VirtualIndex CollectSince(VirtualIndex cursor, std::vector<entt::entity>& out) const
		{
			GE_CORE_ASSERT(cursor >= m_PrevFrameStart || cursor == 0,
				"ChangeView skipped a frame - changes were lost. Read every ChangeView every frame.");
			const VirtualIndex end = m_CurrFrameStart + m_Current.size();
			for (VirtualIndex i = std::max(cursor, m_PrevFrameStart); i < end; ++i)
				out.push_back(At(i));
			return end;
		}

		VirtualIndex Head() const { return m_CurrFrameStart + m_Current.size(); }

	private:
		entt::entity At(VirtualIndex i) const
		{
			return i < m_CurrFrameStart
				? m_Previous[size_t(i - m_PrevFrameStart)]
				: m_Current[size_t(i - m_CurrFrameStart)];
		}

		std::vector<entt::entity> m_Previous, m_Current;
		VirtualIndex m_PrevFrameStart = 1, m_CurrFrameStart = 1;  // start at 1: cursor 0 == "never read"
	};
}
```

Notes:

- Entries may repeat (same entity modified twice) — ChangeView dedups (Phase 6).
- Start indices at 1 so `cursor == 0` unambiguously means "first read ever" (reference engine does
  exactly this and documents that 0 breaks the collect logic).
- Skip the `ChangeIndexer` (the per-archetype O(1) version table) entirely — it's an optimization
  for worlds with tens of thousands of tracked changes per frame; you don't need it and entt has
  no archetype list to hang it on.

### 5.2 Where ChangeBuffers live — modify `Scene.h`

```cpp
	#include "GanymedE/ECS/ChangeBuffer.h"
	// ...
	private:
		// one buffer per tracked component type, lazily created
		std::unordered_map<entt::id_type, ECS::ChangeBuffer> m_ChangeBuffers;

	public:
		template<typename T>
		ECS::ChangeBuffer& GetChangeBuffer()
		{
			static_assert(ComponentTraits<T>::TrackChanges, "Component is not change-tracked");
			return m_ChangeBuffers[entt::type_hash<T>::value()];
		}
```

**Feed creation into the buffer automatically** — component creation counts as a change (a
component removed and re-added in one frame must still appear in ChangeViews; the reference engine
is explicit about this). In `Scene::Scene()` (currently empty, `Scene.cpp:19`) connect entt
signals for every tracked type:

```cpp
	Scene::Scene()
	{
		ForEachType(ComponentList{}, [this](auto typeTag)
		{
			using T = typename decltype(typeTag)::type;
			if constexpr (ComponentTraits<T>::TrackChanges)
			{
				m_Registry.on_construct<T>().connect<&Scene::OnTrackedConstruct<T>>(*this);
			}
		});
	}

	template<typename T>
	void Scene::OnTrackedConstruct(entt::registry&, entt::entity e)
	{
		GetChangeBuffer<T>().Add(e);
	}
```

### 5.3 New file `ECS/ComponentAccessor.h`

The proxy that makes writes explicit. Three cases (mirroring the reference engine's
specializations, minus the "indexable" one you don't need):

```cpp
#pragma once
#include "ChangeBuffer.h"
#include "ComponentTraits.h"

namespace GanymedE::ECS {

	// Primary: read-only (any trackability)
	template<typename T, bool Optional, bool Writeable,
	         bool Trackable = ComponentTraits<T>::TrackChanges>
	class ComponentAccessor
	{
	public:
		using ComponentType = T;
		static constexpr bool IsOptional = Optional;

		ComponentAccessor() = default;
		ComponentAccessor(const T* comp, ChangeBuffer*, entt::entity) : m_Comp(comp) {}

		explicit operator bool() const { return m_Comp != nullptr; }
		const T& operator*()  const { return *m_Comp; }
		const T* operator->() const { return m_Comp; }
		const T* Get()        const { return m_Comp; }

	private:
		const T* m_Comp = nullptr;
	};

	// Writeable, NOT tracked: same API (Modify() exists for uniform call sites), zero overhead
	template<typename T, bool Optional>
	class ComponentAccessor<T, Optional, true, false>
	{
	public:
		using ComponentType = T;
		ComponentAccessor() = default;
		ComponentAccessor(T* comp, ChangeBuffer* buf, entt::entity) : m_Comp(comp)
		{
			GE_CORE_ASSERT(!buf, "Non-tracked component got a change buffer");
		}
		explicit operator bool() const { return m_Comp != nullptr; }
		T& operator*()  const { return *m_Comp; }
		T* operator->() const { return m_Comp; }
		T& Modify()           { return *m_Comp; }
	private:
		T* m_Comp = nullptr;
	};

	// Writeable AND tracked: reads are const; writing requires Modify(), which logs once
	template<typename T, bool Optional>
	class ComponentAccessor<T, Optional, true, true>
	{
	public:
		using ComponentType = T;
		ComponentAccessor() = default;
		ComponentAccessor(T* comp, ChangeBuffer* buf, entt::entity e)
			: m_Comp(comp), m_Buffer(buf), m_Entity(e) {}

		explicit operator bool() const { return m_Comp != nullptr; }
		const T& operator*()  const { return *m_Comp; }
		const T* operator->() const { return m_Comp; }

		T& Modify()
		{
			GE_CORE_ASSERT(m_Comp, "Modify() on missing optional component");
			if (m_Buffer)
			{
				m_Buffer->Add(m_Entity);
				m_Buffer = nullptr;   // log at most once per accessor lifetime
			}
			return *m_Comp;
		}

	private:
		T* m_Comp = nullptr;
		ChangeBuffer* m_Buffer = nullptr;  // null e.g. for graveyard components
		entt::entity m_Entity = entt::null;
	};
}
```

**The invariant that makes everything downstream correct**: a *tracked* + *writeable* slot is
never collapsed to `T&` — the user must go through `Modify()`. Enforce it in the tuple builder
(next phase). Do **not** route writes through `registry.patch<T>()` — you'd pay signal dispatch
per write and can't do the "log once" optimization; the accessor writes the buffer directly.

---

## 6. Phase 3 — AccessView & IterView over entt

### 6.1 Design decision: no archetype index

The reference engine maintains an archetype table so filter matching is per-archetype and
`AccessView::Has()` is a bitmask lookup. **Skip all of it.** entt's sparse sets already give you:

- `IterView` iteration → `registry.view<Includes...>(entt::exclude<Excludes...>)`
- `AccessView::Has()` → `registry.all_of<Includes...>(e) && !registry.any_of<Excludes...>(e)`
- optional components → `registry.try_get<T>(e)`

What entt does *not* give you — and what these classes add — is: RO/RW/Opt declaration, tuple
slots as accessors with `Modify()`, `EntityId` slots, subset access, and (later) reactive views
with identical ergonomics.

### 6.2 New file `ECS/Views.h` — the tuple builder core

Everything hinges on one function that turns a pack element + entity into a tuple slot:

```cpp
#pragma once
#include "AccessWrappers.h"
#include "ComponentAccessor.h"
#include <entt/entt.hpp>
#include <tuple>

namespace GanymedE::ECS::Detail {

	// Slot type selection (the reference engine's "RefConverter" rules):
	//   EntityId                          -> entt::entity
	//   include + RO                      -> const T&
	//   include + RW + untracked         -> T&
	//   include + RW + tracked           -> ComponentAccessor<T,false,true>   (must call Modify())
	//   optional + RO                     -> ComponentAccessor<T,true,false>  (nullable)
	//   optional + RW                     -> ComponentAccessor<T,true,true>
	template<typename Element> struct SlotBuilder;   // one specialization per wrapper kind

	// Example — the include+RW case:
	template<typename T> struct SlotFor
	{
		static constexpr bool Tracked = ComponentTraits<T>::TrackChanges;
		using Type = std::conditional_t<Tracked, ComponentAccessor<T, false, true>, T&>;
	};

	template<typename T>
	auto BuildSlotRW(Scene& scene, entt::registry& reg, entt::entity e)
	{
		T& comp = reg.get<T>(e);
		if constexpr (ComponentTraits<T>::TrackChanges)
			return ComponentAccessor<T, false, true>{ &comp, &scene.GetChangeBuffer<T>(), e };
		else
			return std::ref(comp);   // unwrapped by tuple construction
	}

	template<typename T>
	auto BuildSlotOptRO(Scene&, entt::registry& reg, entt::entity e)
	{
		return ComponentAccessor<T, true, false>{ reg.try_get<T>(e), nullptr, e };
	}
	// ... BuildSlotRO (const T&), BuildSlotOptRW, EntityId (returns e) analogous.

	// BuildTuple<PackElements...>(scene, reg, e) -> std::tuple<Slot...> in pack order,
	// flattening variadic wrappers (RW<A,B> contributes two slots).
}
```

Implementation tip: write `SlotBuilder<RW<Ts...>>::Append(tupleSoFar, scene, reg, e)` returning a
grown tuple via `std::tuple_cat`, fold over pack elements. ~100 lines total.

### 6.3 `IterView` and `AccessView`

```cpp
namespace GanymedE::ECS {

	template<typename... Es>
	class IterView
	{
		using Traits = Detail::AccessorPackTraits<Es...>;
		// EnttView = decltype(reg.view<Includes...>(entt::exclude<Excludes...>))
		// derive via a helper that unpacks Traits::IncludeTypes / ExcludeTypes

	public:
		struct State { /* nothing needed yet; exists for API symmetry + Phase 6 */ };

		IterView(Scene& scene, State&)
			: m_Scene(&scene)
			, m_View(MakeEnttView(scene.Reg(), typename Traits::IncludeTypes{},
			                                    typename Traits::ExcludeTypes{}))
		{}

		class Iterator
		{
			// wraps the entt view iterator; operator* returns
			// Detail::BuildTuple<Es...>(*m_Scene, reg, *m_It)
		};

		Iterator begin() const;  Iterator end() const;
		bool Empty() const;

		// Narrow tuple to fewer components; same entity set (cheap ergonomics helper):
		template<typename... Us> auto Subset() const;

		// Pre-call Modify() on listed tracked components; their slots become T&:
		template<typename... Us> auto Modify() const;

	private:
		Scene* m_Scene;
		/* entt view */
	};

	template<typename... Es>
	class AccessView
	{
		using Traits = Detail::AccessorPackTraits<Es...>;
	public:
		struct State {};
		AccessView(Scene& scene, State&) : m_Scene(&scene) {}

		bool Has(Entity e) const;                 // all_of<Includes> && !any_of<Excludes>
		auto Find(Entity e) const;                // full tuple, all-null if !Has(e)
		auto Get(Entity e) const;                 // full tuple, GE_CORE_ASSERT(Has(e))
		template<typename U> auto FindOne(Entity e) const;
		template<typename U> decltype(auto) GetOne(Entity e) const;
		template<typename... Us> auto FindSome(Entity e) const;
		template<typename... Us> auto GetSome(Entity e) const;

	private:
		Scene* m_Scene;
	};
}
```

Semantics to copy exactly from the reference engine:

- `Find` returns a tuple of **nullable accessors** (all slots default-constructed/null when the
  entity doesn't match) — never references. `Get` returns the ref-collapsed tuple and asserts.
- `FindSome/GetSome/GetOne` statically assert the requested types are actually in the pack
  (`TypeListContainsV`), so misuse is a compile error, not a wrong answer.
- Iteration usage is structured bindings: `for (auto [e, transform, mesh] : view<MeshView>())`.

**Important entt caveat to encode now**: adding/removing components *of iterated types* during
iteration is UB in entt. Until Phase 5 lands, document that view loops must not call
`Entity::AddComponent/RemoveComponent/DestroyEntity`; after Phase 5, route those through the
CommandQueue instead.

---

## 7. Phase 4 — ISystem + ViewHolder; decompose `Scene::OnUpdateRuntime`

### 7.1 New file `ECS/ViewHolder.h`

```cpp
#pragma once
#include "TypeList.h"

namespace GanymedE::ECS {

	template<typename Implementation>
	class ViewHolder
	{
		template<typename TL> struct StateTuple;
		template<typename... Vs> struct StateTuple<TypeList<Vs...>>
		{
			std::tuple<typename Vs::State...> States;
		};

	public:
		explicit ViewHolder(Scene& scene) : m_Scene(scene) {}

		template<typename V>
		V View()
		{
			using Views = typename Implementation::Views;
			constexpr size_t I = TypeListIndexOf<Views, V>::value;
			return V{ m_Scene, std::get<I>(m_States.States) };
		}

	protected:
		Scene& m_Scene;
		StateTuple<typename Implementation::Views> m_States;
	};
}
```

(The reference engine type-erases the state tuple behind an interface pointer to keep headers
light and constructs states with an `EntityManager&`; with your header layout a direct member
tuple is fine. When Phase 6 states need the Scene to register queries, add an
`InitStates(Scene&)` step invoked from the system constructor.)

### 7.2 New file `ECS/System.h`

Skip the reference engine's RTTR-driven registration, nested subsystem lists, and taskflow graphs
— you have no reflection system and no job system. Start minimal and ordered-by-registration:

```cpp
#pragma once
#include "ViewHolder.h"
#include "GanymedE/Core/Timestep.h"

namespace GanymedE::ECS {

	class ISystem
	{
	public:
		explicit ISystem(Scene& scene) : m_Scene(scene) {}
		virtual ~ISystem() = default;

		virtual void OnRuntimeStart() {}
		virtual void OnRuntimeStop()  {}
		virtual void OnUpdate(Timestep ts) = 0;
		virtual void OnUpdateEditor(Timestep ts) {}   // systems that also run in edit mode

	protected:
		Scene& m_Scene;
	};

	// CRTP convenience: using Views = TypeList<...>; then View<MyView>() works.
	template<typename Impl>
	class System : public ISystem, public ViewHolder<Impl>
	{
	public:
		explicit System(Scene& scene) : ISystem(scene), ViewHolder<Impl>(scene) {}
	};

	class SystemManager
	{
	public:
		template<typename S, typename... Args>
		S& Add(Scene& scene, Args&&... args)
		{
			auto sys = CreateScope<S>(scene, std::forward<Args>(args)...);
			S& ref = *sys;
			m_Systems.push_back(std::move(sys));
			return ref;
		}

		void OnRuntimeStart() { for (auto& s : m_Systems) s->OnRuntimeStart(); }
		void OnRuntimeStop()  { for (auto& s : m_Systems) s->OnRuntimeStop(); }
		void OnUpdate(Timestep ts) { for (auto& s : m_Systems) s->OnUpdate(ts); }
		void OnUpdateEditor(Timestep ts) { for (auto& s : m_Systems) s->OnUpdateEditor(ts); }

	private:
		std::vector<Scope<ISystem>> m_Systems;   // execution order == registration order
	};
}
```

### 7.3 Modify `Scene.h/.cpp` — host the SystemManager, gut `OnUpdateRuntime`

`Scene.h` additions:

```cpp
	#include "GanymedE/ECS/System.h"
	// ...
	private:
		ECS::SystemManager m_Systems;
	public:
		ECS::SystemManager& Systems() { return m_Systems; }
```

In the `Scene` constructor register the built-in systems **in the order the current inline code
runs them** (this preserves behavior exactly):

```cpp
	Scene::Scene()
	{
		/* signal hookup from Phase 2 */
		m_Systems.Add<PhysicsSystem>(*this);        // fixed step + collision events + sync
		m_Systems.Add<NativeScriptSystem>(*this);   // instantiate + OnUpdate
		m_Systems.Add<TransformSystem>(*this);      // Phase 12 (world transform cache)
		m_Systems.Add<RenderSystem>(*this);         // camera find + lights/sky + meshes + sprites
	}
```

New system files (suggested: `GanymedE/Scene/Systems/` so they can keep including
`Components.h` naturally):

**`Systems/NativeScriptSystem.h/.cpp`** — move `Scene::InstantiateScripts` +
the script-update block (`Scene.cpp:296-309`) + the destroy loop from `OnRuntimeStop`
(`Scene.cpp:47-58`) into `OnRuntimeStart/OnUpdate/OnRuntimeStop`:

```cpp
	class NativeScriptSystem : public ECS::System<NativeScriptSystem>
	{
	public:
		using ScriptView = ECS::IterView<ECS::EntityId, ECS::RW<NativeScriptComponent>>;
		using Views = TypeList<ScriptView>;
		using System::System;

		void OnUpdate(Timestep ts) override
		{
			for (auto [e, nsc] : View<ScriptView>())      // NSC untracked -> plain reference
			{
				if (!nsc.Instance && nsc.InstantiateScript)
				{
					nsc.Instance = nsc.InstantiateScript();
					nsc.Instance->m_Entity = Entity{ e, &m_Scene };
					nsc.Instance->OnCreate();
				}
				if (nsc.Instance)
					nsc.Instance->OnUpdate(ts);
			}
		}
		// OnRuntimeStop(): the OnDestroy/DestroyScript loop
	};
```

**`Systems/PhysicsSystem.h/.cpp`** — move the fixed-timestep block (`Scene.cpp:275-293`),
`DispatchCollisionEvents` (`Scene.cpp:74-104`), and ownership of `m_PhysicsScene`,
`m_PhysicsAccumulator`, `s_FixedTimestep` out of `Scene`. `PhysicsScene::Step`'s kinematic-body
loop (`PhysicsScene.cpp:502+`) keeps using the registry directly for now; converting `PhysicsScene`
internals is optional later.

**`Systems/RenderSystem.h/.cpp`** — move `SubmitLightsAndSky`, `DrawColliderGizmos`, the camera
search, the mesh loop, and the sprite loop. Its view declarations become live documentation of
what rendering reads:

```cpp
	using CameraView = ECS::IterView<ECS::EntityId, ECS::RO<TransformComponent>, ECS::RO<CameraComponent>>;
	using MeshView   = ECS::IterView<ECS::EntityId, ECS::RO<TransformComponent>, ECS::RO<StaticMeshComponent>>;
	using SpriteView = ECS::IterView<ECS::EntityId, ECS::RO<TransformComponent>, ECS::RO<SpriteRendererComponent>>;
	using DirLightView   = ECS::IterView<ECS::EntityId, ECS::RO<TransformComponent>, ECS::RO<DirectionalLightComponent>>;
	using PointLightView = ECS::IterView<ECS::EntityId, ECS::RO<TransformComponent>, ECS::RO<PointLightComponent>>;
	using SpotLightView  = ECS::IterView<ECS::EntityId, ECS::RO<TransformComponent>, ECS::RO<SpotLightComponent>>;
	using SkyView        = ECS::IterView<ECS::RO<SkyLightComponent>>;
	using Views = TypeList<CameraView, MeshView, SpriteView, DirLightView, PointLightView, SpotLightView, SkyView>;
```

`Scene::OnUpdateRuntime` shrinks to:

```cpp
	void Scene::OnUpdateRuntime(Timestep ts, EditorCamera* fallbackCamera)
	{
		m_FrameBegin();                 // Phase 5/6: queues, graveyard, buffers, epochs
		m_Systems.OnUpdate(ts);
		// fallbackCamera plumbing: pass via a member the RenderSystem reads,
		// or (cleaner) a RenderContext singleton — Phase 7.
	}
```

`OnUpdateEditor` similarly delegates to `m_Systems.OnUpdateEditor(ts)` for the editor-visible
subset (RenderSystem, gizmos).

---

## 8. Phase 5 — Deferred structural changes + graveyard

### 8.1 New file `ECS/CommandQueue.h/.cpp`

```cpp
#pragma once
#include "GanymedE/Scene/Entity.h"
#include <functional>

namespace GanymedE::ECS {

	class CommandQueue
	{
	public:
		// Store type-erased ops; simplest robust encoding is std::function over Scene&.
		// (The reference engine uses typed op lists to batch + order by kind; start simple.)
		template<typename T, typename... Args>
		void AddComponent(Entity e, Args&&... args)
		{
			m_AddOps.push_back([e, ...args = std::forward<Args>(args)](Scene& s) mutable {
				if (s.Reg().valid((entt::entity)e) && !s.Reg().all_of<T>((entt::entity)e))
					Entity{ e }.AddComponent<T>(std::move(args)...);
			});
		}

		template<typename T>
		void RemoveComponent(Entity e) { m_RemoveOps.push_back(...); }
		Entity CreateEntity(const std::string& name);          // pending-entity handle pattern
		void   DestroyEntity(Entity e) { m_DestroyOps.push_back(...); }

	private:
		friend class ::GanymedE::Scene;
		std::vector<std::function<void(Scene&)>> m_RemoveOps, m_AddOps, m_CreateOps, m_DestroyOps;
	};
}
```

### 8.2 Modify `Scene` — apply queues at a fixed point with fixed ordering

Add to `Scene`:

```cpp
	public:
		ECS::CommandQueue& Commands() { return m_Commands; }   // systems mutate through this
	private:
		ECS::CommandQueue m_Commands;
		void FlushCommands();   // called at the TOP of OnUpdateRuntime, before systems
```

`FlushCommands` must apply ops in the reference engine's guaranteed order — it exists to make
same-frame remove+re-add and "new entity needs component added same frame" work:

1. renames / **component removals** on existing entities  (removed instances → graveyard)
2. **component additions** on existing entities
3. **entity creations**, then components on new entities
4. **entity destructions**

Consequence (design decision to accept consciously): structural changes made by systems become
visible **next frame**. Editor code (`SceneHierarchyPanel`, serializer) keeps using the immediate
`Entity::AddComponent` — it runs outside the update loop, which is safe. You do **not** need to
change `Entity.h` — instead add a comment on `AddComponent/RemoveComponent`: *"Immediate; illegal
during system update — use scene.Commands() there."* Optionally enforce with
`GE_CORE_ASSERT(!m_Scene->IsUpdating())` and a bool the update loop sets.

### 8.3 New file `ECS/Graveyard.h` + hookup

FiniView (Phase 6) must read components that were *just removed*. entt destroys the instance
during `remove/destroy`, but its `on_destroy` signal fires **before** removal — so a copy can be
saved automatically:

```cpp
namespace GanymedE::ECS {

	template<typename T>
	class Graveyard
	{
	public:
		void Bury(entt::entity e, const T& comp) { m_Dead[e] = comp; }
		const T* Find(entt::entity e) const
		{
			auto it = m_Dead.find(e);
			return it != m_Dead.end() ? &it->second : nullptr;
		}
		void Clear() { m_Dead.clear(); }   // once per frame, in FrameBegin
	private:
		std::unordered_map<entt::entity, T> m_Dead;
	};
}
```

In `Scene::Scene()`, alongside the Phase-2 hookup, for every component that any FiniView will
react to (gate it with a `ComponentTraits<T>::EnableFini` flag to avoid copying everything):

```cpp
	m_Registry.on_destroy<T>().connect<&Scene::OnTrackedDestroy<T>>(*this);

	template<typename T>
	void Scene::OnTrackedDestroy(entt::registry& reg, entt::entity e)
	{
		GetGraveyard<T>().Bury(e, reg.get<T>(e));     // still alive inside on_destroy
		GetFiniBuffer<T>().push_back(e);              // Phase 6
	}
```

The one-frame lifetime rule: `FrameBegin` clears all graveyards **before** flushing command
queues, so anything removed during last frame's update (or by the editor between frames) stays
readable for exactly one update.

---

## 9. Phase 6 — Reactive views

### 9.1 Per-type Init/Fini buffers + frame epochs — modify `Scene`

```cpp
	private:
		struct ReactiveState
		{
			std::vector<entt::entity> InitThisFrame;   // filled by on_construct during flush
			std::vector<entt::entity> FiniThisFrame;   // filled by on_destroy
		};
		std::unordered_map<entt::id_type, ReactiveState> m_Reactive;
		uint32_t m_FrameEpoch = 1;    // ++ in FrameBegin; MUST start at 1 ("0 == never read")
```

`Scene::FrameBegin()` (the new preamble of `OnUpdateRuntime` / editor update), in this exact order:

```cpp
	void Scene::FrameBegin()
	{
		m_FrameEpoch++;
		for (auto& [id, buf] : m_ChangeBuffers) buf.NextFrame();
		for (auto& [id, st] : m_Reactive) { st.InitThisFrame.clear(); st.FiniThisFrame.clear(); }
		ClearGraveyards();
		FlushCommands();      // fills Init/Fini buffers + graveyards via signals
	}
```

Connect `on_construct<T>` to also append into `InitThisFrame` for reactive types (in addition to
the ChangeBuffer add from Phase 2).

### 9.2 `InitView` / `FiniView` in `ECS/Views.h`

Both follow the **epoch protocol** — implement it once as a helper; it's the part that's easy to
get subtly wrong:

```cpp
	// Returns the entity list this view instance should iterate, or nullptr for "empty".
	// Epoch state machine (copy exactly — each branch is load-bearing):
	//   state.Epoch == 0                 -> FIRST EVER READ:
	//        InitView: synthesize "everything currently matching is new" (fill from live query)
	//        FiniView: nothing (nothing removed yet)
	//   state.Epoch + 1 == frameEpoch    -> first read this frame: use this frame's buffer
	//   state.Epoch == frameEpoch        -> second+ read this frame: EMPTY (already reacted)
	//   state.Epoch <  frameEpoch - 1    -> GE_CORE_ASSERT: view skipped a frame, events lost
	// Afterwards: state.Epoch = frameEpoch.
```

```cpp
	template<typename... Es>
	class InitView
	{
		using Traits = Detail::AccessorPackTraits<Es...>;
		static_assert(Traits::ReactTypes::Size > 0, "InitView needs at least one React<...> element");

	public:
		struct State
		{
			uint32_t Epoch = 0;
			std::vector<entt::entity> Scratch;   // first-read + filtered result reuse
		};

		InitView(Scene& scene, State& state);
		// ctor: select list via epoch protocol; then filter it:
		//   keep e if (all Includes present) && (no Excludes present)   <- *this* frame's state
		// iterate with the same BuildTuple as IterView

		Iterator begin() const;  Iterator end() const;
	};
```

`FiniView` differs in exactly two ways (mirror of the reference engine):

- entity list comes from `FiniThisFrame` buffers;
- tuple slots for the **react types** are built from the `Graveyard<T>` (`SearchGraveyard`
  behavior): try graveyard first, fall back to live pool (covers remove+re-add in one frame,
  where both instances exist and Init/Fini views legitimately see *different* objects). Graveyard
  accessors get a null change buffer — modifying a corpse is not a change.

Filtering caveat for FiniView: the include/exclude filter must be evaluated against the entity's
*previous-frame* composition. Cheap practical approximation used successfully: treat graveyard
presence as "had it". For exact semantics you'd snapshot component masks — skip that until a real
bug demands it.

### 9.3 `ChangeView`

```cpp
	template<typename... Es>
	class ChangeView
	{
		using Traits = Detail::AccessorPackTraits<Es...>;
		static_assert(Traits::ReactTypes::Size > 0, "ChangeView needs at least one React<...> element");
		// static_assert all ReactTypes have ComponentTraits<T>::TrackChanges

	public:
		struct State
		{
			// one cursor per react type, in ReactTypes order; 0 == never read
			std::array<ChangeBuffer::VirtualIndex, Traits::ReactTypes::Size> Cursors{};
			std::vector<entt::entity> Buffer;
		};

		ChangeView(Scene& scene, State& state)
		{
			state.Buffer.clear();
			if (state.Cursors[0] == 0)
			{
				// FIRST READ: everything matching the filter counts as changed
				//   (fill Buffer from registry view over IncludeTypes/ExcludeTypes)
				// then just record cursors:
				ForEachReact([&](auto tag, size_t i) {
					state.Cursors[i] = scene.GetChangeBuffer<TypeOf(tag)>().Head(); });
			}
			else
			{
				ForEachReact([&](auto tag, size_t i) {
					state.Cursors[i] = scene.GetChangeBuffer<TypeOf(tag)>()
						.CollectSince(state.Cursors[i], state.Buffer); });
				std::sort(state.Buffer.begin(), state.Buffer.end());
				state.Buffer.erase(std::unique(state.Buffer.begin(), state.Buffer.end()),
				                   state.Buffer.end());
				// drop entities that died or no longer match include/exclude:
				std::erase_if(state.Buffer, [&](entt::entity e) { return !Matches(scene, e); });
			}
			m_Entities = &state.Buffer;
		}
		// iterate m_Entities with BuildTuple
	};
```

Semantics preserved from the reference engine: **read-time based** (changes since *this system's*
last instantiation, not since last frame), duplicates removed, first read returns the world,
`Modify()` during iteration is picked up by the *next* instantiation.

### 9.4 `EntityChangedView` (optional, cheap)

A single scene-wide `std::vector<entt::entity> m_StructurallyChanged` filled during
`FlushCommands` (created / component added / component removed), plus the same epoch protocol.
Add when you have a consumer (e.g. incremental physics-body creation instead of rebuilding
everything in `PhysicsScene::Start`).

---

## 10. Phase 7 — Singletons over `registry.ctx()`

### 10.1 New file `ECS/Singleton.h`

entt already stores per-registry data (`registry.ctx()`); what's missing is presence-checking
views and change epochs:

```cpp
#pragma once
#include <entt/entt.hpp>

namespace GanymedE::ECS {

	template<typename T>
	struct SingletonTraits { static constexpr bool TrackChanges = false; };

	class SingletonEpochs   // one instance lives in ctx() itself
	{
	public:
		template<typename T> size_t& Get()
		{
			return m_Epochs[entt::type_hash<T>::value()];   // 0 = absent/untracked, starts at 1
		}
	private:
		std::unordered_map<entt::id_type, size_t> m_Epochs;
	};

	template<typename T, bool Writeable>
	class SingletonAccessor
	{
	public:
		SingletonAccessor(T* s, size_t* epoch) : m_Singleton(s), m_Epoch(epoch) {}
		explicit operator bool() const { return m_Singleton != nullptr; }
		const T& operator*()  const { return *m_Singleton; }
		const T* operator->() const { return m_Singleton; }

		T& Modify()   // only enabled when Writeable
		{
			static_assert(Writeable);
			if (m_Epoch) { ++*m_Epoch; m_Epoch = nullptr; }
			return *m_Singleton;
		}
	private:
		T* m_Singleton = nullptr;
		size_t* m_Epoch = nullptr;
	};

	template<typename T>              // T or RW<T>; RO by default like component views
	class SingletonAccessView { /* Has() / Get() / Find() over scene.Reg().ctx() */ };

	template<typename T>
	class SingletonChangeView
	{
	public:
		struct State { size_t LastEpoch = 0; };
		SingletonChangeView(Scene& scene, State& state)
		{
			size_t current = /* epochs.Get<T>() */;
			m_Accessible = (current != state.LastEpoch);
			state.LastEpoch = current;
		}
		bool Has() const;   // presence AND changed
		// Get()/Find() as in SingletonAccessView, gated by m_Accessible
	};
}
```

### 10.2 What in your engine becomes a singleton (fixes A8)

- **`RenderContext`** — new: `{ Camera* MainCamera; glm::mat4 CameraTransform; EditorCamera* Fallback; }`.
  A small `CameraSystem` (runs before `RenderSystem`) does the primary-camera search
  (`Scene.cpp:311-326`) once and writes the singleton; `RenderSystem` and future systems read it.
  This also removes the `fallbackCamera` parameter threading through `OnUpdateRuntime`.
- **`PhysicsSettings`** — your `PhysicsDebugDrawSettings` member + `s_FixedTimestep` move out of
  `Scene` into `ctx()`; editor UI writes it via `SingletonAccessView<RW<PhysicsSettings>>`.
- **Sky/environment**: keep `SkyLightComponent` (it's authored per-entity in scenes), but have a
  small system resolve "first one wins" once into an `EnvironmentState` singleton instead of
  re-deciding inside `SubmitLightsAndSky` every frame.

---

## 11. Phase 8 (optional) — ViewDesc + scheduling

Only worth it when you (a) add threads or (b) have enough systems that ordering bugs appear.
Two options, cheapest first:

1. **`entt::organizer`** (already in your entt 3.16): register each system's update as a vertex;
   organizer derives RO/RW sets from parameter types (`const T&` = read, `T&` = write) and emits a
   dependency graph. You'd expose each system as a free function taking the components it touches.
   Least code, but it bypasses your wrapper grammar.
2. **Port `ViewDesc`**: each view gets `static ViewDesc Desc()` returning
   `{ includeMask, excludeMask, readMask, writeMask, reactMask }` as bitsets keyed by a dense
   component index (add `ComponentIndexOf<T>` = index in `ComponentList`). `SystemManager` unions
   each system's view masks, and at `Add<>` time asserts/reports conflicts + computes a safe
   order: systems conflict iff `A.write ∩ (B.read ∪ B.write) ≠ ∅` (and symmetrically). This keeps
   your wrappers as the single source of truth and matches the reference engine's
   `SystemGraphConstructor` auto-edge rule. Manual `RunBefore/RunAfter` metadata can be two
   `std::vector<std::type_index>` on the system, no reflection needed.

---

## 12. Worked example

**Goal**: kill audit items A1+A2 — the per-entity recursive `GetWorldSpaceTransform` — using
exactly the machinery from Phases 0–6. This is the flagship payoff and the best validation test.

**Step 1** — mark local transforms tracked (`ECS/ComponentTraits.h`):

```cpp
	template<> struct ComponentTraits<TransformComponent>    { static constexpr bool TrackChanges = true; };
	template<> struct ComponentTraits<RelationshipComponent> { static constexpr bool TrackChanges = true; };
```

**Step 2** — add the cache component (`Components.h`, and to `ComponentList`):

```cpp
	struct WorldTransformComponent
	{
		glm::mat4 World{ 1.0f };
	};
```

Add it in `CreateEntityWithUUID` next to `TransformComponent`.

**Step 3** — new `Systems/TransformSystem.h/.cpp`, registered after `PhysicsSystem`, before
`RenderSystem`:

```cpp
	class TransformSystem : public ECS::System<TransformSystem>
	{
	public:
		using DirtyView = ECS::ChangeView<ECS::EntityId,
			ECS::ReactRO<TransformComponent>,
			ECS::ReactRO<RelationshipComponent>,
			ECS::RW<WorldTransformComponent>>;
		using Views = TypeList<DirtyView>;
		using System::System;

		void OnUpdate(Timestep) override
		{
			for (auto [e, world] : View<DirtyView>().Modify<WorldTransformComponent>())
			{
				// recompute this entity's world matrix, then push dirtiness to children
				world.World = ComputeWorld(e);          // parent chain via O(1) FindEntityByUUID
				MarkChildrenDirty(e);                    // children re-enter next ChangeView read;
			}                                            // or recurse here for same-frame correctness
		}
	};
```

(Subtree propagation choice: recursing immediately inside the loop is simplest and gives
same-frame correctness; marking children via their own `Modify()` and letting the ChangeView
converge is what the reference engine's hierarchy does with an explicit dependency ordering. Start
with recursion + a visited set.)

**Step 4** — replace every render-path call
`GetWorldSpaceTransform(Entity{entity, this})` (`Scene.cpp:341, 366, 382, 404, 419, 437, 450,
467, 494, 506, 518`) with reading the cached component in the corresponding view:
add `ECS::RO<WorldTransformComponent>` to `MeshView`/`SpriteView`/light views and use
`worldTransform.World`. Keep `Scene::GetWorldSpaceTransform` itself for editor/tooling
(gizmos, `SceneHierarchyPanel`) — now O(depth) thanks to the UUID map.

**Step 5** — `PhysicsScene::SyncTransforms` writes back interpolated poses: convert it to write
through the tracked path (get the `ChangeBuffer<TransformComponent>` via
`scene->GetChangeBuffer<TransformComponent>()` and `Add(e)` when a pose actually changed, or route
through an `AccessView` + `Modify()`). Result: sleeping bodies stop dirtying the transform cache —
measurable win with many static/inactive bodies.

**Definition of done**: an idle scene (nothing moving) recomputes **zero** world matrices per
frame; dragging one entity in the editor recomputes exactly its subtree.

---

## 13. Checklist

Ship each step independently; every one compiles and improves the engine on its own:

- [x] **0.1** `ECS/TypeList.h`
- [x] **0.2** `ECS/ComponentTraits.h` + `ComponentList`; rewrite `Scene::Copy`, collapse
      `OnComponentAdded` specializations
- [x] **0.3** `m_EntityMap` (UUID → entity); O(1) `FindEntityByUUID` — *immediate perf win*
- [x] **1** `ECS/AccessWrappers.h` (pure compile-time; unit-test the trait lists with static_asserts)
- [x] **2** `ECS/ChangeBuffer.h`, `ECS/ComponentAccessor.h`, scene signal hookup, `FrameBegin` skeleton
- [x] **3** `ECS/Views.h`: tuple builder, `IterView`, `AccessView` (+ Subset/Modify)
- [x] **4** `ECS/ViewHolder.h`, `ECS/System.h`; extract `NativeScriptSystem`, `PhysicsSystem`,
      `RenderSystem`; `OnUpdateRuntime` becomes FrameBegin + `m_Systems.OnUpdate(ts)`
- [x] **5** `ECS/CommandQueue.h` + ordered `FlushCommands`; `ECS/Graveyard.h` + `on_destroy` hookup
- [x] **6** `InitView`/`FiniView` (epoch protocol), `ChangeView` (cursors); port script
      instantiation to an `InitView<ReactRW<NativeScriptComponent>>`, script destruction to a
      `FiniView` (fixes A9)
- [x] **7** `ECS/Singleton.h`; `RenderContext` + `CameraSystem`; `PhysicsSettings` into ctx()
- [x] **12** `TransformSystem` + `WorldTransformComponent` (fixes A2) — the validation milestone
- [x] **8** ViewDesc masks (option 2). Done ahead of threading: with five systems the ordering
      dependencies stopped being obvious, so `SystemManager::ValidateOrdering()` now derives them
      from the declared access and checks the registration order against them. The auto-*reorder*
      half is still deliberately not implemented — ordering stays explicit.

**Rules to never break while porting** (each one guards a class of bugs the reference engine
learned the hard way):

1. Tracked + writeable slots never collapse to `T&` — `Modify()` is the only write path.
2. `Modify()` logs at most once per accessor; graveyard accessors log never.
3. Component creation is a change (on_construct feeds ChangeBuffers).
4. Reactive views must be read every frame — keep the epoch/cursor asserts as `GE_CORE_ASSERT`.
5. First-ever read of a Change/Init view = "the whole matching world"; of a Fini view = nothing.
6. Graveyards and change history live exactly one frame; both die in `FrameBegin`, *before* the
   command flush that refills them.
7. Structural changes during system update go through `CommandQueue` only; editor/tooling code
   outside the update loop may keep the immediate `Entity` API.
