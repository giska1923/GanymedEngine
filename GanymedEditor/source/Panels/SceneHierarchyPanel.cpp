#include "SceneHierarchyPanel.h"

#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>

#include <glm/gtc/type_ptr.hpp>

#include "GanymedE/Scene/Components.h"
#include "GanymedE/Assets/AssetManager.h"
#include "GanymedE/Renderer/Mesh.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace GanymedE {

	SceneHierarchyPanel::SceneHierarchyPanel(const Ref<Scene>& context)
	{
		SetContext(context);
	}

	void SceneHierarchyPanel::SetContext(const Ref<Scene>& context)
	{
		m_Context = context;
		m_SelectionContext = {};
	}

	void SceneHierarchyPanel::OnImGuiRender()
	{
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
						m_Context->Unparent(dropped);
				}
				ImGui::EndDragDropTarget();
			}

			// Right-click on blank space
			if (ImGui::BeginPopupContextWindow(0, ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
			{
				if (ImGui::MenuItem("Create Empty Entity"))
				{
					m_Context->CreateEntity("Empty Entity");
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
					m_Context->SetParent(dropped, entity);
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
		{
			m_Context->DestroyEntity(entity);
			if (m_SelectionContext == entity)
			{
				m_SelectionContext = {};
			}
		}
	}

	static void DrawVec3Control(const std::string& label, glm::vec3& values, float resetValue = 0.0f, float columnWidth = 100.0f)
	{
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
		}

		ImGui::PopFont();
		ImGui::PopStyleColor(3);

		ImGui::SameLine();
		ImGui::DragFloat("##X", &values.x, 0.1f, 0.0f, 0.0f, "%.2f");
		ImGui::PopItemWidth();
		ImGui::SameLine();

		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{ 0.2f, 0.7f, 0.2f, 1.0f });
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{ 0.3f, 0.8f, 0.3f, 1.0f });
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{ 0.2f, 0.7f, 0.2f, 1.0f });
		ImGui::PushFont(boldFont);
		if (ImGui::Button("Y", buttonSize))
		{
			values.y = resetValue;
		}

		ImGui::PopFont();
		ImGui::PopStyleColor(3);

		ImGui::SameLine();
		ImGui::DragFloat("##Y", &values.y, 0.1f, 0.0f, 0.0f, "%.2f");
		ImGui::PopItemWidth();
		ImGui::SameLine();

		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{ 0.1f, 0.25f, 0.8f, 1.0f });
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{ 0.2f, 0.35f, 0.9f, 1.0f });
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{ 0.1f, 0.25f, 0.8f, 1.0f });
		ImGui::PushFont(boldFont);
		if (ImGui::Button("Z", buttonSize))
		{
			values.z = resetValue;
		}

		ImGui::PopFont();
		ImGui::PopStyleColor(3);

		ImGui::SameLine();
		ImGui::DragFloat("##Z", &values.z, 0.1f, 0.0f, 0.0f, "%.2f");
		ImGui::PopItemWidth();

		ImGui::PopStyleVar();

		ImGui::Columns(1);

		ImGui::PopID();
	}

	template<typename T, typename UIFunction>
	static void DrawComponent(const std::string& name, Entity entity, UIFunction uiFunction)
	{
		const ImGuiTreeNodeFlags treeNodeFlags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_AllowItemOverlap | ImGuiTreeNodeFlags_FramePadding;
		if (entity.HasComponent<T>())
		{
			auto& component = entity.GetComponent<T>();
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
				uiFunction(component);
				ImGui::TreePop();
			}

			if (removeComponent)
			{
				entity.RemoveComponent<T>();
			}
		}
	}

	void SceneHierarchyPanel::DrawComponents(Entity entity)
	{
		if (entity.HasComponent<TagComponent>())
		{
			auto& tag = entity.GetComponent<TagComponent>().Tag;

			char buffer[256];
			memset(buffer, 0, sizeof(buffer));
			strncpy(buffer, tag.c_str(), sizeof(buffer) - 1);
			if (ImGui::InputText("##Tag", buffer, sizeof(buffer)))
			{
				tag = std::string(buffer);
			}
		}

		ImGui::SameLine();
		ImGui::PushItemWidth(-1);

		if (ImGui::Button("Add Component"))
		{
			ImGui::OpenPopup("AddComponent");
		}

		if (ImGui::BeginPopup("AddComponent"))
		{
			if (!m_SelectionContext.HasComponent<CameraComponent>())
			{
				if (ImGui::MenuItem("Camera"))
				{
					m_SelectionContext.AddComponent<CameraComponent>();
					ImGui::CloseCurrentPopup();
				}
			}

			if (!m_SelectionContext.HasComponent<SpriteRendererComponent>())
			{
				if (ImGui::MenuItem("Sprite Renderer"))
				{
					m_SelectionContext.AddComponent<SpriteRendererComponent>();
					ImGui::CloseCurrentPopup();
				}
			}

			if (!m_SelectionContext.HasComponent<DirectionalLightComponent>())
			{
				if (ImGui::MenuItem("Directional Light"))
				{
					m_SelectionContext.AddComponent<DirectionalLightComponent>();
					ImGui::CloseCurrentPopup();
				}
			}

			if (!m_SelectionContext.HasComponent<PointLightComponent>())
			{
				if (ImGui::MenuItem("Point Light"))
				{
					m_SelectionContext.AddComponent<PointLightComponent>();
					ImGui::CloseCurrentPopup();
				}
			}

			if (!m_SelectionContext.HasComponent<SpotLightComponent>())
			{
				if (ImGui::MenuItem("Spot Light"))
				{
					m_SelectionContext.AddComponent<SpotLightComponent>();
					ImGui::CloseCurrentPopup();
				}
			}

			if (!m_SelectionContext.HasComponent<SkyLightComponent>())
			{
				if (ImGui::MenuItem("Sky Light"))
				{
					m_SelectionContext.AddComponent<SkyLightComponent>();
					ImGui::CloseCurrentPopup();
				}
			}

			if (!m_SelectionContext.HasComponent<ScriptComponent>())
			{
				if (ImGui::MenuItem("Script"))
				{
					m_SelectionContext.AddComponent<ScriptComponent>();
					ImGui::CloseCurrentPopup();
				}
			}

			if (!m_SelectionContext.HasComponent<RigidBodyComponent>())
			{
				if (ImGui::MenuItem("Rigid Body"))
				{
					m_SelectionContext.AddComponent<RigidBodyComponent>();
					ImGui::CloseCurrentPopup();
				}
			}

			if (!m_SelectionContext.HasComponent<BoxColliderComponent>())
			{
				if (ImGui::MenuItem("Box Collider"))
				{
					m_SelectionContext.AddComponent<BoxColliderComponent>();
					ImGui::CloseCurrentPopup();
				}
			}

			if (!m_SelectionContext.HasComponent<SphereColliderComponent>())
			{
				if (ImGui::MenuItem("Sphere Collider"))
				{
					m_SelectionContext.AddComponent<SphereColliderComponent>();
					ImGui::CloseCurrentPopup();
				}
			}

			if (!m_SelectionContext.HasComponent<CapsuleColliderComponent>())
			{
				if (ImGui::MenuItem("Capsule Collider"))
				{
					m_SelectionContext.AddComponent<CapsuleColliderComponent>();
					ImGui::CloseCurrentPopup();
				}
			}

			ImGui::EndPopup();
		}

		ImGui::PopItemWidth();

		DrawComponent<TransformComponent>("Transform", entity, [&](auto& component)
		{
			const glm::vec3 before[] = { component.Translation, component.Rotation, component.Scale };

			DrawVec3Control("Translation", component.Translation);
			glm::vec3 rotation = glm::degrees(component.Rotation);
			DrawVec3Control("Rotation", rotation);
			component.Rotation = glm::radians(rotation);
			DrawVec3Control("Scale", component.Scale, 1.0f);

			// Editing the component directly is invisible to change tracking, so the cached world
			// transform would never be refreshed. Report it when something actually moved.
			if (before[0] != component.Translation || before[1] != component.Rotation
				|| before[2] != component.Scale)
			{
				m_Context->MarkChanged<TransformComponent>(entity);
			}
		});

		DrawComponent<CameraComponent>("Camera", entity, [](auto& component)
		{
			auto& camera = component.Camera;

			ImGui::Checkbox("Primary", &component.Primary);

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
				}

				float perspectiveNear = camera.GetPerspectiveNearClip();
				if (ImGui::DragFloat("Near", &perspectiveNear))
				{
					camera.SetPerspectiveNearClip(perspectiveNear);
				}

				float perspectiveFar = camera.GetPerspectiveFarClip();
				if (ImGui::DragFloat("Far", &perspectiveFar))
				{
					camera.SetPerspectiveFarClip(perspectiveFar);
				}
			}

			if (camera.GetProjectionType() == SceneCamera::ProjectionType::Orthographic)
			{
				float orthoSize = camera.GetOrthographicSize();
				if (ImGui::DragFloat("Size", &orthoSize))
				{
					camera.SetOrthographicSize(orthoSize);
				}

				float orthoNear = camera.GetOrthographicNearClip();
				if (ImGui::DragFloat("Near", &orthoNear))
				{
					camera.SetOrthographicNearClip(orthoNear);
				}

				float orthoFar = camera.GetOrthographicFarClip();
				if (ImGui::DragFloat("Far", &orthoFar))
				{
					camera.SetOrthographicFarClip(orthoFar);
				}

				ImGui::Checkbox("Fixed Aspect Ratio", &component.FixedAspectRatio);
			}
		});

		DrawComponent<SpriteRendererComponent>("Sprite Renderer", entity, [](auto& component)
		{
			ImGui::ColorEdit4("Color", glm::value_ptr(component.Color));
		});

		DrawComponent<StaticMeshComponent>("Static Mesh", entity, [](auto& component)
		{
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
					ImGui::Text("Materials: %u", (uint32_t)mesh->GetMaterials().size());

					const auto& materials = mesh->GetMaterials();
					for (uint32_t i = 0; i < (uint32_t)materials.size(); i++)
					{
						if (!materials[i])
							continue;

						ImGui::PushID((int)i);
						ImGui::Separator();
						ImGui::Text("%s", materials[i]->GetName().c_str());
						glm::vec4 albedo = materials[i]->GetAlbedoColor();
						if (ImGui::ColorEdit4("Albedo", glm::value_ptr(albedo)))
							materials[i]->SetAlbedoColor(albedo);
						float metallic = materials[i]->GetMetallic();
						if (ImGui::DragFloat("Metallic", &metallic, 0.01f, 0.0f, 1.0f))
							materials[i]->SetMetallic(metallic);
						float roughness = materials[i]->GetRoughness();
						if (ImGui::DragFloat("Roughness", &roughness, 0.01f, 0.0f, 1.0f))
							materials[i]->SetRoughness(roughness);
						bool transparent = materials[i]->IsTransparent();
						if (ImGui::Checkbox("Transparent", &transparent))
							materials[i]->SetTransparent(transparent);
						ImGui::SameLine();
						bool twoSided = materials[i]->IsTwoSided();
						if (ImGui::Checkbox("Two Sided", &twoSided))
							materials[i]->SetTwoSided(twoSided);
						ImGui::PopID();
					}
				}
			}
			else
			{
				ImGui::TextDisabled("No mesh assigned");
			}

			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
				{
					const char* path = (const char*)payload->Data;
					std::string ext = std::filesystem::path(path).extension().string();
					std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
					if (ext == ".gltf" || ext == ".glb")
						component.Mesh = AssetManager::ImportAsset(path);
				}
				ImGui::EndDragDropTarget();
			}
		});

		DrawComponent<ScriptComponent>("Script", entity, [](auto& component)
		{
			if (IsAssetHandleValid(component.Script))
			{
				const AssetMetadata* metadata = AssetManager::GetMetadata(component.Script);
				if (metadata)
					ImGui::Text("Script: %s", metadata->FilePath.c_str());
				else
					ImGui::Text("Script handle: %llu", static_cast<uint64_t>(component.Script));

				if (ImGui::Button("Clear"))
					component.Script = InvalidAssetHandle;
			}
			else
			{
				ImGui::TextDisabled("No script assigned");
			}

			ImGui::TextDisabled("Drop a .lua file here");

			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
				{
					const char* path = (const char*)payload->Data;
					std::string ext = std::filesystem::path(path).extension().string();
					std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
					if (ext == ".lua")
						component.Script = AssetManager::ImportAsset(path);
				}
				ImGui::EndDragDropTarget();
			}
		});

		DrawComponent<DirectionalLightComponent>("Directional Light", entity, [](auto& component)
		{
			ImGui::ColorEdit3("Color", glm::value_ptr(component.Color));
			ImGui::DragFloat("Intensity", &component.Intensity, 0.05f, 0.0f, 100.0f);
			ImGui::Checkbox("Cast Shadows", &component.CastShadows);
			ImGui::TextDisabled("Direction = entity -Z (rotate to aim)");
		});

		DrawComponent<PointLightComponent>("Point Light", entity, [](auto& component)
		{
			ImGui::ColorEdit3("Color", glm::value_ptr(component.Color));
			ImGui::DragFloat("Intensity", &component.Intensity, 0.05f, 0.0f, 1000.0f);
			ImGui::DragFloat("Radius", &component.Radius, 0.1f, 0.0f, 1000.0f);
			ImGui::DragFloat("Falloff", &component.Falloff, 0.05f, 0.01f, 16.0f);
		});

		DrawComponent<SpotLightComponent>("Spot Light", entity, [](auto& component)
		{
			ImGui::ColorEdit3("Color", glm::value_ptr(component.Color));
			ImGui::DragFloat("Intensity", &component.Intensity, 0.05f, 0.0f, 1000.0f);
			ImGui::DragFloat("Range", &component.Range, 0.1f, 0.0f, 1000.0f);

			float inner = glm::degrees(component.InnerConeAngle);
			if (ImGui::DragFloat("Inner Cone", &inner, 0.5f, 0.0f, 89.0f))
				component.InnerConeAngle = glm::radians(inner);

			float outer = glm::degrees(component.OuterConeAngle);
			if (ImGui::DragFloat("Outer Cone", &outer, 0.5f, 0.0f, 89.0f))
				component.OuterConeAngle = glm::radians(glm::max(outer, inner));

			ImGui::DragFloat("Falloff", &component.Falloff, 0.05f, 0.01f, 16.0f);
			ImGui::TextDisabled("Direction = entity -Z (rotate to aim)");
		});

		DrawComponent<SkyLightComponent>("Sky Light", entity, [](auto& component)
		{
			if (IsAssetHandleValid(component.Environment))
			{
				const AssetMetadata* metadata = AssetManager::GetMetadata(component.Environment);
				if (metadata)
					ImGui::Text("Environment: %s", metadata->FilePath.c_str());
				ImGui::TextDisabled("Using HDR IBL (procedural colors are fallback)");
			}
			else
			{
				ImGui::ColorEdit3("Sky Color", glm::value_ptr(component.SkyColor));
				ImGui::ColorEdit3("Ground Color", glm::value_ptr(component.GroundColor));
			}

			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
				{
					const char* path = (const char*)payload->Data;
					std::string ext = std::filesystem::path(path).extension().string();
					std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
					if (ext == ".hdr")
						component.Environment = AssetManager::ImportAsset(path);
				}
				ImGui::EndDragDropTarget();
			}

			ImGui::DragFloat("Intensity", &component.Intensity, 0.02f, 0.0f, 20.0f);
			ImGui::Checkbox("Draw Skybox", &component.DrawSkybox);
		});

		DrawComponent<RigidBodyComponent>("Rigid Body", entity, [](auto& component)
		{
			const char* typeStrings[] = { "Static", "Dynamic", "Kinematic" };
			int type = (int)component.Type;
			if (ImGui::Combo("Type", &type, typeStrings, 3))
				component.Type = (RigidBodyType)type;

			ImGui::DragFloat("Mass", &component.Mass, 0.05f, 0.001f, 100000.0f);
			ImGui::DragFloat("Linear Damping", &component.LinearDamping, 0.01f, 0.0f, 10.0f);
			ImGui::DragFloat("Angular Damping", &component.AngularDamping, 0.01f, 0.0f, 10.0f);
			ImGui::Checkbox("Use Gravity", &component.UseGravity);
		});

		auto drawPhysicsMaterial = [](PhysicsMaterial& mat)
		{
			ImGui::DragFloat("Friction", &mat.Friction, 0.01f, 0.0f, 10.0f);
			ImGui::DragFloat("Restitution", &mat.Restitution, 0.01f, 0.0f, 1.0f);
		};

		DrawComponent<BoxColliderComponent>("Box Collider", entity, [&](auto& component)
		{
			DrawVec3Control("Half Extents", component.HalfExtents, 0.5f);
			DrawVec3Control("Offset", component.Offset);
			drawPhysicsMaterial(component.Material);
		});

		DrawComponent<SphereColliderComponent>("Sphere Collider", entity, [&](auto& component)
		{
			ImGui::DragFloat("Radius", &component.Radius, 0.05f, 0.001f, 1000.0f);
			DrawVec3Control("Offset", component.Offset);
			drawPhysicsMaterial(component.Material);
		});

		DrawComponent<CapsuleColliderComponent>("Capsule Collider", entity, [&](auto& component)
		{
			ImGui::DragFloat("Radius", &component.Radius, 0.05f, 0.001f, 1000.0f);
			ImGui::DragFloat("Half Height", &component.HalfHeight, 0.05f, 0.001f, 1000.0f);
			DrawVec3Control("Offset", component.Offset);
			drawPhysicsMaterial(component.Material);
		});
	}
}
