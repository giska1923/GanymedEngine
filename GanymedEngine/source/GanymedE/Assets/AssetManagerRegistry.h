#pragma once

#include "GanymedE/Assets/AssetTypes.h"
#include "GanymedE/Core/Core.h"
#include "GanymedE/Core/Log.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <unordered_map>

namespace GanymedE {

	// How a C++ type gets an asset manager, and what a manager is.
	//
	// This replaced four hardcoded `unordered_map<AssetHandle, Ref<T>>` members inside
	// `AssetManagerData` plus a `GetAsset<T>` primary template whose body was a
	// `static_assert`. Adding a cached asset type used to mean editing `AssetManager.cpp` in
	// six places; it now means one `GE_ASSET_TYPE` line in `AssetManager.h` and one
	// `Register<T>` call in `RegisterManagers`.
	//
	// The shape is BlankEngine's `IResourceManager` / `ResourceManager<T, Cache>` with its RTTR
	// registration DSL removed - see docs/toDo&done/ASSET_PIPELINE_ROADMAP.md decision 4 for
	// why a reflected registry is not worth a reflection dependency at this scale.

	class IAssetManager;

	using AssetTypeId = uint8_t;

	// Deliberately tight. There are four managed types today and eight `AssetType` values, and
	// only a type with a *runtime object* ever gets a manager - Script, Audio and Prefab are
	// path-resolved by design. BlankEngine's 64 is sized for an engine an order of magnitude larger.
	inline constexpr AssetTypeId MaxAssetManagers = 16;

	namespace Detail {

		// Bumped once per distinct T, on that T's first `AssetTypeIdOf` call.
		AssetTypeId NextAssetTypeId();

		// `AssetManager::GetMetadata` and its unknown-handle warning, reachable without
		// including `AssetManager.h`. The dependency runs one way on purpose: `GetAsset<T>` is
		// built on the manager template, so the template cannot include the facade back.
		const AssetMetadata* FindAssetMetadata(AssetHandle handle);
		void WarnUnknownAssetHandle(AssetHandle handle, const char* expectedTypeName);

		std::array<Scope<IAssetManager>, MaxAssetManagers>& AssetManagerSlots();

	}

	// A dense id assigned on first use, so resolving a manager is an array index rather than a
	// `std::type_index` hash - `RenderSystem` resolves assets by handle per entity per frame and
	// the lookup sits on that path.
	//
	// The value depends on registration order and is therefore **not stable across builds**. It
	// must never be persisted; `AssetType`, stored by name in a `.meta` sidecar, is the persisted
	// form. Registration at `Init` is what makes the assignment deterministic within a run.
	template<typename T>
	AssetTypeId AssetTypeIdOf()
	{
		static const AssetTypeId id = Detail::NextAssetTypeId();
		return id;
	}

	// The CPU-side intermediate handed from Parse to Apply.
	//
	// Polymorphic rather than a `std::any` or a `void*`: the buffer frees itself if Apply never
	// runs, and a mispaired parse/apply is a `static_cast` a debugger can see through instead of
	// a silently reinterpreted pointer. One heap allocation per cold load, against a file read.
	struct AssetParseResult
	{
		virtual ~AssetParseResult() = default;
	};

	// Everything `AssetManager` needs from a manager without knowing its type.
	class IAssetManager
	{
	public:
		virtual ~IAssetManager() = default;

		virtual const char* TypeName() const = 0;
		virtual AssetType Type() const = 0;

		// Drop this manager's ownership of `handle`. Anything still holding a `Ref` keeps the
		// old object, unchanged - the next `Load` builds a *new* one beside it. That is the
		// documented `Reload` contract ("re-fetch by handle each frame, or accept staleness"),
		// and weak caching does not change it.
		virtual void Evict(AssetHandle handle) = 0;
		virtual void EvictAll() = 0;

		// Live objects this manager is tracking, and the subset it is keeping alive itself.
		virtual std::size_t ResidentCount() const = 0;
		virtual std::size_t RetainedCount() const = 0;
	};

	template<typename T>
	class TypedAssetManager : public IAssetManager
	{
	public:
		// Pure CPU work: file IO, decode, deserialize. **No bgfx call may appear anywhere below
		// this function**, because Phase 5 runs it on a worker thread and bgfx resource creation
		// belongs to the submit thread (roadmap decision 13). Null return means the load failed;
		// a null `ParseFn` means this type has no separable CPU stage yet.
		using ParseFn = Scope<AssetParseResult> (*)(const AssetMetadata&);

		// Main thread only: the intermediate becomes the live object, and every bgfx resource is
		// created here. `parsed` is null exactly when `ParseFn` is.
		using ApplyFn = Ref<T> (*)(const AssetMetadata&, Scope<AssetParseResult>);

		TypedAssetManager(const char* typeName, AssetType type, ParseFn parse, ApplyFn apply)
			: m_TypeName(typeName), m_Type(type), m_Parse(parse), m_Apply(apply)
		{
		}

		Ref<T> Load(AssetHandle handle)
		{
			if (!IsAssetHandleValid(handle))
				return nullptr;

			auto it = m_Cache.find(handle);
			if (it != m_Cache.end())
			{
				if (Ref<T> live = it->second.Cached.lock())
					return live;

				// Every holder let go while the entry was unpinned. Erasing here rather than
				// reusing the slot keeps "in the map" and "resident" the same statement.
				m_Cache.erase(it);
			}

			const AssetMetadata* metadata = Detail::FindAssetMetadata(handle);
			if (!metadata)
			{
				Detail::WarnUnknownAssetHandle(handle, m_TypeName);
				return nullptr;
			}

			// The extension typed this asset; a handle whose asset is a texture must not load as
			// a mesh. Silent, as before: the caller asked for the wrong type, not for a file that
			// is broken.
			if (metadata->Type != m_Type)
				return nullptr;

			Scope<AssetParseResult> parsed;
			if (m_Parse)
			{
				parsed = m_Parse(*metadata);
				if (!parsed)
					return nullptr;
			}

			// Nothing may be held into m_Cache across this call. Apply loads nested assets - a
			// mesh's materials pull textures - and while those land in *other* managers today,
			// an iterator held across it would be a landmine for the first type that recurses.
			Ref<T> asset = m_Apply(*metadata, std::move(parsed));
			if (!asset)
				return nullptr;

			Entry& entry = m_Cache[handle];
			entry.Cached = asset;
			entry.Retained = asset;
			return asset;
		}

		// Cache lookup only - never loads. For code that wants the object *if* it is already
		// resident: `Reload` reads a material's texture paths this way before evicting them.
		Ref<T> Find(AssetHandle handle) const
		{
			auto it = m_Cache.find(handle);
			if (it == m_Cache.end())
				return nullptr;
			return it->second.Cached.lock();
		}

		const char* TypeName() const override { return m_TypeName; }
		AssetType Type() const override { return m_Type; }

		void Evict(AssetHandle handle) override { m_Cache.erase(handle); }
		void EvictAll() override { m_Cache.clear(); }

		std::size_t ResidentCount() const override
		{
			std::size_t count = 0;
			for (const auto& [handle, entry] : m_Cache)
				count += entry.Cached.expired() ? 0 : 1;
			return count;
		}

		std::size_t RetainedCount() const override
		{
			std::size_t count = 0;
			for (const auto& [handle, entry] : m_Cache)
				count += entry.Retained ? 1 : 0;
			return count;
		}

	private:
		struct Entry
		{
			// Weak, so an asset is resident exactly as long as something references it. This is
			// the fix for "nothing is ever unloaded": the four caches this replaced held strong
			// refs and had no eviction path but Reload and Shutdown.
			std::weak_ptr<T> Cached;

			// ...except nothing references anything yet. Components store bare `AssetHandle`s
			// and every consumer drops its `Ref` at the end of the frame, so a purely weak cache
			// would re-import a glTF *per entity per frame*. This member is the stand-in owner
			// until Phase 3's `AssetRef<T>` becomes the real one, at which point it is deleted
			// and the weak cache starts collecting. See ASSET_PIPELINE_ROADMAP.md decision 5.
			Ref<T> Retained;
		};

		const char* m_TypeName;
		AssetType m_Type;
		ParseFn m_Parse;
		ApplyFn m_Apply;

		std::unordered_map<AssetHandle, Entry> m_Cache;
	};

	// The flat slot array `AssetTypeIdOf<T>()` indexes.
	class AssetManagerRegistry
	{
	public:
		template<typename T>
		static void Register(const char* typeName, AssetType type,
			typename TypedAssetManager<T>::ParseFn parse,
			typename TypedAssetManager<T>::ApplyFn apply)
		{
			const AssetTypeId id = AssetTypeIdOf<T>();
			GE_CORE_ASSERT(id < MaxAssetManagers,
				"More asset managers than MaxAssetManagers - raise it in AssetManagerRegistry.h");

			Detail::AssetManagerSlots()[id] =
				CreateScope<TypedAssetManager<T>>(typeName, type, parse, apply);
		}

		template<typename T>
		static TypedAssetManager<T>& Get()
		{
			IAssetManager* manager = Detail::AssetManagerSlots()[AssetTypeIdOf<T>()].get();
			GE_CORE_ASSERT(manager, "No asset manager registered for this type - AssetManager::Init "
				"has to run before the first GetAsset");

			return *static_cast<TypedAssetManager<T>*>(manager);
		}

		// By `AssetType` rather than by C++ type, for `Reload`, which starts from a handle's
		// metadata and has no static type to work with.
		static IAssetManager* Find(AssetType type);

		template<typename Fn>
		static void ForEach(Fn&& fn)
		{
			for (const Scope<IAssetManager>& manager : Detail::AssetManagerSlots())
			{
				if (manager)
					fn(*manager);
			}
		}

		static void Clear();
	};

}
