#include "SceneHierarchyPanel.h"
#include "../AssetDragDrop.h"
#include "../EditorFonts.h"
#include "../EditorIcons.h"
#include "../EditorInspector.h"
#include "../EditorPrefabOverrides.h"
#include "../EditorTheme.h"
#include "../EditorWidgets.h"

#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>

#include <glm/gtc/type_ptr.hpp>

// PRIu64. uint64_t is `unsigned long` on LP64 (Linux, macOS) and `unsigned long long` on
// Windows, so no single printf conversion spells it on both - and UUID's conversion operator is
// explicit, which in direct-initialization only yields uint64_t exactly. The macro is the
// portable spelling; <cstdint>'s fixed-width types have no other one.
#include <cinttypes>

#include "GanymedE/Scene/Components.h"
#include "GanymedE/Assets/AssetManager.h"
#include "GanymedE/Assets/AssetPaths.h"
#include "GanymedE/Renderer/Material.h"
#include "GanymedE/Renderer/Mesh.h"
#include "GanymedE/Renderer/MeshImporter.h"
#include "GanymedE/Scene/PrefabSerializer.h"
#include "GanymedE/Scripting/ScriptEngine.h"
#include "GanymedE/Utils/PlatformUtils.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <type_traits>
#include <unordered_set>

namespace GanymedE {

	namespace {

		constexpr float kOutlinerActionCol = 26.0f;

		bool TagContainsI(const std::string& tag, const char* filter)
		{
			if (!filter || !filter[0])
				return true;
			const char* hay = tag.c_str();
			for (; *hay; ++hay)
			{
				const char* h = hay;
				const char* n = filter;
				while (*h && *n &&
					std::tolower(static_cast<unsigned char>(*h)) ==
					std::tolower(static_cast<unsigned char>(*n)))
				{
					++h;
					++n;
				}
				if (!*n)
					return true;
			}
			return false;
		}

		void DominantIcon(const Entity& entity, const char*& icon, ImU32& tint)
		{
			using EditorUI::Theme;
			const EditorUI::EditorTheme& theme = Theme();
			icon = ICON_LC_BOX;
			tint = theme.AssetTint[static_cast<int>(AssetType::None)];

			if (entity.HasComponent<PrefabInstanceComponent>())
			{
				icon = ICON_LC_PACKAGE;
				tint = theme.AssetTint[static_cast<int>(AssetType::Prefab)];
			}
			else if (entity.HasComponent<ScatterGroupComponent>())
			{
				icon = ICON_LC_SPRAY_CAN;
			}
			else if (entity.HasComponent<MarkerComponent>())
			{
				icon = ICON_LC_MAP_PIN;
				const glm::vec4& c = entity.GetComponent<MarkerComponent>().Color;
				tint = ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, 1.0f));
			}
			else if (entity.HasComponent<CameraComponent>())
			{
				icon = ICON_LC_CAMERA;
				tint = theme.TextPrimary;
			}
			else if (entity.HasComponent<DirectionalLightComponent>() ||
				entity.HasComponent<PointLightComponent>() ||
				entity.HasComponent<SpotLightComponent>())
			{
				icon = ICON_LC_LIGHTBULB;
				tint = theme.AssetTint[static_cast<int>(AssetType::Environment)];
			}
			else if (entity.HasComponent<SkyLightComponent>())
			{
				icon = ICON_LC_SUN;
				tint = theme.AssetTint[static_cast<int>(AssetType::Environment)];
			}
			else if (entity.HasComponent<AudioSourceComponent>() ||
				entity.HasComponent<AudioListenerComponent>())
			{
				icon = ICON_LC_VOLUME_2;
				tint = theme.AssetTint[static_cast<int>(AssetType::Audio)];
			}
			else if (entity.HasComponent<ParticleEmitterComponent>())
			{
				icon = ICON_LC_SPARKLES;
				tint = theme.AssetTint[static_cast<int>(AssetType::Material)];
			}
			else if (entity.HasComponent<StaticMeshComponent>())
			{
				icon = ICON_LC_BOX;
				tint = theme.AssetTint[static_cast<int>(AssetType::StaticMesh)];
			}
			else if (entity.HasComponent<SpriteRendererComponent>())
			{
				icon = ICON_LC_IMAGE;
				tint = theme.AssetTint[static_cast<int>(AssetType::Texture)];
			}
			else if (entity.HasComponent<ScriptComponent>() ||
				entity.HasComponent<NativeScriptComponent>())
			{
				icon = ICON_LC_FILE_CODE;
				tint = theme.AssetTint[static_cast<int>(AssetType::Script)];
			}
		}

		// Per-T, not the entity's dominant component: a mesh entity's Transform header
		// still shows the move glyph. Tints reuse the Content Browser AssetTint map
		// where the type has a matching asset class.
		template<typename T>
		void ComponentTypeChrome(const char*& icon, ImU32& tint)
		{
			using EditorUI::Theme;
			const EditorUI::EditorTheme& theme = Theme();
			icon = ICON_LC_BOX;
			tint = theme.TextPrimary;

			if constexpr (std::is_same_v<T, TransformComponent>)
			{
				icon = ICON_LC_MOVE;
			}
			else if constexpr (std::is_same_v<T, PrefabInstanceComponent>)
			{
				icon = ICON_LC_PACKAGE;
				tint = theme.AssetTint[static_cast<int>(AssetType::Prefab)];
			}
			else if constexpr (std::is_same_v<T, ScatterGroupComponent>)
			{
				icon = ICON_LC_SPRAY_CAN;
			}
			else if constexpr (std::is_same_v<T, MarkerComponent>)
			{
				icon = ICON_LC_MAP_PIN;
			}
			else if constexpr (std::is_same_v<T, CameraComponent>)
			{
				icon = ICON_LC_CAMERA;
			}
			else if constexpr (std::is_same_v<T, SpriteRendererComponent>)
			{
				icon = ICON_LC_IMAGE;
				tint = theme.AssetTint[static_cast<int>(AssetType::Texture)];
			}
			else if constexpr (std::is_same_v<T, StaticMeshComponent>)
			{
				icon = ICON_LC_BOX;
				tint = theme.AssetTint[static_cast<int>(AssetType::StaticMesh)];
			}
			else if constexpr (std::is_same_v<T, AnimatorComponent>)
			{
				icon = ICON_LC_BONE;
			}
			else if constexpr (std::is_same_v<T, BoneAttachmentComponent>)
			{
				icon = ICON_LC_ANCHOR;
			}
			else if constexpr (std::is_same_v<T, ScriptComponent>)
			{
				icon = ICON_LC_FILE_CODE;
				tint = theme.AssetTint[static_cast<int>(AssetType::Script)];
			}
			else if constexpr (std::is_same_v<T, DirectionalLightComponent> ||
				std::is_same_v<T, PointLightComponent> ||
				std::is_same_v<T, SpotLightComponent>)
			{
				icon = ICON_LC_LIGHTBULB;
				tint = theme.AssetTint[static_cast<int>(AssetType::Environment)];
			}
			else if constexpr (std::is_same_v<T, SkyLightComponent>)
			{
				icon = ICON_LC_SUN;
				tint = theme.AssetTint[static_cast<int>(AssetType::Environment)];
			}
			else if constexpr (std::is_same_v<T, AudioSourceComponent> ||
				std::is_same_v<T, AudioListenerComponent>)
			{
				icon = ICON_LC_VOLUME_2;
				tint = theme.AssetTint[static_cast<int>(AssetType::Audio)];
			}
			else if constexpr (std::is_same_v<T, ParticleEmitterComponent>)
			{
				icon = ICON_LC_SPARKLES;
				tint = theme.AssetTint[static_cast<int>(AssetType::Material)];
			}
			else if constexpr (std::is_same_v<T, RigidBodyComponent>)
			{
				icon = ICON_LC_WEIGHT;
			}
			else if constexpr (std::is_same_v<T, BoxColliderComponent>)
			{
				icon = ICON_LC_CUBOID;
			}
			else if constexpr (std::is_same_v<T, SphereColliderComponent>)
			{
				icon = ICON_LC_CIRCLE;
			}
			else if constexpr (std::is_same_v<T, CapsuleColliderComponent>)
			{
				icon = ICON_LC_CYLINDER;
			}
		}

	}

	SceneHierarchyPanel::SceneHierarchyPanel(const Ref<Scene>& context)
	{
		SetContext(context);
	}

	void SceneHierarchyPanel::SetContext(const Ref<Scene>& context)
	{
		m_Context = context;
		SelectSingle({});

		// Hidden/locked are UUID sets on this panel, not scene state. RetargetPanels
		// (play/stop) must not clear them: the copied scene reuses the same IDs.
		// New/Open call ClearEditorViewState.

		// A pending edit names an entity in the scene we are leaving.
		DiscardPendingEdit();
	}

	// Undo can destroy the selected entity behind the panel's back - Ctrl+Z on a "Create
	// Entity" is exactly that - and Entity::operator bool does not check registry validity, so
	// the stale handle would look live all the way into GetComponent. Everything else keys on
	// UUID for this reason; the selection is the one place that has to hold a handle, so it is
	// checked once a frame instead.
	void SceneHierarchyPanel::ValidateSelection()
	{
		if (!m_SelectionContext)
			return;

		if (!m_Context || !m_Context->Reg().valid((entt::entity)m_SelectionContext))
			SelectSingle({});
	}

	void SceneHierarchyPanel::OnImGuiRender()
	{
		ValidateSelection();
		RebuildFilterVisibility();

		using EditorUI::BeginPanel;
		using EditorUI::EndPanel;
		using EditorUI::PanelToolbarRow;
		using EditorUI::EndPanelToolbarRow;
		using EditorUI::SearchField;
		using EditorUI::ColumnHeaderRow;
		using EditorUI::IconButton;

		if (BeginPanel("Scene Hierarchy"))
		{
			if (PanelToolbarRow("##OutlinerTB"))
			{
				if (IconButton(ICON_LC_PLUS, "Create"))
					ImGui::OpenPopup("##CreateEntity");
				ImGui::SameLine();
				SearchField("filter", m_Search, sizeof(m_Search), "Search...");
				if (ImGui::BeginPopup("##CreateEntity"))
				{
					DrawCreateMenu();
					ImGui::EndPopup();
				}
			}
			EndPanelToolbarRow();

			ColumnHeaderRow({
				{ "Name", 0.0f },
				{ ICON_LC_EYE, kOutlinerActionCol },
				{ ICON_LC_LOCK, kOutlinerActionCol },
				{ ICON_LC_LINK, kOutlinerActionCol }
			});

			ImGui::BeginChild("##OutlinerTree", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
			if (m_Context)
			{
				m_VisibleOrder.clear();

				auto view = m_Context->m_Registry.view<IDComponent, RelationshipComponent, TagComponent>();
				for (auto entityID : view)
				{
					Entity entity{ entityID, m_Context.get() };
					if (entity.GetComponent<RelationshipComponent>().Parent == UUID{ 0 })
						DrawEntityNode(entity);
				}

				// After the walk, when m_VisibleOrder is complete. See m_PendingRange.
				if (m_PendingRange)
				{
					SelectRange(m_PendingRange);
					m_PendingRange = {};
				}

				if (m_EntityToDelete != UUID{ 0 })
				{
					DeleteEntity(m_Context->FindEntityByUUID(m_EntityToDelete));
					m_EntityToDelete = UUID{ 0 };
				}

				// Click empty space to deselect. `!IsAnyItemHovered()` is what makes it *empty*
				// space: without it this fires on any held-mouse frame where no item happens to own
				// ActiveId, which includes the frame after a tree node was clicked - so a selection
				// made by that very click could be wiped by this line a moment later. Harmless for a
				// plain click, which re-selects one entity anyway, and visibly wrong for a
				// shift-range, which is how it was found.
				if (ImGui::IsMouseDown(0) && ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered())
					SelectSingle({});

				if (ImGui::BeginDragDropTarget())
				{
					if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SCENE_HIERARCHY_ENTITY"))
					{
						UUID droppedID = *(const UUID*)payload->Data;
						Entity dropped = m_Context->FindEntityByUUID(droppedID);
						if (dropped)
							Reparent(dropped, {});
					}
					ImGui::EndDragDropTarget();
				}

				if (ImGui::BeginPopupContextWindow(0, ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
				{
					DrawCreateMenu();
					ImGui::EndPopup();
				}
			}
			ImGui::EndChild();
		}
		EndPanel();

		if (BeginPanel("Properties"))
		{
			if (m_SelectionContext)
			{
				ImGui::Dummy(ImVec2(0.0f, 6.0f));
				ImGui::Indent(8.0f);
				DrawPrefabControls(m_SelectionContext);
				ImGui::Unindent(8.0f);
				DrawComponents(m_SelectionContext);
			}
		}
		EndPanel();

		DrawApplyPrefabModal();

		FlushPendingEdit();
	}

	bool SceneHierarchyPanel::IsSelected(Entity entity) const
	{
		return std::find(m_Selection.begin(), m_Selection.end(), entity) != m_Selection.end();
	}

	void SceneHierarchyPanel::SelectSingle(Entity entity)
	{
		m_SelectionContext = entity;
		m_RangeAnchor = entity;

		m_Selection.clear();
		if (entity)
			m_Selection.push_back(entity);
	}

	// Shift+click: everything between the anchor and `to` in the order the tree is drawn.
	//
	// Replaces the selection rather than adding to it, so shift-clicking twice gives the second
	// range and not the union of both - the behaviour a file browser has, and the one that makes
	// a mis-aimed range recoverable by aiming again.
	void SceneHierarchyPanel::SelectRange(Entity to)
	{
		if (!to || !m_RangeAnchor)
			return;

		const auto anchorIt = std::find(m_VisibleOrder.begin(), m_VisibleOrder.end(), m_RangeAnchor);
		const auto toIt = std::find(m_VisibleOrder.begin(), m_VisibleOrder.end(), to);

		// Either end can be missing: an ancestor of the anchor may have been collapsed since it
		// was set, which takes it out of the drawn tree. Falling back to a plain single select is
		// better than selecting a range measured from something the author cannot see.
		if (anchorIt == m_VisibleOrder.end() || toIt == m_VisibleOrder.end())
		{
			SelectSingle(to);
			return;
		}

		auto first = anchorIt;
		auto last = toIt;
		if (first > last)
			std::swap(first, last);

		m_Selection.clear();

		// `to` first: the primary is what the author just clicked, which is what the gizmo grabs
		// and what the inspector draws. The anchor keeps its value so the next shift-click
		// re-measures from the same place.
		m_Selection.push_back(to);
		for (auto it = first; it <= last; ++it)
		{
			if (*it != to)
				m_Selection.push_back(*it);
		}

		m_SelectionContext = to;
	}

	// Ctrl+click. The primary stays the entity clicked *last*, because everything single-entity
	// in the editor reads GetSelectedEntity() and an author expects the thing they just clicked
	// to be the one the gizmo grabs.
	void SceneHierarchyPanel::ToggleSelection(Entity entity)
	{
		if (!entity)
			return;

		auto it = std::find(m_Selection.begin(), m_Selection.end(), entity);
		if (it != m_Selection.end())
		{
			m_Selection.erase(it);
			m_SelectionContext = m_Selection.empty() ? Entity{} : m_Selection.front();
			return;
		}

		m_Selection.insert(m_Selection.begin(), entity);
		m_SelectionContext = entity;
		m_RangeAnchor = entity;
	}

	void SceneHierarchyPanel::DrawEntityNode(Entity entity)
	{
		const UUID id = entity.GetUUID();
		if (m_Search[0] && m_FilterVisible.count(id) == 0)
			return;

		auto& tag = entity.GetComponent<TagComponent>().Tag;
		auto& relationship = entity.GetComponent<RelationshipComponent>();
		const EditorUI::EditorTheme& theme = EditorUI::Theme();

		const bool filtering = m_Search[0] != '\0';
		const bool selfMatch = !filtering || TagContainsI(tag, m_Search);
		bool hasVisibleChild = false;
		if (!filtering)
			hasVisibleChild = !relationship.Children.empty();
		else
		{
			for (UUID childID : relationship.Children)
			{
				if (m_FilterVisible.count(childID))
				{
					hasVisibleChild = true;
					break;
				}
			}
		}

		if (filtering && !selfMatch && hasVisibleChild)
			ImGui::SetNextItemOpen(true, ImGuiCond_Always);

		ImGui::PushID((int32_t)(entt::entity)entity);

		const bool selected = IsSelected(entity);
		const bool primary = selected && entity == m_SelectionContext;
		const bool hidden = m_Hidden.count(id) != 0;
		const bool locked = m_Locked.count(id) != 0;
		const bool prefab = entity.HasComponent<PrefabInstanceComponent>();

		ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow
			| ImGuiTreeNodeFlags_SpanAvailWidth
			| ImGuiTreeNodeFlags_AllowOverlap
			| ImGuiTreeNodeFlags_FramePadding;
		if (selected)
			flags |= ImGuiTreeNodeFlags_Selected;
		if (!hasVisibleChild)
			flags |= ImGuiTreeNodeFlags_Leaf;

		m_VisibleOrder.push_back(entity);

		if (primary)
		{
			ImGui::PushStyleColor(ImGuiCol_Header, EditorUI::Color(theme.Accent));
			ImGui::PushStyleColor(ImGuiCol_HeaderHovered, EditorUI::Color(theme.AccentHover));
			ImGui::PushStyleColor(ImGuiCol_HeaderActive, EditorUI::Color(theme.AccentActive));
		}
		else if (selected)
		{
			const ImVec4 secondary = EditorUI::Color(EditorUI::WithAlpha(theme.Accent, 0.40f));
			ImGui::PushStyleColor(ImGuiCol_Header, secondary);
			ImGui::PushStyleColor(ImGuiCol_HeaderHovered, secondary);
			ImGui::PushStyleColor(ImGuiCol_HeaderActive, secondary);
		}

		// See the note in ContentBrowserPanel: blank label on purpose, but spelled so GCC's
		// -Wformat-zero-length has nothing to say about it.
		bool opened = ImGui::TreeNodeEx("Entity", flags, "%s", "");
		if (selected)
			ImGui::PopStyleColor(3);

		const bool rowHovered = ImGui::IsItemHovered();
		const bool rowClicked = ImGui::IsItemClicked();

		{
			const char* typeIcon = ICON_LC_BOX;
			ImU32 typeTint = theme.TextPrimary;
			DominantIcon(entity, typeIcon, typeTint);

			ImU32 nameCol = theme.TextPrimary;
			if (primary)
			{
				nameCol = theme.TextOnAccent;
				typeTint = theme.TextOnAccent;
			}
			else if (hidden)
				nameCol = theme.TextDisabled;
			else if (prefab)
				nameCol = theme.Link;

			const ImVec2 rmin = ImGui::GetItemRectMin();
			const ImVec2 rmax = ImGui::GetItemRectMax();
			const float clipRight = rmax.x - 3.0f * kOutlinerActionCol;
			const float labelX = rmin.x + ImGui::GetTreeNodeToLabelSpacing();
			const float textY = rmin.y + (rmax.y - rmin.y - ImGui::GetFontSize()) * 0.5f;
			ImDrawList* draw = ImGui::GetWindowDrawList();
			draw->PushClipRect(rmin, ImVec2(clipRight, rmax.y), true);
			draw->AddText(ImVec2(labelX, textY), typeTint, typeIcon);
			const float iconW = 18.0f;
			draw->AddText(ImVec2(labelX + iconW, textY), nameCol, tag.c_str());
			draw->PopClipRect();
		}

		if (rowClicked && ImGui::GetMousePos().x < ImGui::GetItemRectMax().x - 3.0f * kOutlinerActionCol)
		{
			// Shift extends from the anchor, Ctrl adds to or removes from the selection, and a
			// plain click replaces it. Shift wins over Ctrl when both are held, which is the
			// convention everywhere else.
			if (ImGui::GetIO().KeyShift && m_RangeAnchor)
				m_PendingRange = entity;
			else if (ImGui::GetIO().KeyCtrl)
				ToggleSelection(entity);
			else
				SelectSingle(entity);
		}

		if (ImGui::BeginDragDropSource())
		{
			UUID entityID = id;
			ImGui::SetDragDropPayload("SCENE_HIERARCHY_ENTITY", &entityID, sizeof(UUID));
			ImGui::Text("%s", tag.c_str());
			ImGui::EndDragDropSource();
		}

		if (ImGui::BeginDragDropTarget())
		{
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SCENE_HIERARCHY_ENTITY"))
			{
				UUID droppedID = *(const UUID*)payload->Data;
				Entity dropped = m_Context->FindEntityByUUID(droppedID);
				if (dropped && dropped != entity)
					Reparent(dropped, entity);
			}
			ImGui::EndDragDropTarget();
		}

		bool entityDeleted = false;
		bool createPrefab = false;
		bool revertInstance = false;
		if (ImGui::BeginPopupContextItem())
		{
			if (ImGui::MenuItem("Delete Entity"))
				entityDeleted = true;

			ImGui::Separator();

			if (ImGui::MenuItem("Create Prefab..."))
				createPrefab = true;

			if (prefab)
			{
				if (ImGui::MenuItem("Apply to Prefab..."))
				{
					m_PendingApply = id;
					m_OpenApplyModal = true;
				}

				if (ImGui::MenuItem("Revert Instance"))
					revertInstance = true;
			}

			ImGui::EndPopup();
		}

		const int action = EditorUI::RowActionIcons({
			{ hidden ? ICON_LC_EYE_OFF : ICON_LC_EYE, hidden ? "Show" : "Hide", hidden },
			{ locked ? ICON_LC_LOCK : ICON_LC_LOCK_OPEN, locked ? "Unlock" : "Lock", locked },
			{ prefab ? ICON_LC_LINK : "", nullptr, prefab, false }
		}, rowHovered, kOutlinerActionCol);

		if (action == 0)
		{
			if (hidden)
				m_Hidden.erase(id);
			else
				m_Hidden.insert(id);
		}
		else if (action == 1)
		{
			if (locked)
				m_Locked.erase(id);
			else
				m_Locked.insert(id);
		}

		if (opened)
		{
			std::vector<UUID> children = relationship.Children;
			for (UUID childID : children)
			{
				Entity child = m_Context->FindEntityByUUID(childID);
				if (child)
					DrawEntityNode(child);
			}
			ImGui::TreePop();
		}

		ImGui::PopID();

		if (createPrefab)
			CreatePrefabFrom(entity);

		if (revertInstance)
			RevertInstance(entity);

		if (entityDeleted)
			m_EntityToDelete = id;
	}

	void SceneHierarchyPanel::DrawCreateMenu()
	{
		if (!m_Context)
			return;

		if (ImGui::MenuItem("Create Empty Entity"))
		{
			Entity created = m_Context->CreateEntity("Empty Entity");
			PushAddedEntities("Create Entity", created);
			SelectSingle(created);
		}

		if (ImGui::MenuItem("Instantiate Prefab..."))
		{
			const std::string chosen = FileDialogs::OpenFile("GanymedE Prefab (*.gprefab)\0*.gprefab\0");
			if (!chosen.empty())
				InstantiatePrefab(MakeAssetRelative(chosen));
		}
	}

	void SceneHierarchyPanel::RebuildFilterVisibility()
	{
		m_FilterVisible.clear();
		if (!m_Context || m_Search[0] == '\0')
			return;

		auto view = m_Context->m_Registry.view<IDComponent, RelationshipComponent, TagComponent>();
		for (auto entityID : view)
		{
			Entity entity{ entityID, m_Context.get() };
			if (entity.GetComponent<RelationshipComponent>().Parent == UUID{ 0 })
				MarkFilterVisible(entity);
		}
	}

	bool SceneHierarchyPanel::MarkFilterVisible(Entity entity)
	{
		const bool self = TagContainsI(entity.GetComponent<TagComponent>().Tag, m_Search);
		bool any = self;
		for (UUID childID : entity.GetComponent<RelationshipComponent>().Children)
		{
			Entity child = m_Context->FindEntityByUUID(childID);
			if (child && MarkFilterVisible(child))
				any = true;
		}
		if (any)
			m_FilterVisible.insert(entity.GetUUID());
		return any;
	}

	bool SceneHierarchyPanel::IsLocked(Entity entity) const
	{
		if (!entity)
			return false;
		return m_Locked.count(entity.GetUUID()) != 0;
	}

	void SceneHierarchyPanel::ClearEditorViewState()
	{
		m_Hidden.clear();
		m_Locked.clear();
		m_Search[0] = '\0';
		m_FilterVisible.clear();
	}


	// The tunables a script declares, each row showing this entity's value.
	//
	// The schema comes from the script (ScriptEngine::GetScriptFields, which loads the class
	// without instantiating), while the values come from the component. A row is only written
	// back into component.Fields when the user actually changes it, so an untouched field keeps
	// tracking the script's default rather than being frozen at whatever it was when the entity
	// was created. "Reset" deletes the override to restore that link.
	static bool DrawScriptFields(ScriptComponent& component)
	{
		if (!IsAssetHandleValid(component.Script))
			return false;

		const auto& fields = ScriptEngine::GetScriptFields(component.Script);

		ImGui::Separator();
		if (fields.empty())
		{
			ImGui::TextDisabled("No properties (add a `Properties` table to the script)");
			return false;
		}

		bool edited = false;

		for (const auto& field : fields)
		{
			ImGui::PushID(field.Name.c_str());

			auto it = component.Fields.find(field.Name);
			const bool overridden = it != component.Fields.end()
				&& it->second.index() == field.Default.index();

			// Show the override if there is a usable one, otherwise the script's default.
			ScriptFieldValue value = overridden ? it->second : field.Default;
			bool changed = false;

			std::visit([&](auto& v)
			{
				using T = std::decay_t<decltype(v)>;
				if constexpr (std::is_same_v<T, bool>)
				{
					changed = ImGui::Checkbox(field.Name.c_str(), &v);
				}
				else if constexpr (std::is_same_v<T, double>)
				{
					float scratch = (float)v;
					changed = ImGui::DragFloat(field.Name.c_str(), &scratch, 0.05f);
					if (changed)
						v = scratch;
				}
				else if constexpr (std::is_same_v<T, std::string>)
				{
					// memcpy rather than strncpy: strncpy is deprecated on MSVC and its _s
					// replacement is not portable, and the length is already known here.
					char buffer[256] = {};
					std::memcpy(buffer, v.data(), std::min(v.size(), sizeof(buffer) - 1));
					if (ImGui::InputText(field.Name.c_str(), buffer, sizeof(buffer)))
					{
						v = buffer;
						changed = true;
					}
				}
				else   // glm::vec3
				{
					changed = ImGui::DragFloat3(field.Name.c_str(), glm::value_ptr(v), 0.05f);
				}
			}, value);

			if (changed)
			{
				component.Fields[field.Name] = value;
				edited = true;
			}

			if (overridden)
			{
				ImGui::SameLine();
				if (ImGui::SmallButton("Reset"))
				{
					component.Fields.erase(field.Name);
					edited = true;
				}
			}

			ImGui::PopID();
		}

		return edited;
	}

	// Binds the prefab-override queries for one component type into the inspector's type-erased
	// hook, and draws the component through it. The binding lives on the stack for exactly the
	// duration of the section, which is all the hook's Owner pointer has to outlive.
	//
	// Nothing inside the property drawers knows what a prefab is; this is the whole of the
	// coupling between the two features.
	namespace {

		template<typename T>
		struct OverrideBinding
		{
			Entity Target;
			Scene* Context = nullptr;

			static bool IsOverridden(void* owner, const entt::meta_data& field)
			{
				auto* self = static_cast<OverrideBinding*>(owner);
				return EditorUI::IsPropertyOverridden<T>(self->Target, *self->Context, field);
			}

			static bool Revert(void* owner, const entt::meta_data& field)
			{
				auto* self = static_cast<OverrideBinding*>(owner);
				return EditorUI::RevertProperty<T>(self->Target, *self->Context, field);
			}

			static bool Apply(void* owner, const entt::meta_data& field)
			{
				auto* self = static_cast<OverrideBinding*>(owner);
				const bool ok = EditorUI::ApplyProperty<T>(self->Target, *self->Context, field);

				// Named in the log because it is an asset write with no undo behind it: "which
				// field went into the prefab" is the only record of it afterwards.
				const char* name = field.name() ? field.name() : "<field>";
				if (ok)
					GE_INFO("Applied '{0}' to the prefab", name);
				else
					GE_WARN("Could not apply '{0}' to the prefab", name);

				return ok;
			}
		};

		// Multi-edit binding. `Others` excludes the primary, which is the entity the widgets are
		// actually driving.
		template<typename T>
		struct MultiBinding
		{
			Entity Primary;
			const std::vector<Entity>* Others = nullptr;

			static bool IsMixed(void* owner, const entt::meta_data& field)
			{
				auto* self = static_cast<MultiBinding*>(owner);

				// `self` has dependent type, so every member template reached through it needs
				// the `template` disambiguator - otherwise `<` is parsed as less-than. `other`
				// below is declared `Entity` outright and so needs nothing.
				if (!self->Primary.template HasComponent<T>())
					return false;

				const std::string primary = EmitReflectedValue(
					entt::forward_as_meta(self->Primary.template GetComponent<T>()), field);

				if (primary.empty())
					return false;   // no codec: cannot tell, so do not claim a disagreement

				for (Entity other : *self->Others)
				{
					if (other == self->Primary || !other.HasComponent<T>())
						continue;

					if (EmitReflectedValue(entt::forward_as_meta(other.GetComponent<T>()), field)
						!= primary)
					{
						return true;
					}
				}

				return false;
			}

			static void Propagate(void* owner, const entt::meta_data& field)
			{
				auto* self = static_cast<MultiBinding*>(owner);
				if (!self->Primary.template HasComponent<T>())
					return;

				// Named: meta_data::get builds a meta_handle that binds a non-const lvalue ref,
				// so the temporary cannot be passed inline. See EditorPrefabOverrides.h.
				entt::meta_any primary =
					entt::forward_as_meta(self->Primary.template GetComponent<T>());
				entt::meta_any value = field.get(primary);

				if (!value)
					return;

				for (Entity other : *self->Others)
				{
					if (other == self->Primary || !other.HasComponent<T>())
						continue;

					entt::meta_any target = entt::forward_as_meta(other.GetComponent<T>());
					field.set(target, value);
				}
			}
		};

		template<typename T>
		bool DrawReflected(Entity entity, Scene* scene, const std::vector<Entity>& selection,
			T& component, const EditorUI::FieldFilter& filter = {})
		{
			OverrideBinding<T> binding{ entity, scene };

			EditorUI::OverrideHook hook;
			if (scene && entity.HasComponent<PrefabMemberComponent>())
			{
				hook.IsOverridden = &OverrideBinding<T>::IsOverridden;
				hook.Revert = &OverrideBinding<T>::Revert;
				hook.Apply = &OverrideBinding<T>::Apply;
				hook.Owner = &binding;
			}

			MultiBinding<T> multiBinding{ entity, &selection };

			EditorUI::MultiEditHook multi;
			if (selection.size() > 1)
			{
				multi.IsMixed = &MultiBinding<T>::IsMixed;
				multi.Propagate = &MultiBinding<T>::Propagate;
				multi.Owner = &multiBinding;
			}

			return EditorUI::DrawReflectedComponent(component, hook, multi, filter);
		}

	}

	// ---------------------------------------------------------------------------------------
	// The commit boundary.
	//
	// Inspector widgets edit per tick: a two-second DragFloat drag writes the component on
	// ~120 frames. What undo needs is one command per *gesture* - the value before the drag
	// started and the value after it ended - so somewhere the ticks have to be collapsed.
	//
	// The signal used is ImGui's active item. A widget that holds a gesture (drag, text field,
	// the press half of a checkbox) is the active item for as long as the gesture lasts, so:
	// the frame ActiveId first lands inside a section is the start of a pending edit, and its
	// pre-copy from that frame is the before-value; the frame ActiveId leaves is the commit.
	// Widgets with no active phase at all - a drag-drop assignment landing on the section -
	// report their edit and commit in the same frame.
	//
	// "Did a widget edit the component" is the `uiFunction` return value, not a value diff.
	// Diffing would need 16 operator==s and would still be wrong: the Transform section does a
	// degrees/radians round-trip that can change bits with no user input, and a diff scheme
	// mints a garbage command for it. "The widget said so" is ground truth ImGui already
	// computes; the section lambdas OR their widget returns and the wrapper trusts that.
	//
	// House rule this establishes: an inspector lambda mutates the component only when a
	// widget actually reported an edit, and returns true when it does.
	// ---------------------------------------------------------------------------------------

	static_assert(std::is_same<ImGuiID, uint32_t>::value,
		"PendingEdit::ActiveId stores an ImGuiID; keep the header's type in step");

	template<typename T>
	void SceneHierarchyPanel::TrackCommitBoundary(Entity entity, const std::string& name,
		const T& before, const std::vector<std::pair<UUID, T>>& othersBefore,
		uint32_t activeOnEntry, uint32_t activeOnExit, bool edited)
	{
		if (!Recording())
			return;

		const entt::id_type type = entt::type_hash<T>::value();
		const bool pendingIsHere = m_Pending.Command
			&& m_Pending.Command->GetEntity() == entity.GetUUID()
			&& m_Pending.Command->GetComponentType() == type;

		if (pendingIsHere)
		{
			m_Pending.Visited = true;
			m_Pending.Edited = m_Pending.Edited || edited;

			// The widget let go. One command for the whole gesture, and none at all if no
			// frame of it reported an edit (a click that grabbed a drag and moved nothing).
			if (activeOnExit != m_Pending.ActiveId)
				CommitPendingEdit();

			return;
		}

		// A widget in *this* section just became the active item. Nothing else submitted items
		// between the two reads, so a change in ActiveId can only have come from here.
		if (activeOnExit != 0 && activeOnExit != activeOnEntry)
		{
			m_Pending.Command = CreateScope<ComponentEditCommand<T>>(
				"Edit " + name, entity.GetUUID(), before);

			// From the snapshot DrawComponent took before the widget ran, not from the live
			// components: a widget that becomes active and edits in the same frame has already
			// propagated to the rest of the selection by now.
			m_Pending.Secondary.clear();
			for (const auto& entry : othersBefore)
			{
				m_Pending.Secondary.push_back(CreateScope<ComponentEditCommand<T>>(
					"Edit " + name, entry.first, entry.second));
			}

			m_Pending.ActiveId = activeOnExit;
			m_Pending.Edited = edited;
			m_Pending.Visited = true;
			return;
		}

		// An edit with no active phase: a checkbox, a combo, a drop delivered onto this section,
		// or a popup item that closed in the same frame. There is nothing to wait for, so the
		// command is complete here - before and after both.
		if (edited && entity.HasComponent<T>())
		{
			Scope<EditorCommand> primary = CreateScope<ComponentEditCommand<T>>(
				"Edit " + name, entity.GetUUID(), before, entity.GetComponent<T>());

			if (othersBefore.empty())
			{
				m_UndoStack->Push(std::move(primary));
				return;
			}

			// The rest of the selection was propagated to during the widget, and their
			// before-values survive because DrawComponent snapshotted them first. One composite,
			// for the reason CommitPendingEdit gives: one gesture is one undo entry.
			std::vector<Scope<EditorCommand>> children;
			children.reserve(othersBefore.size() + 1);
			children.push_back(std::move(primary));

			for (const auto& entry : othersBefore)
			{
				Entity other = m_Context->FindEntityByUUID(entry.first);
				if (!other || !other.HasComponent<T>())
					continue;

				children.push_back(CreateScope<ComponentEditCommand<T>>(
					"Edit " + name, entry.first, entry.second, other.GetComponent<T>()));
			}

			const std::string label = "Edit " + std::to_string(children.size()) + " entities";
			m_UndoStack->Push(CreateScope<CompositeCommand>(label, std::move(children)));
		}
	}

	void SceneHierarchyPanel::CommitPendingEdit()
	{
		if (!m_Pending.Command)
			return;

		if (m_Pending.Edited && m_UndoStack && m_Context)
		{
			m_Pending.Command->CaptureAfter(*m_Context);

			if (m_Pending.Secondary.empty())
			{
				m_UndoStack->Push(std::move(m_Pending.Command));
			}
			else
			{
				// One gesture over N entities is one undo entry. Pushing N commands would make
				// Ctrl+Z walk back through the selection one entity at a time, which is the same
				// class of wrongness as one command per frame of a drag.
				std::vector<Scope<EditorCommand>> children;
				children.reserve(m_Pending.Secondary.size() + 1);
				children.push_back(std::move(m_Pending.Command));

				for (Scope<ComponentEditCommandBase>& command : m_Pending.Secondary)
				{
					command->CaptureAfter(*m_Context);
					children.push_back(std::move(command));
				}

				const std::string label = "Edit " + std::to_string(children.size()) + " entities";
				m_UndoStack->Push(CreateScope<CompositeCommand>(label, std::move(children)));
			}
		}

		m_Pending = {};
	}

	void SceneHierarchyPanel::DiscardPendingEdit()
	{
		m_Pending = {};
	}

	void SceneHierarchyPanel::FlushPendingEdit()
	{
		if (!m_Pending.Command)
			return;

		// The section stopped being drawn while an edit was in flight - the selection changed,
		// the tree node was collapsed, the entity was deleted. Commit what was recorded rather
		// than drop it on the floor.
		if (!m_Pending.Visited)
		{
			CommitPendingEdit();
			return;
		}

		m_Pending.Visited = false;
	}

	template<typename T, typename UIFunction>
	void SceneHierarchyPanel::DrawComponent(const std::string& name, Entity entity, UIFunction uiFunction)
	{
		// "Components common to the selection": a section is drawn only when *every* selected
		// entity has it. Showing a component only some of them have would make an edit either
		// silently skip entities or silently add the component to them, and both are surprises.
		for (Entity selected : m_Selection)
		{
			if (!selected.HasComponent<T>())
				return;
		}

		if (!entity.HasComponent<T>())
			return;

		const EditorUI::EditorTheme& theme = EditorUI::Theme();
		const void* typeId = (void*)typeid(T).hash_code();

		// Same storage key TreeNodeEx used, so imgui.ini open-state survives the restyle.
		const ImGuiID openId = ImGui::GetID(typeId);
		bool open = ImGui::GetStateStorage()->GetBool(openId, true);

		ImGui::PushID(typeId);

		constexpr float kOverflow = 24.0f;
		const float height = theme.RowHeight;
		const float width = ImGui::GetContentRegionAvail().x;
		const ImVec2 p0 = ImGui::GetCursorScreenPos();

		// Dummy owns the row in the layout; the hit targets are overlaid. SameLine after a
		// short InvisibleButton leaves IsSameLine set, and SetCursorScreenPos does not
		// clear it — the next header would then share the overflow's line.
		ImGui::Dummy(ImVec2(width, height));

		ImDrawList* draw = ImGui::GetWindowDrawList();
		draw->AddRectFilled(p0, ImVec2(p0.x + width, p0.y + height), theme.ChromeBg);
		draw->AddLine(ImVec2(p0.x, p0.y + height - 1.0f),
			ImVec2(p0.x + width, p0.y + height - 1.0f), theme.Border);

		ImGui::SetCursorScreenPos(p0);
		ImGui::InvisibleButton("##hdr", ImVec2(ImMax(1.0f, width - kOverflow), height));
		const bool headerClicked = ImGui::IsItemClicked();

		const char* typeIcon = ICON_LC_BOX;
		ImU32 typeTint = theme.TextPrimary;
		ComponentTypeChrome<T>(typeIcon, typeTint);

		const float iconY = p0.y + (height - ImGui::GetFontSize()) * 0.5f;
		float x = p0.x + 8.0f;
		draw->AddText(ImVec2(x, iconY), theme.TextDim,
			open ? ICON_LC_CHEVRON_DOWN : ICON_LC_CHEVRON_RIGHT);
		x += 20.0f;
		draw->AddText(ImVec2(x, iconY), typeTint, typeIcon);
		x += 20.0f;

		// The section-level marker is the only override affordance a *hand-written* section
		// gets: the per-field one lives inside the reflected property drawer, so a component
		// the panel still draws by hand can say "something in here differs from the prefab"
		// but not which field.
		const bool sectionOverridden = m_Context
			&& EditorUI::IsComponentOverridden<T>(entity, *m_Context);

		draw->PushClipRect(p0, ImVec2(p0.x + width - kOverflow, p0.y + height), true);
		ImFont* headerFont = EditorUI::EditorFonts::Header();
		if (headerFont)
		{
			const float nameY = p0.y + (height - headerFont->FontSize) * 0.5f;
			draw->AddText(headerFont, headerFont->FontSize, ImVec2(x, nameY),
				theme.TextPrimary, name.c_str());
			if (sectionOverridden)
			{
				const ImVec2 ns = headerFont->CalcTextSizeA(headerFont->FontSize, FLT_MAX, 0.0f,
					name.c_str());
				draw->AddText(headerFont, headerFont->FontSize, ImVec2(x + ns.x, nameY),
					theme.FieldOverride, "  *");
			}
		}
		else
		{
			draw->AddText(ImVec2(x, iconY), theme.TextPrimary, name.c_str());
			if (sectionOverridden)
			{
				const ImVec2 ns = ImGui::CalcTextSize(name.c_str());
				draw->AddText(ImVec2(x + ns.x, iconY), theme.FieldOverride, "  *");
			}
		}
		draw->PopClipRect();

		ImGui::SetCursorScreenPos(ImVec2(p0.x + width - kOverflow,
			p0.y + (height - kOverflow) * 0.5f));
		if (EditorUI::OverflowMenuButton("menu"))
			ImGui::OpenPopup("##CompMenu");

		bool removeComponent = false;
		if (ImGui::BeginPopup("##CompMenu"))
		{
			if (ImGui::MenuItem("Remove component"))
				removeComponent = true;
			ImGui::EndPopup();
		}

		if (headerClicked)
		{
			open = !open;
			ImGui::GetStateStorage()->SetBool(openId, open);
		}

		// Flush against the next header; ItemSpacing would put a 4 px gutter in the stack.
		ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + height));

		if (open)
		{
			ImGui::Indent(8.0f);
			ImGui::Dummy(ImVec2(0.0f, 4.0f));

			auto& component = entity.GetComponent<T>();

			// Taken every frame; only the copy from the frame a widget grabs the active
			// item is kept, and one frame later that value is unrecoverable. This used
			// to claim components were handle/POD/small-string sized. ParticleEmitter
			// is not: an open section heap-copies two keyframe vectors *and* the live
			// pool (up to MaxParticles) every frame. Measured cost is those allocs while
			// that one section is open; acceptable. ComponentEditCommand<T> storing
			// before/after by value doubles it per command — also fine, also noted.
			//
			// ActiveId is read *after* the header. Chevron / ••• sit outside this
			// window; putting the read above them would mint a phantom ComponentEditCommand
			// on collapse.
			T before = component;
			const ImGuiID activeOnEntry = ImGui::GetActiveID();

			// The rest of the selection, at the same instant. This used to be read inside
			// TrackCommitBoundary at the moment a gesture started, which was wrong in two
			// ways: on the no-active-phase path (a checkbox, a combo, a drop) there is no
			// such moment at all, so those entities got no undo entry and their edit was
			// unrecoverable; and even on the gesture path, a widget that became active *and*
			// reported an edit in the same frame had already propagated to them, so their
			// "before" was the new value.
			//
			// Only when something else is selected: for a single selection this is an empty
			// vector and costs nothing, which is every frame of ordinary editing.
			std::vector<std::pair<UUID, T>> othersBefore;
			if (m_Selection.size() > 1)
			{
				othersBefore.reserve(m_Selection.size() - 1);
				for (Entity other : m_Selection)
				{
					if (other == entity || !other.HasComponent<T>())
						continue;

					othersBefore.emplace_back(other.GetUUID(), other.GetComponent<T>());
				}
			}

			const bool edited = uiFunction(component);

			TrackCommitBoundary<T>(entity, name, before, othersBefore,
				activeOnEntry, ImGui::GetActiveID(), edited);

			ImGui::Dummy(ImVec2(0.0f, 4.0f));
			ImGui::Unindent(8.0f);
		}

		ImGui::PopID();

		if (removeComponent)
		{
			if (Recording())
			{
				m_UndoStack->Push(CreateScope<RemoveComponentCommand<T>>(
					"Remove " + name, entity.GetUUID(), entity.GetComponent<T>()));
			}

			// A pending edit on the component about to be removed would commit against a
			// component that no longer exists, pushing a before == after no-op.
			if (m_Pending.Command && m_Pending.Command->GetEntity() == entity.GetUUID()
				&& m_Pending.Command->GetComponentType() == entt::type_hash<T>::value())
			{
				DiscardPendingEdit();
			}

			entity.RemoveComponent<T>();
		}
	}

	template<typename T>
	void SceneHierarchyPanel::DrawAddComponentEntry(const char* label)
	{
		if (m_SelectionContext.HasComponent<T>())
			return;

		if (!ImGui::MenuItem(label))
			return;

		T& component = m_SelectionContext.AddComponent<T>();
		if constexpr (std::is_same_v<T, BoxColliderComponent>)
			SeedBoxColliderFromMesh(m_SelectionContext, component);
		if (Recording())
		{
			m_UndoStack->Push(CreateScope<AddComponentCommand<T>>(
				std::string("Add ") + label, m_SelectionContext.GetUUID(), component));
		}

		ImGui::CloseCurrentPopup();
	}

	void SceneHierarchyPanel::DrawAddComponentButton()
	{
		const EditorUI::EditorTheme& theme = EditorUI::Theme();

		ImGui::Dummy(ImVec2(0.0f, 8.0f));
		ImGui::Indent(8.0f);
		const float width = ImMax(1.0f, ImGui::GetContentRegionAvail().x - 8.0f);

		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorUI::Color(EditorUI::WithAlpha(theme.Accent, 0.15f)));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, EditorUI::Color(EditorUI::WithAlpha(theme.Accent, 0.30f)));
		ImGui::PushStyleColor(ImGuiCol_Border, EditorUI::Color(theme.Accent));
		ImGui::PushStyleColor(ImGuiCol_Text, EditorUI::Color(theme.AccentText));
		if (ImGui::Button("Add Component", ImVec2(width, theme.RowHeight)))
			ImGui::OpenPopup("AddComponent");
		ImGui::PopStyleColor(5);
		ImGui::PopStyleVar();

		if (ImGui::BeginPopup("AddComponent"))
		{
			DrawAddComponentEntry<CameraComponent>("Camera");
			DrawAddComponentEntry<SpriteRendererComponent>("Sprite Renderer");
			DrawAddComponentEntry<DirectionalLightComponent>("Directional Light");
			DrawAddComponentEntry<PointLightComponent>("Point Light");
			DrawAddComponentEntry<SpotLightComponent>("Spot Light");
			DrawAddComponentEntry<SkyLightComponent>("Sky Light");
			DrawAddComponentEntry<AnimatorComponent>("Animator");
			DrawAddComponentEntry<BoneAttachmentComponent>("Bone Attachment");
			DrawAddComponentEntry<ScriptComponent>("Script");
			DrawAddComponentEntry<AudioSourceComponent>("Audio Source");
			DrawAddComponentEntry<AudioListenerComponent>("Audio Listener");
			DrawAddComponentEntry<ParticleEmitterComponent>("Particle Emitter");
			DrawAddComponentEntry<MarkerComponent>("Marker");
			DrawAddComponentEntry<RigidBodyComponent>("Rigid Body");
			DrawAddComponentEntry<BoxColliderComponent>("Box Collider");
			DrawAddComponentEntry<SphereColliderComponent>("Sphere Collider");
			DrawAddComponentEntry<CapsuleColliderComponent>("Capsule Collider");
			ImGui::EndPopup();
		}

		ImGui::Unindent(8.0f);
		ImGui::Dummy(ImVec2(0.0f, 8.0f));
	}

	// ---- Entity operations -----------------------------------------------------------------

	void SceneHierarchyPanel::PushAddedEntities(std::string label, Entity root)
	{
		if (!Recording() || !root)
			return;

		std::vector<EntitySnapshot> snapshots;
		CaptureSubtree(*m_Context, root, snapshots);
		m_UndoStack->Push(CreateScope<AddEntitiesCommand>(std::move(label), std::move(snapshots)));
	}

	// Deletes the whole subtree, not just the entity.
	//
	// Scene::DestroyEntity keeps its orphan-the-children semantics as engine API, but no
	// production editor deletes that way - Unity, Unreal and Godot all take the subtree - and
	// the safety argument for orphaning ("you would lose the children") evaporates once the
	// delete is one Ctrl+Z away.
	void SceneHierarchyPanel::DeleteEntity(Entity entity)
	{
		if (!entity || !m_Context)
			return;

		std::vector<EntitySnapshot> snapshots;
		CaptureSubtree(*m_Context, entity, snapshots);

		const UUID selected = m_SelectionContext ? m_SelectionContext.GetUUID() : UUID{ 0 };
		bool selectionInside = false;
		for (const EntitySnapshot& snapshot : snapshots)
			selectionInside = selectionInside || snapshot.ID == selected;

		if (Recording())
		{
			m_UndoStack->Push(CreateScope<DeleteEntitiesCommand>(
				"Delete '" + entity.GetComponent<TagComponent>().Tag + "'", snapshots));
		}

		if (m_Pending.Command)
		{
			for (const EntitySnapshot& snapshot : snapshots)
			{
				if (m_Pending.Command->GetEntity() == snapshot.ID)
					DiscardPendingEdit();
			}
		}

		RemoveSubtree(*m_Context, snapshots);

		if (selectionInside)
			SelectSingle({});
	}

	// ---- Prefabs -----------------------------------------------------------------------------

	// Serializes the selection's subtree to a chosen path and links the original to it - creation
	// makes the source entity an instance, which is the behavior authors expect from Unity.
	void SceneHierarchyPanel::CreatePrefabFrom(Entity entity)
	{
		if (!entity || !m_Context)
			return;

		const std::string chosen = FileDialogs::SaveFile("GanymedE Prefab (*.gprefab)\0*.gprefab\0");
		if (chosen.empty())
			return;

		std::filesystem::path fullPath = chosen;
		if (fullPath.extension() != ".gprefab")
			fullPath += ".gprefab";

		// A prefab outside assets/ has no registry identity, so nothing could reference it.
		// generic_string, not native(): native() is wstring on Windows and string on POSIX, so a
		// wide literal here would simply not compile on the Linux and macOS targets.
		const std::filesystem::path relativePath = MakeAssetRelative(fullPath);
		if (relativePath.empty() || relativePath.generic_string().rfind("..", 0) == 0)
		{
			GE_WARN("Prefabs must be saved inside the asset root - '{0}' is outside it", chosen);
			return;
		}

		if (!PrefabSerializer::Save(*m_Context, entity, fullPath))
		{
			GE_ERROR("Could not write prefab '{0}'", fullPath.string());
			return;
		}

		AssetHandle handle = AssetManager::ImportAsset(relativePath);
		if (!IsAssetHandleValid(handle))
			return;

		// Linking the source is a scene edit, so it is undoable like any other component add -
		// unlike the file write above, which is not.
		auto& instance = entity.HasComponent<PrefabInstanceComponent>()
			? entity.GetComponent<PrefabInstanceComponent>()
			: entity.AddComponent<PrefabInstanceComponent>();

		instance.Source = handle;

		if (Recording())
		{
			m_UndoStack->Push(CreateScope<AddComponentCommand<PrefabInstanceComponent>>(
				"Link Prefab", entity.GetUUID(), instance));
		}

		GE_INFO("Created prefab '{0}'", relativePath.generic_string());
	}

	// Re-serializes the instance's current subtree over its source file.
	//
	// An asset write, so **not undoable** (undo covers scene edits only) - which makes this the
	// milestone's one silently destructive click, and the reason it is the one operation behind a
	// confirmation. Other instances of the same prefab already in the scene do not update: there
	// is no propagation in v1.
	void SceneHierarchyPanel::ApplyToPrefab(Entity instanceRoot)
	{
		if (!instanceRoot || !m_Context || !instanceRoot.HasComponent<PrefabInstanceComponent>())
			return;

		const AssetHandle source = instanceRoot.GetComponent<PrefabInstanceComponent>().Source;
		const AssetMetadata* metadata = AssetManager::GetMetadata(source);
		if (!metadata)
		{
			GE_ERROR("Prefab source {0} is not in the registry - cannot apply",
				static_cast<uint64_t>(source));
			return;
		}

		const std::filesystem::path fullPath = GetAssetRoot() / metadata->FilePath;

		// Placement is per-instance: keep whatever root transform the file already had rather
		// than baking this instance's position into the asset.
		TransformComponent rootTransform;
		const bool hasStored = PrefabSerializer::ReadRootTransform(fullPath, rootTransform);

		if (PrefabSerializer::Save(*m_Context, instanceRoot, fullPath,
			hasStored ? &rootTransform : nullptr))
		{
			// The file just changed underneath the cached template, and the override diff is
			// computed against that template - so without this every field stays marked as
			// overridden until the editor restarts. Per-property apply needs no equivalent: it
			// edits the template itself, so the two stay in agreement by construction.
			EditorUI::InvalidatePrefabTemplates();

			GE_INFO("Applied to prefab '{0}'", metadata->FilePath);
		}
	}

	// Throws away everything below the root and rebuilds it from the file, keeping the root
	// entity itself - its UUID, so references to it survive, and its transform, because placement
	// belongs to the instance.
	//
	// Unlike Apply this is a scene edit, so it *is* undoable: one composite command holding the
	// delete of the old subtree and the add of the new one.
	void SceneHierarchyPanel::RevertInstance(Entity instanceRoot)
	{
		if (!instanceRoot || !m_Context || !instanceRoot.HasComponent<PrefabInstanceComponent>())
			return;

		const AssetHandle source = instanceRoot.GetComponent<PrefabInstanceComponent>().Source;
		const AssetMetadata* metadata = AssetManager::GetMetadata(source);
		if (!metadata)
		{
			GE_ERROR("Prefab source {0} is not in the registry - cannot revert",
				static_cast<uint64_t>(source));
			return;
		}

		const std::filesystem::path fullPath = GetAssetRoot() / metadata->FilePath;
		if (!std::filesystem::exists(fullPath))
		{
			GE_ERROR("Prefab file '{0}' is missing - leaving the instance untouched",
				metadata->FilePath);
			return;
		}

		const UUID rootID = instanceRoot.GetUUID();
		const TransformComponent rootTransform = instanceRoot.GetComponent<TransformComponent>();
		const UUID parentID = instanceRoot.GetComponent<RelationshipComponent>().Parent;
		const size_t siblingIndex = SiblingIndexOf(*m_Context, instanceRoot);

		std::vector<EntitySnapshot> before;
		CaptureSubtree(*m_Context, instanceRoot, before);

		DiscardPendingEdit();
		RemoveSubtree(*m_Context, before);

		PrefabSerializer::InstantiateOptions options;
		options.RootUUID = rootID;
		options.RootTransform = &rootTransform;

		Entity rebuilt = PrefabSerializer::InstantiateFromAsset(source, *m_Context, options);
		if (!rebuilt)
		{
			// Put the instance back rather than leaving a hole where it was.
			RestoreSubtree(*m_Context, before);
			GE_ERROR("Reverting from '{0}' failed - the instance was restored", metadata->FilePath);
			return;
		}

		if (parentID != UUID{ 0 })
			SetParentAtIndex(*m_Context, rebuilt, m_Context->FindEntityByUUID(parentID), siblingIndex);

		std::vector<EntitySnapshot> after;
		CaptureSubtree(*m_Context, rebuilt, after);

		if (Recording())
		{
			std::vector<Scope<EditorCommand>> steps;
			steps.push_back(CreateScope<DeleteEntitiesCommand>("Revert (remove)", std::move(before)));
			steps.push_back(CreateScope<AddEntitiesCommand>("Revert (rebuild)", std::move(after)));

			m_UndoStack->Push(CreateScope<CompositeCommand>(
				"Revert '" + rebuilt.GetComponent<TagComponent>().Tag + "'", std::move(steps)));
		}

		SelectSingle(rebuilt);
		GE_INFO("Reverted instance from '{0}'", metadata->FilePath);
	}

	Entity SceneHierarchyPanel::InstantiatePrefab(const std::filesystem::path& relativePath,
		bool recordUndo)
	{
		if (!m_Context)
			return {};

		AssetHandle handle = AssetManager::ImportAsset(relativePath);
		if (!IsAssetHandleValid(handle))
			return {};

		Entity root = PrefabSerializer::InstantiateFromAsset(handle, *m_Context);
		if (!root)
			return {};

		if (recordUndo)
			PushAddedEntities("Instantiate '" + root.GetComponent<TagComponent>().Tag + "'", root);
		SelectSingle(root);
		return root;
	}

	bool SceneHierarchyPanel::SeedBoxColliderFromMesh(Entity entity, BoxColliderComponent& collider)
	{
		if (!entity || !entity.HasComponent<StaticMeshComponent>())
			return false;

		return MeshCollision::SeedBoxCollider(
			collider, entity.GetComponent<StaticMeshComponent>().Mesh.Get());
	}

	// The inspector half of the instance UI: where the source came from, and the two propagation
	// buttons. Drawn above the component sections, next to the entity's name.
	void SceneHierarchyPanel::DrawPrefabControls(Entity entity)
	{
		if (!entity.HasComponent<PrefabInstanceComponent>())
			return;

		const AssetHandle source = entity.GetComponent<PrefabInstanceComponent>().Source;
		const AssetMetadata* metadata = AssetManager::GetMetadata(source);

		ImGui::Separator();
		if (metadata)
			ImGui::Text("Prefab instance: %s", metadata->FilePath.c_str());
		else
			ImGui::TextDisabled("Prefab instance: source %" PRIu64 " is not in the registry",
				static_cast<uint64_t>(source));

		if (ImGui::Button("Apply to Prefab..."))
		{
			m_PendingApply = entity.GetUUID();
			m_OpenApplyModal = true;
		}

		ImGui::SameLine();
		if (ImGui::Button("Revert Instance"))
			RevertInstance(entity);

		ImGui::Separator();
	}

	// Apply is the only operation here that destroys something no Ctrl+Z can bring back, so it
	// is the only one that asks first. The modal names the file it is about to overwrite, and
	// says the thing an author would otherwise have to discover: other instances do not follow.
	void SceneHierarchyPanel::DrawApplyPrefabModal()
	{
		if (m_OpenApplyModal)
		{
			ImGui::OpenPopup("Apply to Prefab");
			m_OpenApplyModal = false;
		}

		if (!ImGui::BeginPopupModal("Apply to Prefab", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
			return;

		Entity instanceRoot = m_Context ? m_Context->FindEntityByUUID(m_PendingApply) : Entity{};
		const AssetMetadata* metadata = instanceRoot && instanceRoot.HasComponent<PrefabInstanceComponent>()
			? AssetManager::GetMetadata(instanceRoot.GetComponent<PrefabInstanceComponent>().Source)
			: nullptr;

		if (!metadata)
		{
			ImGui::TextUnformatted("That instance or its source is gone.");
			if (ImGui::Button("Close"))
			{
				m_PendingApply = UUID{ 0 };
				ImGui::CloseCurrentPopup();
			}

			ImGui::EndPopup();
			return;
		}

		ImGui::Text("Overwrite '%s' with this subtree?", metadata->FilePath.c_str());
		ImGui::TextDisabled("This edits the asset and cannot be undone.");
		ImGui::TextDisabled("Other instances already in the scene will not change.");
		ImGui::Separator();

		if (ImGui::Button("Apply"))
		{
			ApplyToPrefab(instanceRoot);
			m_PendingApply = UUID{ 0 };
			ImGui::CloseCurrentPopup();
		}

		ImGui::SameLine();
		if (ImGui::Button("Cancel"))
		{
			m_PendingApply = UUID{ 0 };
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	}

	void SceneHierarchyPanel::Reparent(Entity child, Entity parent)
	{
		if (!child || !m_Context)
			return;

		const UUID oldParent = child.GetComponent<RelationshipComponent>().Parent;
		const size_t oldIndex = SiblingIndexOf(*m_Context, child);

		m_Context->SetParent(child, parent);

		// SetParent silently no-ops when the move would make a cycle, and dropping an entity
		// onto the parent it already has is a no-op too. Recording either would corrupt
		// sibling order on undo, so the record is taken from what actually happened.
		const UUID newParent = child.GetComponent<RelationshipComponent>().Parent;
		if (newParent == oldParent)
			return;

		if (!Recording())
			return;

		m_UndoStack->Push(CreateScope<ReparentCommand>(
			"Reparent '" + child.GetComponent<TagComponent>().Tag + "'",
			child.GetUUID(), oldParent, oldIndex, newParent, SiblingIndexOf(*m_Context, child)));
	}

	void SceneHierarchyPanel::DuplicateSelectedEntity()
	{
		if (!m_Context || !m_SelectionContext)
			return;

		Entity copy = m_Context->DuplicateEntity(m_SelectionContext);
		if (!copy)
			return;

		PushAddedEntities("Duplicate '" + copy.GetComponent<TagComponent>().Tag + "'", copy);
		SelectSingle(copy);
	}

	void SceneHierarchyPanel::DeleteSelectedEntity()
	{
		// A copy, because DeleteEntity mutates the selection as it goes.
		const std::vector<Entity> targets = m_Selection;
		for (Entity entity : targets)
			DeleteEntity(entity);
	}

	void SceneHierarchyPanel::DrawComponents(Entity entity)
	{
		if (entity.HasComponent<TagComponent>())
		{
			auto& tagComponent = entity.GetComponent<TagComponent>();

			// memcpy rather than strncpy: strncpy is deprecated on MSVC and its _s replacement
			// is not portable, and the length is already known (the DrawScriptFields precedent).
			char buffer[256];
			memset(buffer, 0, sizeof(buffer));
			memcpy(buffer, tagComponent.Tag.data(),
				std::min(tagComponent.Tag.size(), sizeof(buffer) - 1));

			ImGui::Indent(8.0f);
			ImGui::SetNextItemWidth(ImMax(1.0f, ImGui::GetContentRegionAvail().x - 8.0f));

			// The rename gets the same pending/commit treatment as a component section: one
			// command for the whole typing session, not one per keystroke. While the field is
			// focused ImGui's own Ctrl+Z is the text undo, which is what every editor does.
			const TagComponent before = tagComponent;
			const ImGuiID activeOnEntry = ImGui::GetActiveID();

			bool edited = false;
			if (ImGui::InputText("##Tag", buffer, sizeof(buffer)))
			{
				tagComponent.Tag = std::string(buffer);
				edited = true;
			}

			// Empty snapshot: a name is per-entity by definition, so the Tag field is the one
			// place multi-edit deliberately does not apply.
			TrackCommitBoundary<TagComponent>(entity, "Name", before, {}, activeOnEntry,
				ImGui::GetActiveID(), edited);
			ImGui::Unindent(8.0f);
			ImGui::Dummy(ImVec2(0.0f, 8.0f));
		}

		DrawComponent<TransformComponent>("Transform", entity, [&](auto& component)
		{
			// The degrees round-trip is Trait::Radians on Rotation, and the reset values are
			// Attr::Reset - both registered, both honoured by the vec3 drawer. What stays here is
			// the one thing reflection cannot express: a side effect.
			const bool edited = DrawReflected(entity, m_Context.get(), m_Selection, component);

			// Editing the component directly is invisible to change tracking, so the cached
			// world transform would never be refreshed.
			if (edited)
				m_Context->MarkChanged<TransformComponent>(entity);

			return edited;
		});

		// Read-only: the link is created by "Create Prefab" and followed by the Apply / Revert
		// buttons above the sections. There is no field to type a handle into on purpose.
		// Source is ReadOnly, so the drawer renders it disabled and reports no edit - the section
		// used to return false by hand for the same reason. ReadOnly also suppresses the drop
		// target, which BeginDisabled alone would not: a payload drop is not an item click.
		DrawComponent<PrefabInstanceComponent>("Prefab Instance", entity, [&](auto& component)
		{
			return DrawReflected(entity, m_Context.get(), m_Selection, component);
		});

		DrawComponent<ScatterGroupComponent>("Scatter Group", entity, [&](auto& component)
		{
			return DrawReflected(entity, m_Context.get(), m_Selection, component);
		});

		DrawComponent<MarkerComponent>("Marker", entity, [&](auto& component)
		{
			return DrawReflected(entity, m_Context.get(), m_Selection, component);
		});

		// SceneCamera's fields are drawn inline by the nested-struct fallback, so Projection and
		// its clip planes appear as rows of this section exactly as they did by hand. The one
		// thing left is which of the two projections' fields to show, and that is visibility
		// rather than a clamp - it has to be decided before anything is submitted, which is what
		// the field filter is for.
		DrawComponent<CameraComponent>("Camera", entity, [&](auto& component)
		{
			const bool ortho = component.Camera.GetProjectionType()
				== SceneCamera::ProjectionType::Orthographic;

			return DrawReflected(entity, m_Context.get(), m_Selection, component,
				[ortho](const entt::meta_data& field)
				{
					const std::string name = field.name() ? field.name() : "";
					if (name.rfind("Perspective", 0) == 0)
						return !ortho;
					if (name.rfind("Orthographic", 0) == 0)
						return ortho;
					return true;
				});
		});

		// ---- Reflected sections -------------------------------------------------------------
		//
		// From here down, a section whose body is one DrawReflectedComponent call is drawn
		// entirely from ComponentReflection.cpp - field order, labels, ranges, drag speeds,
		// colour-vs-position widget choice, enum names and notes all come from the registration.
		// Adding a field to one of these components is one `.data<>` line, not an edit here.
		//
		// The sections that stay hand-written are not leftovers; each has its reason stated at
		// its own site, and R1 anticipated most of them in the registration comments.
		DrawComponent<SpriteRendererComponent>("Sprite Renderer", entity, [&](auto& component)
		{
			return DrawReflected(entity, m_Context.get(), m_Selection, component);
		});

		DrawComponent<StaticMeshComponent>("Static Mesh", entity, [](auto& component)
		{
			bool edited = false;
			if (component.Mesh.HasHandle())
			{
				const AssetMetadata* metadata = AssetManager::GetMetadata(component.Mesh.Handle());
				if (metadata)
					ImGui::Text("Mesh: %s", metadata->FilePath.c_str());
				else
					ImGui::Text("Mesh handle: %" PRIu64,
						static_cast<uint64_t>(component.Mesh.Handle()));

				const Ref<Mesh>& mesh = component.Mesh.Get();
				if (mesh)
				{
					ImGui::Text("Submeshes: %u", (uint32_t)mesh->GetSubmeshes().size());

					// One row per renderer slot. The slot list used to edit the mesh's shared
					// Material objects directly, which changed every entity using that mesh in
					// every scene and persisted nowhere.
					const auto& materials = mesh->GetMaterials();
					component.MaterialOverrides.resize(materials.size());

					ImGui::Text("Materials: %u", (uint32_t)materials.size());
					ImGui::TextDisabled("Drop a .gmat on a slot to override it");

					for (uint32_t i = 0; i < (uint32_t)materials.size(); i++)
					{
						ImGui::PushID((int)i);
						ImGui::Separator();

						const AssetHandle slot = component.MaterialOverrides[i].Handle();
						const AssetMetadata* slotMetadata = IsAssetHandleValid(slot)
							? AssetManager::GetMetadata(slot) : nullptr;

						const std::string imported = materials[i] ? materials[i]->GetName() : std::string("(none)");
						if (slotMetadata)
							ImGui::Text("Slot %u: %s", i, slotMetadata->FilePath.c_str());
						else if (IsAssetHandleValid(slot))
							ImGui::Text("Slot %u: unknown material %" PRIu64, i,
								static_cast<uint64_t>(slot));
						else
							ImGui::Text("Slot %u: (default: %s)", i, imported.c_str());

						// Assigning a slot is an ordinary component edit, so Phase 2's undo
						// covers it with no new code - see docs/editor/editor.md. The slot
						// accepts only a Material because AcceptAssetDropRef reads the accepted
						// type off AssetRef<Material> rather than from a hand-written argument.
						AssetRef<Material> dropped = EditorUI::AcceptAssetDropRef<Material>();
						if (dropped.HasHandle())
						{
							component.MaterialOverrides[i] = dropped;
							edited = true;
						}

						if (IsAssetHandleValid(slot))
						{
							ImGui::SameLine();
							if (ImGui::SmallButton("Clear"))
							{
								component.MaterialOverrides[i].Reset();
								edited = true;
							}

							EditorUI::DrawMaterialAssetEditor(slot);
						}

						ImGui::PopID();
					}
				}
			}
			else
			{
				ImGui::TextDisabled("No mesh assigned");
			}

			// The .gmat editor rows above deliberately do NOT contribute to `edited`: they
			// mutate the shared Material asset, not this component, and an undo command
			// claiming to own an asset edit would lie about its scope. Slot assignment and
			// clearing do, because those are component state.
			AssetRef<Mesh> dropped = EditorUI::AcceptAssetDropRef<Mesh>();
			if (dropped.HasHandle())
			{
				component.Mesh = dropped;

				// The new mesh has its own slot count and its own material identities; keeping
				// the old entity's overrides would apply material 2 of one mesh to material 2
				// of an unrelated one. Clearing is the honest option (the ScriptComponent
				// Fields precedent).
				component.MaterialOverrides.clear();
				edited = true;
			}

			return edited;
		});

		DrawComponent<AnimatorComponent>("Animator", entity, [entity](auto& component)
		{
			bool edited = false;
			Ref<Mesh> mesh = entity.HasComponent<StaticMeshComponent>()
				? entity.GetComponent<StaticMeshComponent>().Mesh.Get()
				: nullptr;

			const bool rigged = mesh && mesh->HasSkeleton();
			if (!rigged)
			{
				ImGui::TextDisabled("No rigged mesh on this entity");
			}
			else
			{
				// A combo over the mesh's own clip names rather than a text field: the name *is*
				// the reference, and typing it by hand is exactly how you end up silently posed
				// at bind while wondering why nothing moves.
				const auto& clips = mesh->GetClips();
				if (ImGui::BeginCombo("Clip", component.Clip.empty() ? "(none)" : component.Clip.c_str()))
				{
					if (ImGui::Selectable("(none)", component.Clip.empty()))
					{
						component.Clip.clear();
						edited = true;
					}

					for (const AnimationClip& clip : clips)
					{
						const bool selected = component.Clip == clip.Name;
						if (ImGui::Selectable(clip.Name.c_str(), selected))
						{
							component.Clip = clip.Name;
							edited = true;
						}
						if (selected)
							ImGui::SetItemDefaultFocus();
					}

					ImGui::EndCombo();
				}

				if (clips.empty())
					ImGui::TextDisabled("Mesh is rigged but carries no clips");
			}

			edited |= ImGui::DragFloat("Speed", &component.Speed, 0.01f, -10.0f, 10.0f);
			edited |= ImGui::Checkbox("Playing", &component.Playing);
			ImGui::SameLine();
			edited |= ImGui::Checkbox("Loop", &component.Loop);

			// Edit mode evaluates the pose but never runs the clock, so this slider is the only
			// way to move a rig without entering play mode. Scrubbing clears Playing, which
			// matters in play mode only - there the clock would otherwise overwrite the scrubbed
			// value on the very next update and the slider would appear not to work at all.
			const AnimationClip* clip = rigged ? mesh->FindClip(component.Clip) : nullptr;
			const float duration = clip ? clip->Duration : 0.0f;
			if (ImGui::DragFloat("Time", &component.Time, 0.01f, 0.0f, duration))
			{
				component.Playing = false;
				edited = true;
			}
			if (duration > 0.0f)
			{
				ImGui::SameLine();
				ImGui::TextDisabled("/ %.2fs", duration);
			}

			if (rigged)
				ImGui::Text("Joints: %u", mesh->GetSkeleton().JointCount());

			return edited;
		});

		DrawComponent<BoneAttachmentComponent>("Bone Attachment", entity, [&](auto& component)
		{
			bool edited = false;

			// Target is an entity UUID, zero meaning parent. The generic drawer would print
			// the number; the useful widget is a drop from the outliner plus a Parent button.
			Entity target;
			std::string targetLabel = "(parent)";
			if (component.Target != UUID{ 0 })
			{
				target = m_Context ? m_Context->FindEntityByUUID(component.Target) : Entity{};
				targetLabel = target ? target.GetName() : "<missing>";
				if (target && targetLabel.empty())
					targetLabel = "(unnamed)";
			}
			else if (m_Context)
			{
				const UUID parentID = entity.GetComponent<RelationshipComponent>().Parent;
				if (parentID != UUID{ 0 })
					target = m_Context->FindEntityByUUID(parentID);
			}

			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted("Target");
			ImGui::SameLine();
			ImGui::Button(targetLabel.c_str(), ImVec2(-1.0f, 0.0f));
			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SCENE_HIERARCHY_ENTITY"))
				{
					UUID droppedID = *(const UUID*)payload->Data;
					if (droppedID != entity.GetUUID())
					{
						component.Target = droppedID;
						component.Resolved = -1;
						edited = true;
					}
				}
				ImGui::EndDragDropTarget();
			}

			if (ImGui::SmallButton("Parent"))
			{
				component.Target = UUID{ 0 };
				component.Resolved = -1;
				edited = true;
			}
			ImGui::SameLine();
			ImGui::TextDisabled("Drop an entity, or Parent for the hierarchy parent");

			Ref<Mesh> mesh;
			if (target && target.HasComponent<StaticMeshComponent>())
				mesh = target.GetComponent<StaticMeshComponent>().Mesh.Get();

			const bool rigged = mesh && mesh->HasSkeleton();
			if (!rigged)
			{
				ImGui::TextDisabled("Target has no rigged mesh");
			}
			else
			{
				const auto& names = mesh->GetSkeleton().JointNames;
				const bool canPick = m_JointTool && m_JointTool->VisualizerOn && Recording();
				const bool armed = canPick && m_JointTool->PickForSocket
					&& m_JointTool->SocketEntity == entity.GetUUID();
				const float pickSlot = 28.0f;
				ImGui::SetNextItemWidth(ImMax(64.0f, ImGui::GetContentRegionAvail().x - pickSlot));
				if (ImGui::BeginCombo("Joint",
					component.Joint.empty() ? "(none)" : component.Joint.c_str()))
				{
					if (ImGui::Selectable("(none)", component.Joint.empty()))
					{
						component.Joint.clear();
						component.Resolved = -1;
						edited = true;
					}

					for (const std::string& name : names)
					{
						const bool selected = component.Joint == name;
						if (ImGui::Selectable(name.c_str(), selected))
						{
							component.Joint = name;
							component.Resolved = -1;
							edited = true;
						}
						if (selected)
							ImGui::SetItemDefaultFocus();
					}

					ImGui::EndCombo();
				}

				ImGui::SameLine();
				ImGui::BeginDisabled(!canPick);
				const char* pickTip = !m_JointTool
					? "Pick joint in the viewport"
					: (!Recording()
						? "Play mode cannot assign a joint"
						: (!m_JointTool->VisualizerOn
							? "Turn on Visualizers → Skeletons to pick in the viewport"
							: "Pick joint in the viewport. Bones win over entity pick. Esc cancels."));
				if (EditorUI::IconButton(ICON_LC_CROSSHAIR, pickTip, armed))
				{
					if (armed)
						m_JointTool->CancelPick();
					else
					{
						m_JointTool->PickForSocket = true;
						m_JointTool->SocketEntity = entity.GetUUID();
					}
				}
				ImGui::EndDisabled();

				if (names.empty())
					ImGui::TextDisabled("Mesh is rigged but lists no joint names");
			}

			edited |= DrawReflected(entity, m_Context.get(), m_Selection, component);
			return edited;
		});

		DrawComponent<ScriptComponent>("Script", entity, [](auto& component)
		{
			bool edited = false;
			if (IsAssetHandleValid(component.Script))
			{
				const AssetMetadata* metadata = AssetManager::GetMetadata(component.Script);
				if (metadata)
					ImGui::Text("Script: %s", metadata->FilePath.c_str());
				else
					ImGui::Text("Script handle: %" PRIu64,
						static_cast<uint64_t>(component.Script));

				if (ImGui::Button("Clear"))
				{
					component.Script = InvalidAssetHandle;
					edited = true;
				}
			}
			else
			{
				ImGui::TextDisabled("No script assigned");
			}

			ImGui::TextDisabled("Drop a .lua file here");

			AssetHandle dropped = EditorUI::AcceptAssetDropHandle(AssetType::Script);
			if (IsAssetHandleValid(dropped))
			{
				component.Script = dropped;
				// Overrides are keyed by name against the old script's declarations;
				// carrying them to a different script would apply values it never asked
				// for. Clearing is the honest option.
				component.Fields.clear();
				edited = true;
			}

			edited |= DrawScriptFields(component);
			return edited;
		});

		DrawComponent<DirectionalLightComponent>("Directional Light", entity, [&](auto& component)
		{
			return DrawReflected(entity, m_Context.get(), m_Selection, component);
		});

		DrawComponent<PointLightComponent>("Point Light", entity, [&](auto& component)
		{
			return DrawReflected(entity, m_Context.get(), m_Selection, component);
		});

		DrawComponent<SpotLightComponent>("Spot Light", entity, [&](auto& component)
		{
			// Drawn generically, then the one cross-field invariant is enforced afterwards. That
			// ordering matters: a generic drawer cannot express "outer >= inner" because it sees
			// one field at a time, but nothing stops the section from fixing up the component the
			// drawer just wrote. The clamp is the section's job; the widgets are not.
			const bool edited = DrawReflected(entity, m_Context.get(), m_Selection, component);

			if (edited && component.OuterConeAngle < component.InnerConeAngle)
				component.OuterConeAngle = component.InnerConeAngle;

			return edited;
		});

		// An assigned environment makes the two procedural colours unreachable fallbacks, so they
		// are filtered out rather than drawn disabled - showing an author a control that cannot
		// affect anything is worse than not showing it.
		DrawComponent<SkyLightComponent>("Sky Light", entity, [&](auto& component)
		{
			const bool hasEnvironment = component.Environment.HasHandle();

			return DrawReflected(entity, m_Context.get(), m_Selection, component,
				[hasEnvironment](const entt::meta_data& field)
				{
					const std::string name = field.name() ? field.name() : "";
					if (name == "SkyColor" || name == "GroundColor")
						return !hasEnvironment;
					return true;
				});
		});

		// The clip slot, its Clear button and its typed drop target all come from the bare
		// AssetHandle drawer, which identifies the field as an asset slot from Attr::Slot -
		// AssetHandle is an alias for UUID, so the type alone could never have said so.
		//
		// Converting moved Group to the end and unpaired the checkboxes that shared a line: the
		// attribute vocabulary has no way to say "put these two together", and adding layout knobs
		// to it was rejected in R1. The order is now the registration order, which is the point.
		DrawComponent<AudioSourceComponent>("Audio Source", entity, [&](auto& component)
		{
			return DrawReflected(entity, m_Context.get(), m_Selection, component);
		});

		DrawComponent<AudioListenerComponent>("Audio Listener", entity, [&](auto& component)
		{
			return DrawReflected(entity, m_Context.get(), m_Selection, component);
		});

		// The largest section in the panel, and the last one to convert. What blocked it was the
		// five min/max pairs: the clamp direction depends on which half the author moved, and a
		// drawer seeing two unrelated floats cannot know. `RangeF` makes each pair one field with
		// one drawer, which does know - see Components.h.
		//
		// Everything else came for free from attributes R1 had already written: the four
		// CollapsingHeaders are `Attr::Section`, the curve and gradient editors are drawers keyed
		// on FloatCurve and ColorGradient, and the three asset slots are AssetRef<T>.
		DrawComponent<ParticleEmitterComponent>("Particle Emitter", entity, [&](auto& component)
		{
			// Preview transport. These mutate Playing/pool/RNG - not authored, not serialized -
			// so they must not join `edited`. A Button still takes ActiveId for the click;
			// TrackCommitBoundary drops the pending edit because Edited stayed false. They are
			// actions rather than fields, which is why they stay hand-written above the
			// reflected ones rather than being expressed as an attribute.
			if (ImGui::Button("Play"))
				component.PlayPreview(entity.GetUUID());
			ImGui::SameLine();
			if (ImGui::Button("Stop"))
				component.StopPreview();
			ImGui::SameLine();
			if (ImGui::Button("Restart"))
				component.RestartPreview(entity.GetUUID());
			ImGui::SameLine();
			ImGui::TextDisabled("%s  %u live  t=%.2f",
				component.Playing ? "Playing" : "Stopped",
				(uint32_t)component.Pool.size(),
				component.Time);

			return DrawReflected(entity, m_Context.get(), m_Selection, component);
		});

		DrawComponent<RigidBodyComponent>("Rigid Body", entity, [&](auto& component)
		{
			// The Type combo's entries come from RigidBodyType's own registration, so adding a
			// body type is one line beside the enum instead of a parallel string array here.
			return DrawReflected(entity, m_Context.get(), m_Selection, component);
		});

		// PhysicsMaterial is drawn by Trait::Flatten, which puts Friction and Restitution beside
		// the collider's own fields rather than under a sub-header - the same shape the flag
		// already forces on SceneSerializer. The `drawPhysicsMaterial` lambda these three shared
		// is gone with them.
		DrawComponent<BoxColliderComponent>("Box Collider", entity, [&](auto& component)
		{
			return DrawReflected(entity, m_Context.get(), m_Selection, component);
		});

		DrawComponent<SphereColliderComponent>("Sphere Collider", entity, [&](auto& component)
		{
			return DrawReflected(entity, m_Context.get(), m_Selection, component);
		});

		DrawComponent<CapsuleColliderComponent>("Capsule Collider", entity, [&](auto& component)
		{
			return DrawReflected(entity, m_Context.get(), m_Selection, component);
		});

		DrawAddComponentButton();
	}
}
