#include "gepch.h"
#include "SceneSerializer.h"

#include "Entity.h"
#include "Components.h"
#include "SceneYaml.h"

#include "GanymedE/Assets/AssetManager.h"

#include <fstream>

#include <yaml-cpp/yaml.h>

namespace GanymedE {

	SceneSerializer::SceneSerializer(const Ref<Scene>& scene)
		: m_Scene(scene)
	{
	}

	void SceneSerializer::SerializeEntity(YAML::Emitter& out, Entity entity)
	{
		GE_CORE_ASSERT(entity.HasComponent<IDComponent>(), "Entity missing IDComponent");

		out << YAML::BeginMap; // Entity
		out << YAML::Key << "Entity" << YAML::Value << static_cast<uint64_t>(entity.GetUUID());

		// ---- Reflected components ---------------------------------------------------------
		//
		// A component whose keys and order are exactly its registration is written and read
		// generically. The registered field name IS the YAML key (roadmap decision 3), and
		// `meta_type::data()` iterates in registration order, so the emitted bytes are the ones
		// the hand-written block produced - verified by diffing both writers' output over every
		// committed scene and prefab, not merely by round-tripping.
		//
		// What is left hand-written below, and why, in each case:
		//   RelationshipComponent - the two sides of a link must agree, so writing either half
		//                           generically would corrupt the hierarchy.
		//   MaterialOverrides     - a flow sequence whose INDEX is the meaning.
		//   ScriptComponent Fields - a sequence of {Name, Type, Value} over a closed variant.
		// Each of those is `Trait::Custom` on the field, so the generic writer skips exactly them
		// and the component's other fields still go through it.
		if (entity.HasComponent<TagComponent>())
			WriteReflectedComponent(out, "TagComponent", entity.GetComponent<TagComponent>());

		if (entity.HasComponent<TransformComponent>())
			WriteReflectedComponent(out, "TransformComponent", entity.GetComponent<TransformComponent>());

		if (entity.HasComponent<RelationshipComponent>())
		{
			out << YAML::Key << "RelationshipComponent";
			out << YAML::BeginMap;

			auto& rc = entity.GetComponent<RelationshipComponent>();
			out << YAML::Key << "Parent" << YAML::Value << static_cast<uint64_t>(rc.Parent);
			out << YAML::Key << "Children" << YAML::Value << YAML::BeginSeq;
			for (UUID child : rc.Children)
				out << static_cast<uint64_t>(child);
			out << YAML::EndSeq;

			out << YAML::EndMap;
		}

		// `Source` carries OmitIfDefault, which writes the key only for a valid handle - the
		// same condition the hand-written `if (IsAssetHandleValid(...))` spelled out.
		if (entity.HasComponent<PrefabInstanceComponent>())
		{
			WriteReflectedComponent(out, "PrefabInstanceComponent",
				entity.GetComponent<PrefabInstanceComponent>());
		}

		if (entity.HasComponent<PrefabMemberComponent>())
		{
			WriteReflectedComponent(out, "PrefabMemberComponent",
				entity.GetComponent<PrefabMemberComponent>());
		}

		// SceneCamera is a reflected struct with no codec, so it writes as a nested MAP under
		// "Camera" - the shape every scene already has. Its seven fields are registered against
		// accessors, which is why a private projection matrix never reaches the file.
		if (entity.HasComponent<CameraComponent>())
			WriteReflectedComponent(out, "CameraComponent", entity.GetComponent<CameraComponent>());

		// SpotLightComponent's *inspector* section is still hand-written: the cross-field clamp
		// that stops it drawing generically has nothing to do with how it is stored. The two
		// consumers of this registration convert independently.
		if (entity.HasComponent<SpriteRendererComponent>())
		{
			WriteReflectedComponent(out, "SpriteRendererComponent",
				entity.GetComponent<SpriteRendererComponent>());
		}

		if (entity.HasComponent<StaticMeshComponent>())
		{
			out << YAML::Key << "StaticMeshComponent";
			out << YAML::BeginMap;

			auto& smc = entity.GetComponent<StaticMeshComponent>();

			// Mesh goes through the generic writer - AssetRef serializes as its handle, and
			// OmitIfDefault reproduces the old `if (smc.Mesh.HasHandle())` exactly. Only
			// MaterialOverrides is left by hand, which is what its `Trait::Custom` says.
			WriteReflected(out, entt::forward_as_meta(smc),
				entt::forward_as_meta(ReflectedDefault<StaticMeshComponent>()));

			// Written only when at least one slot is actually overridden. Emitting an empty
			// sequence for every mesh entity would rewrite every committed scene file for no
			// content change, which is exactly what Phase 1's canonical saves exist to prevent.
			bool hasOverride = false;
			for (const AssetRef<Material>& slot : smc.MaterialOverrides)
				hasOverride = hasOverride || slot.HasHandle();

			if (hasOverride)
			{
				// Handles, not paths: the scene-to-registry currency. Trailing unset slots are
				// kept rather than trimmed, because the index *is* the slot.
				out << YAML::Key << "MaterialOverrides" << YAML::Value << YAML::Flow << YAML::BeginSeq;
				for (const AssetRef<Material>& slot : smc.MaterialOverrides)
					out << static_cast<uint64_t>(slot.Handle());
				out << YAML::EndSeq;
			}

			out << YAML::EndMap;
		}

		// Time and Palette are absent because they are Runtime/NotSerialized: a scene loads at
		// the head of its clip, and the palette is rebuilt every frame. Clip is omitted rather
		// than written empty - an empty scalar reads back as a null node and as<std::string>()
		// throws on those - which is what OmitIfDefault on a std::string does.
		if (entity.HasComponent<AnimatorComponent>())
			WriteReflectedComponent(out, "AnimatorComponent", entity.GetComponent<AnimatorComponent>());

		if (entity.HasComponent<ScriptComponent>())
		{
			out << YAML::Key << "ScriptComponent";
			out << YAML::BeginMap;

			auto& sc = entity.GetComponent<ScriptComponent>();

			// Script is a bare AssetHandle with OmitIfDefault - the generic writer emits the key
			// only for a valid handle. Fields is Custom and stays below.
			WriteReflected(out, entt::forward_as_meta(sc),
				entt::forward_as_meta(ReflectedDefault<ScriptComponent>()));

			// Per-entity property overrides. Each carries its type, because the script that
			// declares it may not be loadable when this is read back (missing file, or a scene
			// opened before the asset registry knows about it) - the value still has to
			// round-trip intact rather than becoming a guess.
			if (!sc.Fields.empty())
			{
				// Sorted, so a scene file does not churn just because a hash map reordered.
				std::vector<const std::pair<const std::string, ScriptFieldValue>*> sorted;
				sorted.reserve(sc.Fields.size());
				for (const auto& field : sc.Fields)
					sorted.push_back(&field);
				std::sort(sorted.begin(), sorted.end(),
					[](const auto* a, const auto* b) { return a->first < b->first; });

				out << YAML::Key << "Fields" << YAML::Value << YAML::BeginSeq;
				for (const auto* field : sorted)
				{
					out << YAML::BeginMap;
					out << YAML::Key << "Name" << YAML::Value << field->first;
					std::visit([&out](const auto& value)
					{
						using T = std::decay_t<decltype(value)>;
						if constexpr (std::is_same_v<T, bool>)
							out << YAML::Key << "Type" << YAML::Value << "Bool";
						else if constexpr (std::is_same_v<T, double>)
							out << YAML::Key << "Type" << YAML::Value << "Float";
						else if constexpr (std::is_same_v<T, std::string>)
							out << YAML::Key << "Type" << YAML::Value << "String";
						else
							out << YAML::Key << "Type" << YAML::Value << "Vec3";
						out << YAML::Key << "Value" << YAML::Value << value;
					}, field->second);
					out << YAML::EndMap;
				}
				out << YAML::EndSeq;
			}

			out << YAML::EndMap;
		}

		if (entity.HasComponent<DirectionalLightComponent>())
		{
			WriteReflectedComponent(out, "DirectionalLightComponent",
				entity.GetComponent<DirectionalLightComponent>());
		}

		if (entity.HasComponent<PointLightComponent>())
		{
			WriteReflectedComponent(out, "PointLightComponent",
				entity.GetComponent<PointLightComponent>());
		}

		if (entity.HasComponent<SpotLightComponent>())
		{
			WriteReflectedComponent(out, "SpotLightComponent",
				entity.GetComponent<SpotLightComponent>());
		}

		if (entity.HasComponent<SkyLightComponent>())
			WriteReflectedComponent(out, "SkyLightComponent", entity.GetComponent<SkyLightComponent>());

		// Group writes by name, not by ordinal: AudioGroup carries Trait::SerializeByName, which
		// is what lets it stay reorderable. Nothing forces stable numbering on it (it is not
		// registry-persisted) and a hand-edited scene reading "Music" beats reading 1.
		if (entity.HasComponent<AudioSourceComponent>())
			WriteReflectedComponent(out, "AudioSourceComponent", entity.GetComponent<AudioSourceComponent>());

		if (entity.HasComponent<AudioListenerComponent>())
		{
			WriteReflectedComponent(out, "AudioListenerComponent",
				entity.GetComponent<AudioListenerComponent>());
		}

		// Every authored field carries OmitIfDefault, so this writes exactly the keys that differ
		// from a default-constructed emitter - the twenty hand-written `if (p.X != d.X)` lines
		// this replaces. The five Min/Max pairs are one RangeF each, flattened back out to
		// LifetimeMin/LifetimeMax by the key prefix on their registration.
		//
		// Playing, Time, EmitAccumulator, BurstPending, Rng, Pool and WorldBounds are Runtime and
		// never reach the file.
		if (entity.HasComponent<ParticleEmitterComponent>())
		{
			WriteReflectedComponent(out, "ParticleEmitterComponent",
				entity.GetComponent<ParticleEmitterComponent>());
		}

		// RigidBodyType persists as its ordinal, which is why that enum is append-only.
		if (entity.HasComponent<RigidBodyComponent>())
		{
			WriteReflectedComponent(out, "RigidBodyComponent",
				entity.GetComponent<RigidBodyComponent>());
		}

		// PhysicsMaterial rides on Trait::Flatten, so Friction and Restitution stay SIBLINGS of
		// HalfExtents rather than moving under a "Material" sub-map - the shape every collider in
		// every saved scene already has.
		if (entity.HasComponent<BoxColliderComponent>())
		{
			WriteReflectedComponent(out, "BoxColliderComponent",
				entity.GetComponent<BoxColliderComponent>());
		}

		if (entity.HasComponent<SphereColliderComponent>())
		{
			WriteReflectedComponent(out, "SphereColliderComponent",
				entity.GetComponent<SphereColliderComponent>());
		}

		if (entity.HasComponent<CapsuleColliderComponent>())
		{
			WriteReflectedComponent(out, "CapsuleColliderComponent",
				entity.GetComponent<CapsuleColliderComponent>());
		}

		out << YAML::EndMap; // Entity
	}

	void SceneSerializer::Serialize(const std::string& filepath)
	{
		// Canonical order: roots sorted by UUID, then depth-first through each root's
		// Children in authored order.
		//
		// The order used to be whatever entt's packed array happened to be, which entt
		// 3.16 iterates backwards and a play/stop cycle reshuffles wholesale - saving the
		// same untouched scene twice produced two different files, so "did this edit
		// change anything?" was unanswerable and every save was a full-file diff.
		//
		// A flat UUID sort would also be deterministic, and would give tighter diffs
		// (entities never move in the file, so a reparent touches only Relationship
		// fields). DFS wins anyway because a subtree comes out as a contiguous block,
		// which is the layout .gprefab needs - one canonical order for both formats
		// rather than two - and because sibling order is authored, user-visible state, so
		// letting it order the file makes the layout content rather than an artifact.
		// The cost, accepted: reparenting moves a block in the diff. Godot orders scene
		// files by node path for the same reasons; Unity instead leans on stable fileIDs.
		std::vector<Entity> roots;
		auto view = m_Scene->m_Registry.view<IDComponent>();
		for (auto entityID : view)
		{
			Entity entity = { entityID, m_Scene.get() };
			if (!entity)
				continue;

			const auto* relationship = m_Scene->m_Registry.try_get<RelationshipComponent>(entityID);
			if (!relationship || relationship->Parent == UUID{ 0 })
				roots.push_back(entity);
		}

		std::sort(roots.begin(), roots.end(), [](Entity a, Entity b)
			{
				return static_cast<uint64_t>(a.GetUUID()) < static_cast<uint64_t>(b.GetUUID());
			});

		std::vector<Entity> ordered;
		std::unordered_set<UUID> visited;
		for (Entity root : roots)
			m_Scene->CollectSubtree(root, ordered, visited);

		// Safety net. An entity reachable from no root should not exist - it means a
		// parent's Children vector disagrees with a child's Parent, or a cycle. Writing
		// it anyway keeps a broken hierarchy from becoming silent data loss, and the
		// warning is what makes the corruption visible.
		std::vector<Entity> unreachable;
		for (auto entityID : view)
		{
			Entity entity = { entityID, m_Scene.get() };
			if (entity && visited.find(entity.GetUUID()) == visited.end())
				unreachable.push_back(entity);
		}

		if (!unreachable.empty())
		{
			std::sort(unreachable.begin(), unreachable.end(), [](Entity a, Entity b)
				{
					return static_cast<uint64_t>(a.GetUUID()) < static_cast<uint64_t>(b.GetUUID());
				});

			for (Entity entity : unreachable)
			{
				if (visited.find(entity.GetUUID()) != visited.end())
					continue;   // already picked up as a descendant of an earlier one

				GE_CORE_WARN("Entity {0} ('{1}') is reachable from no root - its parent's "
					"Children vector does not list it. Appending it to '{2}' in UUID order.",
					static_cast<uint64_t>(entity.GetUUID()),
					entity.HasComponent<TagComponent>() ? entity.GetComponent<TagComponent>().Tag : std::string(),
					filepath);

				m_Scene->CollectSubtree(entity, ordered, visited);
			}
		}

		YAML::Emitter out;
		out << YAML::BeginMap;
		out << YAML::Key << "Scene" << YAML::Value << "Untitled";
		out << YAML::Key << "Entities" << YAML::Value << YAML::BeginSeq;

		for (Entity entity : ordered)
			SerializeEntity(out, entity);

		out << YAML::EndSeq;
		out << YAML::EndMap;

		std::ofstream fout(filepath);
		fout << out.c_str();
	}

	void SceneSerializer::SerializeRuntime(const std::string& filepath)
	{
		// Not implemented
		GE_CORE_ASSERT(false, "not implemented!");
	}

	bool SceneSerializer::Deserialize(const std::string& filepath)
	{
		if (!std::filesystem::exists(filepath))
		{
			GE_CORE_ERROR("Scene '{0}' does not exist", filepath);
			return false;
		}

		try
		{
			return DeserializeUnchecked(filepath);
		}
		catch (const YAML::Exception& e)
		{
			// yaml-cpp throws on malformed documents and on every as<T>() whose node is
			// missing, of the wrong type, or out of range - a 20-digit UUID overflowing
			// uint64_t is how this was found. Unhandled, that terminated the process
			// before a single frame, which is the worst possible failure mode for a
			// shipped game: no window, no message, just an exit code. The scene is
			// left partially populated on purpose - the caller decides whether to
			// discard it, and for the editor a half-loaded scene is still inspectable.
			GE_CORE_ERROR("Scene '{0}' failed to parse: {1}", filepath, e.what());
			return false;
		}
	}

	bool SceneSerializer::DeserializeUnchecked(const std::string& filepath)
	{
		std::ifstream stream(filepath);
		std::stringstream strStream;
		strStream << stream.rdbuf();

		YAML::Node data = YAML::Load(strStream.str());
		if (!data["Scene"])
			return false;

		std::string sceneName = data["Scene"].as<std::string>();
		GE_CORE_TRACE("Deserializing scene '{0}'", sceneName);

		auto entities = data["Entities"];
		if (entities)
		{
			std::unordered_set<uint64_t> usedUUIDs;

			// Parallel arrays in file order: what each entity became, and what it was
			// called in the file. ResolveHierarchy needs both to translate Parent and
			// Children, which DeserializeEntity leaves holding the file's UUIDs.
			std::vector<Entity> created;
			std::vector<UUID> fileUUIDs;

			for (auto entityNode : entities)
			{
				uint64_t fileUUID = entityNode["Entity"].as<uint64_t>();

				// Older scenes serialized a hardcoded ID for every entity - mint a fresh UUID on collision
				uint64_t uuid = fileUUID;
				if (uuid == 0 || usedUUIDs.find(uuid) != usedUUIDs.end())
				{
					uuid = static_cast<uint64_t>(UUID());
					GE_CORE_WARN("Scene '{0}' reuses entity UUID {1}; the duplicate was remapped to {2}",
						filepath, fileUUID, uuid);
				}
				usedUUIDs.insert(uuid);

				Entity deserialized = DeserializeEntity(entityNode, *m_Scene, uuid);
				if (!deserialized)
					continue;

				created.push_back(deserialized);
				fileUUIDs.push_back(fileUUID);
			}

			ResolveHierarchy(*m_Scene, created, fileUUIDs);
		}

		return true;
	}

	Entity SceneSerializer::DeserializeEntity(const YAML::Node& entityNode, Scene& scene, UUID uuid)
	{
		std::string name;
		auto tagComponent = entityNode["TagComponent"];
		if (tagComponent)
			name = tagComponent["Tag"].as<std::string>();

		GE_CORE_TRACE("Deserialized entity with ID = {0}, name = {1}", static_cast<uint64_t>(uuid), name);

		Entity deserializedEntity = scene.CreateEntityWithUUID(uuid, name);

		// TagComponent is read above, not here: the tag is needed to CREATE the entity, so it
		// cannot go through the generic reader that needs an entity to read into.
		auto transformComponent = entityNode["TransformComponent"];
		if (transformComponent)
		{
			// Entities always have transforms, so this reads into the existing one rather than
			// adding it. A missing key now leaves the constructed value instead of throwing,
			// which is the generic reader being more tolerant than the code it replaced.
			ReadReflectedComponent(transformComponent, deserializedEntity.GetComponent<TransformComponent>());
		}

		auto relationshipComponent = entityNode["RelationshipComponent"];
		if (relationshipComponent)
		{
			auto& rc = deserializedEntity.GetComponent<RelationshipComponent>();
			rc.Parent = relationshipComponent["Parent"].as<uint64_t>();
			rc.Children.clear();
			auto children = relationshipComponent["Children"];
			if (children)
			{
				for (auto child : children)
					rc.Children.push_back(child.as<uint64_t>());
			}
		}

		auto prefabInstanceComponent = entityNode["PrefabInstanceComponent"];
		if (prefabInstanceComponent)
		{
			ReadReflectedComponent(prefabInstanceComponent,
				deserializedEntity.AddComponent<PrefabInstanceComponent>());
		}

		auto prefabMemberComponent = entityNode["PrefabMemberComponent"];
		if (prefabMemberComponent)
		{
			ReadReflectedComponent(prefabMemberComponent,
				deserializedEntity.AddComponent<PrefabMemberComponent>());
		}

		// The nested "Camera" map reads through SceneCamera's registered SETTERS, so
		// RecalculateProjection runs per field exactly as the hand-written calls made it. Order
		// still matters and is still registration order: ProjectionType lands first, so the six
		// values that follow recalculate against the projection the file asked for.
		auto cameraComponent = entityNode["CameraComponent"];
		if (cameraComponent)
			ReadReflectedComponent(cameraComponent, deserializedEntity.AddComponent<CameraComponent>());

		auto spriteRendererComponent = entityNode["SpriteRendererComponent"];
		if (spriteRendererComponent)
		{
			ReadReflectedComponent(spriteRendererComponent, deserializedEntity.AddComponent<SpriteRendererComponent>());
		}

		auto staticMeshComponent = entityNode["StaticMeshComponent"];
		if (staticMeshComponent)
		{
			auto& smc = deserializedEntity.AddComponent<StaticMeshComponent>();

			ReadReflectedComponent(staticMeshComponent, smc);

			auto meshHandle = staticMeshComponent["Mesh"];
			if (meshHandle)
			{
				// Resolved here rather than left to the first frame, as it always was, and
				// deliberately NOT inside the AssetRef codec - see the comment there. The
				// difference is that the resolved object now lives in the component: with the
				// weak cache of Phase 2 a warm load whose result was dropped would be collected
				// before anything used it.
				smc.Mesh.Get();
			}
			else
			{
				// Backward compatibility with path-based scenes
				auto meshPath = staticMeshComponent["MeshPath"];
				if (meshPath)
					smc.Mesh = AssetRef<Mesh>(AssetManager::ImportAsset(meshPath.as<std::string>()));
			}

			auto overrides = staticMeshComponent["MaterialOverrides"];
			if (overrides && overrides.IsSequence())
			{
				smc.MaterialOverrides.clear();
				smc.MaterialOverrides.reserve(overrides.size());
				for (auto slot : overrides)
					smc.MaterialOverrides.emplace_back(AssetHandle{ slot.as<uint64_t>() });
			}
		}

		auto animatorComponent = entityNode["AnimatorComponent"];
		if (animatorComponent)
		{
			auto& animator = deserializedEntity.AddComponent<AnimatorComponent>();

			auto clip = animatorComponent["Clip"];
			if (clip)
				animator.Clip = clip.as<std::string>();

			animator.Speed = animatorComponent["Speed"].as<float>();
			animator.Playing = animatorComponent["Playing"].as<bool>();
			animator.Loop = animatorComponent["Loop"].as<bool>();
		}

		auto scriptComponent = entityNode["ScriptComponent"];
		if (scriptComponent)
		{
			auto& sc = deserializedEntity.AddComponent<ScriptComponent>();

			// No GetAsset<> call to match the mesh path above: a script has no runtime object to
			// warm, and ScriptEngine loads the chunk itself on instantiation.
			ReadReflectedComponent(scriptComponent, sc);

			auto scriptHandle = scriptComponent["Script"];
			if (!scriptHandle)
			{
				// Backward compatibility with path-based scenes
				auto scriptPath = scriptComponent["ScriptPath"];
				if (scriptPath)
					sc.Script = AssetManager::ImportAsset(scriptPath.as<std::string>());
			}

			if (auto fields = scriptComponent["Fields"])
			{
				for (auto field : fields)
				{
					auto name = field["Name"];
					auto type = field["Type"];
					auto value = field["Value"];
					if (!name || !type || !value)
						continue;

					const std::string typeName = type.as<std::string>();
					if (typeName == "Bool")
						sc.Fields[name.as<std::string>()] = value.as<bool>();
					// "Int" is accepted but folded into a double - see ScriptFieldValue.
					else if (typeName == "Int" || typeName == "Float")
						sc.Fields[name.as<std::string>()] = value.as<double>();
					else if (typeName == "String")
						sc.Fields[name.as<std::string>()] = value.as<std::string>();
					else if (typeName == "Vec3")
						sc.Fields[name.as<std::string>()] = value.as<glm::vec3>();
					else
						GE_CORE_WARN("SceneSerializer: unknown script field type '{0}' "
							"for '{1}'", typeName, name.as<std::string>());
				}
			}
		}

		auto directionalLightComponent = entityNode["DirectionalLightComponent"];
		if (directionalLightComponent)
		{
			ReadReflectedComponent(directionalLightComponent, deserializedEntity.AddComponent<DirectionalLightComponent>());
		}

		auto pointLightComponent = entityNode["PointLightComponent"];
		if (pointLightComponent)
		{
			ReadReflectedComponent(pointLightComponent, deserializedEntity.AddComponent<PointLightComponent>());
		}

		auto spotLightComponent = entityNode["SpotLightComponent"];
		if (spotLightComponent)
		{
			ReadReflectedComponent(spotLightComponent, deserializedEntity.AddComponent<SpotLightComponent>());
		}

		auto skyLightComponent = entityNode["SkyLightComponent"];
		if (skyLightComponent)
		{
			auto& skc = deserializedEntity.AddComponent<SkyLightComponent>();

			ReadReflectedComponent(skyLightComponent, skc);

			if (!skyLightComponent["Environment"])
			{
				// Backward compatibility with path-based scenes
				auto envPath = skyLightComponent["EnvironmentPath"];
				if (envPath)
					skc.Environment = AssetRef<Environment>(AssetManager::ImportAsset(envPath.as<std::string>()));
			}
		}

		auto audioSourceComponent = entityNode["AudioSourceComponent"];
		if (audioSourceComponent)
		{
			// Every field guarded, which the generic reader does for every component: an absent
			// key leaves the constructed value instead of throwing out of as<T>(). That
			// tolerance used to be spelled out here because these components are young enough
			// that hand-authoring a scene is still normal (the runtime demo is one).
			ReadReflectedComponent(audioSourceComponent,
				deserializedEntity.AddComponent<AudioSourceComponent>());
		}

		auto audioListenerComponent = entityNode["AudioListenerComponent"];
		if (audioListenerComponent)
		{
			ReadReflectedComponent(audioListenerComponent, deserializedEntity.AddComponent<AudioListenerComponent>());
		}

		auto particleEmitterComponent = entityNode["ParticleEmitterComponent"];
		if (particleEmitterComponent)
		{
			ReadReflectedComponent(particleEmitterComponent,
				deserializedEntity.AddComponent<ParticleEmitterComponent>());
		}

		auto rigidBodyComponent = entityNode["RigidBodyComponent"];
		if (rigidBodyComponent)
		{
			ReadReflectedComponent(rigidBodyComponent, deserializedEntity.AddComponent<RigidBodyComponent>());
		}

		auto boxColliderComponent = entityNode["BoxColliderComponent"];
		if (boxColliderComponent)
		{
			ReadReflectedComponent(boxColliderComponent, deserializedEntity.AddComponent<BoxColliderComponent>());
		}

		auto sphereColliderComponent = entityNode["SphereColliderComponent"];
		if (sphereColliderComponent)
		{
			ReadReflectedComponent(sphereColliderComponent, deserializedEntity.AddComponent<SphereColliderComponent>());
		}

		auto capsuleColliderComponent = entityNode["CapsuleColliderComponent"];
		if (capsuleColliderComponent)
		{
			ReadReflectedComponent(capsuleColliderComponent, deserializedEntity.AddComponent<CapsuleColliderComponent>());
		}

		return deserializedEntity;
	}

	void SceneSerializer::ResolveHierarchy(Scene& scene, const std::vector<Entity>& created,
		const std::vector<UUID>& fileUUIDs)
	{
		GE_CORE_ASSERT(created.size() == fileUUIDs.size(), "ResolveHierarchy arrays must be parallel");

		// Runs on every load, not only after a collision: with no remapping the mapping is
		// the identity and this is a no-op that also happens to report dangling references.
		// A rarely-taken repair path is a path that rots.
		//
		// The bug this fixes: the loader mints a fresh UUID for a duplicated one, but
		// Parent/Children elsewhere in the file still name the old one. Those references
		// then resolved to whichever entity won the original UUID, silently severing the
		// remapped entity from its hierarchy - or attaching it to a stranger.
		std::unordered_map<uint64_t, std::vector<UUID>> fileToNew;
		for (size_t i = 0; i < created.size(); ++i)
			fileToNew[static_cast<uint64_t>(fileUUIDs[i])].push_back(created[i].GetUUID());

		// Children first, and one file UUID hands out its instances in order: when a file
		// really does contain the same UUID twice, each child slot claims a different
		// entity. The Children vectors are authoritative because sibling order is content.
		std::unordered_map<uint64_t, size_t> claims;
		std::unordered_map<uint64_t, UUID> ownerOf;   // child UUID -> the parent that claimed it

		for (Entity entity : created)
		{
			auto& relationship = entity.GetComponent<RelationshipComponent>();

			std::vector<UUID> resolved;
			resolved.reserve(relationship.Children.size());

			for (UUID childFileID : relationship.Children)
			{
				auto it = fileToNew.find(static_cast<uint64_t>(childFileID));
				if (it == fileToNew.end())
				{
					GE_CORE_WARN("Entity {0} lists child {1}, which no entity in the file "
						"claims - dropping the reference.",
						static_cast<uint64_t>(entity.GetUUID()), static_cast<uint64_t>(childFileID));
					continue;
				}

				size_t& claim = claims[static_cast<uint64_t>(childFileID)];
				UUID childID = it->second[claim < it->second.size() ? claim : it->second.size() - 1];
				++claim;

				resolved.push_back(childID);
				ownerOf[static_cast<uint64_t>(childID)] = entity.GetUUID();
			}

			relationship.Children = std::move(resolved);
		}

		for (Entity entity : created)
		{
			auto& relationship = entity.GetComponent<RelationshipComponent>();

			// Claimed by a parent's Children vector: that is the answer, whatever the
			// entity's own Parent field says.
			auto owner = ownerOf.find(static_cast<uint64_t>(entity.GetUUID()));
			if (owner != ownerOf.end())
			{
				relationship.Parent = owner->second;
				continue;
			}

			if (relationship.Parent == UUID{ 0 })
				continue;

			// Names a parent that never listed it. Translate the reference so the entity
			// at least points at the right object, but say so: Parent and Children
			// disagreeing is what the save-time unreachable warning will trip on.
			auto it = fileToNew.find(static_cast<uint64_t>(relationship.Parent));
			GE_CORE_WARN("Entity {0} names parent {1}, which does not list it as a child.",
				static_cast<uint64_t>(entity.GetUUID()), static_cast<uint64_t>(relationship.Parent));

			relationship.Parent = it != fileToNew.end() ? it->second.front() : UUID{ 0 };
		}
	}

	bool SceneSerializer::DeserializeRuntime(const std::string& filepath)
	{
		// Not implemented
		GE_CORE_ASSERT(false, "not implemented!");
		return false;
	}

}
