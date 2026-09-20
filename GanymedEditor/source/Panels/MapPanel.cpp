#include "MapPanel.h"
#include "SceneHierarchyPanel.h"

#include "../EditorIcons.h"
#include "../EditorTheme.h"
#include "../EditorUndo.h"
#include "../EditorWidgets.h"

#include "GanymedE/Assets/AssetManager.h"
#include "GanymedE/Assets/AssetPaths.h"
#include "GanymedE/Core/Log.h"
#include "GanymedE/Math/BoundingVolumes.h"
#include "GanymedE/Renderer/EditorCamera.h"
#include "GanymedE/Renderer/Mesh.h"
#include "GanymedE/Scene/Components.h"
#include "GanymedE/Scene/Entity.h"
#include "GanymedE/Scene/Scene.h"
#include "GanymedE/Scene/SceneSingletons.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <imgui/imgui.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace GanymedE {

	namespace {

		std::filesystem::path PalettePath()
		{
			return GetAssetRoot() / ".editor" / "map_palette.yaml";
		}

		std::string TrimCopy(const std::string& s)
		{
			size_t a = 0;
			while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a])))
				++a;
			size_t b = s.size();
			while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
				--b;
			return s.substr(a, b - a);
		}

		std::string ToLowerAscii(std::string s)
		{
			for (char& c : s)
				c = (char)std::tolower(static_cast<unsigned char>(c));
			return s;
		}

		const char* TypeIcon(AssetType type)
		{
			switch (type)
			{
				case AssetType::StaticMesh: return ICON_LC_BOX;
				case AssetType::Prefab:     return ICON_LC_PACKAGE;
				default:                    return ICON_LC_FILE;
			}
		}

		ImU32 TypeTint(AssetType type)
		{
			const int i = static_cast<int>(type);
			if (i >= 0 && i < EditorUI::kAssetTintCount)
				return EditorUI::Theme().AssetTint[i];
			return EditorUI::Theme().TextPrimary;
		}

		constexpr glm::vec4 kMeshOverlay{ 0.25f, 0.85f, 1.0f, 1.0f };
		constexpr glm::vec4 kColliderOverlay{ 1.0f, 0.55f, 0.15f, 1.0f };

		glm::mat4 WorldOf(Entity entity, Scene& scene)
		{
			if (entity.HasComponent<WorldTransformComponent>())
				return entity.GetComponent<WorldTransformComponent>().World;
			return scene.GetWorldSpaceTransform(entity);
		}

		bool HasAnyCollider(Entity entity)
		{
			return entity.HasComponent<BoxColliderComponent>()
				|| entity.HasComponent<SphereColliderComponent>()
				|| entity.HasComponent<CapsuleColliderComponent>();
		}

		const Mesh* ResidentMesh(Entity entity)
		{
			if (!entity.HasComponent<StaticMeshComponent>())
				return nullptr;
			const Ref<Mesh>& mesh = entity.GetComponent<StaticMeshComponent>().Mesh.Get();
			return mesh ? mesh.get() : nullptr;
		}

		glm::mat4 LocalAabbWire(const AABB& local, const glm::mat4& world)
		{
			const glm::vec3 center = (local.Min + local.Max) * 0.5f;
			const glm::vec3 size = local.Max - local.Min;
			return world
				* glm::translate(glm::mat4(1.0f), center)
				* glm::scale(glm::mat4(1.0f), size);
		}

		bool MeshWorldAABB(Entity entity, Scene& scene, AABB& out)
		{
			const Mesh* mesh = ResidentMesh(entity);
			if (!mesh)
				return false;
			out = mesh->GetBounds().Transformed(WorldOf(entity, scene));
			return true;
		}

		AABB BoxColliderWorldAABB(Entity entity, Scene& scene)
		{
			const auto& collider = entity.GetComponent<BoxColliderComponent>();
			const AABB local{ -collider.HalfExtents, collider.HalfExtents };
			return local.Transformed(WorldOf(entity, scene)
				* glm::translate(glm::mat4(1.0f), collider.Offset));
		}

		AABB SphereColliderWorldAABB(Entity entity, Scene& scene)
		{
			const auto& collider = entity.GetComponent<SphereColliderComponent>();
			const glm::mat4 world = WorldOf(entity, scene);
			const glm::vec3 center = glm::vec3(world * glm::vec4(collider.Offset, 1.0f));
			const glm::vec3 scale{
				glm::length(glm::vec3(world[0])),
				glm::length(glm::vec3(world[1])),
				glm::length(glm::vec3(world[2]))
			};
			const float radius = collider.Radius * glm::max(scale.x, glm::max(scale.y, scale.z));
			return AABB(center - glm::vec3(radius), center + glm::vec3(radius));
		}

		AABB CapsuleColliderWorldAABB(Entity entity, Scene& scene)
		{
			const auto& collider = entity.GetComponent<CapsuleColliderComponent>();
			const AABB local{
				glm::vec3(-collider.Radius, -(collider.HalfHeight + collider.Radius), -collider.Radius),
				glm::vec3( collider.Radius,   collider.HalfHeight + collider.Radius,  collider.Radius)
			};
			return local.Transformed(WorldOf(entity, scene)
				* glm::translate(glm::mat4(1.0f), collider.Offset));
		}

		bool ColliderWorldAABB(Entity entity, Scene& scene, AABB& out)
		{
			if (entity.HasComponent<BoxColliderComponent>())
			{
				out = BoxColliderWorldAABB(entity, scene);
				return true;
			}
			if (entity.HasComponent<SphereColliderComponent>())
			{
				out = SphereColliderWorldAABB(entity, scene);
				return true;
			}
			if (entity.HasComponent<CapsuleColliderComponent>())
			{
				out = CapsuleColliderWorldAABB(entity, scene);
				return true;
			}
			return false;
		}

		void Encapsulate(AABB& box, const AABB& other, bool& any)
		{
			if (!any)
			{
				box = other;
				any = true;
				return;
			}
			box.Grow(other.Min);
			box.Grow(other.Max);
		}

		float AabbVolume(const AABB& box)
		{
			const glm::vec3 e = box.Max - box.Min;
			if (e.x <= 0.0f || e.y <= 0.0f || e.z <= 0.0f)
				return 0.0f;
			return e.x * e.y * e.z;
		}

		AABB AabbIntersection(const AABB& a, const AABB& b)
		{
			return AABB(glm::max(a.Min, b.Min), glm::min(a.Max, b.Max));
		}

		bool AxisOutside(float meshMin, float meshMax, float colMin, float colMax, float tol)
		{
			return meshMin < colMin - tol || meshMax > colMax + tol;
		}

		bool AxisLarger(float meshMin, float meshMax, float colMin, float colMax, float tol)
		{
			return colMin < meshMin - tol || colMax > meshMax + tol;
		}

		bool ExtentsAgree(const AABB& mesh, const AABB& col, float tol)
		{
			const glm::vec3 d = glm::abs((mesh.Max - mesh.Min) - (col.Max - col.Min));
			return d.x <= tol && d.y <= tol && d.z <= tol;
		}

		bool CentresDiffer(const AABB& mesh, const AABB& col, float tol)
		{
			const glm::vec3 meshC = (mesh.Min + mesh.Max) * 0.5f;
			const glm::vec3 colC = (col.Min + col.Max) * 0.5f;
			const glm::vec3 d = glm::abs(meshC - colC);
			return d.x > tol || d.y > tol || d.z > tol;
		}

		size_t HashHidden(const std::unordered_set<UUID>& hidden)
		{
			size_t h = hidden.size();
			for (UUID id : hidden)
			{
				h ^= static_cast<size_t>(static_cast<uint64_t>(id))
					+ 0x9e3779b9u + (h << 6) + (h >> 2);
			}
			return h;
		}

		std::unordered_set<UUID> ExpandHidden(Scene& scene, SceneHierarchyPanel* hierarchy)
		{
			std::unordered_set<UUID> out;
			if (!hierarchy)
				return out;
			std::vector<Entity> subtree;
			std::unordered_set<UUID> visited;
			for (UUID id : hierarchy->HiddenEntities())
			{
				Entity entity = scene.FindEntityByUUID(id);
				if (!entity)
					continue;
				subtree.clear();
				visited.clear();
				scene.CollectSubtree(entity, subtree, visited);
				for (Entity child : subtree)
					out.insert(child.GetUUID());
			}
			return out;
		}

	}

	void DrawMapSnapControls(MapSnapSettings& snap)
	{
		ImGui::Checkbox("Enabled (Ctrl inverts)", &snap.Enabled);

		ImGui::SetNextItemWidth(96.0f);
		ImGui::DragFloat("Translate", &snap.Translate, 0.05f, 0.0f, 100.0f, "%.2f m");

		ImGui::SetNextItemWidth(96.0f);
		ImGui::DragFloat("Rotate", &snap.Rotate, 1.0f, 0.0f, 180.0f, "%.0f deg");
		ImGui::SameLine();
		if (ImGui::SmallButton("15"))
			snap.Rotate = 15.0f;
		ImGui::SameLine();
		if (ImGui::SmallButton("45"))
			snap.Rotate = 45.0f;

		ImGui::SetNextItemWidth(96.0f);
		ImGui::DragFloat("Scale", &snap.Scale, 0.01f, 0.0f, 10.0f, "%.2f");

		ImGui::Separator();
		ImGui::Checkbox("Snap to surface", &snap.SnapToSurface);
		ImGui::Checkbox("Align to normal", &snap.AlignToNormal);
		ImGui::Checkbox("Sit on bounds", &snap.SitOnBounds);
		ImGui::SetNextItemWidth(96.0f);
		ImGui::DragFloat("Grid height", &snap.GridHeight, 0.1f, -1000.0f, 1000.0f, "%.2f m");
	}

	void MapPanel::OnImGuiRender(MapSnapSettings& snap, bool editing, bool placing,
		Scene* scene, EditorUndoStack* undo, SceneHierarchyPanel* hierarchy,
		EditorCamera* camera, UUID excludeFromAudit)
	{
		using EditorUI::BeginPanel;
		using EditorUI::BeginPanelBody;
		using EditorUI::EndPanel;
		using EditorUI::EndPanelBody;

		if (!m_PaletteLoaded)
			LoadPalette();

		if (!BeginPanel("Map"))
		{
			EndPanel();
			return;
		}

		BeginPanelBody();

		if (placing)
		{
			ImGui::TextWrapped("Placing. LMB commits, Shift+LMB chains, Alt+LMB is unsnapped, "
				"Esc / RMB cancels, [ ] yaws.");
			ImGui::Separator();
		}

		DrawPalette(editing);
		DrawPlacementOptions(snap);
		DrawDuplicateAlongAxis(editing, placing, scene, undo, hierarchy);
		DrawParityAudit(editing, scene, undo, hierarchy, camera, excludeFromAudit);
		DrawScatter(editing);
		DrawUpcomingSections();

		EndPanelBody();
		EndPanel();
	}

	void MapPanel::DrawPalette(bool editing)
	{
		using EditorUI::Color;
		using EditorUI::IconButton;
		using EditorUI::SearchField;
		using EditorUI::Theme;

		if (!ImGui::CollapsingHeader("Palette", ImGuiTreeNodeFlags_DefaultOpen))
			return;

		if (IconButton(ICON_LC_PLUS, "Pin a prefab or mesh"))
			ImGui::OpenPopup("##PinAsset");

		if (ImGui::BeginPopup("##PinAsset"))
		{
			SearchField("pinfilter", m_PinSearch, sizeof(m_PinSearch), "Filter assets...");

			const std::string filter = ToLowerAscii(m_PinSearch);
			std::vector<const AssetMetadata*> candidates;
			AssetManager::ForEachAsset([&](const AssetMetadata& metadata)
			{
				if (metadata.Type != AssetType::Prefab && metadata.Type != AssetType::StaticMesh)
					return;
				if (!filter.empty())
				{
					const std::string path = ToLowerAscii(metadata.FilePath);
					if (path.find(filter) == std::string::npos)
						return;
				}
				candidates.push_back(&metadata);
			});
			std::sort(candidates.begin(), candidates.end(),
				[](const AssetMetadata* a, const AssetMetadata* b)
				{
					return a->FilePath < b->FilePath;
				});

			ImGui::BeginChild("##PinList", ImVec2(320.0f, 280.0f), ImGuiChildFlags_Borders);
			if (candidates.empty())
				ImGui::TextDisabled("No prefabs or meshes match.");

			for (const AssetMetadata* metadata : candidates)
			{
				const bool pinned = std::find(m_Pinned.begin(), m_Pinned.end(),
					metadata->FilePath) != m_Pinned.end();
				ImGui::PushStyleColor(ImGuiCol_Text, Color(TypeTint(metadata->Type)));
				ImGui::TextUnformatted(TypeIcon(metadata->Type));
				ImGui::PopStyleColor();
				ImGui::SameLine();

				const std::string label = metadata->FilePath + "##pin" +
					std::to_string(static_cast<uint64_t>(metadata->Handle));
				if (ImGui::Selectable(label.c_str(), pinned))
					Pin(metadata->FilePath);
			}
			ImGui::EndChild();
			ImGui::EndPopup();
		}

		ImGui::SameLine();
		ImGui::TextDisabled(m_PaintArmed
			? "Prefabs and meshes. Click to set the scatter source."
			: "Prefabs and meshes. Click to place.");

		if (m_Pinned.empty())
		{
			ImGui::TextDisabled("Nothing pinned. Use + to add from the project.");
			return;
		}

		std::string toUnpin;
		AssetHandle placeHandle = InvalidAssetHandle;
		AssetType placeType = AssetType::None;

		for (const std::string& path : m_Pinned)
		{
			ImGui::PushID(path.c_str());

			const AssetHandle handle = AssetManager::GetHandle(path);
			const AssetMetadata* metadata = AssetManager::GetMetadata(handle);
			const AssetType type = metadata ? metadata->Type : AssetType::None;
			const bool usable = metadata &&
				(type == AssetType::Prefab || type == AssetType::StaticMesh);

			const std::filesystem::path file(path);
			const std::string name = file.filename().string();

			ImGui::BeginDisabled(!editing || !usable);
			ImGui::PushStyleColor(ImGuiCol_Text, Color(usable ? TypeTint(type) : Theme().TextDim));
			ImGui::TextUnformatted(TypeIcon(usable ? type : AssetType::None));
			ImGui::PopStyleColor();
			ImGui::SameLine();

			if (ImGui::Selectable(name.c_str()) && usable && editing)
			{
				placeHandle = handle;
				placeType = type;
			}
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
				ImGui::SetTooltip("%s", path.c_str());
			ImGui::EndDisabled();

			const int clicked = EditorUI::RowActionIcons({
				{ ICON_LC_PIN_OFF, "Unpin", false, true }
			}, ImGui::IsItemHovered());
			if (clicked == 0)
				toUnpin = path;

			ImGui::PopID();
		}

		if (!toUnpin.empty())
			Unpin(toUnpin);

		if (IsAssetHandleValid(placeHandle))
		{
			if (m_PaintArmed)
			{
				m_Scatter.Source = placeHandle;
				m_Scatter.SourceType = placeType;
			}
			else if (m_OnPlace)
			{
				m_OnPlace(placeHandle, placeType);
			}
		}
	}

	void MapPanel::DrawPlacementOptions(MapSnapSettings& snap)
	{
		if (!ImGui::CollapsingHeader("Placement", ImGuiTreeNodeFlags_DefaultOpen))
			return;

		DrawMapSnapControls(snap);
	}

	void MapPanel::DrawDuplicateAlongAxis(bool editing, bool placing, Scene* scene,
		EditorUndoStack* undo, SceneHierarchyPanel* hierarchy)
	{
		if (!ImGui::CollapsingHeader("Duplicate along axis", ImGuiTreeNodeFlags_DefaultOpen))
			return;

		ImGui::SetNextItemWidth(96.0f);
		ImGui::InputInt("Count", &m_DupCount);
		if (m_DupCount < 2)
			m_DupCount = 2;
		if (m_DupCount > 64)
			m_DupCount = 64;

		ImGui::SetNextItemWidth(96.0f);
		ImGui::DragFloat("Spacing", &m_DupSpacing, 0.1f, 0.01f, 1000.0f, "%.2f m");

		const char* axes[] = { "X", "Y", "Z" };
		ImGui::SetNextItemWidth(96.0f);
		ImGui::Combo("Axis", &m_DupAxis, axes, 3);

		ImGui::TextDisabled("Count includes the original. One undo entry.");

		const Entity selected = hierarchy ? hierarchy->GetSelectedEntity() : Entity{};
		const bool canDup = editing && !placing && scene && undo && selected && m_DupCount >= 2;
		ImGui::BeginDisabled(!canDup);
		if (ImGui::Button("Duplicate") && canDup)
		{
			glm::vec3 axis(0.0f);
			axis[m_DupAxis] = 1.0f;

			const glm::vec3 sourceWorld = glm::vec3(scene->GetWorldSpaceTransform(selected)[3]);
			std::vector<Scope<EditorCommand>> steps;
			steps.reserve((size_t)m_DupCount - 1);

			Entity last = selected;
			for (int i = 1; i < m_DupCount; i++)
			{
				Entity copy = scene->DuplicateEntity(selected);
				if (!copy)
					break;

				glm::vec3 worldPos = sourceWorld + axis * (m_DupSpacing * (float)i);
				const UUID parentID = copy.GetComponent<RelationshipComponent>().Parent;
				if (parentID != UUID{ 0 })
				{
					Entity parent = scene->FindEntityByUUID(parentID);
					if (parent)
					{
						const glm::vec4 local = glm::inverse(scene->GetWorldSpaceTransform(parent))
							* glm::vec4(worldPos, 1.0f);
						worldPos = glm::vec3(local);
					}
				}

				auto& tc = copy.GetComponent<TransformComponent>();
				tc.Translation = worldPos;
				scene->MarkChanged<TransformComponent>(copy);

				std::vector<EntitySnapshot> snapshots;
				CaptureSubtree(*scene, copy, snapshots);
				steps.push_back(CreateScope<AddEntitiesCommand>(
					"Duplicate along axis", std::move(snapshots)));
				last = copy;
			}

			if (steps.size() == 1)
			{
				undo->Push(std::move(steps.front()));
			}
			else if (!steps.empty())
			{
				undo->Push(CreateScope<CompositeCommand>(
					"Duplicate along axis (" + std::to_string(m_DupCount) + ")",
					std::move(steps)));
			}

			if (hierarchy && last)
				hierarchy->SetSelectedEntity(last);
		}
		ImGui::EndDisabled();
		if (!canDup && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("Select an entity in Edit mode.");
	}

	void MapPanel::FillOverlay(EditorBoundsOverlay& overlay) const
	{
		overlay.Boxes.clear();
		overlay.Spheres.clear();
		overlay.Boxes.reserve(m_OverlayBoxes.size());
		for (const OverlayBox& box : m_OverlayBoxes)
			overlay.Boxes.push_back({ box.Transform, box.Color });
	}

	void MapPanel::RefreshFocusOverlay(Scene* scene)
	{
		m_OverlayBoxes.clear();
		if (!scene || m_FocusUUID == UUID{ 0 })
			return;

		Entity entity = scene->FindEntityByUUID(m_FocusUUID);
		if (!entity)
		{
			m_FocusUUID = UUID{ 0 };
			return;
		}

		const glm::mat4 world = WorldOf(entity, *scene);
		if (const Mesh* mesh = ResidentMesh(entity))
			m_OverlayBoxes.push_back({ LocalAabbWire(mesh->GetBounds(), world), kMeshOverlay });

		if (entity.HasComponent<BoxColliderComponent>())
		{
			const auto& collider = entity.GetComponent<BoxColliderComponent>();
			const glm::mat4 xform = world
				* glm::translate(glm::mat4(1.0f), collider.Offset)
				* glm::scale(glm::mat4(1.0f), collider.HalfExtents * 2.0f);
			m_OverlayBoxes.push_back({ xform, kColliderOverlay });
		}
	}

	void MapPanel::FocusFinding(Scene* scene, SceneHierarchyPanel* hierarchy,
		EditorCamera* camera, UUID id)
	{
		m_FocusUUID = id;
		RefreshFocusOverlay(scene);
		if (!scene || id == UUID{ 0 })
			return;

		Entity entity = scene->FindEntityByUUID(id);
		if (!entity)
			return;

		if (hierarchy)
			hierarchy->SetSelectedEntity(entity);

		AABB frame;
		bool any = false;
		AABB meshBox;
		if (MeshWorldAABB(entity, *scene, meshBox))
			Encapsulate(frame, meshBox, any);
		AABB colBox;
		if (ColliderWorldAABB(entity, *scene, colBox))
			Encapsulate(frame, colBox, any);
		if (!any || !camera)
			return;

		const glm::vec3 center = (frame.Min + frame.Max) * 0.5f;
		const float radius = glm::max(glm::length(frame.Max - frame.Min) * 0.5f, 0.05f) * 1.15f;
		camera->Frame(center, radius);
	}

	void MapPanel::GenerateCollidersFromMesh(Scene* scene, EditorUndoStack* undo,
		SceneHierarchyPanel* hierarchy)
	{
		if (!scene || !undo || !hierarchy)
			return;

		std::vector<Scope<EditorCommand>> steps;
		int skippedOther = 0;
		int skippedNoMesh = 0;

		for (Entity entity : hierarchy->GetSelection())
		{
			if (!entity)
				continue;
			if (entity.HasComponent<SphereColliderComponent>()
				|| entity.HasComponent<CapsuleColliderComponent>())
			{
				++skippedOther;
				continue;
			}

			BoxColliderComponent seeded;
			if (entity.HasComponent<BoxColliderComponent>())
				seeded = entity.GetComponent<BoxColliderComponent>();

			if (!SceneHierarchyPanel::SeedBoxColliderFromMesh(entity, seeded))
			{
				++skippedNoMesh;
				continue;
			}

			if (entity.HasComponent<BoxColliderComponent>())
			{
				const BoxColliderComponent before = entity.GetComponent<BoxColliderComponent>();
				auto& live = entity.GetComponent<BoxColliderComponent>();
				live.HalfExtents = seeded.HalfExtents;
				live.Offset = seeded.Offset;
				steps.push_back(CreateScope<ComponentEditCommand<BoxColliderComponent>>(
					"Generate box collider", entity.GetUUID(), before, live));
			}
			else
			{
				entity.AddComponent<BoxColliderComponent>() = seeded;
				steps.push_back(CreateScope<AddComponentCommand<BoxColliderComponent>>(
					"Add box collider", entity.GetUUID(), seeded));
			}
		}

		if (steps.size() == 1)
			undo->Push(std::move(steps.front()));
		else if (!steps.empty())
			undo->Push(CreateScope<CompositeCommand>("Generate colliders from mesh", std::move(steps)));

		if (skippedOther)
			GE_WARN("Generate from mesh skipped {0} entit{1} with sphere/capsule colliders",
				skippedOther, skippedOther == 1 ? "y" : "ies");
		if (skippedNoMesh)
			GE_WARN("Generate from mesh skipped {0} entit{1} with no resident mesh",
				skippedNoMesh, skippedNoMesh == 1 ? "y" : "ies");

		m_AuditDirty = true;
		RefreshFocusOverlay(scene);
	}

	void MapPanel::RebuildAudit(Scene* scene, SceneHierarchyPanel* hierarchy, UUID excludeFromAudit)
	{
		m_Findings.clear();
		m_Footprints.clear();
		m_AuditScene = scene;
		m_AuditExclude = excludeFromAudit;
		m_AuditToleranceStamp = m_AuditTolerance;
		m_AuditDirty = false;
		if (!scene)
		{
			m_FocusUUID = UUID{ 0 };
			m_OverlayBoxes.clear();
			return;
		}

		const std::unordered_set<UUID> hidden = ExpandHidden(*scene, hierarchy);
		const float tol = m_AuditTolerance;

		auto view = scene->Reg().view<StaticMeshComponent>();
		for (auto handle : view)
		{
			Entity entity{ handle, scene };
			const UUID id = entity.GetUUID();
			if (id == excludeFromAudit || hidden.count(id))
				continue;
			if (!ResidentMesh(entity))
				continue;

			if (!HasAnyCollider(entity))
			{
				m_Findings.push_back({ id, FindingKind::NoCollider, entity.GetName() });
				continue;
			}

			if (!entity.HasComponent<BoxColliderComponent>())
				continue;

			AABB meshBox;
			if (!MeshWorldAABB(entity, *scene, meshBox))
				continue;
			const AABB colBox = BoxColliderWorldAABB(entity, *scene);

			bool smaller = false;
			bool larger = false;
			for (int i = 0; i < 3; ++i)
			{
				if (AxisOutside(meshBox.Min[i], meshBox.Max[i], colBox.Min[i], colBox.Max[i], tol))
					smaller = true;
				if (AxisLarger(meshBox.Min[i], meshBox.Max[i], colBox.Min[i], colBox.Max[i], tol))
					larger = true;
			}

			if (smaller)
				m_Findings.push_back({ id, FindingKind::Smaller, entity.GetName() });
			if (larger)
				m_Findings.push_back({ id, FindingKind::Larger, entity.GetName() });
			if (!smaller && !larger && ExtentsAgree(meshBox, colBox, tol)
				&& CentresDiffer(meshBox, colBox, tol))
			{
				m_Findings.push_back({ id, FindingKind::OffsetMismatch, entity.GetName() });
			}
		}

		auto roots = scene->Reg().view<RelationshipComponent, TagComponent>();
		for (auto handle : roots)
		{
			Entity root{ handle, scene };
			if (root.GetComponent<RelationshipComponent>().Parent != UUID{ 0 })
				continue;
			if (hidden.count(root.GetUUID()) || root.GetUUID() == excludeFromAudit)
				continue;

			std::vector<Entity> subtree;
			std::unordered_set<UUID> visited;
			scene->CollectSubtree(root, subtree, visited);

			AABB meshUnion, colUnion;
			bool anyMesh = false;
			bool anyCol = false;
			for (Entity entity : subtree)
			{
				const UUID id = entity.GetUUID();
				if (id == excludeFromAudit || hidden.count(id))
					continue;
				AABB meshBox;
				if (MeshWorldAABB(entity, *scene, meshBox))
					Encapsulate(meshUnion, meshBox, anyMesh);
				AABB colBox;
				if (ColliderWorldAABB(entity, *scene, colBox))
					Encapsulate(colUnion, colBox, anyCol);
			}

			if (!anyMesh)
				continue;

			Footprint row;
			row.Root = root.GetUUID();
			row.Name = root.GetName();
			row.HasCollider = anyCol;
			const float meshVol = AabbVolume(meshUnion);
			if (!anyCol || meshVol <= 1.0e-8f)
				row.Coverage = 0.0f;
			else
				row.Coverage = AabbVolume(AabbIntersection(meshUnion, colUnion)) / meshVol;
			m_Footprints.push_back(std::move(row));
		}

		RefreshFocusOverlay(scene);
	}

	void MapPanel::DrawParityAudit(bool editing, Scene* scene, EditorUndoStack* undo,
		SceneHierarchyPanel* hierarchy, EditorCamera* camera, UUID excludeFromAudit)
	{
		if (!ImGui::CollapsingHeader("Parity audit", ImGuiTreeNodeFlags_DefaultOpen))
			return;

		ImGui::TextWrapped("This audit does not detect a hole between two correctly-sized "
			"colliders. Coverage is a footprint roll-up, not a certificate that the shell is closed.");

		ImGui::SetNextItemWidth(96.0f);
		if (ImGui::DragFloat("Tolerance", &m_AuditTolerance, 0.001f, 0.001f, 1.0f, "%.3f m"))
			m_AuditDirty = true;

		const bool canGenerate = editing && scene && undo && hierarchy
			&& !hierarchy->GetSelection().empty();
		ImGui::BeginDisabled(!canGenerate);
		if (ImGui::Button("Generate collider from mesh"))
			GenerateCollidersFromMesh(scene, undo, hierarchy);
		ImGui::EndDisabled();
		if (!canGenerate && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("Select mesh entities in Edit mode. Sphere/capsule colliders are skipped.");

		ImGui::SameLine();
		if (ImGui::Button("Refresh"))
			m_AuditDirty = true;

		const size_t undoDepth = undo ? undo->UndoDepth() : 0;
		const size_t redoDepth = undo ? undo->RedoDepth() : 0;
		const size_t hiddenHash = hierarchy ? HashHidden(hierarchy->HiddenEntities()) : 0;
		if (m_AuditDirty || scene != m_AuditScene || undoDepth != m_AuditUndo
			|| redoDepth != m_AuditRedo || hiddenHash != m_AuditHiddenHash
			|| excludeFromAudit != m_AuditExclude
			|| m_AuditTolerance != m_AuditToleranceStamp)
		{
			RebuildAudit(scene, hierarchy, excludeFromAudit);
			m_AuditUndo = undoDepth;
			m_AuditRedo = redoDepth;
			m_AuditHiddenHash = hiddenHash;
		}

		ImGui::Separator();
		if (m_Findings.empty())
			ImGui::TextDisabled("No box-collider findings.");
		else
			ImGui::Text("%zu finding%s", m_Findings.size(), m_Findings.size() == 1 ? "" : "s");

		for (size_t i = 0; i < m_Findings.size(); ++i)
		{
			const Finding& finding = m_Findings[i];
			const char* kind = "No collider";
			ImVec4 color = ImVec4(0.95f, 0.45f, 0.40f, 1.0f);
			switch (finding.Kind)
			{
				case FindingKind::NoCollider:
					kind = "No collider";
					break;
				case FindingKind::Smaller:
					kind = "Collider smaller than mesh";
					break;
				case FindingKind::Larger:
					kind = "Collider larger than mesh";
					color = ImVec4(0.95f, 0.75f, 0.35f, 1.0f);
					break;
				case FindingKind::OffsetMismatch:
					kind = "Offset mismatch";
					color = ImVec4(0.45f, 0.80f, 0.95f, 1.0f);
					break;
			}

			ImGui::PushID(static_cast<int>(i));
			ImGui::PushStyleColor(ImGuiCol_Text, color);
			const bool selected = finding.Entity == m_FocusUUID;
			if (ImGui::Selectable((finding.Name + "  —  " + kind).c_str(), selected))
				FocusFinding(scene, hierarchy, camera, finding.Entity);
			ImGui::PopStyleColor();
			ImGui::PopID();
		}

		if (!m_Footprints.empty())
		{
			ImGui::Separator();
			ImGui::TextUnformatted("Footprint");
			for (const Footprint& row : m_Footprints)
			{
				if (!row.HasCollider)
					ImGui::TextDisabled("%s  —  no colliders", row.Name.c_str());
				else
					ImGui::Text("%s  —  %.0f%% coverage", row.Name.c_str(), row.Coverage * 100.0f);
			}
		}
	}

	void MapPanel::DrawScatter(bool editing)
	{
		using EditorUI::IconButton;

		if (!ImGui::CollapsingHeader("Scatter", ImGuiTreeNodeFlags_DefaultOpen))
			return;

		if (m_PaintArmed)
		{
			ImGui::TextWrapped("Painting. LMB paints, Shift+LMB erases the active group, "
				"Esc / RMB cancels. Palette clicks set the source.");
			ImGui::Separator();
		}

		const AssetMetadata* sourceMeta = AssetManager::GetMetadata(m_Scatter.Source);
		if (sourceMeta)
			ImGui::Text("Source: %s", sourceMeta->FilePath.c_str());
		else
			ImGui::TextDisabled("Source: click a pinned prefab or mesh.");

		ImGui::BeginDisabled(!editing);
		if (IconButton(ICON_LC_SPRAY_CAN, m_PaintArmed ? "Stop painting" : "Paint", m_PaintArmed))
		{
			if (!m_PaintArmed && !IsAssetHandleValid(m_Scatter.Source))
				GE_WARN("Scatter: pin a prefab or mesh and click it before painting.");
			else
				m_PaintArmed = !m_PaintArmed;
		}
		ImGui::EndDisabled();

		ImGui::SetNextItemWidth(96.0f);
		ImGui::DragFloat("Radius", &m_Scatter.Radius, 0.05f, 0.1f, 50.0f, "%.2f m");
		ImGui::SetNextItemWidth(96.0f);
		ImGui::DragFloat("Density", &m_Scatter.Density, 0.1f, 0.1f, 100.0f, "%.1f /m2");
		ImGui::SetNextItemWidth(96.0f);
		ImGui::DragFloat("Min spacing", &m_Scatter.MinSpacing, 0.05f, 0.0f, 20.0f, "%.2f m");
		ImGui::SetNextItemWidth(96.0f);
		ImGui::DragFloat("Yaw jitter", &m_Scatter.YawJitter, 1.0f, 0.0f, 180.0f, "%.0f deg");
		ImGui::SetNextItemWidth(96.0f);
		ImGui::DragFloat("Scale min", &m_Scatter.ScaleMin, 0.01f, 0.05f, 10.0f, "%.2f");
		ImGui::SetNextItemWidth(96.0f);
		ImGui::DragFloat("Scale max", &m_Scatter.ScaleMax, 0.01f, 0.05f, 10.0f, "%.2f");
		if (m_Scatter.ScaleMax < m_Scatter.ScaleMin)
			m_Scatter.ScaleMax = m_Scatter.ScaleMin;

		ImGui::Checkbox("Align to normal", &m_Scatter.AlignToNormal);
		ImGui::Checkbox("Filter to starting surface", &m_Scatter.FilterToStartSurface);
		ImGui::SetItemTooltip("Drop rays must hit the same mesh asset as the stroke's first hit "
			"(or the work plane). Tiled ground shares a mesh, so one tile starts the stroke "
			"for the whole floor.");

		ImGui::SetNextItemWidth(96.0f);
		int seed = static_cast<int>(m_Scatter.Seed);
		if (ImGui::DragInt("Seed", &seed, 1.0f, 1, INT_MAX))
			m_Scatter.Seed = static_cast<uint32_t>(seed);
		ImGui::SameLine();
		if (ImGui::Button("Randomize"))
			m_Scatter.Seed = static_cast<uint32_t>(ImGui::GetTime() * 1000.0) ^ 0xA5A5u;

		ImGui::SetNextItemWidth(96.0f);
		ImGui::DragInt("Max per stroke", &m_Scatter.MaxInstancesPerStroke, 1.0f, 1, 2000);

		ImGui::TextDisabled("Each instance is an entity. Cap exists because of that, not draw calls.");
	}

	void MapPanel::DrawUpcomingSections()
	{
		ImGui::BeginDisabled();
		if (ImGui::CollapsingHeader("Markers"))
			ImGui::TextUnformatted("M4.");
		ImGui::EndDisabled();
	}

	void MapPanel::LoadPalette()
	{
		m_Pinned.clear();
		m_PaletteLoaded = true;

		std::ifstream in(PalettePath());
		if (!in)
			return;

		std::string line;
		bool inPinned = false;
		while (std::getline(in, line))
		{
			const std::string trimmed = TrimCopy(line);
			if (trimmed.empty() || trimmed.front() == '#')
				continue;

			if (trimmed.rfind("pinned:", 0) == 0)
			{
				inPinned = true;
				continue;
			}

			if (!inPinned)
				continue;

			if (trimmed.front() != '-')
			{
				inPinned = false;
				continue;
			}

			const std::string path = TrimCopy(trimmed.substr(1));
			if (!path.empty())
				m_Pinned.push_back(path);
		}
	}

	void MapPanel::SavePalette() const
	{
		const std::filesystem::path path = PalettePath();
		std::error_code ec;
		std::filesystem::create_directories(path.parent_path(), ec);
		if (ec)
		{
			GE_WARN("Could not create '{0}': {1}", path.parent_path().generic_string(),
				ec.message());
			return;
		}

		std::ofstream out(path);
		if (!out)
		{
			GE_WARN("Could not write map palette '{0}'", path.generic_string());
			return;
		}

		out << "# Ganymed map palette — pinned asset paths relative to the asset root.\n";
		out << "pinned:\n";
		for (const std::string& pin : m_Pinned)
			out << "  - " << pin << "\n";
	}

	void MapPanel::Pin(const std::string& relativePath)
	{
		if (std::find(m_Pinned.begin(), m_Pinned.end(), relativePath) != m_Pinned.end())
			return;
		m_Pinned.push_back(relativePath);
		SavePalette();
	}

	void MapPanel::Unpin(const std::string& relativePath)
	{
		auto it = std::find(m_Pinned.begin(), m_Pinned.end(), relativePath);
		if (it == m_Pinned.end())
			return;
		m_Pinned.erase(it);
		SavePalette();
	}

}
