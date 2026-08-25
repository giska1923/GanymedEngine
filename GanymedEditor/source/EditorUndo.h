#pragma once

#include "GanymedE/Core/Core.h"
#include "GanymedE/Core/Log.h"
#include "GanymedE/Core/UUID.h"
#include "GanymedE/ECS/ComponentTraits.h"
#include "GanymedE/Scene/Components.h"
#include "GanymedE/Scene/Entity.h"
#include "GanymedE/Scene/Scene.h"

#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace GanymedE {

	// ---------------------------------------------------------------------------------------
	// Editor undo/redo.
	//
	// This lives editor-side, not in the engine: undo is an authoring concern and the runtime
	// has no consumer for one. (Unreal puts its transaction system in engine core; Ganymed
	// diverges here, matching how EditorCamera lives engine-side while the *editing model*
	// does not.)
	//
	// Scope, decided up front: SCENE edits only. Inspector property edits, add/remove
	// component, create/delete/duplicate entity, reparenting and gizmo drags are undoable.
	// Asset-level edits - material fields, registry writes - are deliberately NOT, because a
	// scene-local stack would lie about their scope: undoing an asset edit would silently
	// change every scene using it. See docs/toDo&done/CONTENT_AUTHORING_ROADMAP.md.
	//
	// Every command keys entities by UUID and resolves through Scene::FindEntityByUUID.
	// entt::entity handles are not validity-checked by Entity::operator bool and do not
	// survive a destroy/recreate cycle, so a raw handle in an undo record is a dangling
	// reference waiting for a redo. A command whose UUID no longer resolves warns and does
	// nothing - loud, not broken (the AnimationSystem unknown-clip posture).
	// ---------------------------------------------------------------------------------------

	class EditorCommand
	{
	public:
		virtual ~EditorCommand() = default;

		virtual void Undo(Scene& scene) = 0;
		virtual void Redo(Scene& scene) = 0;

		// Human-readable, e.g. "Edit Transform" or "Delete 'Fox'". Carried per command rather
		// than derived, because the entity it names may no longer exist when it is read.
		const std::string& Label() const { return m_Label; }

	protected:
		explicit EditorCommand(std::string label) : m_Label(std::move(label)) {}

		// Shared resolve-or-complain. Returns a falsy Entity after warning.
		static Entity Resolve(Scene& scene, UUID id, const char* what);

	private:
		std::string m_Label;
	};

	// ---- Snapshots -------------------------------------------------------------------------
	//
	// The snapshot currency is an in-memory component tuple, not YAML. That is lossless: it
	// round-trips AnimatorComponent::Time (which the serializer deliberately drops) and it
	// preserves the *absence* of a ScriptComponent field override, which is semantically
	// distinct from "present and equal to the default". It also keeps undo correctness from
	// depending on serializer completeness, and puts no string parsing on the undo path.
	//
	// It is built over ComponentList via ForEachType, so a new component type joins undo for
	// free. The YAML path could not promise that: its two per-component lists are
	// hand-maintained with no compile-time enforcement.

	namespace Detail {

		template<typename TL> struct OptionalTupleOf;
		template<typename... Ts> struct OptionalTupleOf<TypeList<Ts...>>
		{
			using Type = std::tuple<std::optional<Ts>...>;
		};

	}

	struct EntitySnapshot
	{
		UUID ID;
		UUID Parent;                    // 0 for a root
		size_t SiblingIndex = 0;        // position in the parent's Children, lost by SetParent
		std::string Tag;                // outside ComponentList - entity identity, not data

		typename Detail::OptionalTupleOf<ComponentList>::Type Components;
	};

	// Where `entity` sits in its parent's Children. 0 for a root: roots have no container, and
	// Phase 1's canonical save orders them by UUID, so there is no root order to restore.
	size_t SiblingIndexOf(Scene& scene, Entity entity);

	// Snapshots `root` and its descendants, parents-first (the canonical DFS order).
	void CaptureSubtree(Scene& scene, Entity root, std::vector<EntitySnapshot>& out);

	// Recreates a captured subtree with its original UUIDs and re-links it to the surviving
	// parent at the recorded sibling index. Destroys nothing.
	void RestoreSubtree(Scene& scene, const std::vector<EntitySnapshot>& snapshots);

	// Destroys a captured subtree, children-first.
	void RemoveSubtree(Scene& scene, const std::vector<EntitySnapshot>& snapshots);

	// ---- Component edits -------------------------------------------------------------------

	// A component edit whose after-value is not known when the command is created: the
	// inspector captures the before-value the frame a widget grabs the ImGui active item, and
	// the after-value the frame it lets go. One command per drag, never one per tick.
	class ComponentEditCommandBase : public EditorCommand
	{
	public:
		virtual void CaptureAfter(Scene& scene) = 0;

		UUID GetEntity() const { return m_Entity; }
		entt::id_type GetComponentType() const { return m_ComponentType; }

	protected:
		ComponentEditCommandBase(std::string label, UUID entity, entt::id_type componentType)
			: EditorCommand(std::move(label)), m_Entity(entity), m_ComponentType(componentType) {}

		UUID m_Entity;
		entt::id_type m_ComponentType;
	};

	template<typename T>
	class ComponentEditCommand : public ComponentEditCommandBase
	{
	public:
		ComponentEditCommand(std::string label, UUID entity, const T& before, const T& after)
			: ComponentEditCommandBase(std::move(label), entity, entt::type_hash<T>::value())
			, m_Before(before), m_After(after) {}

		ComponentEditCommand(std::string label, UUID entity, const T& before)
			: ComponentEditCommandBase(std::move(label), entity, entt::type_hash<T>::value())
			, m_Before(before), m_After(before) {}

		void Undo(Scene& scene) override { Apply(scene, m_Before); }
		void Redo(Scene& scene) override { Apply(scene, m_After); }

		void CaptureAfter(Scene& scene) override
		{
			Entity entity = Resolve(scene, m_Entity, Label().c_str());
			if (entity && entity.HasComponent<T>())
				m_After = entity.GetComponent<T>();
		}

	private:
		void Apply(Scene& scene, const T& value)
		{
			Entity entity = Resolve(scene, m_Entity, Label().c_str());
			if (!entity || !entity.HasComponent<T>())
				return;

			entity.GetComponent<T>() = value;

			// Restoring a tracked component behind the change-tracker's back is the silent
			// staleness trap: the value moves, nothing recomputes, and the world transform
			// keeps its pre-undo matrix until something unrelated dirties the entity.
			if constexpr (ComponentTraits<T>::TrackChanges)
				scene.MarkChanged<T>(entity);
		}

		T m_Before;
		T m_After;
	};

	template<typename T>
	class AddComponentCommand : public EditorCommand
	{
	public:
		AddComponentCommand(std::string label, UUID entity, const T& value)
			: EditorCommand(std::move(label)), m_Entity(entity), m_Value(value) {}

		void Undo(Scene& scene) override
		{
			Entity entity = Resolve(scene, m_Entity, Label().c_str());
			if (entity && entity.HasComponent<T>())
				entity.RemoveComponent<T>();
		}

		void Redo(Scene& scene) override
		{
			Entity entity = Resolve(scene, m_Entity, Label().c_str());
			if (!entity || entity.HasComponent<T>())
				return;

			entity.AddComponent<T>() = m_Value;
			if constexpr (ComponentTraits<T>::TrackChanges)
				scene.MarkChanged<T>(entity);
		}

	private:
		UUID m_Entity;
		T m_Value;
	};

	// Stores the whole removed value, so undo is a re-add rather than a default-construct.
	//
	// Side effect worth knowing: re-adding a ScriptComponent fires its init path, so the Lua
	// instance is created afresh. That is correct - the component genuinely came back - but it
	// is not a pure value restore, and a script with side effects in its constructor will run
	// them again.
	template<typename T>
	class RemoveComponentCommand : public EditorCommand
	{
	public:
		RemoveComponentCommand(std::string label, UUID entity, const T& value)
			: EditorCommand(std::move(label)), m_Entity(entity), m_Value(value) {}

		void Undo(Scene& scene) override
		{
			Entity entity = Resolve(scene, m_Entity, Label().c_str());
			if (!entity || entity.HasComponent<T>())
				return;

			entity.AddComponent<T>() = m_Value;
			if constexpr (ComponentTraits<T>::TrackChanges)
				scene.MarkChanged<T>(entity);
		}

		void Redo(Scene& scene) override
		{
			Entity entity = Resolve(scene, m_Entity, Label().c_str());
			if (entity && entity.HasComponent<T>())
				entity.RemoveComponent<T>();
		}

	private:
		UUID m_Entity;
		T m_Value;
	};

	// ---- Entity structure ------------------------------------------------------------------

	// One mechanism for every "a subtree appeared / disappeared" edit, differing only in which
	// direction Undo runs. Create, Duplicate and (Phase 4) prefab instantiation are all
	// AddEntitiesCommand; the editor's recursive delete and prefab Revert are
	// DeleteEntitiesCommand. Redoing an add replays the snapshot rather than re-running the
	// operation, which is what guarantees the same UUIDs every time.
	class EntitySubtreeCommand : public EditorCommand
	{
	public:
		const std::vector<EntitySnapshot>& Snapshots() const { return m_Snapshots; }

	protected:
		EntitySubtreeCommand(std::string label, std::vector<EntitySnapshot> snapshots)
			: EditorCommand(std::move(label)), m_Snapshots(std::move(snapshots)) {}

		std::vector<EntitySnapshot> m_Snapshots;
	};

	class AddEntitiesCommand : public EntitySubtreeCommand
	{
	public:
		AddEntitiesCommand(std::string label, std::vector<EntitySnapshot> snapshots)
			: EntitySubtreeCommand(std::move(label), std::move(snapshots)) {}

		void Undo(Scene& scene) override { RemoveSubtree(scene, m_Snapshots); }
		void Redo(Scene& scene) override { RestoreSubtree(scene, m_Snapshots); }
	};

	class DeleteEntitiesCommand : public EntitySubtreeCommand
	{
	public:
		DeleteEntitiesCommand(std::string label, std::vector<EntitySnapshot> snapshots)
			: EntitySubtreeCommand(std::move(label), std::move(snapshots)) {}

		void Undo(Scene& scene) override { RestoreSubtree(scene, m_Snapshots); }
		void Redo(Scene& scene) override { RemoveSubtree(scene, m_Snapshots); }
	};

	// The old sibling index is recorded explicitly because nothing else remembers it:
	// Scene::SetParent push_backs, so undoing a reparent without it silently moves the entity
	// to the end of its old parent's Children - and since Phase 1, sibling order decides the
	// order the scene file is written in, so that is a content change, not a cosmetic one.
	class ReparentCommand : public EditorCommand
	{
	public:
		ReparentCommand(std::string label, UUID entity, UUID oldParent, size_t oldSiblingIndex,
			UUID newParent, size_t newSiblingIndex)
			: EditorCommand(std::move(label)), m_Entity(entity)
			, m_OldParent(oldParent), m_OldSiblingIndex(oldSiblingIndex)
			, m_NewParent(newParent), m_NewSiblingIndex(newSiblingIndex) {}

		void Undo(Scene& scene) override;
		void Redo(Scene& scene) override;

	private:
		UUID m_Entity;
		UUID m_OldParent;
		size_t m_OldSiblingIndex;
		UUID m_NewParent;
		size_t m_NewSiblingIndex;
	};

	// Re-parents `child` under `parent` and then places it at `siblingIndex` in that parent's
	// Children. Scene::SetParent alone cannot express the index.
	void SetParentAtIndex(Scene& scene, Entity child, Entity parent, size_t siblingIndex);

	// ---- The stack -------------------------------------------------------------------------

	class EditorUndoStack
	{
	public:
		void Push(Scope<EditorCommand> command);   // clears the redo stack

		void Undo(Scene& scene);
		void Redo(Scene& scene);

		bool CanUndo() const { return !m_Undo.empty(); }
		bool CanRedo() const { return !m_Redo.empty(); }

		// Both stacks are meaningless across a scene switch: OpenScene/NewScene construct a new
		// Scene object, so every UUID recorded here names an entity that no longer exists.
		void Clear();

		size_t UndoDepth() const { return m_Undo.size(); }
		size_t RedoDepth() const { return m_Redo.size(); }

		// Dirty tracking by stack position rather than a flag, so undoing *back to* the saved
		// state correctly clears the indicator - the property an ad-hoc bool always gets wrong.
		void MarkSaved() { m_SavedDepth = (int64_t)m_Undo.size(); }
		bool IsDirtySinceSave() const { return m_SavedDepth != (int64_t)m_Undo.size(); }

		static constexpr size_t MaxDepth = 100;

	private:
		std::vector<Scope<EditorCommand>> m_Undo;
		std::vector<Scope<EditorCommand>> m_Redo;

		// -1 once the saved state has been dropped off the bottom of the stack: it can no
		// longer be reached by undoing, so the scene is dirty from here on.
		int64_t m_SavedDepth = 0;
	};

}
