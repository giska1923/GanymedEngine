#include "gepch.h"

#include "ComponentAccessor.h"

// Compile-time-only verification of the accessor invariants (Phase 2). Emits no code.
// The rule being guarded: for a tracked + writeable component, reading yields const and Modify()
// is the only path to a mutable reference. If that ever regresses, ChangeViews silently miss
// writes — a failure mode that is invisible at runtime, so it gets pinned down here instead.

namespace GanymedE::ECS {

	namespace {

		struct Untracked { int Value = 0; };
		struct Tracked   { int Value = 0; };
	}
}

// ComponentTraits specializations must live in namespace GanymedE, next to the primary template.
namespace GanymedE {
	template<> struct ComponentTraits<ECS::Tracked> { static constexpr bool TrackChanges = true; };
}

namespace GanymedE::ECS {

	namespace {

		template<typename T, bool Optional, bool Writeable>
		using Accessor = ComponentAccessor<T, Optional, Writeable>;

		// ---- Trackability is picked up from ComponentTraits via the defaulted template arg ----
		static_assert(!ComponentTraits<Untracked>::TrackChanges);
		static_assert(ComponentTraits<Tracked>::TrackChanges);

		// ---- Read-only slots yield const, whether or not the type is tracked ----
		static_assert(std::is_same_v<decltype(*std::declval<Accessor<Untracked, false, false>&>()), const Untracked&>);
		static_assert(std::is_same_v<decltype(*std::declval<Accessor<Tracked,   false, false>&>()), const Tracked&>);
		static_assert(std::is_same_v<decltype(std::declval<Accessor<Tracked, false, false>&>().operator->()), const Tracked*>);

		// ---- Writeable + untracked: operator* is directly mutable (nothing to log) ----
		static_assert(std::is_same_v<decltype(*std::declval<Accessor<Untracked, false, true>&>()), Untracked&>);
		static_assert(std::is_same_v<decltype(std::declval<Accessor<Untracked, false, true>&>().Modify()), Untracked&>);

		// ---- Writeable + tracked: THE invariant. Reads are const, only Modify() is mutable ----
		static_assert(std::is_same_v<decltype(*std::declval<Accessor<Tracked, false, true>&>()), const Tracked&>);
		static_assert(std::is_same_v<decltype(std::declval<Accessor<Tracked, false, true>&>().operator->()), const Tracked*>);
		static_assert(std::is_same_v<decltype(std::declval<Accessor<Tracked, false, true>&>().Modify()), Tracked&>);

		// ---- Optionality is carried on the type so slot builders can branch on it ----
		static_assert(!Accessor<Tracked, false, true>::IsOptional);
		static_assert(Accessor<Tracked, true, true>::IsOptional);
		static_assert(Accessor<Tracked, true, true>::IsWriteable);
		static_assert(!Accessor<Tracked, true, false>::IsWriteable);

		// ---- All slots are default-constructible and null: AccessView::Find returns a tuple of
		//      nullable accessors when the entity does not match, never dangling references ----
		static_assert(std::is_default_constructible_v<Accessor<Tracked, true, true>>);
		static_assert(std::is_default_constructible_v<Accessor<Tracked, false, false>>);
		static_assert(std::is_default_constructible_v<Accessor<Untracked, false, true>>);

		// ---- Accessors stay cheap: they are copied by value into every tuple slot ----
		static_assert(std::is_trivially_copyable_v<Accessor<Tracked, false, true>>);
		static_assert(std::is_trivially_copyable_v<Accessor<Untracked, false, true>>);
		static_assert(sizeof(Accessor<Untracked, false, false>) == sizeof(void*));
	}
}
