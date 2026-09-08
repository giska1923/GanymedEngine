#pragma once

#include "GanymedE/Assets/AssetTypes.h"
#include "GanymedE/Core/Core.h"

#include <filesystem>

namespace GanymedE {

	class Environment;
	class Material;
	class Mesh;
	class Texture2D;

	class AssetManager
	{
	public:
		// writableAssets false makes every write into assets/ a no-op: no `.meta` sidecar is
		// created, refreshed or quarantined. A shipped game must not write into its own
		// install directory - under Program Files that fails outright - and it has nothing to
		// persist anyway, because its sidecars are shipped content rather than a scan product
		// (docs/engine/assets.md, "Identity portability").
		//
		// Handles minted for an asset with no sidecar still work for the session; they just do
		// not outlive it, which is the correct lifetime for something nobody authored.
		static void Init(bool writableAssets = true);
		static void Shutdown();

		// Rebuild the in-memory index by walking assets/: read each asset's `.meta` sidecar,
		// or mint a handle and write one. Called by Init; safe to call again to pick up files
		// added outside the editor, which keeps every identity it already knows and only adds.
		// Entries whose file has since vanished are *not* dropped - a handle a scene still
		// references is more useful stale than absent.
		static void ScanAssets();

		// "Ensure this file has a sidecar, and tell me its handle" (idempotent). Returns
		// InvalidAssetHandle for an unsupported extension. This is the entry point for a file
		// created *after* the scan - an extracted `.glb` texture, a generated `.gmat`.
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
				"AssetManager::GetAsset<T> is only specialized for Mesh, Environment, Texture2D, Material");
			return nullptr;
		}

		// Evict a loaded asset so the next GetAsset re-reads it from disk. For a mesh
		// this also drops its textures and the .meshcache file - "reimport now".
		// Safe to call mid-frame from editor UI; see docs/engine/assets.md for why.
		static void Reload(AssetHandle handle);

		// "This process may write into assets/." False in the shipped runtime, which treats
		// assets/ as read-only content. The sidecar writer and every other asset-file writer
		// share this one flag rather than each inventing a parallel one.
		static bool IsRegistryWritable();

	private:
		// Reads the legacy assets/AssetRegistry.gr, if it still exists, into a path -> handle
		// seed consulted by the scan and by ImportAsset for the rest of the session. That is
		// what makes migration lossless: an asset adopts the handle it already had instead of
		// minting a new one, so no `.ganymede`, `.gprefab` or `.gmat` breaks. The registry is
		// never written again and is redundant once the sidecars exist; deleting it is the
		// user's call. Read once per session, at Init.
		static void LoadLegacyRegistry();

		static Ref<Mesh> LoadMesh(AssetHandle handle);
		static Ref<Environment> LoadEnvironment(AssetHandle handle);
		static Ref<Texture2D> LoadTexture(AssetHandle handle);
		static Ref<Material> LoadMaterial(AssetHandle handle);
	};

	// Declared here rather than only defined in the .cpp: a specialization must be
	// visible before the first use that would otherwise implicitly instantiate the
	// primary template.
	template<> Ref<Mesh>        AssetManager::GetAsset<Mesh>(AssetHandle handle);
	template<> Ref<Environment> AssetManager::GetAsset<Environment>(AssetHandle handle);
	template<> Ref<Texture2D>   AssetManager::GetAsset<Texture2D>(AssetHandle handle);
	template<> Ref<Material>    AssetManager::GetAsset<Material>(AssetHandle handle);

}
