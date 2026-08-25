#pragma once

#include "GanymedE/Core/Core.h"
#include "GanymedE/Core/Log.h"
#include "GanymedE/Scene/Scene.h"
#include "GanymedE/Scene/Entity.h"

#include "../EditorUndo.h"

#include <filesystem>
#include <string>

namespace GanymedE {

	class SceneHierarchyPanel
	{
	public:
		SceneHierarchyPanel() = default;
		SceneHierarchyPanel(const Ref<Scene>& scene);

		void SetContext(const Ref<Scene>& scene);

		// Where scene edits are recorded. EditorLayer passes its stack in Edit state and
		// nullptr in Play, so play-mode edits to the throwaway scene copy are structurally
		// unrecordable rather than filtered out somewhere downstream.
		void SetUndoStack(EditorUndoStack* stack) { m_UndoStack = stack; }

		void OnImGuiRender();

		Entity GetSelectedEntity() const { return m_SelectionContext; }
		void SetSelectedEntity(Entity entity) { m_SelectionContext = entity; }

		// Driven by the editor's Ctrl+D / Delete shortcuts as well as the context menus.
		void DuplicateSelectedEntity();
		void DeleteSelectedEntity();

		// Instantiates a .gprefab into the current scene (viewport drop, or the hierarchy's
		// blank-space menu) and selects the new instance root.
		Entity InstantiatePrefab(const std::filesystem::path& relativePath);
	private:
		void DrawEntityNode(Entity entity);
		void DrawComponents(Entity entity);

		// One inspector section. `uiFunction` returns whether any widget inside it edited the
		// component this frame - see the commit-boundary protocol in the .cpp.
		template<typename T, typename UIFunction>
		void DrawComponent(const std::string& name, Entity entity, UIFunction uiFunction);

		template<typename T>
		void DrawAddComponentEntry(const char* label);

		template<typename T>
		void TrackCommitBoundary(Entity entity, const std::string& name, const T& before,
			uint32_t activeOnEntry, uint32_t activeOnExit, bool edited);

		void CommitPendingEdit();
		void DiscardPendingEdit();
		void FlushPendingEdit();

		// Clears the selection when the entity behind it has been destroyed.
		void ValidateSelection();

		void DeleteEntity(Entity entity);

		// ---- Prefabs ----
		void CreatePrefabFrom(Entity entity);
		void ApplyToPrefab(Entity instanceRoot);
		void RevertInstance(Entity instanceRoot);
		void DrawPrefabControls(Entity entity);
		void DrawApplyPrefabModal();
		void Reparent(Entity child, Entity parent);
		void PushAddedEntities(std::string label, Entity root);

		bool Recording() const { return m_UndoStack != nullptr && m_Context != nullptr; }
	private:
		Ref<Scene> m_Context;
		Entity m_SelectionContext;

		EditorUndoStack* m_UndoStack = nullptr;

		// Deleting inside the hierarchy walk would destroy entities the enclosing entt view is
		// still iterating; the request is serviced after the walk instead.
		UUID m_EntityToDelete = UUID{ 0 };

		// "Apply to prefab" overwrites an asset and is not undoable, so it is the one operation
		// in the editor behind a confirmation. Deferred to the end of the frame because the
		// modal cannot open from inside the context-menu popup that requests it.
		UUID m_PendingApply = UUID{ 0 };
		bool m_OpenApplyModal = false;

		// The one inspector edit in flight. ImGui has a single active item, so one slot is
		// enough for the whole panel.
		struct PendingEdit
		{
			Scope<ComponentEditCommandBase> Command;   // before-value captured at its start
			uint32_t ActiveId = 0;                     // ImGuiID of the widget that owns it
			bool Edited = false;                       // any frame reported a real widget edit
			bool Visited = false;                      // its section was drawn this frame
		};
		PendingEdit m_Pending;
	};
}
