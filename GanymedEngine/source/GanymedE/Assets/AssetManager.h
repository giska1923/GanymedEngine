#pragma once

#include "GanymedE/Assets/AssetTypes.h"
#include "GanymedE/Core/Core.h"

#include <filesystem>

namespace GanymedE {

	class Environment;
	class Mesh;
	class Texture2D;

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

		// The primary template is defined (not just declared) so an unsupported T is a
		// compile error with a message, instead of linking fine and failing later with
		// an unresolved-external. Every new asset type adds one specialization
		// declaration below, one definition in the .cpp, and one cache map.
		template<typename T>
		static Ref<T> GetAsset(AssetHandle handle)
		{
			static_assert(sizeof(T) == 0,
				"AssetManager::GetAsset<T> is only specialized for Mesh, Environment, Texture2D");
			return nullptr;
		}

		static void LoadRegistry();
		static void SaveRegistry();

	private:
		static Ref<Mesh> LoadMesh(AssetHandle handle);
		static Ref<Environment> LoadEnvironment(AssetHandle handle);
		static Ref<Texture2D> LoadTexture(AssetHandle handle);
	};

	// Declared here rather than only defined in the .cpp: a specialization must be
	// visible before the first use that would otherwise implicitly instantiate the
	// primary template.
	template<> Ref<Mesh>        AssetManager::GetAsset<Mesh>(AssetHandle handle);
	template<> Ref<Environment> AssetManager::GetAsset<Environment>(AssetHandle handle);
	template<> Ref<Texture2D>   AssetManager::GetAsset<Texture2D>(AssetHandle handle);

}
