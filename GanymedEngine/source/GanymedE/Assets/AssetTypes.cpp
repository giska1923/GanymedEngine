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
		if (ext == ".gprefab")
			return AssetType::Prefab;
		if (ext == ".lua")
			return AssetType::Script;
		// miniaudio's built-in decoders. .ogg is deliberately absent - Vorbis needs
		// stb_vorbis vendored and wired into the decoding backend.
		if (ext == ".wav" || ext == ".mp3" || ext == ".flac")
			return AssetType::Audio;

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
			case AssetType::Script:       return "Script";
			case AssetType::Audio:        return "Audio";
			case AssetType::Prefab:       return "Prefab";
			default:                      return "None";
		}
	}

	// Spelled out rather than derived from AssetTypeToString, so appending an enum value
	// without a name here is a missing case in one obvious place instead of a name that
	// round-trips one way only.
	AssetType AssetTypeFromString(const std::string& name)
	{
		if (name == "StaticMesh")   return AssetType::StaticMesh;
		if (name == "Environment")  return AssetType::Environment;
		if (name == "Texture")      return AssetType::Texture;
		if (name == "Material")     return AssetType::Material;
		if (name == "Scene")        return AssetType::Scene;
		if (name == "Script")       return AssetType::Script;
		if (name == "Audio")        return AssetType::Audio;
		if (name == "Prefab")       return AssetType::Prefab;

		return AssetType::None;
	}

}
