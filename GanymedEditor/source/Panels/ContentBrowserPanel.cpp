#include "ContentBrowserPanel.h"

#include "GanymedE/Assets/AssetManager.h"
#include "GanymedE/Assets/CompiledCache.h"
#include "GanymedE/Assets/AssetMeta.h"
#include "GanymedE/Assets/AssetPaths.h"
#include "GanymedE/Assets/AssetTypes.h"
#include "GanymedE/Core/Log.h"

#include <imgui/imgui.h>

#include <algorithm>
#include <cctype>
#include <string>

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
			case AssetType::Script:      return ImVec4(1.0f, 0.95f, 0.5f, 1.0f);
			case AssetType::Audio:       return ImVec4(0.55f, 1.0f, 0.95f, 1.0f);
			case AssetType::Prefab:      return ImVec4(0.6f, 0.85f, 0.85f, 1.0f);
			default:                     return ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
		}
	}

	static bool IsImportableAsset(const std::filesystem::path& path)
	{
		AssetType type = AssetTypeFromExtension(path.extension().string());
		return type == AssetType::StaticMesh || type == AssetType::Environment
			|| type == AssetType::Texture || type == AssetType::Material
			|| type == AssetType::Script || type == AssetType::Audio
			|| type == AssetType::Prefab;
	}

	void ContentBrowserPanel::OnImGuiRender()
	{
		ImGui::Begin("Content Browser");

		// Asset-tree maintenance, on the panel's background rather than on a file. It acts on
		// what the last scan found, so it is deliberately not enabled when there is nothing to
		// reap - the disabled item with a count is the report.
		if (ImGui::BeginPopupContextWindow("##ContentBrowserContext", ImGuiPopupFlags_MouseButtonRight
			| ImGuiPopupFlags_NoOpenOverItems))
		{
			const std::size_t orphans = AssetManager::OrphanedMetaCount();
			const std::string label = orphans == 0
				? std::string("No orphaned `.meta` sidecars")
				: "Clean " + std::to_string(orphans) + " orphaned `.meta` sidecar(s)";

			if (ImGui::MenuItem(label.c_str(), nullptr, false, orphans != 0))
				GE_CORE_INFO("Cleaned {0} orphaned sidecar(s)", AssetManager::CleanOrphanedMeta());

			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			{
				ImGui::SetTooltip("Deletes `.meta` files whose asset is missing. Each sidecar "
					"holds the handle scenes use to name its asset, so only do this once you "
					"know the assets are gone rather than not checked out.");
			}

			if (ImGui::MenuItem("Rescan assets/"))
				AssetManager::ScanAssets();

			ImGui::EndPopup();
		}

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
			std::string filenameString = path.filename().string();

			// Dotted entries are engine bookkeeping, not content: `.assets` (the mesh cache)
			// today, `.compiled` when the asset compiler lands.
			if (!filenameString.empty() && filenameString.front() == '.')
				continue;

			// `.meta` sidecars carry asset identity and are committed, but showing them would
			// double every row in the grid and offer Import on a file that is not an asset.
			// They are managed by the AssetManager, never by hand - see docs/engine/assets.md.
			if (AssetMetaSerializer::IsSidecarPath(path))
				continue;

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
				auto relativePath = std::filesystem::relative(path, g_AssetPath);

				if (IsImportableAsset(path))
				{
					if (ImGui::MenuItem("Import"))
					{
						AssetHandle handle = AssetManager::ImportAsset(relativePath);
						if (IsAssetHandleValid(handle))
							GE_CORE_INFO("Imported '{0}'", relativePath.string());
					}
				}

				// Only registered assets can be reloaded - there is nothing to evict
				// for a file the manager has never seen.
				AssetHandle handle = AssetManager::GetHandle(relativePath);
				if (IsAssetHandleValid(handle))
				{
					if (ImGui::MenuItem("Reload"))
						AssetManager::Reload(handle);

					// Reimport is Reload plus throwing away the compiled artifact, for the case
					// the epoch record cannot see: an importer setting edited by hand, or simple
					// doubt about what is in the cache. Blocking, and a BC7 encode of a large
					// texture takes seconds - the tooltip says so rather than letting the editor
					// look hung.
					const bool compiled = CompiledCache::CompilerFor(
						AssetManager::GetAssetType(handle)) != nullptr;

					if (compiled && ImGui::MenuItem("Reimport"))
					{
						if (const AssetMetadata* metadata = AssetManager::GetMetadata(handle))
							CompiledCache::Invalidate(*metadata);

						AssetManager::Reload(handle);
					}

					if (compiled && ImGui::IsItemHovered())
					{
						ImGui::SetTooltip("Recompiles from source. This blocks - a large texture "
							"is seconds.");
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
