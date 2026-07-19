#include "ContentBrowserPanel.h"

#include "GanymedE/Assets/AssetManager.h"
#include "GanymedE/Assets/AssetPaths.h"
#include "GanymedE/Assets/AssetTypes.h"
#include "GanymedE/Core/Log.h"

#include <imgui/imgui.h>

#include <algorithm>
#include <cctype>

namespace GanymedE {

	// Kept for editor code that still references g_AssetPath
	extern const std::filesystem::path g_AssetPath = GetAssetRoot();

	ContentBrowserPanel::ContentBrowserPanel()
		: m_BaseDirectory(g_AssetPath), m_CurrentDirectory(m_BaseDirectory)
	{
		m_DirectoryIcon = Texture2D::Create("resources/icons/ContentBrowser/DirectoryIcon.png");
		m_FileIcon = Texture2D::Create("resources/icons/ContentBrowser/FileIcon.png");
	}

	static ImVec4 GetAssetIconTint(const std::filesystem::path& path, bool isDirectory)
	{
		if (isDirectory)
			return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);

		AssetType type = AssetTypeFromExtension(path.extension().string());
		switch (type)
		{
			case AssetType::StaticMesh:  return ImVec4(0.55f, 0.75f, 1.0f, 1.0f);
			case AssetType::Environment: return ImVec4(1.0f, 0.75f, 0.35f, 1.0f);
			case AssetType::Scene:       return ImVec4(0.55f, 1.0f, 0.65f, 1.0f);
			case AssetType::Texture:     return ImVec4(1.0f, 0.55f, 0.85f, 1.0f);
			case AssetType::Material:    return ImVec4(0.85f, 0.55f, 1.0f, 1.0f);
			default:                     return ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
		}
	}

	static bool IsImportableAsset(const std::filesystem::path& path)
	{
		AssetType type = AssetTypeFromExtension(path.extension().string());
		return type == AssetType::StaticMesh || type == AssetType::Environment
			|| type == AssetType::Texture || type == AssetType::Material;
	}

	void ContentBrowserPanel::OnImGuiRender()
	{
		ImGui::Begin("Content Browser");

		if (m_CurrentDirectory != m_BaseDirectory)
		{
			if (ImGui::Button("<-"))
			{
				m_CurrentDirectory = m_CurrentDirectory.parent_path();

				// Never allow escaping the asset root, even if the path was manipulated
				auto base = std::filesystem::absolute(m_BaseDirectory).lexically_normal();
				auto current = std::filesystem::absolute(m_CurrentDirectory).lexically_normal();
				if (std::mismatch(base.begin(), base.end(), current.begin(), current.end()).first != base.end())
					m_CurrentDirectory = m_BaseDirectory;
			}
		}

		static float padding = 16.0f;
		static float thumbnailSize = 96.0f;
		float cellSize = thumbnailSize + padding;

		float panelWidth = ImGui::GetContentRegionAvail().x;
		int columnCount = (int)(panelWidth / cellSize);
		if (columnCount < 1)
			columnCount = 1;

		ImGui::Columns(columnCount, 0, false);

		for (auto& directoryEntry : std::filesystem::directory_iterator(m_CurrentDirectory))
		{
			const auto& path = directoryEntry.path();
			if (path.filename() == ".assets")
				continue;

			std::string filenameString = path.filename().string();
			bool isDirectory = directoryEntry.is_directory();

			ImGui::PushID(filenameString.c_str());
			Ref<Texture2D> icon = isDirectory ? m_DirectoryIcon : m_FileIcon;
			ImVec4 iconTint = GetAssetIconTint(path, isDirectory);

			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
			// Default UVs: Texture2D already loads in bgfx's top-left origin, so
			// the old {0,1}-{1,0} flip (a GL loader compensation) would invert these.
			ImGui::ImageButton("##thumbnail", (ImTextureID)(uintptr_t)icon->GetRendererID(),
				{ thumbnailSize, thumbnailSize }, { 0, 0 }, { 1, 1 }, ImVec4(0, 0, 0, 0), iconTint);

			if (ImGui::BeginDragDropSource())
			{
				auto relativePath = std::filesystem::relative(path, g_AssetPath);
				std::string itemPath = relativePath.string();
				ImGui::SetDragDropPayload("CONTENT_BROWSER_ITEM", itemPath.c_str(), itemPath.size() + 1);
				ImGui::TextUnformatted(filenameString.c_str());
				ImGui::EndDragDropSource();
			}

			if (!isDirectory && ImGui::BeginPopupContextItem())
			{
				if (IsImportableAsset(path))
				{
					if (ImGui::MenuItem("Import"))
					{
						auto relativePath = std::filesystem::relative(path, g_AssetPath);
						AssetHandle handle = AssetManager::ImportAsset(relativePath);
						if (IsAssetHandleValid(handle))
							GE_CORE_INFO("Imported '{0}'", relativePath.string());
					}
				}
				ImGui::EndPopup();
			}

			ImGui::PopStyleColor();
			if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
			{
				if (isDirectory)
					m_CurrentDirectory /= path.filename();
			}
			ImGui::TextWrapped("%s", filenameString.c_str());

			ImGui::NextColumn();

			ImGui::PopID();
		}

		ImGui::Columns(1);

		ImGui::End();
	}
}
