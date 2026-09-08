#pragma once

#include "GanymedE/Assets/AssetManagerRegistry.h"
#include "GanymedE/Assets/AssetTypes.h"
#include "GanymedE/Core/Core.h"

#include <cstddef>
#include <filesystem>
#include <type_traits>
#include <vector>

namespace GanymedE {

	class Environment;
	class Material;
	class Mesh;
	class Texture2D;

	// "Does this type have an asset manager?" False by default, specialized below for the ones
	// that do.
	//
	// This trait is the whole reason it survives: `GetAsset<T>` used to be a primary template
	// whose body was `static_assert(sizeof(T) == 0)`, which gave a compile error naming the
	// supported types instead of an unresolved external at link time. Forwarding to a registry
	// would have regressed that to a runtime assert, so the check moved into a trait the
	// registration list keeps honest.
	template<typename T>
	struct IsAssetType : std::false_type {};

	// One line per managed type, beside the forward declarations, next to the `Register<T>` call
	// in `AssetManager.cpp` that has to match it. A type declared here but never registered is a
	// runtime assert on first use; a type registered but not declared here will not compile at
	// the call site, which is the failure that gets noticed.
	#define GE_ASSET_TYPE(T) template<> struct IsAssetType<T> : std::true_type {}

	GE_ASSET_TYPE(Mesh);
	GE_ASSET_TYPE(Environment);
	GE_ASSET_TYPE(Texture2D);
	GE_ASSET_TYPE(Material);

	#undef GE_ASSET_TYPE

	// One row per registered manager, for the editor's Stats panel. Type-erased here so the
	// editor never has to see `IAssetManager`.
	struct AssetCacheStats
	{
		const char* TypeName = nullptr;
		std::size_t Resident = 0;  // live objects the manager is tracking
		std::size_t Retained = 0;  // of those, ones the manager itself is keeping alive
	};

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

		// Cached load through the type's manager: cache hit, or Parse then Apply. Every new
		// asset type adds one `GE_ASSET_TYPE` line above and one `Register<T>` call in the
		// `.cpp` - there is no per-type code here any more.
		//
		// Not every asset type wants one. Script, Audio and Prefab are path-resolved by
		// design: the manager answers handle -> path and the consumer loads itself
		// (Lua owns its chunks, miniaudio's resource manager owns decoded audio).
		// A cache here would be a second ref-counted owner of the same resource -
		// see docs/engine/audio.md.
		template<typename T>
		static Ref<T> GetAsset(AssetHandle handle)
		{
			static_assert(IsAssetType<T>::value,
				"AssetManager::GetAsset<T> is only available for Mesh, Environment, Texture2D "
				"and Material. Script, Audio and Prefab are path-resolved by design - resolve "
				"them through GetMetadata (docs/engine/assets.md).");

			return AssetManagerRegistry::Get<T>().Load(handle);
		}

		// Evict a loaded asset so the next GetAsset re-reads it from disk. For a mesh
		// this also drops its textures and the .meshcache file - "reimport now".
		// Safe to call mid-frame from editor UI; see docs/engine/assets.md for why.
		static void Reload(AssetHandle handle);

		// One row per registered manager. Editor-facing; nothing in the engine reads it.
		static std::vector<AssetCacheStats> GetCacheStats();

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

		// Fills the manager slots. One call per managed type, and the list has to agree with
		// the GE_ASSET_TYPE declarations above.
		static void RegisterManagers();
	};

}
