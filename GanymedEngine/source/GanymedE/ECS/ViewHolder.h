#pragma once

#include <tuple>

#include "TypeList.h"
#include "GanymedE/Core/Core.h"

namespace GanymedE {

	class Scene;

	namespace ECS {

		// Views are throwaway stack objects, but some of them (the reactive ones in Phase 6) need
		// persistent state across frames: read cursors, epochs, scratch buffers. That state belongs
		// to the *system*, so that "what changed since my last read" is per-consumer rather than
		// global. ViewHolder is where it lives.

		class ViewStateStorage
		{
		public:
			virtual ~ViewStateStorage() = default;
		};

		template<typename ViewList> class ViewStates;
		template<typename... Vs>
		class ViewStates<TypeList<Vs...>> : public ViewStateStorage
		{
		public:
			std::tuple<typename Vs::State...> States;
		};

		// CRTP base giving a system one State per view it declares in `using Views = TypeList<...>`.
		//
		// The state tuple is type-erased behind ViewStateStorage on purpose: a plain
		// `std::tuple<typename Implementation::Views>` member cannot work, because the base class
		// is instantiated while the derived system is still incomplete, so Implementation::Views
		// is not yet nameable. Member *function* bodies are instantiated later, once the derived
		// class is complete — which is why the allocation happens in the constructor instead.
		template<typename Implementation>
		class ViewHolder
		{
		public:
			explicit ViewHolder(Scene& scene)
				: m_ViewScene(scene)
				, m_ViewStates(CreateScope<ViewStates<typename Implementation::Views>>())
			{}

			template<typename V>
			V View()
			{
				using Views = typename Implementation::Views;
				static_assert(TypeListContainsV<Views, V>,
					"This view is not declared in the system's `using Views = TypeList<...>` list");

				using Storage = ViewStates<Views>;
				constexpr size_t Index = TypeListIndexOfV<Views, V>;
				return V{ m_ViewScene, std::get<Index>(static_cast<Storage*>(m_ViewStates.get())->States) };
			}

		private:
			// Deliberately not named m_Scene: System<> inherits from both ISystem and ViewHolder,
			// and two members of the same name would make `m_Scene` ambiguous in every system.
			Scene& m_ViewScene;
			Scope<ViewStateStorage> m_ViewStates;
		};
	}
}
