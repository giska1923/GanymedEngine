#include "gepch.h"
#include "PrefabSerializer.h"

#include "Components.h"
#include "Scene.h"
#include "SceneSerializer.h"
#include "SceneYaml.h"

#include "GanymedE/ECS/ComponentTraits.h"

#include <fstream>

#include <yaml-cpp/yaml.h>

namespace GanymedE {

	namespace {

		// A scratch scene holding the subtree renumbered 1..N in DFS order.
		//
		// Renumbering in place and putting it back would be faster and is not worth it: a throw
		// or an early return anywhere in between would leave the *live* scene carrying canonical
		// UUIDs, which is unrecoverable corruption in exchange for saving an editor-click's worth
		// of allocation.
		Ref<Scene> BuildCanonicalCopy(Scene& source, Entity root, std::vector<Entity>& orderedOut)
		{
			std::vector<Entity> subtree;
			std::unordered_set<UUID> visited;
			source.CollectSubtree(root, subtree, visited);

			if (subtree.empty())
				return nullptr;

			Ref<Scene> canonical = CreateRef<Scene>();

			std::unordered_map<UUID, UUID> remap;
			orderedOut.clear();
			orderedOut.reserve(subtree.size());

			for (size_t i = 0; i < subtree.size(); i++)
			{
				const UUID canonicalID{ (uint64_t)(i + 1) };   // 1..N, DFS order
				Entity copy = canonical->CreateEntityWithUUID(canonicalID,
					subtree[i].GetComponent<TagComponent>().Tag);

				remap[subtree[i].GetUUID()] = canonicalID;
				orderedOut.push_back(copy);
			}

			for (size_t i = 0; i < subtree.size(); i++)
			{
				auto src = (entt::entity)subtree[i];
				auto dst = (entt::entity)orderedOut[i];

				// Verbatim for everything in ComponentList - which is exactly what leaves the
				// AssetHandle fields alone. IDComponent and TagComponent are outside it.
				ForEachType(ComponentList{}, [&](auto typeTag)
				{
					using T = typename decltype(typeTag)::Type;
					if (auto* component = source.Reg().try_get<T>(src))
						canonical->Reg().emplace_or_replace<T>(dst, *component);
				});

				auto& relationship = canonical->Reg().get<RelationshipComponent>(dst);

				std::vector<UUID> children;
				children.reserve(relationship.Children.size());
				for (UUID childID : relationship.Children)
				{
					auto it = remap.find(childID);
					if (it != remap.end())
						children.push_back(it->second);
				}
				relationship.Children = std::move(children);

				// The subtree root's parent lives outside the prefab; it becomes a root here.
				auto parentIt = remap.find(relationship.Parent);
				relationship.Parent = parentIt != remap.end() ? parentIt->second : UUID{ 0 };
			}

			// An instance root inside a prefab file would be a nested prefab, which v1 does not
			// do - the file describes plain entities.
			for (Entity entity : orderedOut)
			{
				if (entity.HasComponent<PrefabInstanceComponent>())
					entity.RemoveComponent<PrefabInstanceComponent>();
			}

			return canonical;
		}

	}

	namespace PrefabSerializer {

		bool Save(Scene& scene, Entity root, const std::filesystem::path& fullPath,
			const TransformComponent* rootTransform)
		{
			if (!root)
				return false;

			std::vector<Entity> ordered;
			Ref<Scene> canonical = BuildCanonicalCopy(scene, root, ordered);
			if (!canonical || ordered.empty())
				return false;

			if (rootTransform)
				ordered.front().GetComponent<TransformComponent>() = *rootTransform;

			YAML::Emitter out;
			out << YAML::BeginMap;
			out << YAML::Key << "Prefab" << YAML::Value << root.GetComponent<TagComponent>().Tag;
			out << YAML::Key << "Entities" << YAML::Value << YAML::BeginSeq;

			// `ordered` is already the canonical DFS order, so a subtree is contiguous by
			// construction and the root is first.
			for (Entity entity : ordered)
				SceneSerializer::SerializeEntity(out, entity);

			out << YAML::EndSeq;
			out << YAML::EndMap;

			std::error_code ec;
			std::filesystem::create_directories(fullPath.parent_path(), ec);

			std::ofstream fout(fullPath);
			if (!fout)
			{
				GE_CORE_ERROR("Could not open '{0}' for writing", fullPath.string());
				return false;
			}

			fout << out.c_str();
			return true;
		}

		namespace {

			// Shared by ReadRootTransform and Instantiate. Null on anything unreadable; the
			// SceneSerializer posture - malformed content is reported, not fatal.
			bool LoadDocument(const std::filesystem::path& fullPath, YAML::Node& out)
			{
				if (!std::filesystem::exists(fullPath))
				{
					GE_CORE_ERROR("Prefab '{0}' does not exist", fullPath.string());
					return false;
				}

				try
				{
					std::ifstream stream(fullPath);
					std::stringstream buffer;
					buffer << stream.rdbuf();
					out = YAML::Load(buffer.str());
				}
				catch (const YAML::Exception& e)
				{
					GE_CORE_ERROR("Prefab '{0}' failed to parse: {1}", fullPath.string(), e.what());
					return false;
				}

				if (!out["Prefab"] || !out["Entities"] || !out["Entities"].IsSequence())
				{
					GE_CORE_ERROR("Prefab '{0}' is not a prefab document", fullPath.string());
					return false;
				}

				return true;
			}

		}

		bool ReadRootTransform(const std::filesystem::path& fullPath, TransformComponent& out)
		{
			YAML::Node root;
			if (!LoadDocument(fullPath, root))
				return false;

			auto entities = root["Entities"];
			if (entities.size() == 0)
				return false;

			try
			{
				auto transform = entities[0]["TransformComponent"];
				if (!transform)
					return false;

				out.Translation = transform["Translation"].as<glm::vec3>();
				out.Rotation = transform["Rotation"].as<glm::vec3>();
				out.Scale = transform["Scale"].as<glm::vec3>();
			}
			catch (const YAML::Exception& e)
			{
				GE_CORE_ERROR("Prefab '{0}' has a malformed root transform: {1}",
					fullPath.string(), e.what());
				return false;
			}

			return true;
		}

		Entity Instantiate(const std::filesystem::path& fullPath, Scene& scene, AssetHandle source,
			const InstantiateOptions& options)
		{
			YAML::Node root;
			if (!LoadDocument(fullPath, root))
				return {};

			auto entities = root["Entities"];

			std::vector<Entity> created;
			std::vector<UUID> fileUUIDs;
			created.reserve(entities.size());
			fileUUIDs.reserve(entities.size());

			try
			{
				for (auto entityNode : entities)
				{
					const UUID fileUUID{ entityNode["Entity"].as<uint64_t>() };

					// Always fresh, except a caller-pinned root: the file's canonical 1..N would
					// collide with the second instance in the same scene, and with anything else
					// that happens to use a small UUID.
					const bool isRoot = created.empty();
					UUID uuid = (isRoot && options.RootUUID != UUID{ 0 }) ? options.RootUUID : UUID();

					Entity entity = SceneSerializer::DeserializeEntity(entityNode, scene, uuid);
					if (!entity)
						continue;

					created.push_back(entity);
					fileUUIDs.push_back(fileUUID);
				}
			}
			catch (const YAML::Exception& e)
			{
				GE_CORE_ERROR("Prefab '{0}' failed to read: {1} - instantiating what was read",
					fullPath.string(), e.what());
			}

			if (created.empty())
				return {};

			// The same pass a scene load runs, doing the same job: translate the file's UUIDs
			// into the ones the entities were actually created with. Instantiation never trips
			// the collision path, because every UUID here is minted fresh.
			SceneSerializer::ResolveHierarchy(scene, created, fileUUIDs);

			Entity instanceRoot = created.front();

			// Placement is per-instance. Everything below the root is wholly file-owned.
			if (options.RootTransform)
			{
				instanceRoot.GetComponent<TransformComponent>() = *options.RootTransform;
				scene.MarkChanged<TransformComponent>(instanceRoot);
			}

			instanceRoot.AddComponent<PrefabInstanceComponent>().Source = source;

			// The canonical link, recorded while the pairing is still in hand. `created[i]` was
			// built from `fileUUIDs[i]`, and after this function returns there is no way to
			// recover which prefab object an instance entity came from - fresh UUIDs everywhere
			// and structural edits allowed. This is what per-property overrides key on.
			for (std::size_t i = 0; i < created.size(); i++)
				created[i].AddComponent<PrefabMemberComponent>(fileUUIDs[i]);

			for (Entity entity : created)
			{
				scene.MarkChanged<TransformComponent>(entity);
				scene.MarkChanged<RelationshipComponent>(entity);
			}

			return instanceRoot;
		}

	}

}
