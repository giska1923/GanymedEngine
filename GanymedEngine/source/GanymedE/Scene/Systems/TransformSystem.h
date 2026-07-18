#pragma once

#include <unordered_set>

#include "GanymedE/ECS/System.h"
#include "GanymedE/ECS/Views.h"
#include "GanymedE/Scene/Components.h"

namespace GanymedE {

	class Entity;

	// Maintains the WorldTransformComponent cache (audit item A2).
	//
	// Rendering used to call Scene::GetWorldSpaceTransform once per entity per frame, each call
	// walking the whole parent chain — so an idle scene recomputed every world matrix, every frame,
	// for nothing. This system recomputes only the entities whose local transform or parenting
	// actually changed since it last looked, plus their descendants.
	class TransformSystem : public ECS::System<TransformSystem>
	{
	public:
		// Reacts to either input. WorldTransformComponent is untracked, so its slot is a plain
		// reference and needs no Modify() — nothing reacts to the cache itself.
		using DirtyView = ECS::ChangeView<ECS::EntityId,
			ECS::ReactRO<TransformComponent>,
			ECS::ReactRO<RelationshipComponent>,
			ECS::RW<WorldTransformComponent>>;

		using Views = TypeList<DirtyView>;

		using ECS::System<TransformSystem>::System;

		void OnUpdate(Timestep ts) override;
		void OnUpdateEditor(Timestep ts) override;
		const char* Name() const override { return "TransformSystem"; }

	private:
		void RecomputeDirty();

		// Writes `world` into the entity's cache, then walks its children with the freshly
		// computed matrix. Idempotent within a pass thanks to m_Visited.
		void RecomputeSubtree(Entity entity, const glm::mat4& world);

		// Full world matrix derived from local transforms up the parent chain. Deliberately does
		// not read any parent's cache: a parent may itself be dirty and not yet recomputed, and
		// deriving from locals makes the result correct regardless of the order the dirty list
		// happens to be in.
		glm::mat4 WorldFromLocals(Entity entity) const;

		std::unordered_set<entt::entity> m_Visited;
	};
}
