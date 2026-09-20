#include "MapPanel.h"
#include "SceneHierarchyPanel.h"

#include "../EditorIcons.h"
#include "../EditorTheme.h"
#include "../EditorUndo.h"
#include "../EditorWidgets.h"

#include "GanymedE/Assets/AssetManager.h"
#include "GanymedE/Assets/AssetPaths.h"
#include "GanymedE/Core/Log.h"
#include "GanymedE/Scene/Components.h"
#include "GanymedE/Scene/Scene.h"

#include <glm/glm.hpp>
#include <imgui/imgui.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

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
		Scene* scene, EditorUndoStack* undo, SceneHierarchyPanel* hierarchy)
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
		ImGui::TextDisabled("Prefabs and meshes. Click to place.");

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

		if (IsAssetHandleValid(placeHandle) && m_OnPlace)
			m_OnPlace(placeHandle, placeType);
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

	void MapPanel::DrawUpcomingSections()
	{
		ImGui::BeginDisabled();
		if (ImGui::CollapsingHeader("Parity audit"))
			ImGui::TextUnformatted("M2.");
		if (ImGui::CollapsingHeader("Scatter"))
			ImGui::TextUnformatted("M3.");
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
