#pragma once

#include "GanymedE/Assets/AssetTypes.h"
#include "GanymedE/Core/Core.h"
#include "GanymedE/Core/JobSystem.h"
#include "GanymedE/Core/Log.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

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

		// Apply whatever finished parsing. **Main thread**, once per frame, from
		// AssetManager::Update - this is where every bgfx resource on the async path is created.
		virtual void Update() = 0;

		// Block until `handle` is loaded, applying it here. For the few callers that genuinely
		// cannot proceed with an asset that is not there yet; see AssetManager::WaitFor.
		virtual void WaitFor(AssetHandle handle) = 0;

		// Ask every in-flight parse to stop. Does not wait - destruction does that, and
		// cancelling first means the waits overlap instead of running back to back.
		virtual void CancelPending() = 0;

		// Live objects, and cache entries including ones whose object has been collected.
		// The gap between them is eviction actually happening.
		virtual std::size_t ResidentCount() const = 0;
		virtual std::size_t TrackedCount() const = 0;
		virtual std::size_t PendingCount() const = 0;
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

		// **Returns null while the asset is still loading**, and that is the whole shape of the
		// async layer rather than an omission.
		//
		// The roadmap's decision 8 called for a placeholder per type - checkerboard texture, unit
		// cube mesh, error material - on the argument that returning null "forces every call site
		// to branch, and 24 call sites in a render loop is exactly where you don't want that."
		// The premise does not hold here: every one of those call sites has branched on a null
		// asset since long before this phase, because a missing asset has always been possible.
		// What placeholders would actually change is what a *pending* asset looks like, and the
		// fallbacks that already exist are better than the proposed placeholders: a material
		// whose map has not arrived renders with its albedo colour (Material::Bind already gates
		// on a null map), a sky light with no environment renders the procedural sky, an override
		// slot that is not ready falls back to the mesh's own material, and an entity whose mesh
		// is not ready draws nothing. A unit cube at the wrong scale is more confusing than
		// nothing, and an error material would break instancing batches on Ref identity.
		Ref<T> Load(AssetHandle handle)
		{
			// Load is main-thread-only, which is stronger than the roadmap's "assert on Apply"
			// and cheaper to reason about: m_Cache, m_Pending and m_Failed are then plain
			// containers with no lock, and every bgfx call downstream is inside Apply.
			GE_CORE_ASSERT(JobSystem::IsMainThread(),
				"AssetManager::Load must run on the main thread - the parse it queues is the "
				"part that goes to a worker");

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

			// A load that failed is remembered, and that is not an optimisation. An AssetRef
			// whose Get() returns null re-asks every frame by design, so without this a broken
			// asset would queue a job, fail, and queue another - forever, at frame rate, with a
			// log line each time. Reload and Evict clear it, which is how a fixed file recovers.
			if (m_Failed.count(handle) != 0)
				return nullptr;

			if (m_Pending.count(handle) != 0)
				return nullptr;

			const AssetMetadata* metadata = Detail::FindAssetMetadata(handle);
			if (!metadata)
			{
				// Deliberately *not* recorded as failed: a handle with no index entry may still
				// gain one, because ImportAsset runs during scene deserialization. The warning
				// is already once-per-handle.
				Detail::WarnUnknownAssetHandle(handle, m_TypeName);
				return nullptr;
			}

			// The extension typed this asset; a handle whose asset is a texture must not load as
			// a mesh. Silent, as before: the caller asked for the wrong type, not for a file that
			// is broken.
			if (metadata->Type != AssetTypeOf<T>::value)
				return nullptr;

			// No Parse stage means there is nothing to move off the main thread - Environment is
			// the case, where the work is an IBL bake through bgfx views. Applying inline keeps
			// its behaviour exactly what it was.
			if (!m_Parse)
			{
				Ref<T> asset = m_Apply(*metadata, nullptr);
				if (!asset)
				{
					m_Failed.insert(handle);
					return nullptr;
				}

				m_Cache[handle] = asset;
				m_Handoff[handle] = { asset, 0 };
				return asset;
			}

			PendingLoad pending;

			// The metadata is **copied**, not pointed at. The registry's nodes are stable, so a
			// pointer would in fact survive - but "in fact survives" is a property of today's
			// container choice, and a worker reading engine state that the main thread can
			// mutate is exactly the class of bug this phase is supposed to avoid.
			pending.Metadata = *metadata;

			ParseFn parse = m_Parse;
			pending.Parse = JobSystem::Submit(JobPriority::Normal,
				[metadata = pending.Metadata, parse]() -> Scope<AssetParseResult>
				{
					return parse(metadata);
				});

			m_Pending.emplace(handle, std::move(pending));
			return nullptr;
		}

		void Update() override
		{
			GE_CORE_ASSERT(JobSystem::IsMainThread(), "Apply creates GPU resources - main thread only");

			// Cancelled jobs, released once they have actually retired. Erasing a retired Future
			// does not block; erasing one still running would, which is the whole point of
			// deferring it to here instead of doing it inside Evict.
			for (auto it = m_Draining.begin(); it != m_Draining.end(); )
				it = it->IsReady() ? m_Draining.erase(it) : it + 1;

			if (m_Pending.empty())
				return;

			// Collected before applying, rather than applied while iterating. Apply loads nested
			// assets - a mesh pulls its textures - and while those land in *other* managers
			// today, an iterator held across it would be a landmine for the first type that
			// reaches back into its own manager.
			std::vector<std::pair<AssetHandle, PendingLoad>> ready;
			for (auto it = m_Pending.begin(); it != m_Pending.end(); )
			{
				if (!it->second.Parse.IsReady())
				{
					++it;
					continue;
				}

				ready.emplace_back(it->first, std::move(it->second));
				it = m_Pending.erase(it);
			}

			for (auto& [handle, pending] : ready)
				Finish(handle, pending);

			AgeHandoff();
		}

		void WaitFor(AssetHandle handle) override
		{
			GE_CORE_ASSERT(JobSystem::IsMainThread(), "WaitFor applies on the main thread");

			auto it = m_Pending.find(handle);
			if (it == m_Pending.end())
				return;

			// Future::Wait pumps other queued jobs while it blocks, so waiting here cannot
			// deadlock the pool even though this is called from inside a frame.
			it->second.Parse.Wait();

			PendingLoad pending = std::move(it->second);
			m_Pending.erase(it);
			Finish(handle, pending);
		}

		void CancelPending() override
		{
			for (auto& [handle, pending] : m_Pending)
				pending.Parse.Cancel();

			for (Future<Scope<AssetParseResult>>& draining : m_Draining)
				draining.Cancel();
		}

		// Assets applied recently enough that whoever asked for them may not have collected the
		// result yet.
		//
		// **This exists because a weak cache and asynchronous loading do not compose on their
		// own.** Load returns null while a parse is in flight, so the caller walks away with
		// nothing; when Update finally applies the asset, the only reference to it is the local
		// in Finish. Insert it into a weak cache and it is collected before the function returns,
		// the next frame's Load sees an expired entry, and the whole parse runs again - forever,
		// at frame rate. That is exactly what happened the first time this ran: one mesh stuck
		// "pending", nothing rendered, and the compiled-cache hit counter climbed past 3800.
		//
		// So the manager holds the asset itself for a few frames. Anything that wants it is
		// polling every frame - an AssetRef whose Get() returned null re-asks by design - so it
		// takes ownership on the next call and the manager's grip stops mattering. Nothing takes
		// it, it ages out and is collected, which is the weak cache behaving exactly as intended.
		// The count is small and deliberate: one frame would be enough in principle and leaves no
		// slack for a consumer that skips a frame.
		void AgeHandoff()
		{
			for (auto it = m_Handoff.begin(); it != m_Handoff.end(); )
			{
				if (++it->second.FramesHeld >= kHandoffFrames)
					it = m_Handoff.erase(it);
				else
					++it;
			}
		}

		// Every asset of this type that is *currently alive*, as (handle, object). Depth-1
		// dependency questions are answered by walking this - "which resident materials
		// reference this texture" - rather than by maintaining a reverse-edge map; see
		// AssetManager.cpp's EvictTextureDependents for why the scan is the better shape under
		// a weak cache.
		//
		// **Do not evict from inside `fn`**: Evict erases from the very map this iterates.
		// Collect handles, then act.
		template<typename Fn>
		void ForEachResident(Fn&& fn) const
		{
			for (const auto& entry : m_Cache)
			{
				if (Ref<T> live = entry.second.lock())
					fn(entry.first, live);
			}
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
			m_Failed.erase(handle);
			m_Handoff.erase(handle);

			// A load still in flight is cancelled and *set aside*, not waited on here.
			//
			// Waiting would be correct but slow in exactly the wrong place: a Reload issued
			// while a large texture is mid-encode blocked the main thread for the rest of that
			// encode - 186 ms, measured, before this. Cancellation cannot dequeue a started task
			// (enkiTS has no such call), so the only choices are to block or to hold the Future
			// somewhere until it retires. Holding it is safe because the job writes into its own
			// result slot and nothing reads it: the pending entry is already gone, so the result
			// can never be applied.
			if (auto it = m_Pending.find(handle); it != m_Pending.end())
			{
				it->second.Parse.Cancel();
				m_Draining.push_back(std::move(it->second.Parse));
				m_Pending.erase(it);
			}

			++Detail::g_AssetEvictionEpoch;
		}

		void EvictAll() override
		{
			CancelPending();
			m_Pending.clear();
			m_Draining.clear();
			m_Failed.clear();
			m_Handoff.clear();
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
		std::size_t PendingCount() const override { return m_Pending.size(); }

		~TypedAssetManager() override
		{
			// Ask everything to stop before any single Future's destructor starts waiting on
			// it, so the waits overlap. Without this a scene's worth of in-flight parses would
			// be cancelled and drained one at a time at shutdown.
			CancelPending();
		}

	private:
		struct PendingLoad
		{
			AssetMetadata Metadata;

			// Move-only, and its destructor cancels and waits - which is the property this whole
			// design leans on. Dropping a PendingLoad (a scene swap, a shutdown, an Evict) can
			// never leave a worker writing into state that has gone away.
			Future<Scope<AssetParseResult>> Parse;
		};

		void Finish(AssetHandle handle, PendingLoad& pending)
		{
			std::optional<Scope<AssetParseResult>> parsed = pending.Parse.Get();

			// nullopt means the job was cancelled before its body ran; a null Scope means the
			// parse itself failed. Both are terminal for this handle until something evicts it.
			if (!parsed || !*parsed)
			{
				m_Failed.insert(handle);
				return;
			}

			Ref<T> asset = m_Apply(pending.Metadata, std::move(*parsed));
			if (!asset)
			{
				m_Failed.insert(handle);
				return;
			}

			m_Cache[handle] = asset;
			m_Handoff[handle] = { asset, 0 };
		}

		// Weak, so an asset is resident exactly as long as something references it. That is the
		// fix for "nothing is ever unloaded": the four maps this replaced held strong refs and
		// had no eviction path but Reload and Shutdown. The owner is now whoever holds an
		// `AssetRef<T>` - which is a component, and therefore a scene.
		std::unordered_map<AssetHandle, std::weak_ptr<T>> m_Cache;

		// In flight. Keyed by handle so a second Load for the same asset joins the first rather
		// than queueing a duplicate parse - the race decision 5's "two Load calls racing to
		// insert" warns about, made impossible by Load being main-thread-only.
		std::unordered_map<AssetHandle, PendingLoad> m_Pending;

		// Handles whose load will not be retried. See the note in Load.
		std::unordered_set<AssetHandle> m_Failed;

		static constexpr uint32_t kHandoffFrames = 4;

		struct Handoff
		{
			Ref<T> Asset;
			uint32_t FramesHeld = 0;
		};

		// The only strong references this manager holds, and only briefly. See AgeHandoff.
		std::unordered_map<AssetHandle, Handoff> m_Handoff;

		// Cancelled parses waiting to retire. Nothing reads their results; they are held only so
		// that dropping them cannot happen while the worker is still inside the body. See Evict.
		std::vector<Future<Scope<AssetParseResult>>> m_Draining;

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
