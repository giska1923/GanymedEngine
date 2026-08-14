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
		// saveRegistryOnShutdown false makes Shutdown() skip the registry write. A shipped
		// game must not write into its own install directory on exit - under Program Files
		// that fails outright - and it has nothing to persist anyway: the registry it
		// loaded is the one it shipped with.
		static void Init(bool saveRegistryOnShutdown = true);
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

		// Evict a loaded asset so the next GetAsset re-reads it from disk. For a mesh
		// this also drops its textures and the .meshcache file - "reimport now".
		// Safe to call mid-frame from editor UI; see docs/engine/assets.md for why.
		static void Reload(AssetHandle handle);

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
