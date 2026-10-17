#include "gepch.h"
#include "PhysicsSystem.h"

#include "GanymedE/ECS/Singleton.h"
#include "GanymedE/Net/NetTest.h"
#include "GanymedE/Net/World.h"
#include "GanymedE/Scene/Entity.h"
#include "GanymedE/Scene/Scene.h"
#include "GanymedE/Scene/SceneRole.h"
#include "GanymedE/Scene/SceneSingletons.h"
#include "GanymedE/Scripting/ScriptEngine.h"

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
		LuaScriptAccess luaScripts = View<LuaScriptAccess>();

		for (const auto& event : m_PhysicsScene->GetCollisionEvents())
		{
			Entity a = m_Scene.FindEntityByUUID(event.EntityA);
			Entity b = m_Scene.FindEntityByUUID(event.EntityB);
			if (!a || !b)
				continue;

			// Each side hears where it touched the *other*: the point on the other body's
			// surface, and that surface's outward normal, which faces the listener. A round
			// hitting a wall gets the spot on the wall to put its spark, the wall gets the spot
			// on the round. Jolt's normal points from A toward B, so B's surface faces -Normal
			// and A's faces +Normal.
			auto notify = [&](Entity self, Entity other, const glm::vec3& point, const glm::vec3& normal)
			{
				// Native and Lua scripts are independent: an entity may carry either,
				// both, or neither, and both hear about the same collision.
				auto script = scripts.FindOne<NativeScriptComponent>(self);
				if (script && script->Instance)
				{
					if (event.Entered)
						script->Instance->OnCollisionEnter(other);
					else
						script->Instance->OnCollisionExit(other);
				}

				if (luaScripts.Has(self))
				{
					if (event.Entered)
						ScriptEngine::OnCollisionEnter(self, other, point, normal);
					else
						ScriptEngine::OnCollisionExit(self, other);
				}
			};

			notify(a, b, event.PointOnB, -event.Normal);
			notify(b, a, event.PointOnA, event.Normal);
		}

		m_PhysicsScene->ClearCollisionEvents();
	}

	void PhysicsSystem::OnUpdate(Timestep ts)
	{
		if (!m_PhysicsScene || !m_PhysicsScene->IsActive())
			return;

		// Before anything else this frame: the command queue flushed at the top of FrameBegin,
		// so a prefab a script spawned last frame exists now and should simulate from this step
		// rather than the next one. Replicated client bodies are kinematic first, so SyncBodies
		// creates them that way and the step copies the interpolated transform in.
		m_PhysicsScene->AdoptReplicatedMotion(&m_Scene);
		m_PhysicsScene->SyncBodies(&m_Scene);
		Net::PredictLocal(m_Scene, m_PhysicsScene.get());

		ECS::SingletonAccessView<PhysicsSettings> settingsView{ m_Scene };
		const PhysicsSettings& settings = *settingsView.Get();
		const float fixedTimestep = settings.FixedTimestep;
		const int maxSteps = settings.MaxStepsPerFrame;

		m_Accumulator += ts;

		// Spiral-of-death guard
		int steps = 0;
		while (m_Accumulator >= fixedTimestep && steps < maxSteps)
		{
			Net::BeginStep(m_Scene, m_PhysicsScene.get());
			Net::Test::AfterScripts(m_Scene);
			m_PhysicsScene->Step(fixedTimestep);
			DispatchCollisionEvents();
			Net::NoteSimulatedStep(m_Scene);
			m_Accumulator -= fixedTimestep;
			steps++;

			// A snapshot is every second tick, not every frame. A hitch that burns several
			// steps would otherwise publish only the last tick and leave a hole the client's
			// 100 ms delay cannot cover. The pose captured here is the discrete tick;
			// the blend below is only the local render.
			if (m_Scene.GetRole() == SceneRole::Server)
			{
				m_PhysicsScene->SyncTransforms(&m_Scene, 1.0f);
				Net::CaptureWorld(m_Scene, m_PhysicsScene.get());
			}
		}
		if (steps == maxSteps)
			m_Accumulator = 0.0f;

		const float alpha = m_Accumulator / fixedTimestep;
		m_PhysicsScene->SyncTransforms(&m_Scene, alpha);
		Net::ApplyVisualError(m_Scene, ts);
	}
}
