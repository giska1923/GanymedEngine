#pragma once

#include "GanymedE/Assets/AssetRef.h"
#include "GanymedE/Assets/AssetTypes.h"

#include <filesystem>
#include <initializer_list>
#include <optional>

namespace GanymedE::EditorUI {

	struct AssetDrop
	{
		AssetType Type = AssetType::None;
		std::filesystem::path Path; // relative to assets/

		explicit operator bool() const { return Type != AssetType::None; }
	};

	// Call immediately after the widget that should accept drops. Wraps
	// BeginDragDropTarget / AcceptDragDropPayload("CONTENT_BROWSER_ITEM") /
	// EndDragDropTarget, and makes AssetTypeFromExtension the single source of
	// truth for what a target accepts - the editor used to hand-roll extension
	// string compares at every drop site.
	//
	// A mismatched drop is silently ignored, matching the previous behavior.
	//
	// Multi-type targets MUST use the initializer_list overload rather than
	// calling this twice after the same widget: ImGui::EndDragDropTarget calls
	// ClearDragDrop as soon as a payload is delivered, so a second
	// BeginDragDropTarget in the same frame sees no active drag. Since both calls
	// would share the CONTENT_BROWSER_ITEM payload type and only differ in the
	// extension filter we apply afterwards, the first call always wins the
	// delivery and the second type would never fire.
	std::optional<std::filesystem::path> AcceptAssetDrop(AssetType type);
	AssetDrop AcceptAssetDrop(std::initializer_list<AssetType> types);

	// Convenience for component handle fields: ImportAsset (idempotent) on match.
	AssetHandle AcceptAssetDropHandle(AssetType type);

	// The typed form, for an AssetRef<T> slot. The accepted AssetType comes from AssetTypeOf<T>,
	// so a slot can no longer declare one type and filter on another - which was possible while
	// every call passed the enum by hand next to a differently-typed field. Returns an unset ref
	// when nothing matching was dropped, so the caller tests HasHandle().
	template<typename T>
	AssetRef<T> AcceptAssetDropRef()
	{
		return AssetRef<T>(AcceptAssetDropHandle(AssetTypeOf<T>::value));
	}

}
