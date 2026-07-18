#pragma once

#include <cstddef>
#include <type_traits>

// Minimal compile-time type-list toolbox. Every later ECS phase (access wrappers, pack traits,
// view state tuples) is built on top of these primitives, so they intentionally stay dependency-free.

namespace GanymedE {

	// C++17 stand-in for std::type_identity (C++20). Used to pass a type as a runtime value
	// to lambdas in ForEachType.
	template<typename T>
	struct TypeIdentity { using Type = T; };

	template<typename... Ts>
	struct TypeList
	{
		static constexpr size_t Size = sizeof...(Ts);
	};

	// ---- Merge: TypeListMergeT<TypeList<A>, TypeList<B, C>> == TypeList<A, B, C> ----
	template<typename... Ls> struct TypeListMerge;
	template<> struct TypeListMerge<> { using Type = TypeList<>; };
	template<typename... As> struct TypeListMerge<TypeList<As...>> { using Type = TypeList<As...>; };
	template<typename... As, typename... Bs, typename... Rest>
	struct TypeListMerge<TypeList<As...>, TypeList<Bs...>, Rest...>
		: TypeListMerge<TypeList<As..., Bs...>, Rest...> {};
	template<typename... Ls> using TypeListMergeT = typename TypeListMerge<Ls...>::Type;

	// ---- Contains ----
	template<typename TL, typename T> struct TypeListContains;
	template<typename T, typename... Ts>
	struct TypeListContains<TypeList<Ts...>, T>
		: std::bool_constant<(std::is_same_v<T, Ts> || ...)> {};
	template<typename TL, typename T>
	inline constexpr bool TypeListContainsV = TypeListContains<TL, T>::value;

	// ---- IndexOf: position of T in TL; hard error when absent ----
	namespace Detail {

		template<typename T, typename... Ts>
		constexpr size_t TypeListIndexOfImpl()
		{
			// Leading false keeps the array non-empty for TypeList<>
			constexpr bool matches[] = { false, std::is_same_v<T, Ts>... };
			for (size_t i = 1; i <= sizeof...(Ts); i++)
			{
				if (matches[i])
					return i - 1;
			}
			return sizeof...(Ts);
		}
	}

	template<typename TL, typename T> struct TypeListIndexOf;
	template<typename T, typename... Ts>
	struct TypeListIndexOf<TypeList<Ts...>, T>
	{
		static_assert(TypeListContainsV<TypeList<Ts...>, T>, "Type is not a member of this TypeList!");
		static constexpr size_t value = Detail::TypeListIndexOfImpl<T, Ts...>();
	};
	template<typename TL, typename T>
	inline constexpr size_t TypeListIndexOfV = TypeListIndexOf<TL, T>::value;

	// ---- Element<N> ----
	template<typename TL, size_t N> struct TypeListElement;
	template<typename T, typename... Ts>
	struct TypeListElement<TypeList<T, Ts...>, 0> { using Type = T; };
	template<size_t N, typename T, typename... Ts>
	struct TypeListElement<TypeList<T, Ts...>, N>
	{
		static_assert(N <= sizeof...(Ts), "TypeList index out of range!");
		using Type = typename TypeListElement<TypeList<Ts...>, N - 1>::Type;
	};
	template<typename TL, size_t N>
	using TypeListElementT = typename TypeListElement<TL, N>::Type;

	// ---- Map: applies the trait F to every element, yielding TypeList<typename F<Ts>::Type...> ----
	template<typename TL, template<typename> class F> struct TypeListMap;
	template<template<typename> class F, typename... Ts>
	struct TypeListMap<TypeList<Ts...>, F> { using Type = TypeList<typename F<Ts>::Type...>; };
	template<typename TL, template<typename> class F>
	using TypeListMapT = typename TypeListMap<TL, F>::Type;

	// ---- Filter: keeps elements for which Pred<T>::value is true ----
	template<typename TL, template<typename> class Pred> struct TypeListFilter;
	template<template<typename> class Pred>
	struct TypeListFilter<TypeList<>, Pred> { using Type = TypeList<>; };
	template<template<typename> class Pred, typename T, typename... Ts>
	struct TypeListFilter<TypeList<T, Ts...>, Pred>
	{
	private:
		using Tail = typename TypeListFilter<TypeList<Ts...>, Pred>::Type;
	public:
		using Type = std::conditional_t<Pred<T>::value, TypeListMergeT<TypeList<T>, Tail>, Tail>;
	};
	template<typename TL, template<typename> class Pred>
	using TypeListFilterT = typename TypeListFilter<TL, Pred>::Type;

	// ---- Runtime visitation: calls func(TypeIdentity<T>{}) once per element, in order ----
	// Usage:
	//   ForEachType(ComponentList{}, [&](auto typeTag)
	//   {
	//       using T = typename decltype(typeTag)::Type;
	//       ...
	//   });
	template<typename... Ts, typename F>
	void ForEachType(TypeList<Ts...>, F&& func)
	{
		(func(TypeIdentity<Ts>{}), ...);
	}
}
