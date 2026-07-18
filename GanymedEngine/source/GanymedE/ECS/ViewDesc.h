#pragma once

#include <bitset>

#include "AccessWrappers.h"
#include "ComponentTraits.h"

// Runtime access metadata, derived from the very same view declarations the systems already write.
//
// The wrapper grammar has always carried this information — RO/RW/React say exactly what a view
// reads and writes. ViewDesc just projects it into bitsets so it can be reasoned about at runtime:
// which systems conflict, and which must run before which.
//
// Single-threaded, the payoff is ordering validation. A system that *reads* a component another
// system *writes* has to run after it, or it silently operates on last frame's values — the exact
// shape of bug that TransformSystem -> CameraSystem -> RenderSystem would have if reordered.

namespace GanymedE::ECS {

	inline constexpr size_t MaxComponents = ComponentList::Size;
	using ComponentMask = std::bitset<MaxComponents>;

	// Dense index into ComponentList. Hard error for a component that is not registered there,
	// which is the point: the list is the single source of truth.
	template<typename T>
	inline constexpr size_t ComponentIndexOf = TypeListIndexOfV<ComponentList, T>;

	struct ViewDesc
	{
		ComponentMask Include;
		ComponentMask Exclude;
		ComponentMask Read;
		ComponentMask Write;
		ComponentMask React;

		ViewDesc& operator|=(const ViewDesc& other)
		{
			Include |= other.Include;
			Exclude |= other.Exclude;
			Read    |= other.Read;
			Write   |= other.Write;
			React   |= other.React;
			return *this;
		}
	};

	namespace Detail {

		template<typename TL> struct ComponentMaskOf;
		template<typename... Ts>
		struct ComponentMaskOf<TypeList<Ts...>>
		{
			static ComponentMask Value()
			{
				ComponentMask mask;
				(mask.set(ComponentIndexOf<Ts>), ...);
				return mask;
			}
		};

		template<typename Traits>
		ViewDesc MakeViewDesc()
		{
			ViewDesc desc;
			desc.Include = ComponentMaskOf<typename Traits::IncludeTypes>::Value();
			desc.Exclude = ComponentMaskOf<typename Traits::ExcludeTypes>::Value();
			desc.Read    = ComponentMaskOf<typename Traits::ReadTypes>::Value();
			desc.Write   = ComponentMaskOf<typename Traits::WriteTypes>::Value();
			desc.React   = ComponentMaskOf<typename Traits::ReactTypes>::Value();
			return desc;
		}

		// Unions every view a system declares into one access description.
		template<typename ViewList> struct SystemDescOf;
		template<typename... Vs>
		struct SystemDescOf<TypeList<Vs...>>
		{
			static ViewDesc Value()
			{
				ViewDesc desc;
				((desc |= Vs::Desc()), ...);
				return desc;
			}
		};
	}
}
