#pragma once

#include "TypeList.h"

#include <cstdint>

// The wrapper grammar. A view's template arguments declare *exactly* what it includes, excludes,
// reads and writes, and that declaration is the single source of truth for:
//   - which entt view gets built            (IncludeTypes / ExcludeTypes)
//   - what each result-tuple slot looks like (Optional / Writeable, Phase 3)
//   - which change buffers get consulted     (ReactTypes, Phase 6)
//   - scheduling metadata                    (ReadTypes / WriteTypes, Phase 8)
//
//   Has<T>    include, no access        No<T>     exclude, no access
//   RO<T>     include, read-only        RW<T>     include, read-write
//   OptRO<T>  optional, read-only       OptRW<T>  optional, read-write
//   ReactHas / ReactRO / ReactRW / ReactOpt / ReactOptRO / ReactOptRW   (reactive views)
//   bare T == RO<T>;  EntityId yields the entity id in the result tuple
//
// Wrappers are variadic: RW<A, B> is exactly equivalent to RW<A>, RW<B>.

namespace GanymedE::ECS {

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

	// Pack marker requesting the entity id as a tuple slot. Views yield entt::entity; wrapping it
	// back into a GanymedE::Entity happens at the API surface, not here.
	struct EntityId {};

	namespace Detail {

		enum class FilterLevel : uint8_t { Ignore, Include, Exclude };
		enum class AccessLevel : uint8_t { None, ReadOnly, ReadWrite };

		template<bool Condition, typename TL>
		using ListIf = std::conditional_t<Condition, TL, TypeList<>>;

		template<FilterLevel FL, AccessLevel AL, bool React, typename... Ts>
		struct ElementTraitsImpl
		{
			static_assert(((!std::is_const_v<Ts> && !std::is_volatile_v<Ts>) && ...),
				"Access wrappers take non-cv-qualified component types");
			static_assert((!std::is_reference_v<Ts> && ...),
				"Access wrappers take component types, not references");
			static_assert(!(FL == FilterLevel::Exclude && AL != AccessLevel::None),
				"Cannot access excluded components");

			using AllTypes     = TypeList<Ts...>;

			using IncludeTypes = ListIf<FL == FilterLevel::Include, AllTypes>;
			using ExcludeTypes = ListIf<FL == FilterLevel::Exclude, AllTypes>;
			using ReadTypes    = ListIf<AL != AccessLevel::None,    AllTypes>;
			using WriteTypes   = ListIf<AL == AccessLevel::ReadWrite, AllTypes>;
			using ReactTypes   = ListIf<React,                      AllTypes>;

			// Everything this element touches at all — same set as ReadTypes, named for the
			// slot-building code in Phase 3 which cares about "does this produce a tuple slot".
			using AccessedTypes = ReadTypes;

			// Slot shape, consumed by Phase 3's tuple builder.
			static constexpr bool Optional   = FL == FilterLevel::Ignore;
			static constexpr bool Writeable  = AL == AccessLevel::ReadWrite;
			static constexpr bool IsEntityId = false;

			// Number of result-tuple slots contributed. Elements that filter without granting
			// access (Has / No / ReactHas / ReactOpt) contribute none — they constrain which
			// entities match without appearing in the tuple.
			static constexpr size_t SlotCount = AccessedTypes::Size;
		};

		template<typename T> struct ElementTraits
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

		// The entity id contributes no component type to any list, but does produce one tuple slot.
		template<> struct ElementTraits<EntityId>
		{
			using AllTypes      = TypeList<>;
			using IncludeTypes  = TypeList<>;
			using ExcludeTypes  = TypeList<>;
			using ReadTypes     = TypeList<>;
			using WriteTypes    = TypeList<>;
			using ReactTypes    = TypeList<>;
			using AccessedTypes = TypeList<>;

			static constexpr bool Optional   = false;
			static constexpr bool Writeable  = false;
			static constexpr bool IsEntityId = true;
			static constexpr size_t SlotCount = 1;
		};

		// ---- Rebinding: re-wrap an element around a different component type ----
		// Used to normalize a pack into one-type-per-element form, so that RW<A, B> becomes
		// RW<A>, RW<B> and every downstream slot rule can assume a single component per element.
		template<typename E> struct ElementRebind        { template<typename U> using To = RO<U>; };  // bare T
		template<typename... Ts> struct ElementRebind<Has<Ts...>>        { template<typename U> using To = Has<U>; };
		template<typename... Ts> struct ElementRebind<RO<Ts...>>         { template<typename U> using To = RO<U>; };
		template<typename... Ts> struct ElementRebind<RW<Ts...>>         { template<typename U> using To = RW<U>; };
		template<typename... Ts> struct ElementRebind<OptRO<Ts...>>      { template<typename U> using To = OptRO<U>; };
		template<typename... Ts> struct ElementRebind<OptRW<Ts...>>      { template<typename U> using To = OptRW<U>; };
		template<typename... Ts> struct ElementRebind<No<Ts...>>         { template<typename U> using To = No<U>; };
		template<typename... Ts> struct ElementRebind<ReactHas<Ts...>>   { template<typename U> using To = ReactHas<U>; };
		template<typename... Ts> struct ElementRebind<ReactRO<Ts...>>    { template<typename U> using To = ReactRO<U>; };
		template<typename... Ts> struct ElementRebind<ReactRW<Ts...>>    { template<typename U> using To = ReactRW<U>; };
		template<typename... Ts> struct ElementRebind<ReactOpt<Ts...>>   { template<typename U> using To = ReactOpt<U>; };
		template<typename... Ts> struct ElementRebind<ReactOptRO<Ts...>> { template<typename U> using To = ReactOptRO<U>; };
		template<typename... Ts> struct ElementRebind<ReactOptRW<Ts...>> { template<typename U> using To = ReactOptRW<U>; };
		template<> struct ElementRebind<EntityId>                        { template<typename U> using To = EntityId; };

		// Expands one element into its slot-producing, single-type form.
		// Has/No/ReactHas/ReactOpt have no accessed types and so vanish; EntityId is kept as-is.
		template<typename E, typename AccessedList> struct NormalizeElementImpl;
		template<typename E, typename... Ts>
		struct NormalizeElementImpl<E, TypeList<Ts...>>
		{
			using Type = TypeList<typename ElementRebind<E>::template To<Ts>...>;
		};

		template<typename E>
		struct NormalizeElement : NormalizeElementImpl<E, typename ElementTraits<E>::AccessedTypes> {};
		template<> struct NormalizeElement<EntityId> { using Type = TypeList<EntityId>; };

		// The slot pack: pack elements in declaration order, one component each, filters dropped.
		template<typename PackList> struct NormalizePack;
		template<typename... Es>
		struct NormalizePack<TypeList<Es...>>
		{
			using Type = TypeListMergeT<typename NormalizeElement<Es>::Type...>;
		};
		template<typename PackList> using NormalizePackT = typename NormalizePack<PackList>::Type;

		// Flattens a whole pack of elements into the five type lists views are built from.
		template<typename... Es>
		struct AccessorPackTraits
		{
			using AllTypes     = TypeListMergeT<typename ElementTraits<Es>::AllTypes...>;
			using IncludeTypes = TypeListMergeT<typename ElementTraits<Es>::IncludeTypes...>;
			using ExcludeTypes = TypeListMergeT<typename ElementTraits<Es>::ExcludeTypes...>;
			using ReadTypes    = TypeListMergeT<typename ElementTraits<Es>::ReadTypes...>;
			using WriteTypes   = TypeListMergeT<typename ElementTraits<Es>::WriteTypes...>;
			using ReactTypes   = TypeListMergeT<typename ElementTraits<Es>::ReactTypes...>;

			using PackElements = TypeList<Es...>;   // original order, so tuple slots stay stable

			static constexpr size_t SlotCount = (size_t{ 0 } + ... + ElementTraits<Es>::SlotCount);
		};
	}
}
