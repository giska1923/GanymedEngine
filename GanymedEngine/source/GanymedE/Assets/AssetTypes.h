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
		Scene
	};

	inline bool IsAssetHandleValid(AssetHandle handle)
	{
		return static_cast<uint64_t>(handle) != 0;
	}

	AssetType AssetTypeFromExtension(const std::string& extension);
	const char* AssetTypeToString(AssetType type);

	struct AssetMetadata
	{
		AssetHandle Handle = InvalidAssetHandle;
		AssetType Type = AssetType::None;
		std::string FilePath; // relative to assets/
	};

}
