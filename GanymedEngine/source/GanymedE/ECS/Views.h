#pragma once

#include <entt/entt.hpp>
#include <algorithm>
#include <array>
#include <tuple>
#include <type_traits>
#include <vector>

#include "AccessWrappers.h"
#include "ComponentAccessor.h"
#include "ViewDesc.h"
#include "ComponentTraits.h"
#include "GanymedE/Scene/Entity.h"
#include "GanymedE/Scene/Scene.h"

// Views over entt. Deliberately NOT an archetype index — entt's sparse sets already provide the
// matching and iteration. What these add on top is the declared-access grammar: RO/RW/Opt slots,
// Modify()-gated writes on tracked components, EntityId slots, and (Phase 6) reactive variants
// with identical ergonomics.
//
// Views are throwaway stack objects; constructing one is O(1).
//
// IMPORTANT (until Phase 5 lands): adding or removing components of the *iterated* types during
// iteration is undefined behaviour in entt. Do not call Entity::AddComponent / RemoveComponent /
// Scene::DestroyEntity inside a view loop; after Phase 5 these go through the CommandQueue.

namespace GanymedE::ECS {

	namespace Detail {

		// ---- Slot rules -------------------------------------------------------------------
		//   EntityId                        -> entt::entity
		//   include + RO                    -> const T&
		//   include + RW + untracked        -> T&
		//   include + RW + tracked          -> ComponentAccessor<T, false, true>  (Modify() to write)
		//   optional + RO                   -> ComponentAccessor<T, true, false>  (nullable)
		//   optional + RW                   -> ComponentAccessor<T, true, true>
		// Slot packs are normalized first, so every element here carries exactly one component.

		// Marker for a slot whose Modify() has already been called by IterView::Modify<>().
		template<typename T> struct PreModified {};

		// PreModified accesses T exactly as RW<T> does; giving it real traits keeps it usable in
		// the generic slot machinery (SlotElementFor, MakeOptionalSlot, ...) instead of falling
		// through to the bare-type primary template, which would treat PreModified<T> itself as
		// the component.
		template<typename T>
		struct ElementTraits<PreModified<T>>
			: ElementTraitsImpl<FilterLevel::Include, AccessLevel::ReadWrite, false, T> {};

		template<typename E>
		struct SlotBuilder
		{
			using Component = TypeListElementT<typename ElementTraits<E>::AccessedTypes, 0>;

			static constexpr bool Optional  = ElementTraits<E>::Optional;
			static constexpr bool Writeable = ElementTraits<E>::Writeable;
			static constexpr bool Tracked   = ComponentTraits<Component>::TrackChanges;

			using Type = std::conditional_t<Optional,
				ComponentAccessor<Component, true, Writeable>,
				std::conditional_t<Writeable,
					std::conditional_t<Tracked, ComponentAccessor<Component, false, true>, Component&>,
					const Component&>>;

			static Type Build(Scene& scene, entt::registry& registry, entt::entity entity)
			{
				if constexpr (Optional)
				{
					Component* component = registry.try_get<Component>(entity);
					if constexpr (Writeable && Tracked)
						return Type{ component, component ? &scene.GetChangeBuffer<Component>() : nullptr, entity };
					else
						return Type{ component, nullptr, entity };
				}
				else if constexpr (Writeable)
				{
					Component& component = registry.get<Component>(entity);
					if constexpr (Tracked)
						return Type{ &component, &scene.GetChangeBuffer<Component>(), entity };
					else
						return component;
				}
				else
				{
					return registry.get<Component>(entity);
				}
			}

			// Only meaningful for nullable slots; AccessView::Find normalizes every slot to
			// optional before asking for one, so reference slots never reach here.
			static Type Null()
			{
				static_assert(Optional, "Only optional slots have a null form");
				return Type{};
			}
		};

		template<>
		struct SlotBuilder<EntityId>
		{
			using Type = entt::entity;
			static Type Build(Scene&, entt::registry&, entt::entity entity) { return entity; }
			static Type Null() { return entt::null; }
		};

		template<typename T>
		struct SlotBuilder<PreModified<T>>
		{
			using Type = T&;
			static Type Build(Scene& scene, entt::registry& registry, entt::entity entity)
			{
				T& component = registry.get<T>(entity);
				if constexpr (ComponentTraits<T>::TrackChanges)
					scene.GetChangeBuffer<T>().Add(entity);
				return component;
			}
		};

		// ---- Tuple assembly ---------------------------------------------------------------

		template<typename SlotPack> struct TupleBuilder;
		template<typename... Ss>
		struct TupleBuilder<TypeList<Ss...>>
		{
			using Type = std::tuple<typename SlotBuilder<Ss>::Type...>;

			static Type Build(Scene& scene, entt::registry& registry, entt::entity entity)
			{
				return Type(SlotBuilder<Ss>::Build(scene, registry, entity)...);
			}

			static Type Null() { return Type(SlotBuilder<Ss>::Null()...); }
		};

		// ---- Slot pack transforms ---------------------------------------------------------

		// Force every slot nullable — the shape AccessView::Find returns, so a non-matching
		// entity yields null accessors rather than dangling references.
		template<typename E> struct MakeOptionalSlot
		{
			using Component = TypeListElementT<typename ElementTraits<E>::AccessedTypes, 0>;
			using Type = std::conditional_t<ElementTraits<E>::Writeable, OptRW<Component>, OptRO<Component>>;
		};
		template<> struct MakeOptionalSlot<EntityId> { using Type = EntityId; };
		template<typename T> struct MakeOptionalSlot<PreModified<T>> { using Type = OptRW<T>; };

		template<typename SlotPack> struct MakeOptionalPack;
		template<typename... Ss>
		struct MakeOptionalPack<TypeList<Ss...>>
		{
			using Type = TypeList<typename MakeOptionalSlot<Ss>::Type...>;
		};

		// Replace the slots for Us... with PreModified<U>, so IterView::Modify<Us...>() hands out
		// plain T& and logs the change once per iterated entity.
		// (These mappings live at namespace scope: a full explicit specialization is ill-formed at
		// class scope, even though MSVC accepts it as an extension.)
		template<typename E, typename... Us>
		struct PreModifySlot
		{
			using Component = TypeListElementT<typename ElementTraits<E>::AccessedTypes, 0>;
			static constexpr bool Listed = (std::is_same_v<Component, Us> || ...);
			using Type = std::conditional_t<Listed, PreModified<Component>, E>;
		};
		template<typename... Us> struct PreModifySlot<EntityId, Us...> { using Type = EntityId; };
		template<typename T, typename... Us>
		struct PreModifySlot<PreModified<T>, Us...> { using Type = PreModified<T>; };

		template<typename SlotPack, typename... Us> struct MakeModifiedPack;
		template<typename... Ss, typename... Us>
		struct MakeModifiedPack<TypeList<Ss...>, Us...>
		{
			using Type = TypeList<typename PreModifySlot<Ss, Us...>::Type...>;
		};

		// Keep only the slots for Us..., preserving each one's declared access. An EntityId slot is
		// kept when the original pack declared it — "which entity" is almost always still wanted.
		template<typename E, typename... Us>
		struct KeepSlot
		{
			using Component = TypeListElementT<typename ElementTraits<E>::AccessedTypes, 0>;
			static constexpr bool Keep = (std::is_same_v<Component, Us> || ...);
			using Type = std::conditional_t<Keep, TypeList<E>, TypeList<>>;
		};
		template<typename... Us> struct KeepSlot<EntityId, Us...> { using Type = TypeList<EntityId>; };
		template<typename T, typename... Us>
		struct KeepSlot<PreModified<T>, Us...>
		{
			static constexpr bool Keep = (std::is_same_v<T, Us> || ...);
			using Type = std::conditional_t<Keep, TypeList<PreModified<T>>, TypeList<>>;
		};

		template<typename SlotPack, typename... Us> struct MakeSubsetPack;
		template<typename... Ss, typename... Us>
		struct MakeSubsetPack<TypeList<Ss...>, Us...>
		{
			using Type = TypeListMergeT<typename KeepSlot<Ss, Us...>::Type...>;
		};

		// ---- entt plumbing ----------------------------------------------------------------

		template<typename... Is, typename... Xs>
		auto MakeEnttView(entt::registry& registry, TypeList<Is...>, TypeList<Xs...>)
		{
			return registry.view<Is...>(entt::exclude_t<Xs...>{});
		}

		template<typename IncludeList, typename ExcludeList>
		using EnttViewFor = decltype(MakeEnttView(std::declval<entt::registry&>(),
			IncludeList{}, ExcludeList{}));

		template<typename... Is, typename... Xs>
		bool MatchesFilter(const entt::registry& registry, entt::entity entity, TypeList<Is...>, TypeList<Xs...>)
		{
			if (!registry.valid(entity))
				return false;
			if constexpr (sizeof...(Is) > 0)
			{
				if (!registry.all_of<Is...>(entity))
					return false;
			}
			if constexpr (sizeof...(Xs) > 0)
			{
				if (registry.any_of<Xs...>(entity))
					return false;
			}
			return true;
		}

		template<typename... Xs>
		bool MatchesExcludes(const entt::registry& registry, entt::entity entity, TypeList<Xs...>)
		{
			if constexpr (sizeof...(Xs) > 0)
				return !registry.any_of<Xs...>(entity);
			else
				return true;
		}

		// Locate the (normalized) slot element that provides component U.
		template<typename U, typename... Ss> struct FindSlotElement;
		template<typename U> struct FindSlotElement<U>
		{
			static_assert(sizeof(U) == 0, "Component is not part of this view's declared access");
		};
		template<bool Found, typename U, typename S, typename... Rest> struct FindSlotElementImpl;
		template<typename U, typename S, typename... Rest>
		struct FindSlotElementImpl<true, U, S, Rest...> { using Type = S; };
		template<typename U, typename S, typename... Rest>
		struct FindSlotElementImpl<false, U, S, Rest...> : FindSlotElement<U, Rest...> {};
		template<typename U, typename S, typename... Rest>
		struct FindSlotElement<U, S, Rest...>
			: FindSlotElementImpl<TypeListContainsV<typename ElementTraits<S>::AccessedTypes, U>, U, S, Rest...> {};

		template<typename SlotPack, typename U> struct SlotElementFor;
		template<typename... Ss, typename U>
		struct SlotElementFor<TypeList<Ss...>, U> : FindSlotElement<U, Ss...> {};

		// ---- The iterator, kept self-contained so it never points back at its range ---------

		template<typename EnttIterator, typename SlotPack>
		class TupleIterator
		{
		public:
			TupleIterator(Scene* scene, EnttIterator iterator)
				: m_Scene(scene), m_Iterator(iterator) {}

			auto operator*() const
			{
				return TupleBuilder<SlotPack>::Build(*m_Scene, m_Scene->Reg(), *m_Iterator);
			}

			TupleIterator& operator++() { ++m_Iterator; return *this; }

			bool operator==(const TupleIterator& other) const { return m_Iterator == other.m_Iterator; }
			bool operator!=(const TupleIterator& other) const { return m_Iterator != other.m_Iterator; }

		private:
			Scene* m_Scene = nullptr;
			EnttIterator m_Iterator;
		};

		// ---- Graveyard-backed slots (FiniView) --------------------------------------------
		// A Fini event means the component is already gone from the registry, so its slot reads
		// the copy taken in on_destroy instead.

		template<typename E> struct GraveyardSlot {};

		template<typename E>
		struct ElementTraits<GraveyardSlot<E>> : ElementTraits<E> {};

		template<typename E>
		struct SlotBuilder<GraveyardSlot<E>>
		{
			using Component = TypeListElementT<typename ElementTraits<E>::AccessedTypes, 0>;
			static constexpr bool Writeable = ElementTraits<E>::Writeable;

			using Type = std::conditional_t<Writeable, Component&, const Component&>;

			static Type Build(Scene& scene, entt::registry& registry, entt::entity entity)
			{
				// Graveyard first, then the live pool: a component removed and re-added within one
				// update has both a corpse and a new instance, and the Fini event refers to the
				// corpse. Note the accessor never gets a change buffer — modifying a corpse is not
				// a change anyone should react to.
				if (Component* buried = scene.GetGraveyard<Component>().Find(entity))
					return *buried;

				Component* live = registry.valid(entity) ? registry.try_get<Component>(entity) : nullptr;
				GE_CORE_ASSERT(live,
					"FiniView slot has neither a buried nor a live component - is EnableFini set on it?");
				return *live;
			}
		};

		// Rewrites the react-type slots of a pack to read from the graveyard.
		template<typename E, typename ReactList> struct FiniSlot;
		template<typename E, typename... Rs>
		struct FiniSlot<E, TypeList<Rs...>>
		{
			using Component = TypeListElementT<typename ElementTraits<E>::AccessedTypes, 0>;
			static constexpr bool IsReact = (std::is_same_v<Component, Rs> || ...);
			using Type = std::conditional_t<IsReact, GraveyardSlot<E>, E>;
		};
		template<typename... Rs> struct FiniSlot<EntityId, TypeList<Rs...>> { using Type = EntityId; };

		template<typename SlotPack, typename ReactList> struct MakeFiniPack;
		template<typename... Ss, typename ReactList>
		struct MakeFiniPack<TypeList<Ss...>, ReactList>
		{
			using Type = TypeList<typename FiniSlot<Ss, ReactList>::Type...>;
		};

		// ---- Trait guards for the reactive views -------------------------------------------

		template<typename TL> struct AllTrackChanges;
		template<typename... Ts> struct AllTrackChanges<TypeList<Ts...>>
			: std::bool_constant<(ComponentTraits<Ts>::TrackChanges && ...)> {};

		template<typename TL> struct AllEnableInit;
		template<typename... Ts> struct AllEnableInit<TypeList<Ts...>>
			: std::bool_constant<(ComponentTraits<Ts>::EnableInit && ...)> {};

		template<typename TL> struct AllEnableFini;
		template<typename... Ts> struct AllEnableFini<TypeList<Ts...>>
			: std::bool_constant<(ComponentTraits<Ts>::EnableFini && ...)> {};

		// ---- Gathering reactive events -----------------------------------------------------

		inline void AppendAll(std::vector<entt::entity>& out, const std::vector<entt::entity>& in)
		{
			out.insert(out.end(), in.begin(), in.end());
		}

		template<typename ReactList> struct ReactiveGather;
		template<typename... Rs>
		struct ReactiveGather<TypeList<Rs...>>
		{
			static void Init(const Scene& scene, std::vector<entt::entity>& out)
			{
				(AppendAll(out, scene.GetInitBuffer<Rs>()), ...);
			}

			static void Fini(const Scene& scene, std::vector<entt::entity>& out)
			{
				(AppendAll(out, scene.GetFiniBuffer<Rs>()), ...);
			}

			template<size_t N>
			static void RecordHeads(Scene& scene, std::array<ChangeBuffer::VirtualIndex, N>& cursors)
			{
				size_t index = 0;
				((cursors[index++] = scene.GetChangeBuffer<Rs>().Head()), ...);
			}

			template<size_t N>
			static void CollectSince(Scene& scene, std::array<ChangeBuffer::VirtualIndex, N>& cursors,
				std::vector<entt::entity>& out)
			{
				size_t index = 0;
				((cursors[index] = scene.GetChangeBuffer<Rs>().CollectSince(cursors[index], out),
				  index++), ...);
			}
		};

		inline void SortAndUnique(std::vector<entt::entity>& entities)
		{
			std::sort(entities.begin(), entities.end());
			entities.erase(std::unique(entities.begin(), entities.end()), entities.end());
		}

		// A range over an entt view that builds tuples from an arbitrary slot pack. IterView is
		// one of these; Subset<> and Modify<> return others sharing the same entt view.
		template<typename EnttView, typename SlotPack>
		class SlotRange
		{
		public:
			using Iterator = TupleIterator<typename EnttView::iterator, SlotPack>;

			SlotRange(Scene& scene, const EnttView& view)
				: m_Scene(&scene), m_View(view) {}

			Iterator begin() const { return Iterator{ m_Scene, m_View.begin() }; }
			Iterator end()   const { return Iterator{ m_Scene, m_View.end() }; }
			bool Empty()     const { return m_View.begin() == m_View.end(); }

		protected:
			Scene* m_Scene = nullptr;
			EnttView m_View;
		};

		// A range over an explicit entity list — what the reactive views iterate, since their
		// contents come from event buffers rather than from a live entt query.
		template<typename SlotPack>
		class EntityListRange
		{
		public:
			using Iterator = TupleIterator<std::vector<entt::entity>::const_iterator, SlotPack>;

			EntityListRange(Scene& scene, const std::vector<entt::entity>& entities)
				: m_Scene(&scene), m_Entities(&entities) {}

			Iterator begin() const { return Iterator{ m_Scene, m_Entities->begin() }; }
			Iterator end()   const { return Iterator{ m_Scene, m_Entities->end() }; }
			bool Empty()     const { return m_Entities->empty(); }
			size_t Size()    const { return m_Entities->size(); }

		protected:
			Scene* m_Scene = nullptr;
			const std::vector<entt::entity>* m_Entities = nullptr;
		};
	}

	// ---- IterView ------------------------------------------------------------------------
	// Iterates every entity matching the declared filter, yielding one tuple per entity.
	// Usage: for (auto [entity, transform, mesh] : View<MeshView>()) { ... }

	template<typename... Es>
	class IterView : public Detail::SlotRange<
		Detail::EnttViewFor<typename Detail::AccessorPackTraits<Es...>::IncludeTypes,
		                    typename Detail::AccessorPackTraits<Es...>::ExcludeTypes>,
		Detail::NormalizePackT<TypeList<Es...>>>
	{
		using Traits = Detail::AccessorPackTraits<Es...>;
		using EnttView = Detail::EnttViewFor<typename Traits::IncludeTypes, typename Traits::ExcludeTypes>;
		using SlotPack = Detail::NormalizePackT<TypeList<Es...>>;
		using Base = Detail::SlotRange<EnttView, SlotPack>;

		static_assert(Traits::IncludeTypes::Size > 0,
			"IterView needs at least one included component to iterate - a view of only optional "
			"and excluded elements would have to walk every entity in the scene.");

	public:
		// Persistent per-system storage. Empty for IterView; exists for API symmetry with the
		// reactive views (Phase 6), whose State holds cursors and scratch buffers.
		struct State {};

		// Access metadata for ordering/scheduling (Phase 8), derived from this pack's wrappers.
		static ViewDesc Desc() { return Detail::MakeViewDesc<Traits>(); }

		IterView(Scene& scene, State&)
			: Base(scene, Detail::MakeEnttView(scene.Reg(),
				typename Traits::IncludeTypes{}, typename Traits::ExcludeTypes{})) {}

		explicit IterView(Scene& scene)
			: Base(scene, Detail::MakeEnttView(scene.Reg(),
				typename Traits::IncludeTypes{}, typename Traits::ExcludeTypes{})) {}

		// Same entity set, tuple narrowed to the listed components (plus EntityId if declared).
		template<typename... Us>
		auto Subset() const
		{
			static_assert((TypeListContainsV<typename Traits::ReadTypes, Us> && ...),
				"Subset<> requested a component this view does not access");
			using NewPack = typename Detail::MakeSubsetPack<SlotPack, Us...>::Type;
			return Detail::SlotRange<EnttView, NewPack>{ *this->m_Scene, this->m_View };
		}

		// Pre-call Modify() on the listed components: their slots become plain T& and the change
		// is logged once per iterated entity. Use when the loop writes unconditionally; skip it
		// when only some iterations write, or the change log fills with false positives.
		template<typename... Us>
		auto Modify() const
		{
			static_assert((TypeListContainsV<typename Traits::WriteTypes, Us> && ...),
				"Modify<> requested a component this view did not declare as writeable");
			using NewPack = typename Detail::MakeModifiedPack<SlotPack, Us...>::Type;
			return Detail::SlotRange<EnttView, NewPack>{ *this->m_Scene, this->m_View };
		}
	};

	// ---- Reactive views ------------------------------------------------------------------
	//
	// All three answer "what happened since *this system* last looked", which is why their state
	// lives in the system's ViewHolder rather than globally.
	//
	// InitView and FiniView follow the epoch protocol; each branch below is load-bearing:
	//
	//   state.Epoch == 0               first ever read
	//                                    InitView: everything currently matching counts as new
	//                                    FiniView: nothing (nothing has been removed yet)
	//   state.Epoch == frameEpoch      already read this update -> empty
	//   state.Epoch + 1 == frameEpoch  first read this update -> use this update's buffers
	//   state.Epoch < frameEpoch - 1   a whole update was skipped -> events were lost, assert
	//
	// ChangeView does not use epochs: it is read-time based, tracking a cursor per react type into
	// that type's ChangeBuffer, so "since my last read" is exact even across uneven read cadences.

	template<typename... Es>
	class InitView : public Detail::EntityListRange<Detail::NormalizePackT<TypeList<Es...>>>
	{
		using Traits = Detail::AccessorPackTraits<Es...>;
		using SlotPack = Detail::NormalizePackT<TypeList<Es...>>;
		using Base = Detail::EntityListRange<SlotPack>;

		static_assert(Traits::ReactTypes::Size > 0,
			"InitView needs at least one React<...> element to react to");
		static_assert(Detail::AllEnableInit<typename Traits::ReactTypes>::value,
			"Every React<...> type in an InitView needs ComponentTraits<T>::EnableInit");
		static_assert(Traits::IncludeTypes::Size > 0,
			"InitView needs at least one included component");

	public:
		struct State
		{
			uint32_t Epoch = 0;
			std::vector<entt::entity> Scratch;
		};

		// Access metadata for ordering/scheduling (Phase 8), derived from this pack's wrappers.
		static ViewDesc Desc() { return Detail::MakeViewDesc<Traits>(); }

		InitView(Scene& scene, State& state) : Base(scene, Select(scene, state)) {}

	private:
		static const std::vector<entt::entity>& Select(Scene& scene, State& state)
		{
			const uint32_t frameEpoch = scene.GetFrameEpoch();
			std::vector<entt::entity>& out = state.Scratch;
			out.clear();

			if (state.Epoch == frameEpoch)
			{
				// Already reacted this update; a second read sees nothing.
			}
			else if (state.Epoch == 0)
			{
				for (auto entity : Detail::MakeEnttView(scene.Reg(),
					typename Traits::IncludeTypes{}, typename Traits::ExcludeTypes{}))
				{
					out.push_back(entity);
				}
			}
			else
			{
				GE_CORE_ASSERT(state.Epoch + 1 == frameEpoch,
					"InitView skipped an update - creation events were lost. Read every reactive view every update.");

				Detail::ReactiveGather<typename Traits::ReactTypes>::Init(scene, out);
				Detail::SortAndUnique(out);

				// Filter against the entity's state *now*: something created and then destroyed
				// before any system looked simply never shows up.
				out.erase(std::remove_if(out.begin(), out.end(), [&](entt::entity entity)
				{
					return !Detail::MatchesFilter(scene.Reg(), entity,
						typename Traits::IncludeTypes{}, typename Traits::ExcludeTypes{});
				}), out.end());
			}

			state.Epoch = frameEpoch;
			return out;
		}
	};

	template<typename... Es>
	class FiniView : public Detail::EntityListRange<
		typename Detail::MakeFiniPack<Detail::NormalizePackT<TypeList<Es...>>,
		                              typename Detail::AccessorPackTraits<Es...>::ReactTypes>::Type>
	{
		using Traits = Detail::AccessorPackTraits<Es...>;
		using SlotPack = typename Detail::MakeFiniPack<Detail::NormalizePackT<TypeList<Es...>>,
			typename Traits::ReactTypes>::Type;
		using Base = Detail::EntityListRange<SlotPack>;

		static_assert(Traits::ReactTypes::Size > 0,
			"FiniView needs at least one React<...> element to react to");
		static_assert(Detail::AllEnableFini<typename Traits::ReactTypes>::value,
			"Every React<...> type in a FiniView needs ComponentTraits<T>::EnableFini, or there "
			"would be no buried copy left to read");

		// Restriction beyond the guide, on purpose: a Fini event may be an entity destruction, in
		// which case *every* component on it is gone, not just the reacted-to one. Only react types
		// have a graveyard copy to fall back on, so a non-react slot would dangle. Reading extra
		// components during teardown needs component-mask snapshots; until something needs that,
		// refusing the pack is safer than handing out a reference into a destroyed entity.
		static_assert(Traits::ReadTypes::Size == Traits::ReactTypes::Size,
			"FiniView may only access its React<...> types - other components may already be gone");

	public:
		struct State
		{
			uint32_t Epoch = 0;
			std::vector<entt::entity> Scratch;
		};

		// Access metadata for ordering/scheduling (Phase 8), derived from this pack's wrappers.
		static ViewDesc Desc() { return Detail::MakeViewDesc<Traits>(); }

		FiniView(Scene& scene, State& state) : Base(scene, Select(scene, state)) {}

	private:
		static const std::vector<entt::entity>& Select(Scene& scene, State& state)
		{
			const uint32_t frameEpoch = scene.GetFrameEpoch();
			std::vector<entt::entity>& out = state.Scratch;
			out.clear();

			// First ever read sees nothing: at that point nothing has been removed yet. This is
			// the one place InitView and FiniView deliberately disagree.
			if (state.Epoch != 0 && state.Epoch != frameEpoch)
			{
				GE_CORE_ASSERT(state.Epoch + 1 == frameEpoch,
					"FiniView skipped an update - removal events were lost. Read every reactive view every update.");

				Detail::ReactiveGather<typename Traits::ReactTypes>::Fini(scene, out);
				Detail::SortAndUnique(out);

				// Include filters are meaningless here (the component is already gone, which is
				// the whole point), so only exclusions are applied, and only where the entity
				// still exists to check.
				out.erase(std::remove_if(out.begin(), out.end(), [&](entt::entity entity)
				{
					if (!scene.Reg().valid(entity))
						return false;
					return !Detail::MatchesExcludes(scene.Reg(), entity, typename Traits::ExcludeTypes{});
				}), out.end());
			}

			state.Epoch = frameEpoch;
			return out;
		}
	};

	template<typename... Es>
	class ChangeView : public Detail::EntityListRange<Detail::NormalizePackT<TypeList<Es...>>>
	{
		using Traits = Detail::AccessorPackTraits<Es...>;
		using SlotPack = Detail::NormalizePackT<TypeList<Es...>>;
		using Base = Detail::EntityListRange<SlotPack>;

		static_assert(Traits::ReactTypes::Size > 0,
			"ChangeView needs at least one React<...> element to react to");
		static_assert(Detail::AllTrackChanges<typename Traits::ReactTypes>::value,
			"Every React<...> type in a ChangeView needs ComponentTraits<T>::TrackChanges");
		static_assert(Traits::IncludeTypes::Size > 0,
			"ChangeView needs at least one included component");

	public:
		struct State
		{
			// One cursor per react type, in ReactTypes order. 0 means "never read".
			std::array<ChangeBuffer::VirtualIndex, Traits::ReactTypes::Size> Cursors{};
			std::vector<entt::entity> Buffer;
		};


		// Access metadata for ordering/scheduling (Phase 8), derived from this pack's wrappers.
		static ViewDesc Desc() { return Detail::MakeViewDesc<Traits>(); }

		ChangeView(Scene& scene, State& state) : Base(scene, Select(scene, state)) {}

	private:
		static const std::vector<entt::entity>& Select(Scene& scene, State& state)
		{
			std::vector<entt::entity>& out = state.Buffer;
			out.clear();

			if (state.Cursors[0] == 0)
			{
				// First read: everything matching counts as changed, and the cursors start at the
				// current head so the next read reports only genuine changes.
				for (auto entity : Detail::MakeEnttView(scene.Reg(),
					typename Traits::IncludeTypes{}, typename Traits::ExcludeTypes{}))
				{
					out.push_back(entity);
				}
				Detail::ReactiveGather<typename Traits::ReactTypes>::RecordHeads(scene, state.Cursors);
			}
			else
			{
				Detail::ReactiveGather<typename Traits::ReactTypes>::CollectSince(scene, state.Cursors, out);
				Detail::SortAndUnique(out);

				// Drop entities that died or no longer match since the change was logged.
				out.erase(std::remove_if(out.begin(), out.end(), [&](entt::entity entity)
				{
					return !Detail::MatchesFilter(scene.Reg(), entity,
						typename Traits::IncludeTypes{}, typename Traits::ExcludeTypes{});
				}), out.end());
			}

			return out;
		}
	};

	// ---- AccessView ----------------------------------------------------------------------
	// Random access by entity rather than iteration. Get* assert the entity matches; Find*
	// return nullable accessors so a miss is checkable rather than fatal.

	template<typename... Es>
	class AccessView
	{
		using Traits = Detail::AccessorPackTraits<Es...>;
		using SlotPack = Detail::NormalizePackT<TypeList<Es...>>;
		using OptionalPack = typename Detail::MakeOptionalPack<SlotPack>::Type;

	public:
		struct State {};

		// Access metadata for ordering/scheduling (Phase 8), derived from this pack's wrappers.
		static ViewDesc Desc() { return Detail::MakeViewDesc<Traits>(); }

		AccessView(Scene& scene, State&) : m_Scene(&scene) {}
		explicit AccessView(Scene& scene) : m_Scene(&scene) {}

		bool Has(Entity entity) const
		{
			return Detail::MatchesFilter(m_Scene->Reg(), (entt::entity)entity,
				typename Traits::IncludeTypes{}, typename Traits::ExcludeTypes{});
		}

		// Full tuple of nullable accessors; every slot is null when the entity does not match.
		auto Find(Entity entity) const
		{
			using Builder = Detail::TupleBuilder<OptionalPack>;
			if (!Has(entity))
				return Builder::Null();
			return Builder::Build(*m_Scene, m_Scene->Reg(), (entt::entity)entity);
		}

		// Full tuple with the view's declared slot shape. Asserts the entity matches.
		auto Get(Entity entity) const
		{
			GE_CORE_ASSERT(Has(entity), "AccessView::Get on an entity that does not match the view");
			return Detail::TupleBuilder<SlotPack>::Build(*m_Scene, m_Scene->Reg(), (entt::entity)entity);
		}

		template<typename U>
		auto FindOne(Entity entity) const
		{
			AssertDeclared<U>();
			using Slot = Detail::SlotBuilder<typename Detail::MakeOptionalSlot<
				typename Detail::SlotElementFor<SlotPack, U>::Type>::Type>;
			if (!Has(entity))
				return Slot::Null();
			return Slot::Build(*m_Scene, m_Scene->Reg(), (entt::entity)entity);
		}

		template<typename U>
		decltype(auto) GetOne(Entity entity) const
		{
			AssertDeclared<U>();
			GE_CORE_ASSERT(Has(entity), "AccessView::GetOne on an entity that does not match the view");
			using Slot = Detail::SlotBuilder<typename Detail::SlotElementFor<SlotPack, U>::Type>;
			return Slot::Build(*m_Scene, m_Scene->Reg(), (entt::entity)entity);
		}

		template<typename... Us>
		auto FindSome(Entity entity) const
		{
			AssertDeclared<Us...>();
			using Builder = Detail::TupleBuilder<
				typename Detail::MakeOptionalPack<
					typename Detail::MakeSubsetPack<SlotPack, Us...>::Type>::Type>;
			if (!Has(entity))
				return Builder::Null();
			return Builder::Build(*m_Scene, m_Scene->Reg(), (entt::entity)entity);
		}

		template<typename... Us>
		auto GetSome(Entity entity) const
		{
			AssertDeclared<Us...>();
			GE_CORE_ASSERT(Has(entity), "AccessView::GetSome on an entity that does not match the view");
			using Builder = Detail::TupleBuilder<typename Detail::MakeSubsetPack<SlotPack, Us...>::Type>;
			return Builder::Build(*m_Scene, m_Scene->Reg(), (entt::entity)entity);
		}

	private:
		// Asking a view for a type it never declared is a compile error, not a wrong answer.
		template<typename... Us>
		static constexpr void AssertDeclared()
		{
			static_assert((TypeListContainsV<typename Traits::ReadTypes, Us> && ...),
				"Requested a component this view does not access");
		}

		Scene* m_Scene = nullptr;
	};
}
