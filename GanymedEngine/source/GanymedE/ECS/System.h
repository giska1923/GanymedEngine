#pragma once

#include <entt/entt.hpp>
#include <unordered_map>
#include <vector>

#include "ViewHolder.h"
#include "GanymedE/Core/Core.h"
#include "GanymedE/Core/Timestep.h"

namespace GanymedE {

	class Scene;

	namespace ECS {

		// Deliberately minimal: no reflection-driven registration, no nested subsystem lists, no
		// task graph. Execution order is registration order. Phase 8 can derive ordering from the
		// views' declared read/write sets once there is a reason to (threads, or enough systems
		// that ordering bugs appear).
		class ISystem
		{
		public:
			explicit ISystem(Scene& scene) : m_Scene(scene) {}
			virtual ~ISystem() = default;

			ISystem(const ISystem&) = delete;
			ISystem& operator=(const ISystem&) = delete;

			virtual void OnRuntimeStart() {}
			virtual void OnRuntimeStop() {}
			virtual void OnUpdate(Timestep ts) = 0;

			// Systems that also run in edit mode override this; most do nothing.
			virtual void OnUpdateEditor(Timestep ts) { (void)ts; }

		protected:
			Scene& m_Scene;
		};

		// CRTP convenience: declare `using Views = TypeList<...>` and View<MyView>() works.
		template<typename Implementation>
		class System : public ISystem, public ViewHolder<Implementation>
		{
		public:
			explicit System(Scene& scene)
				: ISystem(scene), ViewHolder<Implementation>(scene) {}
		};

		class SystemManager
		{
		public:
			template<typename S, typename... Args>
			S& Add(Scene& scene, Args&&... args)
			{
				auto system = CreateScope<S>(scene, std::forward<Args>(args)...);
				S& reference = *system;
				m_Systems.push_back(std::move(system));
				m_Lookup[entt::type_hash<S>::value()] = &reference;
				return reference;
			}

			// Direct system-to-system access. Prefer communicating through components or (Phase 7)
			// singletons; this exists for the one case that has no other route yet — the renderer
			// needing the live PhysicsScene to draw Jolt's debug view.
			template<typename S>
			S* Get() const
			{
				auto it = m_Lookup.find(entt::type_hash<S>::value());
				return it != m_Lookup.end() ? static_cast<S*>(it->second) : nullptr;
			}

			void OnUpdate(Timestep ts) { for (auto& system : m_Systems) system->OnUpdate(ts); }
			void OnUpdateEditor(Timestep ts) { for (auto& system : m_Systems) system->OnUpdateEditor(ts); }

			// Lifecycle deliberately runs opposite to update order.
			//
			// Registration order is *update* order (physics steps before scripts run). Startup
			// needs the opposite: scripts must be instantiated before PhysicsScene::Start walks the
			// registry building bodies, so that a rigid body added by a script's OnCreate is
			// present in the initial simulation state. Teardown then mirrors startup, stopping
			// physics before the script instances it may reference are destroyed.
			//
			// This reproduces exactly what Scene::OnRuntimeStart/Stop did inline before Phase 4.
			void OnRuntimeStart()
			{
				for (auto it = m_Systems.rbegin(); it != m_Systems.rend(); ++it)
					(*it)->OnRuntimeStart();
			}

			void OnRuntimeStop() { for (auto& system : m_Systems) system->OnRuntimeStop(); }

		private:
			std::vector<Scope<ISystem>> m_Systems;          // execution order == registration order
			std::unordered_map<entt::id_type, ISystem*> m_Lookup;
		};
	}
}
