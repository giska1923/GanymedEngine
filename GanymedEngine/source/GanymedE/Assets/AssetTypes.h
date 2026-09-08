#pragma once

#include "GanymedE/Core/UUID.h"

#include <cstdint>
#include <string>

namespace GanymedE {

	using AssetHandle = UUID;

	inline const AssetHandle InvalidAssetHandle = AssetHandle(0);

	enum class AssetType : uint16_t
	{
		None = 0,
		StaticMesh,
		Environment,
		Texture,
		Material,
		Scene,
		Script,
		// Append only: the legacy AssetRegistry.gr persists this enum by ordinal, so
		// reordering it silently retypes every asset in every registry still on disk.
		// The `.meta` sidecar that replaced it stores the *name* instead, and so does not
		// inherit the constraint - see AssetMeta.h.
		Audio,
		Prefab
	};

	inline bool IsAssetHandleValid(AssetHandle handle)
	{
		return static_cast<uint64_t>(handle) != 0;
	}

	AssetType AssetTypeFromExtension(const std::string& extension);
	const char* AssetTypeToString(AssetType type);

	// Inverse of AssetTypeToString, for the by-name `Type` in a `.meta` sidecar. Returns
	// None for an unrecognized name - which a sidecar written by a *newer* engine
	// legitimately produces, so callers treat it as "re-derive from the extension" rather
	// than as corruption.
	AssetType AssetTypeFromString(const std::string& name);

	struct AssetMetadata
	{
		AssetHandle Handle = InvalidAssetHandle;
		AssetType Type = AssetType::None;
		std::string FilePath; // relative to assets/
	};

}
