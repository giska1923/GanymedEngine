#include "SceneHierarchyPanel.h"
#include "../AssetDragDrop.h"
#include "../EditorInspector.h"
#include "../EditorWidgets.h"

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
#include "GanymedE/Scene/PrefabSerializer.h"
#include "GanymedE/Scripting/ScriptEngine.h"
#include "GanymedE/Utils/PlatformUtils.h"

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

				if (ImGui::MenuItem("Instantiate Prefab..."))
				{
					const std::string chosen = FileDialogs::OpenFile("GanymedE Prefab (*.gprefab)\0*.gprefab\0");
					if (!chosen.empty())
						InstantiatePrefab(MakeAssetRelative(chosen));
				}

				ImGui::EndPopup();
			}
		}

		ImGui::End();

		ImGui::Begin("Properties");
		if (m_SelectionContext)
		{
			DrawPrefabControls(m_SelectionContext);
			DrawComponents(m_SelectionContext);
		}

		ImGui::End();

		DrawApplyPrefabModal();

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
		bool createPrefab = false;
		bool revertInstance = false;
		if (ImGui::BeginPopupContextItem())
		{
			if (ImGui::MenuItem("Delete Entity"))
			{
				entityDeleted = true;
			}

			ImGui::Separator();

			if (ImGui::MenuItem("Create Prefab..."))
				createPrefab = true;

			if (entity.HasComponent<PrefabInstanceComponent>())
			{
				if (ImGui::MenuItem("Apply to Prefab..."))
				{
					m_PendingApply = entity.GetUUID();
					m_OpenApplyModal = true;
				}

				if (ImGui::MenuItem("Revert Instance"))
					revertInstance = true;
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

		// Deferred past the tree walk for the same reason delete is: both restructure the scene
		// the enclosing entt view is iterating.
		if (createPrefab)
			CreatePrefabFrom(entity);

		if (revertInstance)
			RevertInstance(entity);

		if (entityDeleted)
			m_EntityToDelete = entity.GetUUID();
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
				// item is kept, and one frame later that value is unrecoverable. This used
				// to claim components were handle/POD/small-string sized. ParticleEmitter
				// is not: an open section heap-copies two keyframe vectors *and* the live
				// pool (up to MaxParticles) every frame. Measured cost is those allocs while
				// that one section is open; acceptable. ComponentEditCommand<T> storing
				// before/after by value doubles it per command — also fine, also noted.
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

		Entity rebuilt = PrefabSerializer::Instantiate(fullPath, *m_Context, source, options);
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

		m_SelectionContext = rebuilt;
		GE_INFO("Reverted instance from '{0}'", metadata->FilePath);
	}

	Entity SceneHierarchyPanel::InstantiatePrefab(const std::filesystem::path& relativePath)
	{
		if (!m_Context)
			return {};

		AssetHandle handle = AssetManager::ImportAsset(relativePath);
		if (!IsAssetHandleValid(handle))
			return {};

		Entity root = PrefabSerializer::Instantiate(GetAssetRoot() / relativePath, *m_Context, handle);
		if (!root)
			return {};

		PushAddedEntities("Instantiate '" + root.GetComponent<TagComponent>().Tag + "'", root);
		m_SelectionContext = root;
		return root;
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
			ImGui::TextDisabled("Prefab instance: source %llu is not in the registry",
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
			DrawAddComponentEntry<ParticleEmitterComponent>("Particle Emitter");
			DrawAddComponentEntry<RigidBodyComponent>("Rigid Body");
			DrawAddComponentEntry<BoxColliderComponent>("Box Collider");
			DrawAddComponentEntry<SphereColliderComponent>("Sphere Collider");
			DrawAddComponentEntry<CapsuleColliderComponent>("Capsule Collider");

			ImGui::EndPopup();
		}

		ImGui::PopItemWidth();

		DrawComponent<TransformComponent>("Transform", entity, [&](auto& component)
		{
			bool edited = EditorUI::DrawVec3Control("Translation", component.Translation);

			// Rotation is stored in radians and shown in degrees, and the round-trip is not
			// exact. Writing back unconditionally therefore changed the stored value on frames
			// with no user input at all - harmless for rendering, but under any value-diff
			// scheme it mints a phantom undo command and marks the scene dirty on selection.
			// Write back only when the row says it was edited.
			glm::vec3 rotation = glm::degrees(component.Rotation);
			if (EditorUI::DrawVec3Control("Rotation", rotation))
			{
				component.Rotation = glm::radians(rotation);
				edited = true;
			}

			edited |= EditorUI::DrawVec3Control("Scale", component.Scale, 1.0f);

			// Editing the component directly is invisible to change tracking, so the cached
			// world transform would never be refreshed.
			if (edited)
				m_Context->MarkChanged<TransformComponent>(entity);

			return edited;
		});

		// Read-only: the link is created by "Create Prefab" and followed by the Apply / Revert
		// buttons above the sections. There is no field to type a handle into on purpose.
		DrawComponent<PrefabInstanceComponent>("Prefab Instance", entity, [](auto& component)
		{
			const AssetMetadata* metadata = AssetManager::GetMetadata(component.Source);
			if (metadata)
				ImGui::Text("Source: %s", metadata->FilePath.c_str());
			else
				ImGui::TextDisabled("Source %llu is not in the registry",
					static_cast<uint64_t>(component.Source));

			ImGui::TextDisabled("Removing this unlinks the entity from its prefab");
			return false;
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

		// ---- Reflected sections -------------------------------------------------------------
		//
		// From here down, a section whose body is one DrawReflectedComponent call is drawn
		// entirely from ComponentReflection.cpp - field order, labels, ranges, drag speeds,
		// colour-vs-position widget choice, enum names and notes all come from the registration.
		// Adding a field to one of these components is one `.data<>` line, not an edit here.
		//
		// The sections that stay hand-written are not leftovers; each has its reason stated at
		// its own site, and R1 anticipated most of them in the registration comments.
		DrawComponent<SpriteRendererComponent>("Sprite Renderer", entity, [](auto& component)
		{
			return EditorUI::DrawReflectedComponent(component);
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
					ImGui::Text("Mesh handle: %llu", static_cast<uint64_t>(component.Mesh.Handle()));

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
							ImGui::Text("Slot %u: unknown material %llu", i, static_cast<uint64_t>(slot));
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
			return EditorUI::DrawReflectedComponent(component);
		});

		DrawComponent<PointLightComponent>("Point Light", entity, [](auto& component)
		{
			return EditorUI::DrawReflectedComponent(component);
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
			if (component.Environment.HasHandle())
			{
				const AssetMetadata* metadata = AssetManager::GetMetadata(component.Environment.Handle());
				if (metadata)
					ImGui::Text("Environment: %s", metadata->FilePath.c_str());
				ImGui::TextDisabled("Using HDR IBL (procedural colors are fallback)");
			}
			else
			{
				edited |= ImGui::ColorEdit3("Sky Color", glm::value_ptr(component.SkyColor));
				edited |= ImGui::ColorEdit3("Ground Color", glm::value_ptr(component.GroundColor));
			}

			AssetRef<Environment> dropped = EditorUI::AcceptAssetDropRef<Environment>();
			if (dropped.HasHandle())
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
			return EditorUI::DrawReflectedComponent(component);
		});

		DrawComponent<ParticleEmitterComponent>("Particle Emitter", entity, [entity](auto& component)
		{
			bool edited = false;

			// Preview only. These mutate Playing/pool/RNG — not authored, not serialized —
			// so they must not join `edited`. A Button still takes ActiveId for the click;
			// TrackCommitBoundary drops the pending edit because Edited stayed false.
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

			auto minMax = [&](const char* minLabel, float& minV, const char* maxLabel, float& maxV, float speed)
			{
				if (ImGui::DragFloat(minLabel, &minV, speed))
				{
					if (minV > maxV)
						maxV = minV;
					edited = true;
				}
				if (ImGui::DragFloat(maxLabel, &maxV, speed))
				{
					if (maxV < minV)
						minV = maxV;
					edited = true;
				}
			};

			if (ImGui::CollapsingHeader("Emission", ImGuiTreeNodeFlags_DefaultOpen))
			{
				edited |= ImGui::DragFloat("Rate Over Time", &component.RateOverTime, 0.1f, 0.0f, 100000.0f);
				int maxParticles = (int)component.MaxParticles;
				if (ImGui::DragInt("Max Particles", &maxParticles, 1.0f, 0, 100000))
				{
					component.MaxParticles = (uint32_t)std::max(maxParticles, 0);
					edited = true;
				}
				edited |= ImGui::Checkbox("Looping", &component.Looping);
				edited |= ImGui::DragFloat("Duration", &component.Duration, 0.05f, 0.0f, 1000.0f);
				edited |= ImGui::Checkbox("Play On Start", &component.PlayOnStart);
			}

			if (ImGui::CollapsingHeader("Initial", ImGuiTreeNodeFlags_DefaultOpen))
			{
				minMax("Lifetime Min", component.LifetimeMin, "Lifetime Max", component.LifetimeMax, 0.02f);
				minMax("Speed Min", component.SpeedMin, "Speed Max", component.SpeedMax, 0.05f);
				edited |= ImGui::DragFloat("Cone Angle", &component.ConeAngle, 0.5f, 0.0f, 180.0f);
				minMax("Start Size Min", component.StartSizeMin, "Start Size Max", component.StartSizeMax, 0.01f);
				minMax("Start Rotation Min", component.StartRotationMin, "Start Rotation Max", component.StartRotationMax, 1.0f);
				minMax("Rotation Speed Min", component.RotationSpeedMin, "Rotation Speed Max", component.RotationSpeedMax, 1.0f);
				edited |= ImGui::DragFloat("Gravity Modifier", &component.GravityModifier, 0.05f);
				edited |= ImGui::Checkbox("World Space", &component.WorldSpace);
				int seed = (int)component.Seed;
				if (ImGui::DragInt("Seed", &seed, 1.0f, 0, 2147483647))
				{
					component.Seed = (uint32_t)std::max(seed, 0);
					edited = true;
				}
				ImGui::TextDisabled("0 derives from the entity UUID at play");
			}

			if (ImGui::CollapsingHeader("Over Lifetime", ImGuiTreeNodeFlags_DefaultOpen))
			{
				edited |= EditorUI::CurveEditor("Size Curve", component.SizeCurve, 0.0f, 2.0f);
				edited |= EditorUI::GradientEditor("Color Over Lifetime", component.ColorOverLifetime);
			}

			if (ImGui::CollapsingHeader("Rendering", ImGuiTreeNodeFlags_DefaultOpen))
			{
				const char* modeStrings[] = { "Billboard", "Mesh" };
				int mode = (int)component.RenderMode;
				if (ImGui::Combo("Render Mode", &mode, modeStrings, 2))
				{
					component.RenderMode = (ParticleEmitterComponent::Mode)mode;
					edited = true;
				}

				// Generic over the slot's asset type rather than taking an AssetType next to an
				// untyped handle: the drop filter comes from the field itself now, so a slot
				// cannot accept something the component would not know how to load.
				auto assetSlot = [&](const char* label, auto& slot, const char* dropHint)
				{
					using SlotType = typename std::decay_t<decltype(slot)>::AssetT;

					if (slot.HasHandle())
					{
						const AssetMetadata* metadata = AssetManager::GetMetadata(slot.Handle());
						if (metadata)
							ImGui::Text("%s: %s", label, metadata->FilePath.c_str());
						else
							ImGui::Text("%s handle: %llu", label, static_cast<uint64_t>(slot.Handle()));

						ImGui::PushID(label);
						if (ImGui::Button("Clear"))
						{
							slot.Reset();
							edited = true;
						}
						ImGui::PopID();
					}
					else
					{
						ImGui::TextDisabled("No %s assigned", label);
					}

					ImGui::TextDisabled("%s", dropHint);
					AssetRef<SlotType> dropped = EditorUI::AcceptAssetDropRef<SlotType>();
					if (dropped.HasHandle())
					{
						slot = dropped;
						edited = true;
					}
				};

				if (component.RenderMode == ParticleEmitterComponent::Mode::Billboard)
				{
					assetSlot("Texture", component.Texture, "Drop a texture here; unset is white");
					const char* blendStrings[] = { "Alpha", "Additive" };
					int blend = (int)component.Blend;
					if (ImGui::Combo("Blend", &blend, blendStrings, 2))
					{
						component.Blend = (ParticleBlend)blend;
						edited = true;
					}
				}
				else
				{
					assetSlot("Mesh", component.Mesh, "Drop a mesh here");
					assetSlot("Material", component.Material, "Drop a .gmat here; unset is the mesh default");
					ImGui::TextDisabled("Mesh particles must use opaque materials");
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("A Transparent .gmat submits one draw per particle instead of the opaque instanced path.");
				}
			}

			return edited;
		});

		DrawComponent<RigidBodyComponent>("Rigid Body", entity, [](auto& component)
		{
			// The Type combo's entries come from RigidBodyType's own registration, so adding a
			// body type is one line beside the enum instead of a parallel string array here.
			return EditorUI::DrawReflectedComponent(component);
		});

		// PhysicsMaterial is drawn by Trait::Flatten, which puts Friction and Restitution beside
		// the collider's own fields rather than under a sub-header - the same shape the flag
		// already forces on SceneSerializer. The `drawPhysicsMaterial` lambda these three shared
		// is gone with them.
		DrawComponent<BoxColliderComponent>("Box Collider", entity, [](auto& component)
		{
			return EditorUI::DrawReflectedComponent(component);
		});

		DrawComponent<SphereColliderComponent>("Sphere Collider", entity, [](auto& component)
		{
			return EditorUI::DrawReflectedComponent(component);
		});

		DrawComponent<CapsuleColliderComponent>("Capsule Collider", entity, [](auto& component)
		{
			return EditorUI::DrawReflectedComponent(component);
		});
	}
}
