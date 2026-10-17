#include "gepch.h"
#include "Entity.h"

#include "Components.h"
#include "SceneRole.h"

#include "GanymedE/Net/World.h"

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

	bool Entity::IsLocallyControlled() const
	{
		if (!m_Scene || m_EntityHandle == entt::null)
			return false;
		if (m_Scene->GetRole() == SceneRole::Standalone)
			return true;
		if (m_Scene->GetRole() != SceneRole::Client)
			return false;
		return Net::Controls(GetUUID());
	}

}
