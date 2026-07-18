#pragma once

#include <entt/entt.hpp>
#include <tuple>
#include <type_traits>

#include "AccessWrappers.h"
#include "ComponentAccessor.h"
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
