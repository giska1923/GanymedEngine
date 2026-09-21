#pragma once

#include "GanymedE/Assets/AssetTypes.h"
#include "GanymedE/Core/Core.h"
#include "GanymedE/Renderer/Texture.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace GanymedE {

	class ContentBrowserPanel
	{
	public:
		ContentBrowserPanel();

		void OnImGuiRender();

		// Absolute path of the selected file or folder; empty when nothing is selected.
		const std::filesystem::path& GetSelectedPath() const { return m_Selected; }

		// Fired when the selection actually changes, including a clear. EditorLayer wires
		// this to the Asset Inspector the same way it wires MapPanel's place handler.
		void SetSelectionChangedCallback(std::function<void(const std::filesystem::path&)> callback)
		{
			m_OnSelectionChanged = std::move(callback);
		}
	private:
		void SetSelected(const std::filesystem::path& path);
		enum class SortMode { Name, Type };
		enum class ViewMode { Grid, List };

		struct ListingItem
		{
			std::filesystem::path Path;
			std::filesystem::path Relative;
			std::filesystem::path ParentRelative;
			std::string Name;
			std::string NameLower;
			bool IsDirectory = false;
			AssetType Type = AssetType::None;
		};

		struct FolderNode
		{
			std::string Name;
			std::filesystem::path Relative;
			std::vector<FolderNode> Children;
		};

		bool IsUnderAssetRoot(const std::filesystem::path& path) const;
		std::filesystem::path CurrentRelative() const;
		bool TryNavigate(const std::filesystem::path& dest, bool recordHistory);
		void GoBack();
		void GoForward();
		void GoUp();

		void RefreshCaches();
		void RefreshIndex();
		void WalkDir(FolderNode& node, const std::filesystem::path& absolute,
			const std::filesystem::path& relative);
		void CollectVisible();

		void DrawToolbar();
		void DrawBreadcrumb();
		void DrawFolderTree();
		void DrawFolderNode(const FolderNode& node);
		void DrawFilePane();
		void DrawGrid();
		void DrawList();
		void DrawFooter();
		void DrawItemContextMenu(const ListingItem& item);
		void BeginItemDrag(const ListingItem& item);

		std::function<void(const std::filesystem::path&)> m_OnSelectionChanged;

		std::filesystem::path m_BaseDirectory;
		std::filesystem::path m_CurrentDirectory;
		std::filesystem::path m_Selected;

		std::vector<std::filesystem::path> m_Back;
		std::vector<std::filesystem::path> m_Forward;

		char m_Search[128] = {};
		SortMode m_Sort = SortMode::Name;
		ViewMode m_View = ViewMode::Grid;

		std::vector<ListingItem> m_Index;
		std::vector<const ListingItem*> m_Visible;
		FolderNode m_Tree;
		bool m_IndexDirty = true;
		std::filesystem::file_time_type m_ListingMtime{};
		uint32_t m_SeenReloads = 0;
		std::chrono::steady_clock::time_point m_LastTreeWalk{};

		Ref<Texture2D> m_DirectoryIcon;
		Ref<Texture2D> m_FileIcon;
	};
}
