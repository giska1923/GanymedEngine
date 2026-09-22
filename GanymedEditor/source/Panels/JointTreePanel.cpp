#include "JointTreePanel.h"

#include "../EditorIcons.h"
#include "../EditorJoint.h"
#include "../EditorPicking.h"
#include "../EditorTheme.h"
#include "../EditorWidgets.h"

#include "GanymedE/Renderer/Mesh.h"
#include "GanymedE/Scene/Components.h"
#include "GanymedE/Scene/Scene.h"

#include <imgui/imgui.h>

#include <cctype>
#include <cstring>
#include <string>

namespace GanymedE {

	namespace {

		bool NameContainsI(const std::string& name, const char* filter)
		{
			if (!filter || !filter[0])
				return true;
			const char* hay = name.c_str();
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

	}

	void JointTreePanel::OnImGuiRender(Scene* scene, EditorJointTool& tool,
		const std::vector<Entity>& selection)
	{
		using EditorUI::BeginPanel;
		using EditorUI::BeginPanelBody;
		using EditorUI::ColumnHeaderRow;
		using EditorUI::EndPanel;
		using EditorUI::EndPanelBody;
		using EditorUI::EndPanelToolbarRow;
		using EditorUI::PanelToolbarRow;
		using EditorUI::SearchField;

		m_Scene = scene;
		m_Tool = &tool;
		m_Skeleton = {};

		if (BeginPanel("Joints"))
		{
			if (PanelToolbarRow("##JointsTB"))
				SearchField("filter", m_Search, sizeof(m_Search), "Search joints...");
			EndPanelToolbarRow();

			ColumnHeaderRow({ { "Joint", 0.0f } });
			BeginPanelBody();

			if (!scene)
			{
				ImGui::TextDisabled("No scene");
			}
			else
			{
				if (tool.HasJoint())
				{
					Entity named = scene->FindEntityByUUID(tool.SkeletonEntity);
					if (EntityHasSkinnedPose(named))
						m_Skeleton = named;
					else
						tool.ClearJoint();
				}

				if (!m_Skeleton)
				{
					for (Entity entity : selection)
					{
						m_Skeleton = FindSkinnedMeshInHierarchy(*scene, entity);
						if (m_Skeleton)
							break;
					}
				}

				if (!m_Skeleton)
				{
					ImGui::TextDisabled("Select a skinned entity.");
				}
				else
				{
					const Skeleton& skeleton = m_Skeleton.GetComponent<StaticMeshComponent>()
						.Mesh.Get()->GetSkeleton();
					const uint32_t count = skeleton.JointCount();
					if (tool.HasJoint() && (uint32_t)tool.Joint >= count)
						tool.ClearJoint();

					m_Children.assign(count, {});
					m_Roots.clear();
					for (uint32_t i = 0; i < count; i++)
					{
						const int32_t parent = skeleton.ParentIndices[i];
						if (parent < 0 || (uint32_t)parent >= count)
							m_Roots.push_back((int32_t)i);
						else
							m_Children[(uint32_t)parent].push_back((int32_t)i);
					}

					m_FilterVisible.assign(count, 0);
					if (m_Search[0] != '\0')
					{
						for (uint32_t i = 0; i < count; i++)
						{
							const std::string& name = (size_t)i < skeleton.JointNames.size()
								? skeleton.JointNames[i] : std::string{};
							if (NameContainsI(name, m_Search))
								MarkFilterVisible((int32_t)i);
						}
					}

					ImGui::BeginChild("##JointTree", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
					if (m_Search[0] != '\0')
					{
						bool any = false;
						for (uint8_t v : m_FilterVisible)
						{
							if (v)
							{
								any = true;
								break;
							}
						}
						if (!any)
							ImGui::TextDisabled("No joints match.");
					}

					for (int32_t root : m_Roots)
						DrawJoint(root);
					ImGui::EndChild();
				}
			}

			EndPanelBody();
		}
		EndPanel();
	}

	bool JointTreePanel::MarkFilterVisible(int32_t index)
	{
		if (!m_Skeleton || index < 0)
			return false;
		const Skeleton& skeleton = m_Skeleton.GetComponent<StaticMeshComponent>()
			.Mesh.Get()->GetSkeleton();
		if ((uint32_t)index >= skeleton.JointCount())
			return false;
		if (m_FilterVisible[(uint32_t)index])
			return true;
		m_FilterVisible[(uint32_t)index] = 1;
		const int32_t parent = skeleton.ParentIndices[(uint32_t)index];
		if (parent >= 0)
			MarkFilterVisible(parent);
		return true;
	}

	void JointTreePanel::DrawJoint(int32_t index)
	{
		if (!m_Skeleton || !m_Tool || index < 0)
			return;
		const Skeleton& skeleton = m_Skeleton.GetComponent<StaticMeshComponent>()
			.Mesh.Get()->GetSkeleton();
		if ((uint32_t)index >= skeleton.JointCount())
			return;

		const bool filtering = m_Search[0] != '\0';
		if (filtering && !m_FilterVisible[(uint32_t)index])
			return;

		const std::string& name = (size_t)index < skeleton.JointNames.size()
			? skeleton.JointNames[(size_t)index] : std::string{};
		const char* label = name.empty() ? "?" : name.c_str();

		bool hasVisibleChild = false;
		for (int32_t child : m_Children[(uint32_t)index])
		{
			if (!filtering || m_FilterVisible[(uint32_t)child])
			{
				hasVisibleChild = true;
				break;
			}
		}

		if (filtering && hasVisibleChild)
			ImGui::SetNextItemOpen(true, ImGuiCond_Always);

		if (m_Tool->ScrollToJoint && m_Tool->HasJoint()
			&& m_Tool->SkeletonEntity == m_Skeleton.GetUUID())
		{
			int32_t walk = m_Tool->Joint;
			while (walk >= 0 && (uint32_t)walk < skeleton.JointCount())
			{
				if (skeleton.ParentIndices[(uint32_t)walk] == index)
				{
					ImGui::SetNextItemOpen(true, ImGuiCond_Always);
					break;
				}
				walk = skeleton.ParentIndices[(uint32_t)walk];
			}
		}

		const bool selected = m_Tool->HasJoint()
			&& m_Tool->SkeletonEntity == m_Skeleton.GetUUID()
			&& m_Tool->Joint == index;

		ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow
			| ImGuiTreeNodeFlags_SpanAvailWidth
			| ImGuiTreeNodeFlags_FramePadding;
		if (selected)
			flags |= ImGuiTreeNodeFlags_Selected;
		if (!hasVisibleChild)
			flags |= ImGuiTreeNodeFlags_Leaf;

		const EditorUI::EditorTheme& theme = EditorUI::Theme();
		if (selected)
		{
			ImGui::PushStyleColor(ImGuiCol_Header, EditorUI::Color(theme.Accent));
			ImGui::PushStyleColor(ImGuiCol_HeaderHovered, EditorUI::Color(theme.AccentHover));
			ImGui::PushStyleColor(ImGuiCol_HeaderActive, EditorUI::Color(theme.AccentActive));
		}

		ImGui::PushID(index);
		const bool opened = ImGui::TreeNodeEx("Joint", flags, "%s", "");
		if (selected)
			ImGui::PopStyleColor(3);

		if (ImGui::IsItemClicked())
			m_Tool->Select(m_Skeleton.GetUUID(), index);

		if (selected && m_Tool->ScrollToJoint)
		{
			ImGui::SetScrollHereY(0.5f);
			m_Tool->ScrollToJoint = false;
		}

		{
			const ImVec2 min = ImGui::GetItemRectMin();
			const float rowH = ImGui::GetItemRectSize().y;
			ImDrawList* draw = ImGui::GetWindowDrawList();
			const ImU32 iconCol = selected ? theme.TextOnAccent : theme.TextPrimary;
			const ImU32 nameCol = selected ? theme.TextOnAccent : theme.TextPrimary;
			const ImVec2 iconSize = ImGui::CalcTextSize(ICON_LC_BONE);
			const float iconY = min.y + (rowH - iconSize.y) * 0.5f;
			const float textY = min.y + (rowH - ImGui::GetFontSize()) * 0.5f;
			const float x = min.x + ImGui::GetTreeNodeToLabelSpacing();
			draw->AddText(ImVec2(x, iconY), iconCol, ICON_LC_BONE);
			draw->AddText(ImVec2(x + iconSize.x + 6.0f, textY), nameCol, label);
		}

		if (opened)
		{
			for (int32_t child : m_Children[(uint32_t)index])
				DrawJoint(child);
			ImGui::TreePop();
		}
		ImGui::PopID();
	}

}
