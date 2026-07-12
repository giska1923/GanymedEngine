#include "gepch.h"
#include "Entity.h"

#include "Components.h"

namespace GanymedE {

	Entity::Entity(entt::entity handle, Scene* scene)
		: m_EntityHandle(handle), m_Scene(scene)
	{
	}

	UUID Entity::GetUUID() const
	{
		return GetComponent<IDComponent>().ID;
	}

	const std::string& Entity::GetName() const
	{
		return GetComponent<TagComponent>().Tag;
	}

}
