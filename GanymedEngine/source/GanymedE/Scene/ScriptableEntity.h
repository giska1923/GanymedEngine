#pragma once

#include "Entity.h"

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
	protected:
		virtual void OnCreate() {}
		virtual void OnDestroy() {}
		virtual void OnUpdate(Timestep ts) {}
		virtual void OnCollisionEnter(Entity other) { (void)other; }
		virtual void OnCollisionExit(Entity other) { (void)other; }
	private:
		Entity m_Entity;
		friend class Scene;
	};
}
