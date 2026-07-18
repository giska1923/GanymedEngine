#include "gepch.h"

#include "AccessWrappers.h"

// Compile-time-only verification of the wrapper grammar (Phase 1). This TU emits no code and no
// symbols — it exists so that breaking the trait lists is a build error rather than a subtly wrong
// view somewhere downstream. Keep it building; delete an assert only when the rule itself changes.

namespace GanymedE::ECS::Detail {

	namespace {

		struct A {};
		struct B {};
		struct C {};
		struct Unrelated {};

		template<typename Element> using Traits = ElementTraits<Element>;

		template<typename Lhs, typename Rhs>
		inline constexpr bool SameList = std::is_same_v<Lhs, Rhs>;

		// ---- bare T is exactly RO<T> ----
		static_assert(SameList<Traits<A>::IncludeTypes, TypeList<A>>);
		static_assert(SameList<Traits<A>::ReadTypes,    TypeList<A>>);
		static_assert(SameList<Traits<A>::WriteTypes,   TypeList<>>);
		static_assert(SameList<Traits<A>::ExcludeTypes, TypeList<>>);
		static_assert(SameList<Traits<A>::ReactTypes,   TypeList<>>);
		static_assert(!Traits<A>::Optional && !Traits<A>::Writeable);

		static_assert(SameList<Traits<RO<A>>::IncludeTypes, Traits<A>::IncludeTypes>);
		static_assert(SameList<Traits<RO<A>>::ReadTypes,    Traits<A>::ReadTypes>);
		static_assert(SameList<Traits<RO<A>>::WriteTypes,   Traits<A>::WriteTypes>);

		// ---- Has: filters but grants no access ----
		static_assert(SameList<Traits<Has<A>>::IncludeTypes, TypeList<A>>);
		static_assert(SameList<Traits<Has<A>>::ReadTypes,    TypeList<>>);
		static_assert(Traits<Has<A>>::SlotCount == 0);   // filters, but never appears in the tuple
		static_assert(Traits<No<A>>::SlotCount == 0);
		static_assert(Traits<ReactHas<A>>::SlotCount == 0);
		static_assert(Traits<ReactOpt<A>>::SlotCount == 0);
		static_assert(Traits<RO<A>>::SlotCount == 1);
		static_assert(Traits<RW<A, B>>::SlotCount == 2);

		// ---- No: excludes, never accesses ----
		static_assert(SameList<Traits<No<A>>::ExcludeTypes, TypeList<A>>);
		static_assert(SameList<Traits<No<A>>::IncludeTypes, TypeList<>>);
		static_assert(SameList<Traits<No<A>>::ReadTypes,    TypeList<>>);

		// ---- RW implies readable ----
		static_assert(SameList<Traits<RW<A>>::ReadTypes,  TypeList<A>>);
		static_assert(SameList<Traits<RW<A>>::WriteTypes, TypeList<A>>);
		static_assert(Traits<RW<A>>::Writeable && !Traits<RW<A>>::Optional);

		// ---- Opt* are ignored by the filter but still accessed ----
		static_assert(SameList<Traits<OptRO<A>>::IncludeTypes, TypeList<>>);
		static_assert(SameList<Traits<OptRO<A>>::ExcludeTypes, TypeList<>>);
		static_assert(SameList<Traits<OptRO<A>>::ReadTypes,    TypeList<A>>);
		static_assert(Traits<OptRO<A>>::Optional && !Traits<OptRO<A>>::Writeable);
		static_assert(Traits<OptRW<A>>::Optional && Traits<OptRW<A>>::Writeable);

		// ---- React* mirror their non-reactive counterparts, plus ReactTypes ----
		static_assert(SameList<Traits<ReactRW<A>>::IncludeTypes, Traits<RW<A>>::IncludeTypes>);
		static_assert(SameList<Traits<ReactRW<A>>::WriteTypes,   Traits<RW<A>>::WriteTypes>);
		static_assert(SameList<Traits<ReactRW<A>>::ReactTypes,   TypeList<A>>);
		static_assert(SameList<Traits<ReactHas<A>>::ReactTypes,  TypeList<A>>);
		static_assert(SameList<Traits<ReactOpt<A>>::ReadTypes,   TypeList<>>);
		static_assert(Traits<ReactOpt<A>>::Optional);

		// ---- EntityId touches no component but yields one slot ----
		static_assert(SameList<Traits<EntityId>::AllTypes, TypeList<>>);
		static_assert(Traits<EntityId>::IsEntityId);
		static_assert(Traits<EntityId>::SlotCount == 1);
		static_assert(!Traits<A>::IsEntityId);

		// ---- Variadic wrappers are sugar: RW<A, B> == RW<A>, RW<B> ----
		using Wide   = AccessorPackTraits<RW<A, B>>;
		using Narrow = AccessorPackTraits<RW<A>, RW<B>>;
		static_assert(SameList<Wide::IncludeTypes, Narrow::IncludeTypes>);
		static_assert(SameList<Wide::WriteTypes,   Narrow::WriteTypes>);
		static_assert(Wide::SlotCount == Narrow::SlotCount);
		static_assert(Wide::SlotCount == 2);

		// ---- A realistic pack flattens into the five lists, order preserved ----
		using Pack = AccessorPackTraits<EntityId, RO<A>, RW<B>, OptRO<C>, No<A>>;
		static_assert(SameList<Pack::IncludeTypes, TypeList<A, B>>);
		static_assert(SameList<Pack::ExcludeTypes, TypeList<A>>);
		static_assert(SameList<Pack::ReadTypes,    TypeList<A, B, C>>);
		static_assert(SameList<Pack::WriteTypes,   TypeList<B>>);
		static_assert(SameList<Pack::ReactTypes,   TypeList<>>);
		static_assert(SameList<Pack::PackElements, TypeList<EntityId, RO<A>, RW<B>, OptRO<C>, No<A>>>);
		static_assert(Pack::SlotCount == 4);   // EntityId + A + B + C; No<A> contributes nothing

		// Phase 3's FindSome/GetSome gate on this: asking a view for a type it never declared
		// must be a compile error, not a silently wrong answer.
		static_assert(TypeListContainsV<Pack::ReadTypes, C>);
		static_assert(!TypeListContainsV<Pack::ReadTypes, Unrelated>);

		// ---- Reactive pack, as TransformSystem will declare it in Phase 12 ----
		using ReactivePack = AccessorPackTraits<EntityId, ReactRO<A>, ReactRO<B>, RW<C>>;
		static_assert(SameList<ReactivePack::ReactTypes,   TypeList<A, B>>);
		static_assert(SameList<ReactivePack::IncludeTypes, TypeList<A, B, C>>);
		static_assert(SameList<ReactivePack::WriteTypes,   TypeList<C>>);
		static_assert(ReactivePack::ReactTypes::Size > 0);   // the Init/Change/FiniView guard

		// ---- Empty pack degrades gracefully ----
		static_assert(SameList<AccessorPackTraits<>::AllTypes, TypeList<>>);
		static_assert(AccessorPackTraits<>::SlotCount == 0);
	}
}
