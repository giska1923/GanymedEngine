#include "gepch.h"
#include "Entity.h"

namespace GanymedE {

	Entity::Entity(entt::entity handle, Scene* scene)
		: m_EntityHandle(handle), m_Scene(scene)
	{
	}
}