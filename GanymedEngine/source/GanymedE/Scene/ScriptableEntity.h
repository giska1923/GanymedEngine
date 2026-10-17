#pragma once

#include "Entity.h"

#include "GanymedE/Net/Command.h"

namespace GanymedE {

	class ScriptableEntity
	{
	public:
		virtual ~ScriptableEntity() {}

		template<typename T>
		T& GetComponent()
		{
			return m_Entity.GetComponent<T>();
		}

		// Once per physics step, before that step. The command is only valid for this call.
		void DispatchTick(uint32_t tick, const Net::PlayerCommand& command) { OnTick(tick, command); }
	protected:
		virtual void OnCreate() {}
		virtual void OnDestroy() {}
		virtual void OnUpdate(Timestep ts) {}
		virtual void OnTick(uint32_t tick, const Net::PlayerCommand& command)
		{
			(void)tick;
			(void)command;
		}
		virtual void OnCollisionEnter(Entity other) { (void)other; }
		virtual void OnCollisionExit(Entity other) { (void)other; }
	private:
		Entity m_Entity;

		// The lifecycle hooks above are protected; these are the systems that drive them.
		// (Scene stays a friend: it still owns entity identity and the registry.)
		friend class Scene;
		friend class NativeScriptSystem;
		friend class PhysicsSystem;
	};
}
