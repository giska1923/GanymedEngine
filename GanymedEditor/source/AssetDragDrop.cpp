#include "AssetDragDrop.h"

#include "GanymedE/Assets/AssetManager.h"

#include <imgui/imgui.h>

#include <algorithm>

namespace GanymedE::EditorUI {

	namespace {

		// The payload is the item path relative to assets/, NUL-terminated
		// (ContentBrowserPanel's drag source).
		std::optional<std::filesystem::path> AcceptContentBrowserPayload()
		{
			if (!ImGui::BeginDragDropTarget())
				return std::nullopt;

			std::optional<std::filesystem::path> result;
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
				result = std::filesystem::path((const char*)payload->Data);

			ImGui::EndDragDropTarget();
			return result;
		}

	}

	std::optional<std::filesystem::path> AcceptAssetDrop(AssetType type)
	{
		auto dropped = AcceptContentBrowserPayload();
		if (!dropped)
			return std::nullopt;

		if (AssetTypeFromExtension(dropped->extension().string()) != type)
			return std::nullopt;

		return dropped;
	}

	AssetDrop AcceptAssetDrop(std::initializer_list<AssetType> types)
	{
		auto dropped = AcceptContentBrowserPayload();
		if (!dropped)
			return {};

		AssetType droppedType = AssetTypeFromExtension(dropped->extension().string());
		if (std::find(types.begin(), types.end(), droppedType) == types.end())
			return {};

		return { droppedType, *dropped };
	}

	AssetHandle AcceptAssetDropHandle(AssetType type)
	{
		auto dropped = AcceptAssetDrop(type);
		if (!dropped)
			return InvalidAssetHandle;

		// ImportAsset persists identity itself now - one `.meta` sidecar beside the dropped
		// file, not a rewrite of a shared registry - so there is nothing to flush.
		return AssetManager::ImportAsset(*dropped);
	}

}
