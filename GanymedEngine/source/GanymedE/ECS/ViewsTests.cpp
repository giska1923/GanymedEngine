#include "gepch.h"

#include "Views.h"

// Compile-time-only verification of the Phase 3 slot rules. Emits no code.
// These pin the tuple shapes: the whole design rests on a tracked+writeable slot never being
// spelled T&, because that is the one mistake that produces silently unlogged writes.

namespace GanymedE::ECS {

	namespace {

		struct Plain    { int Value = 0; };   // untracked
		struct Watched  { int Value = 0; };   // tracked (below)
		struct Marker   {};
		struct Excluded {};
	}
}

namespace GanymedE {
	template<> struct ComponentTraits<ECS::Watched> { static constexpr bool TrackChanges = true; };
}

namespace GanymedE::ECS {

	namespace {

		template<typename Lhs, typename Rhs>
		inline constexpr bool Same = std::is_same_v<Lhs, Rhs>;

		template<typename... Es>
		using Slots = Detail::NormalizePackT<TypeList<Es...>>;

		template<typename... Es>
		using Tuple = typename Detail::TupleBuilder<Slots<Es...>>::Type;

		// ---- Normalization: variadic wrappers split, filters drop out, order is preserved ----
		static_assert(Same<Slots<RW<Plain, Watched>>, TypeList<RW<Plain>, RW<Watched>>>);
		static_assert(Same<Slots<Has<Marker>>,        TypeList<>>);
		static_assert(Same<Slots<No<Excluded>>,       TypeList<>>);
		static_assert(Same<Slots<EntityId, RO<Plain>, Has<Marker>, RW<Watched>>,
		                   TypeList<EntityId, RO<Plain>, RW<Watched>>>);

		// ---- Slot rules ----
		static_assert(Same<Tuple<EntityId>, std::tuple<entt::entity>>);

		// bare T == RO<T>; both yield const T&
		static_assert(Same<Tuple<Plain>,     std::tuple<const Plain&>>);
		static_assert(Same<Tuple<RO<Plain>>, std::tuple<const Plain&>>);
		// read-only stays const even for a tracked component — reading is never a change
		static_assert(Same<Tuple<RO<Watched>>, std::tuple<const Watched&>>);

		// writeable + untracked collapses to a plain reference (nothing to log)
		static_assert(Same<Tuple<RW<Plain>>, std::tuple<Plain&>>);

		// THE invariant: writeable + tracked must be an accessor, never Watched&
		static_assert(Same<Tuple<RW<Watched>>, std::tuple<ComponentAccessor<Watched, false, true>>>);
		static_assert(!Same<Tuple<RW<Watched>>, std::tuple<Watched&>>);

		// optional slots are nullable accessors regardless of trackability
		static_assert(Same<Tuple<OptRO<Plain>>,   std::tuple<ComponentAccessor<Plain, true, false>>>);
		static_assert(Same<Tuple<OptRW<Plain>>,   std::tuple<ComponentAccessor<Plain, true, true>>>);
		static_assert(Same<Tuple<OptRW<Watched>>, std::tuple<ComponentAccessor<Watched, true, true>>>);

		// reactive wrappers produce the same slots as their non-reactive counterparts
		static_assert(Same<Tuple<ReactRO<Watched>>, Tuple<RO<Watched>>>);
		static_assert(Same<Tuple<ReactRW<Watched>>, Tuple<RW<Watched>>>);
		static_assert(Same<Tuple<ReactHas<Watched>>, std::tuple<>>);

		// a realistic mixed pack, in declaration order
		static_assert(Same<Tuple<EntityId, RO<Plain>, RW<Watched>, OptRO<Marker>, No<Excluded>>,
			std::tuple<entt::entity, const Plain&, ComponentAccessor<Watched, false, true>,
			           ComponentAccessor<Marker, true, false>>>);

		// ---- Modify<>: listed slots become plain references, others are untouched ----
		using Mixed = Slots<EntityId, RO<Plain>, RW<Watched>>;
		using Modified = typename Detail::MakeModifiedPack<Mixed, Watched>::Type;
		static_assert(Same<Modified, TypeList<EntityId, RO<Plain>, Detail::PreModified<Watched>>>);
		static_assert(Same<typename Detail::TupleBuilder<Modified>::Type,
			std::tuple<entt::entity, const Plain&, Watched&>>);

		// Modify<> with no arguments changes nothing
		static_assert(Same<typename Detail::MakeModifiedPack<Mixed>::Type, Mixed>);

		// ---- Subset<>: narrows the tuple, keeps declared access, keeps EntityId ----
		using Subsetted = typename Detail::MakeSubsetPack<Mixed, Watched>::Type;
		static_assert(Same<Subsetted, TypeList<EntityId, RW<Watched>>>);
		static_assert(Same<typename Detail::TupleBuilder<Subsetted>::Type,
			std::tuple<entt::entity, ComponentAccessor<Watched, false, true>>>);

		// ---- Find shape: every slot nullable, so a miss yields nulls not dangling refs ----
		using Optionalized = typename Detail::MakeOptionalPack<Mixed>::Type;
		static_assert(Same<Optionalized, TypeList<EntityId, OptRO<Plain>, OptRW<Watched>>>);
		static_assert(Same<typename Detail::TupleBuilder<Optionalized>::Type,
			std::tuple<entt::entity, ComponentAccessor<Plain, true, false>,
			           ComponentAccessor<Watched, true, true>>>);

		// ---- Slot lookup by component, used by GetOne/FindOne/GetSome ----
		static_assert(Same<typename Detail::SlotElementFor<Mixed, Plain>::Type, RO<Plain>>);
		static_assert(Same<typename Detail::SlotElementFor<Mixed, Watched>::Type, RW<Watched>>);

		// ---- Include/exclude sets reach the entt view correctly ----
		using FilterTraits = Detail::AccessorPackTraits<EntityId, RO<Plain>, Has<Marker>,
		                                                OptRO<Watched>, No<Excluded>>;
		static_assert(Same<typename FilterTraits::IncludeTypes, TypeList<Plain, Marker>>);
		static_assert(Same<typename FilterTraits::ExcludeTypes, TypeList<Excluded>>);
		// optional components must NOT constrain which entities match
		static_assert(!TypeListContainsV<typename FilterTraits::IncludeTypes, Watched>);

		// ---- Views are cheap throwaway stack objects ----
		static_assert(std::is_empty_v<IterView<RO<Plain>>::State>);
		static_assert(std::is_empty_v<AccessView<RO<Plain>>::State>);
		static_assert(sizeof(AccessView<RO<Plain>>) == sizeof(void*));
	}
}
