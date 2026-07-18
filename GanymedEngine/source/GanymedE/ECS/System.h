#pragma once

#include <entt/entt.hpp>
#include <unordered_map>
#include <vector>

#include "ViewDesc.h"
#include "ViewHolder.h"
#include "GanymedE/Core/Core.h"
#include "GanymedE/Core/Log.h"
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

			// Union of everything this system's declared views read and write.
			virtual ViewDesc Access() const = 0;

			// For diagnostics only.
			virtual const char* Name() const { return "System"; }

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

			// Derived from the view declarations — no separate list to keep in sync.
			ViewDesc Access() const override
			{
				return Detail::SystemDescOf<typename Implementation::Views>::Value();
			}
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

			// Checks registration order against the dependencies implied by the view declarations.
			//
			// The rule: if system A writes component X and a system B only reads X, B must run
			// after A, or it silently operates on the previous frame's value. Systems that both
			// write X are excluded — two writers need an order, but neither is a stale reader, and
			// which order is correct is a decision the declarations cannot make.
			//
			// Returns the number of violations, having logged each one.
			size_t ValidateOrdering() const
			{
				size_t violations = 0;

				for (size_t reader = 0; reader < m_Systems.size(); reader++)
				{
					const ViewDesc readerAccess = m_Systems[reader]->Access();

					// Anything written later that this system only reads.
					for (size_t writer = reader + 1; writer < m_Systems.size(); writer++)
					{
						const ViewDesc writerAccess = m_Systems[writer]->Access();
						const ComponentMask staleReads =
							readerAccess.Read & ~readerAccess.Write & writerAccess.Write;

						if (staleReads.none())
							continue;

						violations++;
						GE_CORE_ERROR("System ordering: '{0}' reads component(s) that '{1}' writes "
							"later in the same update, so it sees the previous frame's values. "
							"Register '{1}' first. (mask {2})",
							m_Systems[reader]->Name(), m_Systems[writer]->Name(), staleReads.to_string());
					}
				}

				return violations;
			}

		private:
			std::vector<Scope<ISystem>> m_Systems;          // execution order == registration order
			std::unordered_map<entt::id_type, ISystem*> m_Lookup;
		};
	}
}
