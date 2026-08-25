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
		// writableRegistry false makes every registry write a no-op: Shutdown() skips its
		// save, and so does ImportAsset. A shipped game must not write into its own
		// install directory - under Program Files that fails outright - and it has nothing
		// to persist anyway, because for a runtime the registry is shipped content rather
		// than a scanned cache (docs/engine/assets.md, "Registry portability").
		//
		// Handles minted by ImportAsset still work for the session; they just do not
		// outlive it, which is the correct lifetime for something nobody authored.
		static void Init(bool writableRegistry = true);
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
		//
		// Not every asset type wants one. Script and Audio are path-resolved by
		// design: the manager answers handle -> path and the consumer loads itself
		// (Lua owns its chunks, miniaudio's resource manager owns decoded audio).
		// A cache here would be a second ref-counted owner of the same resource -
		// see docs/engine/audio.md.
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

		// Writes the registry only if an ImportAsset has dirtied it since the last write.
		// ImportAsset no longer saves on every call - a scene load or a mesh drop mints a
		// handful of handles and used to rewrite the whole file once per handle. Flush at
		// the end of a user-visible action instead (drop handled, import menu clicked,
		// scene deserialized) so a crash mid-session costs at most the current action's
		// imports, not the afternoon's - see docs/engine/assets.md, "Registry flush points".
		static void FlushRegistry();

		// "This process may write into assets/." False in the shipped runtime, which treats
		// assets/ as read-only content. The registry guard and every future asset-file
		// writer share this one flag rather than each inventing a parallel one.
		static bool IsRegistryWritable();

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
