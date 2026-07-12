#pragma once

#include "GanymedE/Assets/AssetTypes.h"
#include "GanymedE/Core/Core.h"

#include <filesystem>

namespace GanymedE {

	class Environment;
	class Mesh;

	class AssetManager
	{
	public:
		static void Init();
		static void Shutdown();

		// Register an asset by relative path (idempotent). Returns InvalidAssetHandle if unsupported.
		static AssetHandle ImportAsset(const std::filesystem::path& relativePath);

		static AssetHandle GetHandle(const std::filesystem::path& relativePath);
		static const AssetMetadata* GetMetadata(AssetHandle handle);
		static AssetType GetAssetType(AssetHandle handle);

		template<typename T>
		static Ref<T> GetAsset(AssetHandle handle);

		static void LoadRegistry();
		static void SaveRegistry();

	private:
		static Ref<Mesh> LoadMesh(AssetHandle handle);
		static Ref<Environment> LoadEnvironment(AssetHandle handle);
	};

}
