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
	// six places; it now means one `GE_ASSET_TYPE` line below and one `Register<T>` call in
	// `RegisterManagers`.
	//
	// The shape is BlankEngine's `IResourceManager` / `ResourceManager<T, Cache>` with its RTTR
	// registration DSL removed - see docs/toDo&done/ASSET_PIPELINE_ROADMAP.md decision 4 for
	// why a reflected registry is not worth a reflection dependency at this scale.

	class Environment;
	class IAssetManager;
	class Material;
	class Mesh;
	class Texture2D;

	using AssetTypeId = uint8_t;

	// Deliberately tight. There are four managed types today and eight `AssetType` values, and
	// only a type with a *runtime object* ever gets a manager - Script, Audio and Prefab are
	// path-resolved by design. BlankEngine's 64 is sized for an engine an order of magnitude larger.
	inline constexpr AssetTypeId MaxAssetManagers = 16;

	// "Does this type have an asset manager?" False by default, true for the four below.
	//
	// This trait is the whole reason the good diagnostic survives: `GetAsset<T>` used to be a
	// primary template whose body was `static_assert(sizeof(T) == 0)`, which gave a compile error
	// naming the supported types instead of an unresolved external at link time. Forwarding to a
	// registry would have regressed that to a runtime assert.
	template<typename T>
	struct IsAssetType : std::false_type {};

	// The `AssetType` a managed C++ type corresponds to. Having it at compile time is what lets
	// an editor asset slot be spelled `AcceptAssetDropRef<Material>()` instead of passing both a
	// template argument and a matching enum that nothing checks against each other.
	template<typename T>
	struct AssetTypeOf;

	// One line per managed type. It is also what `Register<T>` reads its `AssetType` from, so the
	// two can no longer disagree - the enum used to be a second argument at the registration call.
	#define GE_ASSET_TYPE(T, Enum)                                                        \
		template<> struct IsAssetType<T> : std::true_type {};                             \
		template<> struct AssetTypeOf<T> : std::integral_constant<AssetType, AssetType::Enum> {}

	GE_ASSET_TYPE(Mesh, StaticMesh);
	GE_ASSET_TYPE(Environment, Environment);
	GE_ASSET_TYPE(Texture2D, Texture);
	GE_ASSET_TYPE(Material, Material);

	#undef GE_ASSET_TYPE

	namespace Detail {

		// Bumped by every eviction, and read by `AssetRef<T>` to decide whether the object it
		// cached is still the one the manager would hand out.
		//
		// This exists because `AssetRef` broke an invariant the asset layer used to rely on:
		// consumers re-fetched by handle every frame, so `Reload` landed in the viewport on the
		// next frame for free. A reference that caches the object would otherwise keep the stale
		// one forever. One counter for every type rather than one per manager, because eviction
		// is a rare editor action and the cost of over-invalidating is a single manager cache hit
		// per live reference, once. Phase 6's reload-in-place is what eventually makes this
		// unnecessary.
		inline uint32_t g_AssetEvictionEpoch = 0;

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
		// old object, unchanged - the next `Load` builds a *new* one beside it. Live `AssetRef`s
		// notice through the eviction epoch and re-resolve.
		virtual void Evict(AssetHandle handle) = 0;
		virtual void EvictAll() = 0;

		// Live objects, and cache entries including ones whose object has been collected.
		// The gap between them is eviction actually happening.
		virtual std::size_t ResidentCount() const = 0;
		virtual std::size_t TrackedCount() const = 0;
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

		TypedAssetManager(const char* typeName, ParseFn parse, ApplyFn apply)
			: m_TypeName(typeName), m_Parse(parse), m_Apply(apply)
		{
		}

		Ref<T> Load(AssetHandle handle)
		{
			if (!IsAssetHandleValid(handle))
				return nullptr;

			auto it = m_Cache.find(handle);
			if (it != m_Cache.end())
			{
				if (Ref<T> live = it->second.lock())
					return live;

				// Every holder let go. Erasing here rather than reusing the slot keeps "in the
				// map" and "known to have existed" the same statement, and it is the only place
				// expired entries are reclaimed - a handle nobody loads again keeps its empty
				// entry, which is a bounded and very small cost.
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
			if (metadata->Type != AssetTypeOf<T>::value)
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

			m_Cache[handle] = asset;
			return asset;
		}

		// Cache lookup only - never loads. For code that wants the object *if* it is already
		// resident: `Reload` reads a material's texture paths this way before evicting them.
		Ref<T> Find(AssetHandle handle) const
		{
			auto it = m_Cache.find(handle);
			if (it == m_Cache.end())
				return nullptr;
			return it->second.lock();
		}

		const char* TypeName() const override { return m_TypeName; }
		AssetType Type() const override { return AssetTypeOf<T>::value; }

		void Evict(AssetHandle handle) override
		{
			m_Cache.erase(handle);
			++Detail::g_AssetEvictionEpoch;
		}

		void EvictAll() override
		{
			m_Cache.clear();
			++Detail::g_AssetEvictionEpoch;
		}

		std::size_t ResidentCount() const override
		{
			std::size_t count = 0;
			for (const auto& entry : m_Cache)
				count += entry.second.expired() ? 0 : 1;
			return count;
		}

		std::size_t TrackedCount() const override { return m_Cache.size(); }

	private:
		// Weak, so an asset is resident exactly as long as something references it. That is the
		// fix for "nothing is ever unloaded": the four maps this replaced held strong refs and
		// had no eviction path but Reload and Shutdown. The owner is now whoever holds an
		// `AssetRef<T>` - which is a component, and therefore a scene.
		std::unordered_map<AssetHandle, std::weak_ptr<T>> m_Cache;

		const char* m_TypeName;
		ParseFn m_Parse;
		ApplyFn m_Apply;
	};

	// The flat slot array `AssetTypeIdOf<T>()` indexes.
	class AssetManagerRegistry
	{
	public:
		template<typename T>
		static void Register(const char* typeName,
			typename TypedAssetManager<T>::ParseFn parse,
			typename TypedAssetManager<T>::ApplyFn apply)
		{
			static_assert(IsAssetType<T>::value,
				"Register<T> needs a GE_ASSET_TYPE(T, ...) line in AssetManagerRegistry.h first");

			const AssetTypeId id = AssetTypeIdOf<T>();
			GE_CORE_ASSERT(id < MaxAssetManagers,
				"More asset managers than MaxAssetManagers - raise it in AssetManagerRegistry.h");

			Detail::AssetManagerSlots()[id] =
				CreateScope<TypedAssetManager<T>>(typeName, parse, apply);
		}

		template<typename T>
		static TypedAssetManager<T>& Get()
		{
			static_assert(IsAssetType<T>::value,
				"No asset manager for this type. Managed types are Mesh, Environment, Texture2D "
				"and Material; Script, Audio and Prefab are path-resolved by design - resolve "
				"them through AssetManager::GetMetadata (docs/engine/assets.md).");

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
