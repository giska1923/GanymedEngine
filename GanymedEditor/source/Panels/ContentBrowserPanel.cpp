#include "ContentBrowserPanel.h"

#include "../EditorIcons.h"
#include "../EditorTheme.h"
#include "../EditorWidgets.h"

#include "GanymedE/Assets/AssetManager.h"
#include "GanymedE/Assets/AssetMeta.h"
#include "GanymedE/Assets/AssetPaths.h"
#include "GanymedE/Assets/AssetTypes.h"
#include "GanymedE/Assets/AssetWatcher.h"
#include "GanymedE/Assets/CompiledCache.h"
#include "GanymedE/Core/Log.h"

#include <imgui/imgui.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <system_error>
#include <utility>

namespace GanymedE {

	namespace {

		constexpr float kThumbnail = 96.0f;
		constexpr float kCellPad = 16.0f;
		constexpr float kBreadcrumbH = 32.0f;
		constexpr float kFooterH = 26.0f;
		constexpr float kSidebarDefault = 200.0f;
		constexpr double kTreeRescanSeconds = 0.25;

		std::string ToLowerAscii(const char* s)
		{
			std::string out;
			if (!s)
				return out;
			out.resize(std::strlen(s));
			for (size_t i = 0; i < out.size(); ++i)
				out[i] = (char)std::tolower(static_cast<unsigned char>(s[i]));
			return out;
		}

		std::string ToLowerAscii(const std::string& s)
		{
			return ToLowerAscii(s.c_str());
		}

		bool IsHiddenBrowserEntry(const std::filesystem::path& path)
		{
			const std::string name = path.filename().string();
			if (!name.empty() && name.front() == '.')
				return true;
			return AssetMetaSerializer::IsSidecarPath(path);
		}

		// Asset-tree maintenance, on the panel's background rather than on a file. It acts on
		// what the last scan found, so it is deliberately not enabled when there is nothing to
		// reap - the disabled item with a count is the report.
		void DrawAssetTreeContextMenu()
		{
			if (!ImGui::BeginPopupContextWindow("##ContentBrowserContext", ImGuiPopupFlags_MouseButtonRight
				| ImGuiPopupFlags_NoOpenOverItems))
				return;

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

		ImVec4 GetAssetIconTint(AssetType type, bool isDirectory)
		{
			if (isDirectory)
				return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);

			const int i = static_cast<int>(type);
			if (i >= 0 && i < EditorUI::kAssetTintCount)
				return EditorUI::Color(EditorUI::Theme().AssetTint[i]);
			return EditorUI::Color(EditorUI::Theme().AssetTint[0]);
		}

		void FileTypeChrome(bool isDirectory, AssetType type, const char*& icon, ImU32& tint)
		{
			const EditorUI::EditorTheme& theme = EditorUI::Theme();
			if (isDirectory)
			{
				icon = ICON_LC_FOLDER;
				tint = theme.TextPrimary;
				return;
			}

			tint = theme.AssetTint[0];
			const int i = static_cast<int>(type);
			if (i >= 0 && i < EditorUI::kAssetTintCount)
				tint = theme.AssetTint[i];

			switch (type)
			{
				case AssetType::StaticMesh:  icon = ICON_LC_BOX; break;
				case AssetType::Environment: icon = ICON_LC_SUN; break;
				case AssetType::Texture:     icon = ICON_LC_IMAGE; break;
				case AssetType::Material:    icon = ICON_LC_SPARKLES; break;
				case AssetType::Scene:       icon = ICON_LC_FILE; break;
				case AssetType::Script:      icon = ICON_LC_FILE_CODE; break;
				case AssetType::Audio:       icon = ICON_LC_VOLUME_2; break;
				case AssetType::Prefab:      icon = ICON_LC_PACKAGE; break;
				default:                     icon = ICON_LC_FILE; break;
			}
		}

		bool IsImportableAsset(AssetType type)
		{
			return type == AssetType::StaticMesh || type == AssetType::Environment
				|| type == AssetType::Texture || type == AssetType::Material
				|| type == AssetType::Script || type == AssetType::Audio
				|| type == AssetType::Prefab;
		}

		std::filesystem::path Normalize(const std::filesystem::path& path)
		{
			return std::filesystem::absolute(path).lexically_normal();
		}

	}

	// The root here is a placeholder, not the answer. This panel is a by-value member of
	// EditorLayer, so its constructor runs before EditorLayer::OnAttach calls AssetManager::Init -
	// GetAssetRoot() is still the default at this point, and with --project it is the *wrong*
	// default. RefreshCaches re-homes it once the real root is set.
	//
	// This is the third form of one bug. It began as `extern const path g_AssetPath =
	// GetAssetRoot();` at namespace scope, captured before main; that was replaced by this
	// member, captured before OnAttach. Copying the root anywhere is the mistake - see the
	// comment in AssetPaths.cpp.
	ContentBrowserPanel::ContentBrowserPanel()
		: m_BaseDirectory(GetAssetRoot()), m_CurrentDirectory(m_BaseDirectory)
	{
		m_DirectoryIcon = Texture2D::Create("resources/icons/ContentBrowser/DirectoryIcon.png");
		m_FileIcon = Texture2D::Create("resources/icons/ContentBrowser/FileIcon.png");
	}

	bool ContentBrowserPanel::IsUnderAssetRoot(const std::filesystem::path& path) const
	{
		const auto base = Normalize(m_BaseDirectory);
		const auto current = Normalize(path);
		return std::mismatch(base.begin(), base.end(), current.begin(), current.end()).first == base.end();
	}

	std::filesystem::path ContentBrowserPanel::CurrentRelative() const
	{
		std::error_code ec;
		std::filesystem::path rel = std::filesystem::relative(m_CurrentDirectory, m_BaseDirectory, ec);
		if (ec || rel == ".")
			return {};
		return rel;
	}

	bool ContentBrowserPanel::TryNavigate(const std::filesystem::path& dest, bool recordHistory)
	{
		std::error_code ec;
		std::filesystem::path next = dest;
		if (!std::filesystem::is_directory(next, ec) || !IsUnderAssetRoot(next))
			next = m_BaseDirectory;

		if (Normalize(next) == Normalize(m_CurrentDirectory))
			return false;

		if (recordHistory)
		{
			m_Back.push_back(m_CurrentDirectory);
			m_Forward.clear();
		}

		m_CurrentDirectory = next;
		m_Selected.clear();
		{
			std::error_code mtimeEc;
			m_ListingMtime = std::filesystem::last_write_time(m_CurrentDirectory, mtimeEc);
		}
		return true;
	}

	void ContentBrowserPanel::GoBack()
	{
		if (m_Back.empty())
			return;
		m_Forward.push_back(m_CurrentDirectory);
		const std::filesystem::path dest = m_Back.back();
		m_Back.pop_back();
		TryNavigate(dest, false);
	}

	void ContentBrowserPanel::GoForward()
	{
		if (m_Forward.empty())
			return;
		m_Back.push_back(m_CurrentDirectory);
		const std::filesystem::path dest = m_Forward.back();
		m_Forward.pop_back();
		TryNavigate(dest, false);
	}

	void ContentBrowserPanel::GoUp()
	{
		if (Normalize(m_CurrentDirectory) == Normalize(m_BaseDirectory))
			return;
		TryNavigate(m_CurrentDirectory.parent_path(), true);
	}

	void ContentBrowserPanel::RefreshCaches()
	{
		// First, because everything below is relative to it. Comparing rather than assigning
		// unconditionally keeps navigation: re-homing on every frame would drop the user back to
		// the project root each time they opened a folder.
		if (Normalize(m_BaseDirectory) != Normalize(GetAssetRoot()))
		{
			m_BaseDirectory = GetAssetRoot();
			m_CurrentDirectory = m_BaseDirectory;
			m_IndexDirty = true;
		}

		const uint32_t reloads = AssetWatcher::GetStats().Reloads;
		if (reloads != m_SeenReloads)
		{
			m_SeenReloads = reloads;
			m_IndexDirty = true;
		}

		std::error_code ec;
		if (!std::filesystem::is_directory(m_CurrentDirectory, ec))
		{
			m_CurrentDirectory = m_BaseDirectory;
			m_IndexDirty = true;
		}

		const auto mtime = std::filesystem::last_write_time(m_CurrentDirectory, ec);
		if (!ec && mtime != m_ListingMtime)
		{
			m_ListingMtime = mtime;
			m_IndexDirty = true;
		}

		// One recursive walk fills the flat index *and* the folder tree. AssetWatcher only
		// stamps indexed files, so an empty folder created in Explorer never increments
		// Reloads — the 0.25 s cadence is the catch-all. Search never walks; it filters
		// m_Index. Navigate does not dirty the index.
		const auto now = std::chrono::steady_clock::now();
		if (m_IndexDirty || m_LastTreeWalk.time_since_epoch().count() == 0
			|| std::chrono::duration<double>(now - m_LastTreeWalk).count() >= kTreeRescanSeconds)
		{
			RefreshIndex();
		}
	}

	void ContentBrowserPanel::WalkDir(FolderNode& node, const std::filesystem::path& absolute,
		const std::filesystem::path& relative)
	{
		node.Relative = relative;

		std::error_code ec;
		auto it = std::filesystem::directory_iterator(absolute,
			std::filesystem::directory_options::skip_permission_denied, ec);
		if (ec)
			return;

		std::vector<std::pair<std::string, std::filesystem::path>> dirs;
		for (const auto& end = std::filesystem::directory_iterator(); it != end; it.increment(ec))
		{
			if (ec)
				break;

			const auto& path = it->path();
			if (IsHiddenBrowserEntry(path))
				continue;

			ListingItem item;
			item.Path = path;
			item.Name = path.filename().string();
			item.NameLower = ToLowerAscii(item.Name);
			item.Relative = relative / item.Name;
			item.ParentRelative = relative;
			item.IsDirectory = it->is_directory(ec);
			if (!item.IsDirectory)
				item.Type = AssetTypeFromExtension(path.extension().string());
			m_Index.push_back(std::move(item));

			if (m_Index.back().IsDirectory)
				dirs.emplace_back(m_Index.back().Name, path);
		}

		std::sort(dirs.begin(), dirs.end(),
			[](const auto& a, const auto& b) { return a.first < b.first; });

		node.Children.reserve(dirs.size());
		for (const auto& [name, path] : dirs)
		{
			FolderNode child;
			child.Name = name;
			WalkDir(child, path, relative / name);
			node.Children.push_back(std::move(child));
		}
	}

	void ContentBrowserPanel::RefreshIndex()
	{
		m_Index.clear();
		m_Tree = {};
		m_Tree.Name = m_BaseDirectory.filename().string();
		if (m_Tree.Name.empty())
			m_Tree.Name = "assets";
		WalkDir(m_Tree, m_BaseDirectory, {});
		m_IndexDirty = false;
		m_LastTreeWalk = std::chrono::steady_clock::now();
	}

	void ContentBrowserPanel::CollectVisible()
	{
		m_Visible.clear();

		const bool searching = m_Search[0] != '\0';
		const std::string needle = searching ? ToLowerAscii(m_Search) : std::string{};
		const std::filesystem::path current = CurrentRelative().lexically_normal();

		for (const ListingItem& item : m_Index)
		{
			if (searching)
			{
				if (item.NameLower.find(needle) == std::string::npos)
					continue;
			}
			else if (item.ParentRelative.lexically_normal() != current)
			{
				continue;
			}

			m_Visible.push_back(&item);
		}

		std::sort(m_Visible.begin(), m_Visible.end(),
			[this](const ListingItem* a, const ListingItem* b)
			{
				if (a->IsDirectory != b->IsDirectory)
					return a->IsDirectory;
				if (m_Sort == SortMode::Type && a->Type != b->Type)
					return static_cast<uint16_t>(a->Type) < static_cast<uint16_t>(b->Type);
				return a->Name < b->Name;
			});
	}

	void ContentBrowserPanel::BeginItemDrag(const ListingItem& item)
	{
		if (!ImGui::BeginDragDropSource())
			return;

		const std::filesystem::path relativePath = MakeAssetRelative(item.Path);
		const std::string itemPath = relativePath.string();
		ImGui::SetDragDropPayload("CONTENT_BROWSER_ITEM", itemPath.c_str(), itemPath.size() + 1);
		ImGui::TextUnformatted(item.Name.c_str());
		ImGui::EndDragDropSource();
	}

	void ContentBrowserPanel::DrawItemContextMenu(const ListingItem& item)
	{
		if (item.IsDirectory || !ImGui::BeginPopupContextItem())
			return;

		const std::filesystem::path relativePath = MakeAssetRelative(item.Path);

		if (IsImportableAsset(item.Type))
		{
			if (ImGui::MenuItem("Import"))
			{
				AssetHandle handle = AssetManager::ImportAsset(relativePath);
				if (IsAssetHandleValid(handle))
					GE_CORE_INFO("Imported '{0}'", relativePath.string());
			}
		}

		AssetHandle handle = AssetManager::GetHandle(relativePath);
		if (IsAssetHandleValid(handle))
		{
			if (ImGui::MenuItem("Reload"))
				AssetManager::Reload(handle);

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

	void ContentBrowserPanel::DrawToolbar()
	{
		using EditorUI::EndPanelToolbarRow;
		using EditorUI::IconButton;
		using EditorUI::PanelToolbarRow;
		using EditorUI::SearchField;

		if (PanelToolbarRow("##BrowserTB"))
		{
			if (IconButton(ICON_LC_ARROW_UP_AZ, "Sort"))
				ImGui::OpenPopup("##Sort");
			if (ImGui::BeginPopup("##Sort"))
			{
				if (ImGui::MenuItem("Name", nullptr, m_Sort == SortMode::Name))
					m_Sort = SortMode::Name;
				if (ImGui::MenuItem("Type", nullptr, m_Sort == SortMode::Type))
					m_Sort = SortMode::Type;
				ImGui::EndPopup();
			}

			ImGui::SameLine();
			SearchField("filter", m_Search, sizeof(m_Search), "Search assets...");
		}
		EndPanelToolbarRow();
	}

	void ContentBrowserPanel::DrawBreadcrumb()
	{
		using EditorUI::IconButton;
		using EditorUI::Theme;
		using EditorUI::ToolbarSeparator;

		const EditorUI::EditorTheme& theme = Theme();
		ImGui::PushStyleColor(ImGuiCol_ChildBg, EditorUI::Color(theme.SurfaceSunken));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 0.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 0.0f));
		ImGui::BeginChild("##crumbs", ImVec2(0.0f, kBreadcrumbH), ImGuiChildFlags_AlwaysUseWindowPadding,
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav);

		const float y = (ImGui::GetContentRegionAvail().y - 24.0f) * 0.5f;
		if (y > 0.0f)
			ImGui::SetCursorPosY(ImGui::GetCursorPosY() + y);

		if (IconButton(ICON_LC_ARROW_LEFT, "Back") && !m_Back.empty())
			GoBack();
		ImGui::SameLine();
		if (IconButton(ICON_LC_ARROW_RIGHT, "Forward") && !m_Forward.empty())
			GoForward();
		ImGui::SameLine();
		if (IconButton(ICON_LC_ARROW_UP, "Parent folder"))
			GoUp();
		ImGui::SameLine();
		ToolbarSeparator();
		ImGui::SameLine();
		if (IconButton(ICON_LC_HOME, "Asset root"))
			TryNavigate(m_BaseDirectory, true);

		std::filesystem::path acc;
		for (const auto& part : CurrentRelative())
		{
			acc /= part;
			ImGui::SameLine();
			ImGui::TextColored(EditorUI::Color(theme.TextDim), "/");
			ImGui::SameLine();

			const std::string label = part.string();
			ImGui::PushID(acc.generic_string().c_str());
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorUI::Color(EditorUI::WithAlpha(theme.Accent, 0.15f)));
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, EditorUI::Color(EditorUI::WithAlpha(theme.Accent, 0.30f)));
			ImGui::PushStyleColor(ImGuiCol_Text, EditorUI::Color(theme.TextPrimary));
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 2.0f));
			if (ImGui::Button(label.c_str()))
				TryNavigate(m_BaseDirectory / acc, true);
			ImGui::PopStyleVar();
			ImGui::PopStyleColor(4);
			ImGui::PopID();
		}

		ImGui::EndChild();
		ImGui::PopStyleVar(2);
		ImGui::PopStyleColor();
	}

	void ContentBrowserPanel::DrawFolderNode(const FolderNode& node)
	{
		using EditorUI::Theme;
		const EditorUI::EditorTheme& theme = Theme();

		const std::filesystem::path currentRel = CurrentRelative();
		const bool selected = node.Relative == currentRel;
		const bool isRoot = node.Relative.empty();

		ImGui::PushID(isRoot ? "##root" : node.Relative.generic_string().c_str());

		ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow
			| ImGuiTreeNodeFlags_SpanAvailWidth
			| ImGuiTreeNodeFlags_FramePadding;
		if (selected)
			flags |= ImGuiTreeNodeFlags_Selected;
		if (node.Children.empty())
			flags |= ImGuiTreeNodeFlags_Leaf;
		if (isRoot)
			ImGui::SetNextItemOpen(true, ImGuiCond_Once);

		// Theme Header is ChromeBg (inspector CollapsingHeaders). Idle selected
		// TreeNodes use Header, not HeaderActive — without this override, TextOnAccent
		// (#1A1A1A) lands on ChromeBg (#1A1A1A).
		if (selected)
		{
			ImGui::PushStyleColor(ImGuiCol_Header, EditorUI::Color(theme.Accent));
			ImGui::PushStyleColor(ImGuiCol_HeaderHovered, EditorUI::Color(theme.AccentHover));
			ImGui::PushStyleColor(ImGuiCol_HeaderActive, EditorUI::Color(theme.AccentActive));
		}
		// "%s" with an empty argument rather than an empty format: the blank label is
		// deliberate - the row draws its own icon and text - but a zero-length format string is
		// a GCC warning (-Wformat-zero-length).
		const bool open = ImGui::TreeNodeEx("##folder", flags, "%s", "");
		if (selected)
			ImGui::PopStyleColor(3);
		const bool clicked = ImGui::IsItemClicked();
		const ImVec2 rmin = ImGui::GetItemRectMin();
		const ImVec2 rmax = ImGui::GetItemRectMax();

		const char* icon = selected ? ICON_LC_FOLDER_OPEN : ICON_LC_FOLDER;
		const ImU32 iconTint = selected ? theme.TextOnAccent : theme.TextPrimary;
		const ImU32 nameCol = selected ? theme.TextOnAccent : theme.TextPrimary;
		const float textY = rmin.y + (rmax.y - rmin.y - ImGui::GetFontSize()) * 0.5f;
		const float labelX = rmin.x + ImGui::GetTreeNodeToLabelSpacing();
		ImDrawList* draw = ImGui::GetWindowDrawList();
		draw->AddText(ImVec2(labelX, textY), iconTint, icon);
		draw->AddText(ImVec2(labelX + 18.0f, textY), nameCol, node.Name.c_str());

		if (clicked && ImGui::GetMousePos().x >= rmin.x + ImGui::GetTreeNodeToLabelSpacing())
			TryNavigate(m_BaseDirectory / node.Relative, true);

		if (open)
		{
			for (const FolderNode& child : node.Children)
				DrawFolderNode(child);
			ImGui::TreePop();
		}

		ImGui::PopID();
	}

	void ContentBrowserPanel::DrawFolderTree()
	{
		ImGui::BeginChild("##folders", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
		DrawAssetTreeContextMenu();
		DrawFolderNode(m_Tree);
		ImGui::EndChild();
	}

	void ContentBrowserPanel::DrawGrid()
	{
		using EditorUI::Theme;
		const EditorUI::EditorTheme& theme = Theme();
		const bool searching = m_Search[0] != '\0';

		const float cell = kThumbnail + kCellPad;
		int columns = (int)(ImGui::GetContentRegionAvail().x / cell);
		if (columns < 1)
			columns = 1;

		if (!ImGui::BeginTable("##grid", columns,
			ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings))
			return;

		for (const ListingItem* item : m_Visible)
		{
			ImGui::TableNextColumn();
			ImGui::PushID(item->Relative.generic_string().c_str());

			const bool selected = m_Selected == item->Path;
			const ImVec2 p0 = ImGui::GetCursorScreenPos();
			ImGui::InvisibleButton("##cell", ImVec2(kThumbnail, kThumbnail + 32.0f));
			const bool hovered = ImGui::IsItemHovered();
			const ImVec2 rmin = ImGui::GetItemRectMin();
			const ImVec2 rmax = ImGui::GetItemRectMax();

			if (selected || hovered)
			{
				const ImU32 fill = selected ? theme.Accent : theme.SurfaceSunken;
				ImGui::GetWindowDrawList()->AddRectFilled(rmin, rmax, fill);
			}

			if (ImGui::IsItemClicked())
				m_Selected = item->Path;

			if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && item->IsDirectory)
				TryNavigate(item->Path, true);

			if (searching)
				ImGui::SetItemTooltip("%s", item->Relative.generic_string().c_str());

			BeginItemDrag(*item);
			DrawItemContextMenu(*item);

			Ref<Texture2D> icon = item->IsDirectory ? m_DirectoryIcon : m_FileIcon;
			const ImU32 tint = ImGui::ColorConvertFloat4ToU32(
				GetAssetIconTint(item->Type, item->IsDirectory));
			ImDrawList* draw = ImGui::GetWindowDrawList();
			draw->AddImage((ImTextureID)(uintptr_t)icon->GetRendererID(),
				p0, ImVec2(p0.x + kThumbnail, p0.y + kThumbnail),
				ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), tint);

			draw->PushClipRect(p0, rmax, true);
			draw->AddText(ImVec2(p0.x, p0.y + kThumbnail + 2.0f),
				selected ? theme.TextOnAccent : theme.TextPrimary, item->Name.c_str());
			draw->PopClipRect();

			ImGui::PopID();
		}

		ImGui::EndTable();
	}

	void ContentBrowserPanel::DrawList()
	{
		using EditorUI::Theme;
		const EditorUI::EditorTheme& theme = Theme();
		const float rowH = theme.RowHeight;
		const bool searching = m_Search[0] != '\0';

		if (!ImGui::BeginTable("##list", 1,
			ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings))
			return;

		ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch);

		for (const ListingItem* item : m_Visible)
		{
			ImGui::TableNextRow(ImGuiTableRowFlags_None, rowH);
			ImGui::TableNextColumn();
			ImGui::PushID(item->Relative.generic_string().c_str());

			const bool selected = m_Selected == item->Path;
			if (selected)
			{
				ImGui::PushStyleColor(ImGuiCol_Header, EditorUI::Color(theme.Accent));
				ImGui::PushStyleColor(ImGuiCol_HeaderHovered, EditorUI::Color(theme.AccentHover));
				ImGui::PushStyleColor(ImGuiCol_HeaderActive, EditorUI::Color(theme.AccentActive));
			}
			ImGui::Selectable("##row", selected,
				ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
				ImVec2(0.0f, rowH));
			if (selected)
				ImGui::PopStyleColor(3);

			if (ImGui::IsItemClicked())
				m_Selected = item->Path;
			if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)
				&& item->IsDirectory)
				TryNavigate(item->Path, true);

			BeginItemDrag(*item);
			DrawItemContextMenu(*item);

			const char* typeIcon = ICON_LC_FILE;
			ImU32 typeTint = theme.TextPrimary;
			FileTypeChrome(item->IsDirectory, item->Type, typeIcon, typeTint);
			if (item->IsDirectory && m_Selected == item->Path)
				typeIcon = ICON_LC_FOLDER_OPEN;

			const ImVec2 rmin = ImGui::GetItemRectMin();
			const ImVec2 rmax = ImGui::GetItemRectMax();
			const float textY = rmin.y + (rowH - ImGui::GetFontSize()) * 0.5f;
			ImDrawList* draw = ImGui::GetWindowDrawList();
			draw->PushClipRect(rmin, rmax, true);
			draw->AddText(ImVec2(rmin.x + 6.0f, textY), typeTint, typeIcon);
			draw->AddText(ImVec2(rmin.x + 26.0f, textY),
				selected ? theme.TextOnAccent : theme.TextPrimary, item->Name.c_str());
			if (searching && !item->ParentRelative.empty())
			{
				const ImVec2 ns = ImGui::CalcTextSize(item->Name.c_str());
				const std::string parent = item->ParentRelative.generic_string();
				draw->AddText(ImVec2(rmin.x + 26.0f + ns.x + 8.0f, textY),
					theme.TextDim, parent.c_str());
			}
			draw->PopClipRect();

			ImGui::PopID();
		}

		ImGui::EndTable();
	}

	void ContentBrowserPanel::DrawFilePane()
	{
		ImGui::BeginChild("##files", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
		DrawAssetTreeContextMenu();

		if (m_View == ViewMode::Grid)
			DrawGrid();
		else
			DrawList();

		if (ImGui::IsMouseDown(0) && ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered())
			m_Selected.clear();

		ImGui::EndChild();
	}

	void ContentBrowserPanel::DrawFooter()
	{
		using EditorUI::IconButton;
		using EditorUI::Theme;

		const EditorUI::EditorTheme& theme = Theme();
		ImGui::PushStyleColor(ImGuiCol_ChildBg, EditorUI::Color(theme.SurfaceBg));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 0.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 0.0f));
		ImGui::BeginChild("##footer", ImVec2(0.0f, kFooterH), ImGuiChildFlags_AlwaysUseWindowPadding,
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav);

		const ImVec2 wp = ImGui::GetWindowPos();
		ImGui::GetWindowDrawList()->AddLine(
			ImVec2(wp.x, wp.y), ImVec2(wp.x + ImGui::GetWindowSize().x, wp.y), theme.Border);

		const float y = (ImGui::GetContentRegionAvail().y - 24.0f) * 0.5f;
		if (y > 0.0f)
			ImGui::SetCursorPosY(ImGui::GetCursorPosY() + y);

		ImGui::Text("%d items", (int)m_Visible.size());

		ImGui::SameLine();
		const float right = ImGui::GetWindowContentRegionMax().x;
		ImGui::SetCursorPosX(right - (24.0f * 2.0f + 4.0f));
		if (IconButton(ICON_LC_LAYOUT_GRID, "Grid view", m_View == ViewMode::Grid))
			m_View = ViewMode::Grid;
		ImGui::SameLine();
		if (IconButton(ICON_LC_LIST, "List view", m_View == ViewMode::List))
			m_View = ViewMode::List;

		ImGui::EndChild();
		ImGui::PopStyleVar(2);
		ImGui::PopStyleColor();
	}

	void ContentBrowserPanel::OnImGuiRender()
	{
		using EditorUI::BeginPanel;
		using EditorUI::EndPanel;

		RefreshCaches();

		if (BeginPanel("Content Browser"))
		{
			DrawAssetTreeContextMenu();
			DrawToolbar();
			CollectVisible();
			DrawBreadcrumb();

			const float bodyH = ImGui::GetContentRegionAvail().y - kFooterH;
			if (ImGui::BeginTable("##AssetSplit", 2,
				ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV
				| ImGuiTableFlags_SizingStretchProp,
				ImVec2(0.0f, bodyH > 0.0f ? bodyH : 0.0f)))
			{
				ImGui::TableSetupColumn("folders", ImGuiTableColumnFlags_WidthFixed, kSidebarDefault);
				ImGui::TableSetupColumn("files", ImGuiTableColumnFlags_WidthStretch);
				ImGui::TableNextColumn();
				DrawFolderTree();
				ImGui::TableNextColumn();
				DrawFilePane();
				ImGui::EndTable();
			}

			DrawFooter();
		}
		EndPanel();
	}
}
