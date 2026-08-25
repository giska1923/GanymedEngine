#include "SceneHierarchyPanel.h"
#include "../AssetDragDrop.h"

#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>

#include <glm/gtc/type_ptr.hpp>

#include "GanymedE/Scene/Components.h"
#include "GanymedE/Assets/AssetManager.h"
#include "GanymedE/Assets/AssetPaths.h"
#include "GanymedE/Assets/MaterialSerializer.h"
#include "GanymedE/Assets/TextureImporter.h"
#include "GanymedE/Renderer/Material.h"
#include "GanymedE/Renderer/Mesh.h"
#include "GanymedE/Scripting/ScriptEngine.h"

#include <algorithm>

namespace GanymedE {

	SceneHierarchyPanel::SceneHierarchyPanel(const Ref<Scene>& context)
	{
		SetContext(context);
	}

	void SceneHierarchyPanel::SetContext(const Ref<Scene>& context)
	{
		m_Context = context;
		m_SelectionContext = {};

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
			m_SelectionContext = {};
	}

	void SceneHierarchyPanel::OnImGuiRender()
	{
		ValidateSelection();

		ImGui::Begin("Scene Hierarchy");

		if (m_Context)
		{
			// Draw root entities only; children are drawn recursively
			auto view = m_Context->m_Registry.view<IDComponent, RelationshipComponent, TagComponent>();
			for (auto entityID : view)
			{
				Entity entity{ entityID, m_Context.get() };
				if (entity.GetComponent<RelationshipComponent>().Parent == UUID{ 0 })
					DrawEntityNode(entity);
			}

			// Serviced here rather than inside the walk: an editor delete now takes the whole
			// subtree, and destroying entities the view above is still iterating invalidates it.
			if (m_EntityToDelete != UUID{ 0 })
			{
				DeleteEntity(m_Context->FindEntityByUUID(m_EntityToDelete));
				m_EntityToDelete = UUID{ 0 };
			}

			if (ImGui::IsMouseDown(0) && ImGui::IsWindowHovered())
			{
				m_SelectionContext = {};
			}

			// Drop onto empty space → unparent
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

			// Right-click on blank space
			if (ImGui::BeginPopupContextWindow(0, ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
			{
				if (ImGui::MenuItem("Create Empty Entity"))
				{
					Entity created = m_Context->CreateEntity("Empty Entity");
					PushAddedEntities("Create Entity", created);
					m_SelectionContext = created;
				}

				ImGui::EndPopup();
			}
		}

		ImGui::End();

		ImGui::Begin("Properties");
		if (m_SelectionContext)
		{
			DrawComponents(m_SelectionContext);
		}

		ImGui::End();

		// An inspector edit whose section stopped being drawn mid-gesture has nowhere else to
		// be noticed.
		FlushPendingEdit();
	}

	void SceneHierarchyPanel::DrawEntityNode(Entity entity)
	{
		auto& tag = entity.GetComponent<TagComponent>().Tag;
		auto& relationship = entity.GetComponent<RelationshipComponent>();

		// Use the entt handle for ImGui IDs — always unique in-session.
		// UUIDs can collide in older scene files that serialized a hardcoded ID.
		ImGui::PushID((int32_t)(entt::entity)entity);

		ImGuiTreeNodeFlags flags = ((m_SelectionContext == entity) ? ImGuiTreeNodeFlags_Selected : 0)
			| ImGuiTreeNodeFlags_OpenOnArrow
			| ImGuiTreeNodeFlags_SpanAvailWidth;
		if (relationship.Children.empty())
			flags |= ImGuiTreeNodeFlags_Leaf;

		bool opened = ImGui::TreeNodeEx("Entity", flags, "%s", tag.c_str());
		if (ImGui::IsItemClicked())
		{
			m_SelectionContext = entity;
		}

		if (ImGui::BeginDragDropSource())
		{
			UUID entityID = entity.GetUUID();
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
		if (ImGui::BeginPopupContextItem())
		{
			if (ImGui::MenuItem("Delete Entity"))
			{
				entityDeleted = true;
			}

			ImGui::EndPopup();
		}

		if (opened)
		{
			// Copy children first — SetParent during drag can mutate the vector we're iterating
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

		if (entityDeleted)
			m_EntityToDelete = entity.GetUUID();
	}

	// Returns true when a widget in this row actually edited `values` - the ground truth the
	// undo commit boundary is built on. It used to return void, which meant the caller had to
	// diff the value instead, and a value diff cannot tell a real edit from a float that came
	// back changed through a degrees/radians round-trip.
	static bool DrawVec3Control(const std::string& label, glm::vec3& values, float resetValue = 0.0f, float columnWidth = 100.0f)
	{
		bool edited = false;
		ImGuiIO& io = ImGui::GetIO();
		auto boldFont = io.Fonts->Fonts[0];

		ImGui::PushID(label.c_str());

		ImGui::Columns(2);
		ImGui::SetColumnWidth(0, columnWidth);
		ImGui::Text(label.c_str());
		ImGui::NextColumn();

		ImGui::PushMultiItemsWidths(3, ImGui::CalcItemWidth());
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{ 0, 0 });

		float lineHeight = GImGui->Font->FontSize + GImGui->Style.FramePadding.y * 2.0f;
		ImVec2 buttonSize = { lineHeight + 3.0f, lineHeight };

		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{ 0.8f, 0.1f, 0.15f, 1.0f });
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{ 0.9f, 0.2f, 0.2f, 1.0f });
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{ 0.8f, 0.1f, 0.15f, 1.0f });
		ImGui::PushFont(boldFont);
		if (ImGui::Button("X", buttonSize))
		{
			values.x = resetValue;
			edited = true;
		}

		ImGui::PopFont();
		ImGui::PopStyleColor(3);

		ImGui::SameLine();
		edited |= ImGui::DragFloat("##X", &values.x, 0.1f, 0.0f, 0.0f, "%.2f");
		ImGui::PopItemWidth();
		ImGui::SameLine();

		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{ 0.2f, 0.7f, 0.2f, 1.0f });
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{ 0.3f, 0.8f, 0.3f, 1.0f });
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{ 0.2f, 0.7f, 0.2f, 1.0f });
		ImGui::PushFont(boldFont);
		if (ImGui::Button("Y", buttonSize))
		{
			values.y = resetValue;
			edited = true;
		}

		ImGui::PopFont();
		ImGui::PopStyleColor(3);

		ImGui::SameLine();
		edited |= ImGui::DragFloat("##Y", &values.y, 0.1f, 0.0f, 0.0f, "%.2f");
		ImGui::PopItemWidth();
		ImGui::SameLine();

		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{ 0.1f, 0.25f, 0.8f, 1.0f });
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{ 0.2f, 0.35f, 0.9f, 1.0f });
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{ 0.1f, 0.25f, 0.8f, 1.0f });
		ImGui::PushFont(boldFont);
		if (ImGui::Button("Z", buttonSize))
		{
			values.z = resetValue;
			edited = true;
		}

		ImGui::PopFont();
		ImGui::PopStyleColor(3);

		ImGui::SameLine();
		edited |= ImGui::DragFloat("##Z", &values.z, 0.1f, 0.0f, 0.0f, "%.2f");
		ImGui::PopItemWidth();

		ImGui::PopStyleVar();

		ImGui::Columns(1);

		ImGui::PopID();
		return edited;
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
		const T& before, uint32_t activeOnEntry, uint32_t activeOnExit, bool edited)
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
			m_Pending.ActiveId = activeOnExit;
			m_Pending.Edited = edited;
			m_Pending.Visited = true;
			return;
		}

		// An edit with no active phase: a drop delivered onto this section, or a popup item
		// that closed in the same frame. There is nothing to wait for.
		if (edited && entity.HasComponent<T>())
		{
			m_UndoStack->Push(CreateScope<ComponentEditCommand<T>>(
				"Edit " + name, entity.GetUUID(), before, entity.GetComponent<T>()));
		}
	}

	void SceneHierarchyPanel::CommitPendingEdit()
	{
		if (!m_Pending.Command)
			return;

		if (m_Pending.Edited && m_UndoStack && m_Context)
		{
			m_Pending.Command->CaptureAfter(*m_Context);
			m_UndoStack->Push(std::move(m_Pending.Command));
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

	// The inline editor for one .gmat asset.
	//
	// These edits are live on the shared Ref, so they show up immediately in every entity and
	// every scene using this material - the header text says so, because a global edit that
	// looks local is the worst version of this UI. They are also **not undoable** (undo covers
	// scene edits only); Save and Revert are the asset-level transaction model instead, and
	// Revert is just AssetManager::Reload.
	static void DrawMaterialAssetEditor(AssetHandle handle)
	{
		const AssetMetadata* metadata = AssetManager::GetMetadata(handle);
		Ref<Material> material = AssetManager::GetAsset<Material>(handle);
		if (!metadata || !material)
		{
			ImGui::TextDisabled("Material asset could not be loaded");
			return;
		}

		ImGui::PushID((int)(uint64_t)handle);
		ImGui::Separator();
		ImGui::Text("%s", metadata->FilePath.c_str());
		ImGui::TextDisabled("Edits apply to this asset everywhere it is used, and are not undoable");

		glm::vec4 albedo = material->GetAlbedoColor();
		if (ImGui::ColorEdit4("Albedo", glm::value_ptr(albedo)))
			material->SetAlbedoColor(albedo);

		float metallic = material->GetMetallic();
		if (ImGui::DragFloat("Metallic", &metallic, 0.01f, 0.0f, 1.0f))
			material->SetMetallic(metallic);

		float roughness = material->GetRoughness();
		if (ImGui::DragFloat("Roughness", &roughness, 0.01f, 0.0f, 1.0f))
			material->SetRoughness(roughness);

		bool transparent = material->IsTransparent();
		if (ImGui::Checkbox("Transparent", &transparent))
			material->SetTransparent(transparent);
		ImGui::SameLine();
		bool twoSided = material->IsTwoSided();
		if (ImGui::Checkbox("Two Sided", &twoSided))
			material->SetTwoSided(twoSided);

		// Texture maps are assigned by dragging a texture asset; the material stores the path,
		// because a .gmat has to stay self-describing and hand-mergeable.
		struct MapRow
		{
			const char* Label;
			const std::string& (Material::*GetPath)() const;
			void (Material::*SetPath)(const std::string&);
			void (Material::*SetTexture)(const Ref<Texture2D>&);
		};

		const MapRow rows[] = {
			{ "Albedo Map",     &Material::GetAlbedoMapPath,            &Material::SetAlbedoMapPath,            &Material::SetAlbedoMap },
			{ "Normal Map",     &Material::GetNormalMapPath,            &Material::SetNormalMapPath,            &Material::SetNormalMap },
			{ "Metal/Rough Map",&Material::GetMetallicRoughnessMapPath, &Material::SetMetallicRoughnessMapPath, &Material::SetMetallicRoughnessMap },
		};

		for (const MapRow& row : rows)
		{
			ImGui::PushID(row.Label);

			const std::string& path = (material.get()->*row.GetPath)();
			ImGui::Text("%s: %s", row.Label, path.empty() ? "(none)" : path.c_str());

			if (auto dropped = EditorUI::AcceptAssetDrop(AssetType::Texture))
			{
				const std::string relative = dropped->generic_string();
				(material.get()->*row.SetPath)(relative);
				(material.get()->*row.SetTexture)(TextureImporter::LoadMaterialMap(relative));
			}

			if (!path.empty())
			{
				ImGui::SameLine();
				if (ImGui::SmallButton("Clear"))
				{
					(material.get()->*row.SetPath)(std::string());
					(material.get()->*row.SetTexture)(nullptr);
				}
			}

			ImGui::PopID();
		}

		ImGui::TextDisabled("Drop a texture on a map row to assign it");

		if (ImGui::Button("Save"))
		{
			if (MaterialSerializer::Save(material, GetAssetRoot() / metadata->FilePath))
				GE_INFO("Saved material '{0}'", metadata->FilePath);
		}

		ImGui::SameLine();
		if (ImGui::Button("Revert"))
			AssetManager::Reload(handle);

		ImGui::PopID();
	}

	template<typename T, typename UIFunction>
	void SceneHierarchyPanel::DrawComponent(const std::string& name, Entity entity, UIFunction uiFunction)
	{
		const ImGuiTreeNodeFlags treeNodeFlags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_AllowItemOverlap | ImGuiTreeNodeFlags_FramePadding;
		if (entity.HasComponent<T>())
		{
			ImVec2 contentRegionAvailable = ImGui::GetContentRegionAvail();

			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{ 4, 4 });
			float lineHeight = GImGui->Font->FontSize + GImGui->Style.FramePadding.y * 2.0f;
			ImGui::Separator();
			bool open = ImGui::TreeNodeEx((void*)typeid(T).hash_code(), treeNodeFlags, name.c_str());
			ImGui::PopStyleVar();
			ImGui::SameLine(contentRegionAvailable.x - lineHeight * 0.5f);
			ImGui::PushID((int)typeid(T).hash_code());
			if (ImGui::Button("+", ImVec2{ lineHeight, lineHeight }))
			{
				ImGui::OpenPopup("ComponentSettings");
			}

			bool removeComponent = false;
			if (ImGui::BeginPopup("ComponentSettings"))
			{
				if (ImGui::MenuItem("Remove component"))
				{
					removeComponent = true;
				}

				ImGui::EndPopup();
			}
			ImGui::PopID();

			if (open)
			{
				auto& component = entity.GetComponent<T>();

				// Taken every frame; only the copy from the frame a widget grabs the active
				// item is kept, and one frame later that value is unrecoverable. Components
				// are handle/POD/small-string sized, so the copy is not worth avoiding.
				T before = component;
				const ImGuiID activeOnEntry = ImGui::GetActiveID();

				const bool edited = uiFunction(component);

				TrackCommitBoundary<T>(entity, name, before, activeOnEntry, ImGui::GetActiveID(), edited);
				ImGui::TreePop();
			}

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
	}

	template<typename T>
	void SceneHierarchyPanel::DrawAddComponentEntry(const char* label)
	{
		if (m_SelectionContext.HasComponent<T>())
			return;

		if (!ImGui::MenuItem(label))
			return;

		T& component = m_SelectionContext.AddComponent<T>();
		if (Recording())
		{
			m_UndoStack->Push(CreateScope<AddComponentCommand<T>>(
				std::string("Add ") + label, m_SelectionContext.GetUUID(), component));
		}

		ImGui::CloseCurrentPopup();
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
			m_SelectionContext = {};
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
		m_SelectionContext = copy;
	}

	void SceneHierarchyPanel::DeleteSelectedEntity()
	{
		DeleteEntity(m_SelectionContext);
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

			TrackCommitBoundary<TagComponent>(entity, "Name", before, activeOnEntry,
				ImGui::GetActiveID(), edited);
		}

		ImGui::SameLine();
		ImGui::PushItemWidth(-1);

		if (ImGui::Button("Add Component"))
		{
			ImGui::OpenPopup("AddComponent");
		}

		if (ImGui::BeginPopup("AddComponent"))
		{
			DrawAddComponentEntry<CameraComponent>("Camera");
			DrawAddComponentEntry<SpriteRendererComponent>("Sprite Renderer");
			DrawAddComponentEntry<DirectionalLightComponent>("Directional Light");
			DrawAddComponentEntry<PointLightComponent>("Point Light");
			DrawAddComponentEntry<SpotLightComponent>("Spot Light");
			DrawAddComponentEntry<SkyLightComponent>("Sky Light");
			DrawAddComponentEntry<AnimatorComponent>("Animator");
			DrawAddComponentEntry<ScriptComponent>("Script");
			DrawAddComponentEntry<AudioSourceComponent>("Audio Source");
			DrawAddComponentEntry<AudioListenerComponent>("Audio Listener");
			DrawAddComponentEntry<RigidBodyComponent>("Rigid Body");
			DrawAddComponentEntry<BoxColliderComponent>("Box Collider");
			DrawAddComponentEntry<SphereColliderComponent>("Sphere Collider");
			DrawAddComponentEntry<CapsuleColliderComponent>("Capsule Collider");

			ImGui::EndPopup();
		}

		ImGui::PopItemWidth();

		DrawComponent<TransformComponent>("Transform", entity, [&](auto& component)
		{
			bool edited = DrawVec3Control("Translation", component.Translation);

			// Rotation is stored in radians and shown in degrees, and the round-trip is not
			// exact. Writing back unconditionally therefore changed the stored value on frames
			// with no user input at all - harmless for rendering, but under any value-diff
			// scheme it mints a phantom undo command and marks the scene dirty on selection.
			// Write back only when the row says it was edited.
			glm::vec3 rotation = glm::degrees(component.Rotation);
			if (DrawVec3Control("Rotation", rotation))
			{
				component.Rotation = glm::radians(rotation);
				edited = true;
			}

			edited |= DrawVec3Control("Scale", component.Scale, 1.0f);

			// Editing the component directly is invisible to change tracking, so the cached
			// world transform would never be refreshed.
			if (edited)
				m_Context->MarkChanged<TransformComponent>(entity);

			return edited;
		});

		DrawComponent<CameraComponent>("Camera", entity, [](auto& component)
		{
			auto& camera = component.Camera;

			bool edited = ImGui::Checkbox("Primary", &component.Primary);

			const char* projectionTypeStrings[] = { "Perspective", "Orthographic" };
			const char* currentProjectionTypeString = projectionTypeStrings[(int)camera.GetProjectionType()];
			if (ImGui::BeginCombo("Projection", currentProjectionTypeString))
			{
				for (int i = 0; i < 2; i++)
				{
					bool isSelected = currentProjectionTypeString == projectionTypeStrings[i];
					if (ImGui::Selectable(projectionTypeStrings[i], isSelected))
					{
						currentProjectionTypeString = projectionTypeStrings[i];
						camera.SetProjectionType((SceneCamera::ProjectionType)i);
						edited = true;
					}

					if (isSelected)
					{
						ImGui::SetItemDefaultFocus();
					}
				}

				ImGui::EndCombo();
			}

			if (camera.GetProjectionType() == SceneCamera::ProjectionType::Perspective)
			{
				float perspectiveVerticalFov = glm::degrees(camera.GetPerspectiveVerticalFOV());
				if (ImGui::DragFloat("Vertical FOV", &perspectiveVerticalFov))
				{
					camera.SetPerspectiveVerticalFOV(glm::radians(perspectiveVerticalFov));
					edited = true;
				}

				float perspectiveNear = camera.GetPerspectiveNearClip();
				if (ImGui::DragFloat("Near", &perspectiveNear))
				{
					camera.SetPerspectiveNearClip(perspectiveNear);
					edited = true;
				}

				float perspectiveFar = camera.GetPerspectiveFarClip();
				if (ImGui::DragFloat("Far", &perspectiveFar))
				{
					camera.SetPerspectiveFarClip(perspectiveFar);
					edited = true;
				}
			}

			if (camera.GetProjectionType() == SceneCamera::ProjectionType::Orthographic)
			{
				float orthoSize = camera.GetOrthographicSize();
				if (ImGui::DragFloat("Size", &orthoSize))
				{
					camera.SetOrthographicSize(orthoSize);
					edited = true;
				}

				float orthoNear = camera.GetOrthographicNearClip();
				if (ImGui::DragFloat("Near", &orthoNear))
				{
					camera.SetOrthographicNearClip(orthoNear);
					edited = true;
				}

				float orthoFar = camera.GetOrthographicFarClip();
				if (ImGui::DragFloat("Far", &orthoFar))
				{
					camera.SetOrthographicFarClip(orthoFar);
					edited = true;
				}

				edited |= ImGui::Checkbox("Fixed Aspect Ratio", &component.FixedAspectRatio);
			}

			return edited;
		});

		DrawComponent<SpriteRendererComponent>("Sprite Renderer", entity, [](auto& component)
		{
			return ImGui::ColorEdit4("Color", glm::value_ptr(component.Color));
		});

		DrawComponent<StaticMeshComponent>("Static Mesh", entity, [](auto& component)
		{
			bool edited = false;
			if (IsAssetHandleValid(component.Mesh))
			{
				const AssetMetadata* metadata = AssetManager::GetMetadata(component.Mesh);
				if (metadata)
					ImGui::Text("Mesh: %s", metadata->FilePath.c_str());
				else
					ImGui::Text("Mesh handle: %llu", static_cast<uint64_t>(component.Mesh));

				Ref<Mesh> mesh = AssetManager::GetAsset<Mesh>(component.Mesh);
				if (mesh)
				{
					ImGui::Text("Submeshes: %u", (uint32_t)mesh->GetSubmeshes().size());

					// One row per renderer slot. The slot list used to edit the mesh's shared
					// Material objects directly, which changed every entity using that mesh in
					// every scene and persisted nowhere.
					const auto& materials = mesh->GetMaterials();
					component.MaterialOverrides.resize(materials.size(), InvalidAssetHandle);

					ImGui::Text("Materials: %u", (uint32_t)materials.size());
					ImGui::TextDisabled("Drop a .gmat on a slot to override it");

					for (uint32_t i = 0; i < (uint32_t)materials.size(); i++)
					{
						ImGui::PushID((int)i);
						ImGui::Separator();

						const AssetHandle slot = component.MaterialOverrides[i];
						const AssetMetadata* slotMetadata = IsAssetHandleValid(slot)
							? AssetManager::GetMetadata(slot) : nullptr;

						const std::string imported = materials[i] ? materials[i]->GetName() : std::string("(none)");
						if (slotMetadata)
							ImGui::Text("Slot %u: %s", i, slotMetadata->FilePath.c_str());
						else if (IsAssetHandleValid(slot))
							ImGui::Text("Slot %u: unknown material %llu", i, static_cast<uint64_t>(slot));
						else
							ImGui::Text("Slot %u: (default: %s)", i, imported.c_str());

						// Assigning a slot is an ordinary component edit, so Phase 2's undo
						// covers it with no new code - see docs/editor/editor.md.
						AssetHandle dropped = EditorUI::AcceptAssetDropHandle(AssetType::Material);
						if (IsAssetHandleValid(dropped))
						{
							component.MaterialOverrides[i] = dropped;
							edited = true;
						}

						if (IsAssetHandleValid(slot))
						{
							ImGui::SameLine();
							if (ImGui::SmallButton("Clear"))
							{
								component.MaterialOverrides[i] = InvalidAssetHandle;
								edited = true;
							}

							DrawMaterialAssetEditor(slot);
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
			AssetHandle dropped = EditorUI::AcceptAssetDropHandle(AssetType::StaticMesh);
			if (IsAssetHandleValid(dropped))
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
				? AssetManager::GetAsset<Mesh>(entity.GetComponent<StaticMeshComponent>().Mesh)
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

		DrawComponent<ScriptComponent>("Script", entity, [](auto& component)
		{
			bool edited = false;
			if (IsAssetHandleValid(component.Script))
			{
				const AssetMetadata* metadata = AssetManager::GetMetadata(component.Script);
				if (metadata)
					ImGui::Text("Script: %s", metadata->FilePath.c_str());
				else
					ImGui::Text("Script handle: %llu", static_cast<uint64_t>(component.Script));

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

		DrawComponent<DirectionalLightComponent>("Directional Light", entity, [](auto& component)
		{
			bool edited = ImGui::ColorEdit3("Color", glm::value_ptr(component.Color));
			edited |= ImGui::DragFloat("Intensity", &component.Intensity, 0.05f, 0.0f, 100.0f);
			edited |= ImGui::Checkbox("Cast Shadows", &component.CastShadows);
			ImGui::TextDisabled("Direction = entity -Z (rotate to aim)");
			return edited;
		});

		DrawComponent<PointLightComponent>("Point Light", entity, [](auto& component)
		{
			bool edited = ImGui::ColorEdit3("Color", glm::value_ptr(component.Color));
			edited |= ImGui::DragFloat("Intensity", &component.Intensity, 0.05f, 0.0f, 1000.0f);
			edited |= ImGui::DragFloat("Radius", &component.Radius, 0.1f, 0.0f, 1000.0f);
			edited |= ImGui::DragFloat("Falloff", &component.Falloff, 0.05f, 0.01f, 16.0f);
			return edited;
		});

		DrawComponent<SpotLightComponent>("Spot Light", entity, [](auto& component)
		{
			bool edited = ImGui::ColorEdit3("Color", glm::value_ptr(component.Color));
			edited |= ImGui::DragFloat("Intensity", &component.Intensity, 0.05f, 0.0f, 1000.0f);
			edited |= ImGui::DragFloat("Range", &component.Range, 0.1f, 0.0f, 1000.0f);

			float inner = glm::degrees(component.InnerConeAngle);
			if (ImGui::DragFloat("Inner Cone", &inner, 0.5f, 0.0f, 89.0f))
			{
				component.InnerConeAngle = glm::radians(inner);
				edited = true;
			}

			float outer = glm::degrees(component.OuterConeAngle);
			if (ImGui::DragFloat("Outer Cone", &outer, 0.5f, 0.0f, 89.0f))
			{
				component.OuterConeAngle = glm::radians(glm::max(outer, inner));
				edited = true;
			}

			edited |= ImGui::DragFloat("Falloff", &component.Falloff, 0.05f, 0.01f, 16.0f);
			ImGui::TextDisabled("Direction = entity -Z (rotate to aim)");
			return edited;
		});

		DrawComponent<SkyLightComponent>("Sky Light", entity, [](auto& component)
		{
			bool edited = false;
			if (IsAssetHandleValid(component.Environment))
			{
				const AssetMetadata* metadata = AssetManager::GetMetadata(component.Environment);
				if (metadata)
					ImGui::Text("Environment: %s", metadata->FilePath.c_str());
				ImGui::TextDisabled("Using HDR IBL (procedural colors are fallback)");
			}
			else
			{
				edited |= ImGui::ColorEdit3("Sky Color", glm::value_ptr(component.SkyColor));
				edited |= ImGui::ColorEdit3("Ground Color", glm::value_ptr(component.GroundColor));
			}

			AssetHandle dropped = EditorUI::AcceptAssetDropHandle(AssetType::Environment);
			if (IsAssetHandleValid(dropped))
			{
				component.Environment = dropped;
				edited = true;
			}

			edited |= ImGui::DragFloat("Intensity", &component.Intensity, 0.02f, 0.0f, 20.0f);
			edited |= ImGui::Checkbox("Draw Skybox", &component.DrawSkybox);
			return edited;
		});

		DrawComponent<AudioSourceComponent>("Audio Source", entity, [](auto& component)
		{
			bool edited = false;
			if (IsAssetHandleValid(component.Clip))
			{
				const AssetMetadata* metadata = AssetManager::GetMetadata(component.Clip);
				if (metadata)
					ImGui::Text("Clip: %s", metadata->FilePath.c_str());
				else
					ImGui::Text("Clip handle: %llu", static_cast<uint64_t>(component.Clip));

				if (ImGui::Button("Clear"))
				{
					component.Clip = InvalidAssetHandle;
					edited = true;
				}
			}
			else
			{
				ImGui::TextDisabled("No clip assigned");
			}

			ImGui::TextDisabled("Drop a .wav, .mp3 or .flac file here");

			// Typed drop: AssetTypeFromExtension is the single source of truth for what this
			// field accepts, so a .lua dragged here is simply ignored.
			AssetHandle dropped = EditorUI::AcceptAssetDropHandle(AssetType::Audio);
			if (IsAssetHandleValid(dropped))
			{
				component.Clip = dropped;
				edited = true;
			}

			const char* groupStrings[] = { "Master", "Music", "SFX" };
			int group = (int)component.Group;
			if (ImGui::Combo("Group", &group, groupStrings, 3))
			{
				component.Group = (AudioGroup)group;
				edited = true;
			}

			edited |= ImGui::DragFloat("Volume", &component.Volume, 0.01f, 0.0f, 1.0f);
			edited |= ImGui::DragFloat("Pitch", &component.Pitch, 0.01f, 0.25f, 4.0f);

			edited |= ImGui::Checkbox("Loop", &component.Loop);
			ImGui::SameLine();
			edited |= ImGui::Checkbox("Play On Start", &component.PlayOnStart);

			edited |= ImGui::Checkbox("Spatialize", &component.Spatialize);
			ImGui::SameLine();
			edited |= ImGui::Checkbox("Stream", &component.Stream);

			// Worth saying out loud, because the other four fields DO apply live: these three
			// are baked into the voice when it is created.
			ImGui::TextDisabled("Clip, Spatialize and Stream apply when play starts");
			return edited;
		});

		DrawComponent<AudioListenerComponent>("Audio Listener", entity, [](auto& component)
		{
			const bool edited = ImGui::Checkbox("Primary", &component.Primary);
			ImGui::TextDisabled("Falls back to the primary camera when absent");
			return edited;
		});

		DrawComponent<RigidBodyComponent>("Rigid Body", entity, [](auto& component)
		{
			bool edited = false;
			const char* typeStrings[] = { "Static", "Dynamic", "Kinematic" };
			int type = (int)component.Type;
			if (ImGui::Combo("Type", &type, typeStrings, 3))
			{
				component.Type = (RigidBodyType)type;
				edited = true;
			}

			edited |= ImGui::DragFloat("Mass", &component.Mass, 0.05f, 0.001f, 100000.0f);
			edited |= ImGui::DragFloat("Linear Damping", &component.LinearDamping, 0.01f, 0.0f, 10.0f);
			edited |= ImGui::DragFloat("Angular Damping", &component.AngularDamping, 0.01f, 0.0f, 10.0f);
			edited |= ImGui::Checkbox("Use Gravity", &component.UseGravity);
			return edited;
		});

		auto drawPhysicsMaterial = [](PhysicsMaterial& mat)
		{
			bool edited = ImGui::DragFloat("Friction", &mat.Friction, 0.01f, 0.0f, 10.0f);
			edited |= ImGui::DragFloat("Restitution", &mat.Restitution, 0.01f, 0.0f, 1.0f);
			return edited;
		};

		DrawComponent<BoxColliderComponent>("Box Collider", entity, [&](auto& component)
		{
			bool edited = DrawVec3Control("Half Extents", component.HalfExtents, 0.5f);
			edited |= DrawVec3Control("Offset", component.Offset);
			edited |= drawPhysicsMaterial(component.Material);
			return edited;
		});

		DrawComponent<SphereColliderComponent>("Sphere Collider", entity, [&](auto& component)
		{
			bool edited = ImGui::DragFloat("Radius", &component.Radius, 0.05f, 0.001f, 1000.0f);
			edited |= DrawVec3Control("Offset", component.Offset);
			edited |= drawPhysicsMaterial(component.Material);
			return edited;
		});

		DrawComponent<CapsuleColliderComponent>("Capsule Collider", entity, [&](auto& component)
		{
			bool edited = ImGui::DragFloat("Radius", &component.Radius, 0.05f, 0.001f, 1000.0f);
			edited |= ImGui::DragFloat("Half Height", &component.HalfHeight, 0.05f, 0.001f, 1000.0f);
			edited |= DrawVec3Control("Offset", component.Offset);
			edited |= drawPhysicsMaterial(component.Material);
			return edited;
		});
	}
}
