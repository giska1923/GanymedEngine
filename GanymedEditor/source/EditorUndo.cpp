#include "EditorUndo.h"

#include <algorithm>
#include <unordered_set>

namespace GanymedE {

	Entity EditorCommand::Resolve(Scene& scene, UUID id, const char* what)
	{
		Entity entity = scene.FindEntityByUUID(id);
		if (!entity)
		{
			GE_WARN("Undo: '{0}' refers to entity {1}, which no longer exists - skipping.",
				what, static_cast<uint64_t>(id));
		}

		return entity;
	}

	// ---- Snapshots -------------------------------------------------------------------------

	size_t SiblingIndexOf(Scene& scene, Entity entity)
	{
		if (!entity)
			return 0;

		const auto& relationship = entity.GetComponent<RelationshipComponent>();
		if (relationship.Parent == UUID{ 0 })
			return 0;

		Entity parent = scene.FindEntityByUUID(relationship.Parent);
		if (!parent)
			return 0;

		const auto& children = parent.GetComponent<RelationshipComponent>().Children;
		auto it = std::find(children.begin(), children.end(), entity.GetUUID());
		return it == children.end() ? children.size() : (size_t)(it - children.begin());
	}

	void CaptureSubtree(Scene& scene, Entity root, std::vector<EntitySnapshot>& out)
	{
		if (!root)
			return;

		std::vector<Entity> subtree;
		std::unordered_set<UUID> visited;
		scene.CollectSubtree(root, subtree, visited);

		out.reserve(out.size() + subtree.size());
		for (Entity entity : subtree)
		{
			EntitySnapshot snapshot;
			snapshot.ID = entity.GetUUID();
			snapshot.Parent = entity.GetComponent<RelationshipComponent>().Parent;
			snapshot.SiblingIndex = SiblingIndexOf(scene, entity);
			snapshot.Tag = entity.GetComponent<TagComponent>().Tag;

			ForEachType(ComponentList{}, [&](auto typeTag)
			{
				using T = typename decltype(typeTag)::Type;
				if (entity.HasComponent<T>())
					std::get<std::optional<T>>(snapshot.Components) = entity.GetComponent<T>();
			});

			out.push_back(std::move(snapshot));
		}
	}

	void RestoreSubtree(Scene& scene, const std::vector<EntitySnapshot>& snapshots)
	{
		if (snapshots.empty())
			return;

		// Two passes: RelationshipComponent names entities by UUID, so every entity in the
		// subtree has to exist before any of them can be linked up.
		for (const EntitySnapshot& snapshot : snapshots)
		{
			if (scene.FindEntityByUUID(snapshot.ID))
			{
				GE_WARN("Undo: entity {0} ('{1}') already exists - not recreating it.",
					static_cast<uint64_t>(snapshot.ID), snapshot.Tag);
				continue;
			}

			scene.CreateEntityWithUUID(snapshot.ID, snapshot.Tag);
		}

		for (const EntitySnapshot& snapshot : snapshots)
		{
			Entity entity = scene.FindEntityByUUID(snapshot.ID);
			if (!entity)
				continue;

			ForEachType(ComponentList{}, [&](auto typeTag)
			{
				using T = typename decltype(typeTag)::Type;
				const auto& stored = std::get<std::optional<T>>(snapshot.Components);

				if (stored.has_value())
					scene.Reg().emplace_or_replace<T>((entt::entity)entity, *stored);
				else if (entity.HasComponent<T>())
					entity.RemoveComponent<T>();

				// emplace_or_replace does not fire the on_construct signal the change tracker
				// hooks, so a restored transform would never reach TransformSystem.
				if constexpr (ComponentTraits<T>::TrackChanges)
					scene.MarkChanged<T>(entity);
			});

			// A native script instance belonged to the destroyed entity and was deleted with
			// it; the pointer in the snapshot is stale. NativeScriptSystem re-instantiates.
			if (auto* nsc = scene.Reg().try_get<NativeScriptComponent>((entt::entity)entity))
				nsc->Instance = nullptr;
		}

		// The subtree's own links came back with the components. What did not is the surviving
		// parent's Children vector: it lost the root when the subtree was destroyed.
		const EntitySnapshot& rootSnapshot = snapshots.front();
		Entity root = scene.FindEntityByUUID(rootSnapshot.ID);
		if (!root || rootSnapshot.Parent == UUID{ 0 })
			return;

		Entity parent = scene.FindEntityByUUID(rootSnapshot.Parent);
		if (!parent)
		{
			// The parent was destroyed after the snapshot was taken. Leaving Parent pointing at
			// it would produce exactly the dangling reference Phase 1's save-time walk warns
			// about, so drop back to being a root.
			root.GetComponent<RelationshipComponent>().Parent = UUID{ 0 };
			scene.MarkChanged<RelationshipComponent>(root);
			return;
		}

		auto& children = parent.GetComponent<RelationshipComponent>().Children;
		if (std::find(children.begin(), children.end(), root.GetUUID()) == children.end())
		{
			const size_t index = std::min(rootSnapshot.SiblingIndex, children.size());
			children.insert(children.begin() + index, root.GetUUID());
			scene.MarkChanged<RelationshipComponent>(parent);
		}
	}

	void RemoveSubtree(Scene& scene, const std::vector<EntitySnapshot>& snapshots)
	{
		// Children-first. Scene::DestroyEntity unparents a destroyed entity's children to root
		// rather than destroying them, so removing a parent first would strand its descendants
		// as roots for the rest of the loop - and any that failed to resolve would stay.
		for (auto it = snapshots.rbegin(); it != snapshots.rend(); ++it)
		{
			Entity entity = scene.FindEntityByUUID(it->ID);
			if (entity)
				scene.DestroyEntity(entity);
		}
	}

	// ---- Reparenting -----------------------------------------------------------------------

	void SetParentAtIndex(Scene& scene, Entity child, Entity parent, size_t siblingIndex)
	{
		if (!child)
			return;

		scene.SetParent(child, parent);

		// SetParent silently no-ops on a cycle (parenting to self or to a descendant), so the
		// index fix-up has to check that the move actually happened rather than assume it.
		if (!parent)
			return;

		const UUID expected = parent.GetUUID();
		if (child.GetComponent<RelationshipComponent>().Parent != expected)
			return;

		auto& children = parent.GetComponent<RelationshipComponent>().Children;
		auto it = std::find(children.begin(), children.end(), child.GetUUID());
		if (it == children.end())
			return;

		children.erase(it);
		children.insert(children.begin() + std::min(siblingIndex, children.size()), child.GetUUID());
		scene.MarkChanged<RelationshipComponent>(parent);
	}

	void ReparentCommand::Undo(Scene& scene)
	{
		Entity entity = Resolve(scene, m_Entity, Label().c_str());
		if (!entity)
			return;

		SetParentAtIndex(scene, entity, scene.FindEntityByUUID(m_OldParent), m_OldSiblingIndex);
	}

	void ReparentCommand::Redo(Scene& scene)
	{
		Entity entity = Resolve(scene, m_Entity, Label().c_str());
		if (!entity)
			return;

		SetParentAtIndex(scene, entity, scene.FindEntityByUUID(m_NewParent), m_NewSiblingIndex);
	}

	// ---- The stack -------------------------------------------------------------------------

	void EditorUndoStack::Push(Scope<EditorCommand> command)
	{
		if (!command)
			return;

		GE_TRACE("Undo: pushed '{0}' (depth {1})", command->Label(), m_Undo.size() + 1);

		m_Undo.push_back(std::move(command));
		m_Redo.clear();

		if (m_Undo.size() > MaxDepth)
		{
			m_Undo.erase(m_Undo.begin());

			// The saved state moved one slot closer to the bottom; once it falls off, no amount
			// of undoing can reach it again and the scene stays dirty until the next save.
			m_SavedDepth = m_SavedDepth > 0 ? m_SavedDepth - 1 : -1;
		}
	}

	void EditorUndoStack::Undo(Scene& scene)
	{
		if (m_Undo.empty())
			return;

		Scope<EditorCommand> command = std::move(m_Undo.back());
		m_Undo.pop_back();

		GE_TRACE("Undo: '{0}'", command->Label());
		command->Undo(scene);
		m_Redo.push_back(std::move(command));
	}

	void EditorUndoStack::Redo(Scene& scene)
	{
		if (m_Redo.empty())
			return;

		Scope<EditorCommand> command = std::move(m_Redo.back());
		m_Redo.pop_back();

		GE_TRACE("Redo: '{0}'", command->Label());
		command->Redo(scene);
		m_Undo.push_back(std::move(command));
	}

	void EditorUndoStack::Clear()
	{
		m_Undo.clear();
		m_Redo.clear();
		m_SavedDepth = 0;
	}

}
