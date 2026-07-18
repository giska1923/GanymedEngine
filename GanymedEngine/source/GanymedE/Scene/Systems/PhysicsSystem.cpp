#include "gepch.h"
#include "PhysicsSystem.h"

#include "GanymedE/ECS/Singleton.h"
#include "GanymedE/Scene/Entity.h"
#include "GanymedE/Scene/Scene.h"
#include "GanymedE/Scene/SceneSingletons.h"

namespace GanymedE {

	void PhysicsSystem::OnRuntimeStart()
	{
		m_PhysicsScene = CreateScope<PhysicsScene>();
		m_PhysicsScene->Start(&m_Scene);
		m_Accumulator = 0.0f;
	}

	void PhysicsSystem::OnRuntimeStop()
	{
		if (m_PhysicsScene)
		{
			m_PhysicsScene->Stop();
			m_PhysicsScene.reset();
		}
		m_Accumulator = 0.0f;
	}

	void PhysicsSystem::DispatchCollisionEvents()
	{
		if (!m_PhysicsScene)
			return;

		ScriptAccess scripts = View<ScriptAccess>();

		for (const auto& event : m_PhysicsScene->GetCollisionEvents())
		{
			Entity a = m_Scene.FindEntityByUUID(event.EntityA);
			Entity b = m_Scene.FindEntityByUUID(event.EntityB);
			if (!a || !b)
				continue;

			auto notify = [&](Entity self, Entity other)
			{
				auto script = scripts.FindOne<NativeScriptComponent>(self);
				if (!script || !script->Instance)
					return;

				if (event.Entered)
					script->Instance->OnCollisionEnter(other);
				else
					script->Instance->OnCollisionExit(other);
			};

			notify(a, b);
			notify(b, a);
		}

		m_PhysicsScene->ClearCollisionEvents();
	}

	void PhysicsSystem::OnUpdate(Timestep ts)
	{
		if (!m_PhysicsScene || !m_PhysicsScene->IsActive())
			return;

		ECS::SingletonAccessView<PhysicsSettings> settingsView{ m_Scene };
		const PhysicsSettings& settings = *settingsView.Get();
		const float fixedTimestep = settings.FixedTimestep;
		const int maxSteps = settings.MaxStepsPerFrame;

		m_Accumulator += ts;

		// Spiral-of-death guard
		int steps = 0;
		while (m_Accumulator >= fixedTimestep && steps < maxSteps)
		{
			m_PhysicsScene->Step(fixedTimestep);
			DispatchCollisionEvents();
			m_Accumulator -= fixedTimestep;
			steps++;
		}
		if (steps == maxSteps)
			m_Accumulator = 0.0f;

		const float alpha = m_Accumulator / fixedTimestep;
		m_PhysicsScene->SyncTransforms(&m_Scene, alpha);
	}
}
