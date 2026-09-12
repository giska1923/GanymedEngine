#include "EditorPrefabOverrides.h"

#include "GanymedE/Assets/AssetManager.h"
#include "GanymedE/Assets/AssetPaths.h"
#include "GanymedE/Core/Log.h"
#include "GanymedE/Scene/Components.h"
#include "GanymedE/Scene/PrefabSerializer.h"
#include "GanymedE/Scene/Scene.h"

#include <filesystem>
#include <unordered_map>

namespace GanymedE::EditorUI {

	namespace {

		struct Template
		{
			// Detached: never rendered, never ticked, and no entity from it outlives the frame it
			// was asked for.
			Ref<Scene> Storage;

			// The prefab file's own numbering (1..N) -> the entity holding it.
			std::unordered_map<uint64_t, entt::entity> ByCanonicalID;

			// The subtree root, kept so a per-property apply can write the template back out.
			entt::entity Root = entt::null;

			bool Valid = false;
		};

		std::unordered_map<uint64_t, Template>& Templates()
		{
			static std::unordered_map<uint64_t, Template> s_Templates;
			return s_Templates;
		}

		const Template& GetTemplate(AssetHandle source)
		{
			auto& cache = Templates();

			auto it = cache.find((uint64_t)source);
			if (it != cache.end())
				return it->second;

			Template built;
			built.Storage = CreateRef<Scene>();

			if (const AssetMetadata* metadata = AssetManager::GetMetadata(source))
			{
				const std::filesystem::path full = GetAssetRoot() / metadata->FilePath;

				// Instantiated rather than parsed by hand, so the template goes through exactly
				// the path a real instance does. A diff can then never disagree with reality
				// because the two sides were built differently.
				Entity root = PrefabSerializer::Instantiate(full, *built.Storage, source);
				if (root)
				{
					built.Valid = true;
					built.Root = (entt::entity)root;
					built.Storage->Reg().view<PrefabMemberComponent>().each(
						[&built](entt::entity handle, const PrefabMemberComponent& member)
						{
							built.ByCanonicalID[(uint64_t)member.CanonicalID] = handle;
						});
				}
				else
				{
					GE_WARN("Prefab overrides: '{0}' could not be instantiated as a template - "
						"nothing on its instances will be reported as overridden",
						metadata->FilePath);
				}
			}

			return cache.emplace((uint64_t)source, std::move(built)).first->second;
		}

		// The prefab an entity belongs to: its own PrefabInstanceComponent when it is the root,
		// otherwise the nearest ancestor carrying one. A member has no such ancestor only if it
		// was re-parented out of its instance, which the prefab milestone allows.
		AssetHandle FindSourceHandle(Entity entity, Scene& scene)
		{
			Entity current = entity;
			while (current)
			{
				if (current.HasComponent<PrefabInstanceComponent>())
					return current.GetComponent<PrefabInstanceComponent>().Source;

				if (!current.HasComponent<RelationshipComponent>())
					break;

				const UUID parent = current.GetComponent<RelationshipComponent>().Parent;
				if (parent == UUID{ 0 })
					break;

				current = scene.FindEntityByUUID(parent);
			}

			return InvalidAssetHandle;
		}

	}

	Entity FindPrefabTemplate(Entity entity, Scene& scene)
	{
		if (!entity || !entity.HasComponent<PrefabMemberComponent>())
			return {};

		const AssetHandle source = FindSourceHandle(entity, scene);
		if (!IsAssetHandleValid(source))
			return {};

		const Template& templ = GetTemplate(source);
		if (!templ.Valid)
			return {};

		const uint64_t canonical = (uint64_t)entity.GetComponent<PrefabMemberComponent>().CanonicalID;
		auto it = templ.ByCanonicalID.find(canonical);
		if (it == templ.ByCanonicalID.end())
			return {};

		return Entity{ it->second, templ.Storage.get() };
	}

	void InvalidatePrefabTemplates()
	{
		Templates().clear();
	}

	namespace Detail {

		bool WriteTemplateToDisk(Entity instanceEntity, Scene& scene)
		{
			const AssetHandle source = FindSourceHandle(instanceEntity, scene);
			if (!IsAssetHandleValid(source))
				return false;

			auto it = Templates().find((uint64_t)source);
			if (it == Templates().end() || !it->second.Valid || it->second.Root == entt::null)
				return false;

			const AssetMetadata* metadata = AssetManager::GetMetadata(source);
			if (!metadata)
			{
				GE_ERROR("Prefab source {0} is not in the registry - cannot apply",
					static_cast<uint64_t>(source));
				return false;
			}

			Template& templ = it->second;
			Entity root{ templ.Root, templ.Storage.get() };

			// **No `rootTransform` argument, and that is the difference from whole-instance
			// apply.** That one passes the file's existing root transform so an instance's
			// placement is never baked into the asset. Here the thing being saved *is* the
			// template, whose root transform came from the file in the first place - so writing
			// it back preserves placement by construction. It also means applying a root
			// transform field does what it says, rather than being silently dropped by a guard
			// aimed at a different operation.
			return PrefabSerializer::Save(*templ.Storage, root,
				GetAssetRoot() / metadata->FilePath);
		}

	}

}
