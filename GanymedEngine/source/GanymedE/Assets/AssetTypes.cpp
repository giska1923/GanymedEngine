#include "gepch.h"
#include "GanymedE/Assets/AssetTypes.h"

#include <algorithm>
#include <cctype>

namespace GanymedE {

	AssetType AssetTypeFromExtension(const std::string& extension)
	{
		std::string ext = extension;
		std::transform(ext.begin(), ext.end(), ext.begin(),
			[](unsigned char c) { return (char)std::tolower(c); });

		if (ext == ".gltf" || ext == ".glb")
			return AssetType::StaticMesh;
		if (ext == ".hdr")
			return AssetType::Environment;
		if (ext == ".png" || ext == ".jpg" || ext == ".jpeg")
			return AssetType::Texture;
		if (ext == ".gmat")
			return AssetType::Material;
		if (ext == ".ganymede")
			return AssetType::Scene;

		return AssetType::None;
	}

	const char* AssetTypeToString(AssetType type)
	{
		switch (type)
		{
			case AssetType::StaticMesh:   return "StaticMesh";
			case AssetType::Environment:  return "Environment";
			case AssetType::Texture:      return "Texture";
			case AssetType::Material:     return "Material";
			case AssetType::Scene:        return "Scene";
			default:                      return "None";
		}
	}

}
