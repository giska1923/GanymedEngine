#pragma once

#include "GanymedE/Assets/AssetManagerRegistry.h"
#include "GanymedE/Assets/AssetTypes.h"
#include "GanymedE/Core/Core.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <vector>

namespace GanymedE {

	// One row per registered manager, for the editor's Stats panel. Type-erased here so the
	// editor never has to see `IAssetManager`.
	struct AssetCacheStats
	{
		const char* TypeName = nullptr;
		std::size_t Resident = 0;  // live objects
		std::size_t Tracked = 0;   // cache entries, including ones whose object was collected
		std::size_t Pending = 0;   // parses in flight
	};

	// What the last frame's main-thread Apply pass cost, and what it had to leave for later.
	// Editor-facing; nothing in the engine reads it.
	struct AssetApplyStats
	{
		uint32_t Applied = 0;    // assets that became GPU resources last frame
		uint32_t Deferred = 0;   // finished parsing but over budget - they land on a later frame
		double Milliseconds = 0.0;
		double BudgetMs = 0.0;
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

		// `.meta` sidecars the last scan found with no asset beside them, and the action that
		// deletes them. Split in two because the detection is safe and the delete is not: a
		// sidecar holds the handle every scene uses to name that asset, so reaping one whose
		// file is only *temporarily* absent - an incomplete checkout, a branch without it -
		// permanently breaks every reference. Detection therefore runs at every scan and only
		// warns; the delete is a person's decision. CleanOrphanedMeta re-checks each file
		// before removing it and returns how many it took.
		static std::size_t OrphanedMetaCount();
		static std::size_t CleanOrphanedMeta();

		// "Ensure this file has a sidecar, and tell me its handle" (idempotent). Returns
		// InvalidAssetHandle for an unsupported extension. This is the entry point for a file
		// created *after* the scan - an extracted `.glb` texture, a generated `.gmat`.
		static AssetHandle ImportAsset(const std::filesystem::path& relativePath);

		static AssetHandle GetHandle(const std::filesystem::path& relativePath);
		static const AssetMetadata* GetMetadata(AssetHandle handle);
		static AssetType GetAssetType(AssetHandle handle);

		// Every indexed asset, in unspecified order. For AssetWatcher, which needs a handle and
		// a path per row every quarter second and has no business keeping its own copy of the
		// index. Do not mutate the index from inside `fn`.
		static void ForEachAsset(const std::function<void(const AssetMetadata&)>& fn);

		// Cached load through the type's manager: cache hit, or Parse then Apply. Every new
		// asset type adds one `GE_ASSET_TYPE` line in AssetManagerRegistry.h and one
		// `Register<T>` call in the `.cpp` - there is no per-type code here any more.
		//
		// Prefer an `AssetRef<T>` member over calling this per frame: it resolves once and
		// caches, which is the whole point of AssetRef.h. This stays for one-shot lookups and
		// for editor code that has a handle and no place to keep a reference.
		//
		// Not every asset type wants one. Script, Audio and Prefab are path-resolved by
		// design: the manager answers handle -> path and the consumer loads itself
		// (Lua owns its chunks, miniaudio's resource manager owns decoded audio).
		// A cache here would be a second ref-counted owner of the same resource -
		// see docs/engine/audio.md.
		template<typename T>
		static Ref<T> GetAsset(AssetHandle handle)
		{
			// The diagnostic lives in AssetManagerRegistry::Get<T>, so it reads the same
			// whether a caller went through this facade or through an AssetRef.
			return AssetManagerRegistry::Get<T>().Load(handle);
		}

		// Apply what finished parsing since the last call, up to a per-frame time budget. **Main
		// thread**, once per frame, from Application::Run - this is the only place the async path
		// creates GPU resources, which is what keeps bgfx on the thread that owns it.
		//
		// Bounded because Apply cannot leave the main thread and bursts are easy to cause: a hot
		// reload across many files, or a cold open of a large scene, would otherwise turn every
		// finished parse into GPU resources in one frame. Anything that does not fit stays
		// pending and lands on a later frame - see AssetApplyStats and docs/engine/assets.md.
		static void Update();

		static AssetApplyStats GetApplyStats();

		// Block until this asset is loaded and applied. Pumps other queued jobs while it waits,
		// so it cannot deadlock the pool.
		//
		// **Keep the call sites countable.** There is one: MeshImporter::Instantiate, which has
		// to enumerate a mesh's material slots to fill the new entity's overrides and therefore
		// cannot proceed with "not yet". Everything else on a frame path should tolerate a null
		// asset for a frame or two - that is the whole point of the phase.
		static void WaitFor(AssetHandle handle);

		// Assets whose parse is in flight, across every manager. Editor status, not a poll.
		static std::size_t PendingCount();

		// Evict a loaded asset so the next GetAsset re-reads it from disk. For a mesh
		// this also drops its textures and its compiled blob - "reimport now".
		// Safe to call mid-frame from editor UI; see docs/engine/assets.md for why.
		static void Reload(AssetHandle handle);

		// "This file changed on disk." The hot-reload entry point, called from AssetWatcher.
		// Returns false when there was nothing to do - a type with no manager, or a handle the
		// index does not know.
		//
		// **Narrower than Reload, in both directions, and both differences are deliberate.**
		// Reload means *reimport now*: it deletes the compiled output and reaches down into an
		// asset's own textures, because a person pressed a button that says so. This is a file
		// event, so it evicts what actually changed plus whatever captured it, and leaves the
		// compiled output to the epoch record - which is the machinery that already tells a real
		// edit apart from an editor rewriting identical bytes. Forcing a recompile here would
		// turn every save-to-temp-and-rename into a BC7 encode.
		static bool OnAssetModified(AssetHandle handle);

		// One row per registered manager. Editor-facing; nothing in the engine reads it.
		static std::vector<AssetCacheStats> GetCacheStats();

		// "This process may write into assets/." False in the shipped runtime, which treats
		// assets/ as read-only content. The sidecar writer and every other asset-file writer
		// share this one flag rather than each inventing a parallel one.
		//
		// Named IsRegistryWritable until it was renamed: it never gated the registry. The
		// registry it referred to is the legacy assets/AssetRegistry.gr, which is read once for
		// migration and never written at all.
		static bool IsAssetsWritable();

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
