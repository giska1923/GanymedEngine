#pragma once

#include "GanymedE/Assets/AssetManagerRegistry.h"
#include "GanymedE/Assets/AssetTypes.h"
#include "GanymedE/Core/Core.h"

#include <cstdint>
#include <utility>

namespace GanymedE {

	// A typed reference to an asset: the handle that identifies it, plus the object once
	// something has asked for it.
	//
	// This is the abstraction the asset milestone exists for. A component used to store a bare
	// `AssetHandle` and every consumer wrote `AssetManager::GetAsset<Mesh>(handle)` - a hash
	// lookup per entity per frame, and a shape that hardcodes "loading is a blocking call that
	// returns the finished object." `AssetRef<T>` moves both decisions behind one type: the
	// resolve happens once and caches, and when loading becomes asynchronous the call sites do
	// not change, because they never said *how* the object arrived.
	//
	// **Composition, not inheritance from `Ref<T>`.** ASSET_PIPELINE_ROADMAP.md decision 6 called
	// for deriving from `Ref<T>` the way BlankEngine's `ResPtr` derives from `shared_ptr`, arguing
	// that the slicing risk is bounded because "`AssetRef` adds no data members, so a slice loses
	// only API, not state." That is true of `ResPtr`, which genuinely has no members because it
	// resolves eagerly at construction. It is not true here: lazy resolution *requires* the
	// handle to be a member, so slicing to `Ref<T>` would silently drop the identity and leave a
	// reference that can never be re-resolved or reloaded - and every mutating `shared_ptr`
	// member (`reset`, `operator=`, `swap`) would be a public way to desync it. The drop-in
	// benefit that justified deriving was worth ~24 call sites; the ones that need a `Ref<T>`
	// now say `.Get()`.
	//
	// **Serializes as its handle**, so every existing `.ganymede` and `.gprefab` is unchanged -
	// see SceneSerializer.
	template<typename T>
	class AssetRef
	{
	public:
		static_assert(IsAssetType<T>::value,
			"AssetRef<T> needs a managed asset type: Mesh, Environment, Texture2D or Material. "
			"Script, Audio and Prefab are path-resolved by design and stay bare AssetHandles - "
			"see docs/engine/assets.md.");

		// Lets generic code recover the asset type from a slot - the editor's particle inspector
		// takes `auto& slot` and needs to know what kind of drop that slot accepts.
		using AssetT = T;

		AssetRef() = default;
		explicit AssetRef(AssetHandle handle) : m_Handle(handle) {}

		AssetHandle Handle() const { return m_Handle; }

		// Re-point at a different asset. The cached object goes with it, or the next Get would
		// hand back the old one.
		void SetHandle(AssetHandle handle)
		{
			m_Handle = handle;
			m_Asset.reset();
		}

		void Reset() { SetHandle(InvalidAssetHandle); }

		// "Is a reference authored here?" Cheap, and it does **not** load - this is the
		// replacement for `IsAssetHandleValid(component.Field)` at a call site that only wants to
		// know whether the slot is filled.
		bool HasHandle() const { return IsAssetHandleValid(m_Handle); }

		// "Is there an object to use?" Resolves. A valid handle whose asset is missing or fails
		// to load answers false, which is the distinction `HasHandle` cannot make.
		bool Ready() const { return Get() != nullptr; }

		// Resolves on first use and caches. Null for an unset handle, and for one whose asset
		// could not be loaded - callers that already null-checked a `GetAsset` result keep the
		// exact same shape.
		const Ref<T>& Get() const
		{
			// Re-resolve after any eviction. Without this, `AssetManager::Reload` would stop
			// reaching the viewport: the old "re-fetch by handle every frame" invariant is
			// precisely what caching the object removes. See Detail::g_AssetEvictionEpoch.
			if (m_Asset && m_Epoch == Detail::g_AssetEvictionEpoch)
				return m_Asset;

			if (IsAssetHandleValid(m_Handle))
			{
				// Resolved into a temporary and only then assigned, which is load-bearing rather
				// than style. The epoch is global, so a Reload of *any* handle sends every live
				// reference back through here; releasing m_Asset first would drop the last
				// reference to an asset this happens to be the sole owner of, the weak cache
				// entry would expire, and one Reload would re-import the whole scene. Holding
				// the old object across the Load means an unaffected handle gets a cache hit and
				// the very same pointer back.
				Ref<T> resolved = AssetManagerRegistry::Get<T>().Load(m_Handle);
				m_Asset = std::move(resolved);
			}
			else
			{
				m_Asset.reset();
			}

			m_Epoch = Detail::g_AssetEvictionEpoch;
			return m_Asset;
		}

		// Convenience for code that has already established the asset is there. Dereferencing an
		// unresolvable reference is a null dereference, exactly as it was when call sites held a
		// `Ref<T>` from `GetAsset`.
		T* operator->() const { return Get().get(); }
		T& operator*() const { return *Get(); }

		// Identity, not object equality: two references to the same asset are equal whether or
		// not either has resolved. Deliberately no `operator bool` - "has a handle" and "has an
		// object" are different questions and a single implicit answer would hide which one a
		// call site meant.
		bool operator==(const AssetRef& other) const { return m_Handle == other.m_Handle; }
		bool operator!=(const AssetRef& other) const { return !(*this == other); }

	private:
		AssetHandle m_Handle = InvalidAssetHandle;

		// Mutable so `Get()` can be const, which is what lets a const component reference resolve
		// - the alternative is a const_cast at every use.
		mutable Ref<T> m_Asset;
		mutable uint32_t m_Epoch = 0;
	};

}
