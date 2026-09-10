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

		if (entity.HasComponent<TagComponent>())
		{
			out << YAML::Key << "TagComponent";
			out << YAML::BeginMap; // TagComponent

			auto& tag = entity.GetComponent<TagComponent>().Tag;
			out << YAML::Key << "Tag" << YAML::Value << tag;

			out << YAML::EndMap; // TagComponent
		}

		if (entity.HasComponent<TransformComponent>())
		{
			out << YAML::Key << "TransformComponent";
			out << YAML::BeginMap; // TransformComponent

			auto& tc = entity.GetComponent<TransformComponent>();
			out << YAML::Key << "Translation" << YAML::Value << tc.Translation;
			out << YAML::Key << "Rotation" << YAML::Value << tc.Rotation;
			out << YAML::Key << "Scale" << YAML::Value << tc.Scale;

			out << YAML::EndMap; // TransformComponent
		}

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

		if (entity.HasComponent<PrefabInstanceComponent>())
		{
			out << YAML::Key << "PrefabInstanceComponent";
			out << YAML::BeginMap;

			auto& prefab = entity.GetComponent<PrefabInstanceComponent>();
			if (IsAssetHandleValid(prefab.Source))
				out << YAML::Key << "Source" << YAML::Value << static_cast<uint64_t>(prefab.Source);

			out << YAML::EndMap;
		}

		if (entity.HasComponent<CameraComponent>())
		{
			out << YAML::Key << "CameraComponent";
			out << YAML::BeginMap; // CameraComponent

			auto& cameraComponent = entity.GetComponent<CameraComponent>();
			auto& camera = cameraComponent.Camera;

			out << YAML::Key << "Camera" << YAML::Value;
			out << YAML::BeginMap; // Camera
			out << YAML::Key << "ProjectionType" << YAML::Value << (int)camera.GetProjectionType();
			out << YAML::Key << "PerspectiveFOV" << YAML::Value << camera.GetPerspectiveVerticalFOV();
			out << YAML::Key << "PerspectiveNear" << YAML::Value << camera.GetPerspectiveNearClip();
			out << YAML::Key << "PerspectiveFar" << YAML::Value << camera.GetPerspectiveFarClip();
			out << YAML::Key << "OrthographicSize" << YAML::Value << camera.GetOrthographicSize();
			out << YAML::Key << "OrthographicNear" << YAML::Value << camera.GetOrthographicNearClip();
			out << YAML::Key << "OrthographicFar" << YAML::Value << camera.GetOrthographicFarClip();
			out << YAML::EndMap; // Camera

			out << YAML::Key << "Primary" << YAML::Value << cameraComponent.Primary;
			out << YAML::Key << "FixedAspectRatio" << YAML::Value << cameraComponent.FixedAspectRatio;

			out << YAML::EndMap; // CameraComponent
		}

		// ---- Reflected components ---------------------------------------------------------
		//
		// From here on, a component whose keys and order are exactly its registration is written
		// and read generically. The registered field name IS the YAML key (roadmap decision 3),
		// and `meta_type::data()` iterates in registration order, so the emitted bytes are the
		// ones the hand-written block produced - verified by diffing both writers' output for the
		// same input scene, not merely by round-tripping.
		//
		// SpotLightComponent is here while its *inspector* section is still hand-written: the
		// cross-field clamp that stops it drawing generically has nothing to do with how it is
		// stored. The two consumers of this registration convert independently.
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

			// AssetRef serializes as its handle, so every key and value below is byte-identical
			// to what the bare-handle version wrote. That is the whole compatibility story.
			if (smc.Mesh.HasHandle())
				out << YAML::Key << "Mesh" << YAML::Value << static_cast<uint64_t>(smc.Mesh.Handle());

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

		if (entity.HasComponent<AnimatorComponent>())
		{
			out << YAML::Key << "AnimatorComponent";
			out << YAML::BeginMap;

			// Time and Palette are deliberately absent: a scene loads at the head of its clip,
			// and the palette is rebuilt every frame.
			auto& animator = entity.GetComponent<AnimatorComponent>();
			// Omitted rather than written empty: an empty scalar reads back as a null node, and
			// as<std::string>() throws on those.
			if (!animator.Clip.empty())
				out << YAML::Key << "Clip" << YAML::Value << animator.Clip;
			out << YAML::Key << "Speed" << YAML::Value << animator.Speed;
			out << YAML::Key << "Playing" << YAML::Value << animator.Playing;
			out << YAML::Key << "Loop" << YAML::Value << animator.Loop;

			out << YAML::EndMap;
		}

		if (entity.HasComponent<ScriptComponent>())
		{
			out << YAML::Key << "ScriptComponent";
			out << YAML::BeginMap;

			auto& sc = entity.GetComponent<ScriptComponent>();
			if (IsAssetHandleValid(sc.Script))
				out << YAML::Key << "Script" << YAML::Value << static_cast<uint64_t>(sc.Script);

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
		{
			out << YAML::Key << "SkyLightComponent";
			out << YAML::BeginMap;

			auto& skc = entity.GetComponent<SkyLightComponent>();
			if (skc.Environment.HasHandle())
				out << YAML::Key << "Environment" << YAML::Value << static_cast<uint64_t>(skc.Environment.Handle());
			out << YAML::Key << "SkyColor" << YAML::Value << skc.SkyColor;
			out << YAML::Key << "GroundColor" << YAML::Value << skc.GroundColor;
			out << YAML::Key << "Intensity" << YAML::Value << skc.Intensity;
			out << YAML::Key << "DrawSkybox" << YAML::Value << skc.DrawSkybox;

			out << YAML::EndMap;
		}

		if (entity.HasComponent<AudioSourceComponent>())
		{
			out << YAML::Key << "AudioSourceComponent";
			out << YAML::BeginMap;

			auto& source = entity.GetComponent<AudioSourceComponent>();
			if (IsAssetHandleValid(source.Clip))
				out << YAML::Key << "Clip" << YAML::Value << static_cast<uint64_t>(source.Clip);

			// By name, not by ordinal. AudioGroup is not registry-persisted, so nothing forces
			// stable numbering on it, and a hand-edited scene reading "Music" beats reading 1.
			out << YAML::Key << "Group" << YAML::Value << AudioGroupToString(source.Group);
			out << YAML::Key << "Volume" << YAML::Value << source.Volume;
			out << YAML::Key << "Pitch" << YAML::Value << source.Pitch;
			out << YAML::Key << "Loop" << YAML::Value << source.Loop;
			out << YAML::Key << "PlayOnStart" << YAML::Value << source.PlayOnStart;
			out << YAML::Key << "Spatialize" << YAML::Value << source.Spatialize;
			out << YAML::Key << "Stream" << YAML::Value << source.Stream;

			out << YAML::EndMap;
		}

		if (entity.HasComponent<AudioListenerComponent>())
		{
			WriteReflectedComponent(out, "AudioListenerComponent",
				entity.GetComponent<AudioListenerComponent>());
		}

		if (entity.HasComponent<ParticleEmitterComponent>())
		{
			out << YAML::Key << "ParticleEmitterComponent";
			out << YAML::BeginMap;

			const auto& p = entity.GetComponent<ParticleEmitterComponent>();
			const ParticleEmitterComponent d{};

			if (p.RateOverTime != d.RateOverTime)
				out << YAML::Key << "RateOverTime" << YAML::Value << p.RateOverTime;
			if (p.MaxParticles != d.MaxParticles)
				out << YAML::Key << "MaxParticles" << YAML::Value << p.MaxParticles;
			if (p.Looping != d.Looping)
				out << YAML::Key << "Looping" << YAML::Value << p.Looping;
			if (p.Duration != d.Duration)
				out << YAML::Key << "Duration" << YAML::Value << p.Duration;
			if (p.PlayOnStart != d.PlayOnStart)
				out << YAML::Key << "PlayOnStart" << YAML::Value << p.PlayOnStart;

			if (p.LifetimeMin != d.LifetimeMin)
				out << YAML::Key << "LifetimeMin" << YAML::Value << p.LifetimeMin;
			if (p.LifetimeMax != d.LifetimeMax)
				out << YAML::Key << "LifetimeMax" << YAML::Value << p.LifetimeMax;
			if (p.SpeedMin != d.SpeedMin)
				out << YAML::Key << "SpeedMin" << YAML::Value << p.SpeedMin;
			if (p.SpeedMax != d.SpeedMax)
				out << YAML::Key << "SpeedMax" << YAML::Value << p.SpeedMax;
			if (p.ConeAngle != d.ConeAngle)
				out << YAML::Key << "ConeAngle" << YAML::Value << p.ConeAngle;
			if (p.StartSizeMin != d.StartSizeMin)
				out << YAML::Key << "StartSizeMin" << YAML::Value << p.StartSizeMin;
			if (p.StartSizeMax != d.StartSizeMax)
				out << YAML::Key << "StartSizeMax" << YAML::Value << p.StartSizeMax;
			if (p.StartRotationMin != d.StartRotationMin)
				out << YAML::Key << "StartRotationMin" << YAML::Value << p.StartRotationMin;
			if (p.StartRotationMax != d.StartRotationMax)
				out << YAML::Key << "StartRotationMax" << YAML::Value << p.StartRotationMax;
			if (p.RotationSpeedMin != d.RotationSpeedMin)
				out << YAML::Key << "RotationSpeedMin" << YAML::Value << p.RotationSpeedMin;
			if (p.RotationSpeedMax != d.RotationSpeedMax)
				out << YAML::Key << "RotationSpeedMax" << YAML::Value << p.RotationSpeedMax;
			if (p.GravityModifier != d.GravityModifier)
				out << YAML::Key << "GravityModifier" << YAML::Value << p.GravityModifier;
			if (p.WorldSpace != d.WorldSpace)
				out << YAML::Key << "WorldSpace" << YAML::Value << p.WorldSpace;
			if (p.Seed != d.Seed)
				out << YAML::Key << "Seed" << YAML::Value << p.Seed;

			if (!p.SizeCurve.IsDefault())
				out << YAML::Key << "SizeCurve" << YAML::Value << p.SizeCurve;
			if (!p.ColorOverLifetime.IsDefault())
				out << YAML::Key << "ColorOverLifetime" << YAML::Value << p.ColorOverLifetime;

			if (p.RenderMode != d.RenderMode)
				out << YAML::Key << "RenderMode" << YAML::Value << (int)p.RenderMode;
			if (p.Texture.HasHandle())
				out << YAML::Key << "Texture" << YAML::Value << static_cast<uint64_t>(p.Texture.Handle());
			if (p.Blend != d.Blend)
				out << YAML::Key << "Blend" << YAML::Value << (int)p.Blend;
			if (p.Mesh.HasHandle())
				out << YAML::Key << "Mesh" << YAML::Value << static_cast<uint64_t>(p.Mesh.Handle());
			if (p.Material.HasHandle())
				out << YAML::Key << "Material" << YAML::Value << static_cast<uint64_t>(p.Material.Handle());

			// Playing, Time, EmitAccumulator, BurstPending, Rng, Pool, WorldBounds: runtime-only.

			out << YAML::EndMap;
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

		auto transformComponent = entityNode["TransformComponent"];
		if (transformComponent)
		{
			// Entities always have transforms
			auto& tc = deserializedEntity.GetComponent<TransformComponent>();
			tc.Translation = transformComponent["Translation"].as<glm::vec3>();
			tc.Rotation = transformComponent["Rotation"].as<glm::vec3>();
			tc.Scale = transformComponent["Scale"].as<glm::vec3>();
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
			auto& prefab = deserializedEntity.AddComponent<PrefabInstanceComponent>();
			if (auto source = prefabInstanceComponent["Source"])
				prefab.Source = AssetHandle{ source.as<uint64_t>() };
		}

		auto cameraComponent = entityNode["CameraComponent"];
		if (cameraComponent)
		{
			auto& cc = deserializedEntity.AddComponent<CameraComponent>();

			auto cameraProps = cameraComponent["Camera"];
			cc.Camera.SetProjectionType((SceneCamera::ProjectionType)cameraProps["ProjectionType"].as<int>());

			cc.Camera.SetPerspectiveVerticalFOV(cameraProps["PerspectiveFOV"].as<float>());
			cc.Camera.SetPerspectiveNearClip(cameraProps["PerspectiveNear"].as<float>());
			cc.Camera.SetPerspectiveFarClip(cameraProps["PerspectiveFar"].as<float>());

			cc.Camera.SetOrthographicSize(cameraProps["OrthographicSize"].as<float>());
			cc.Camera.SetOrthographicNearClip(cameraProps["OrthographicNear"].as<float>());
			cc.Camera.SetOrthographicFarClip(cameraProps["OrthographicFar"].as<float>());

			cc.Primary = cameraComponent["Primary"].as<bool>();
			cc.FixedAspectRatio = cameraComponent["FixedAspectRatio"].as<bool>();
		}

		auto spriteRendererComponent = entityNode["SpriteRendererComponent"];
		if (spriteRendererComponent)
		{
			ReadReflectedComponent(spriteRendererComponent, deserializedEntity.AddComponent<SpriteRendererComponent>());
		}

		auto staticMeshComponent = entityNode["StaticMeshComponent"];
		if (staticMeshComponent)
		{
			auto& smc = deserializedEntity.AddComponent<StaticMeshComponent>();

			auto meshHandle = staticMeshComponent["Mesh"];
			if (meshHandle)
			{
				smc.Mesh = AssetRef<Mesh>(AssetHandle(meshHandle.as<uint64_t>()));

				// Resolved here rather than left to the first frame, as it always was. The
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

			auto scriptHandle = scriptComponent["Script"];
			if (scriptHandle)
			{
				// No GetAsset<> call to match the mesh path above: a script has no runtime
				// object to warm, and ScriptEngine loads the chunk itself on instantiation.
				sc.Script = scriptHandle.as<uint64_t>();
			}
			else
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

			auto envHandle = skyLightComponent["Environment"];
			if (envHandle)
				skc.Environment = AssetRef<Environment>(AssetHandle(envHandle.as<uint64_t>()));
			else
			{
				// Backward compatibility with path-based scenes
				auto envPath = skyLightComponent["EnvironmentPath"];
				if (envPath)
					skc.Environment = AssetRef<Environment>(AssetManager::ImportAsset(envPath.as<std::string>()));
			}
			skc.SkyColor = skyLightComponent["SkyColor"].as<glm::vec3>();
			skc.GroundColor = skyLightComponent["GroundColor"].as<glm::vec3>();
			skc.Intensity = skyLightComponent["Intensity"].as<float>();
			skc.DrawSkybox = skyLightComponent["DrawSkybox"].as<bool>();
		}

		auto audioSourceComponent = entityNode["AudioSourceComponent"];
		if (audioSourceComponent)
		{
			auto& source = deserializedEntity.AddComponent<AudioSourceComponent>();

			// Every field guarded, unlike RigidBodyComponent above. These components are
			// young enough that hand-authored scenes are still a normal way to make one
			// (the runtime demo is), and an absent key would otherwise throw out of
			// as<T>() and take the whole scene load with it.
			if (auto clip = audioSourceComponent["Clip"])
				source.Clip = clip.as<uint64_t>();
			if (auto group = audioSourceComponent["Group"])
				source.Group = AudioGroupFromString(group.as<std::string>(), source.Group);
			if (auto volume = audioSourceComponent["Volume"])
				source.Volume = volume.as<float>();
			if (auto pitch = audioSourceComponent["Pitch"])
				source.Pitch = pitch.as<float>();
			if (auto loop = audioSourceComponent["Loop"])
				source.Loop = loop.as<bool>();
			if (auto playOnStart = audioSourceComponent["PlayOnStart"])
				source.PlayOnStart = playOnStart.as<bool>();
			if (auto spatialize = audioSourceComponent["Spatialize"])
				source.Spatialize = spatialize.as<bool>();
			if (auto stream = audioSourceComponent["Stream"])
				source.Stream = stream.as<bool>();
		}

		auto audioListenerComponent = entityNode["AudioListenerComponent"];
		if (audioListenerComponent)
		{
			ReadReflectedComponent(audioListenerComponent, deserializedEntity.AddComponent<AudioListenerComponent>());
		}

		auto particleEmitterComponent = entityNode["ParticleEmitterComponent"];
		if (particleEmitterComponent)
		{
			auto& p = deserializedEntity.AddComponent<ParticleEmitterComponent>();

			if (auto n = particleEmitterComponent["RateOverTime"])
				p.RateOverTime = n.as<float>();
			if (auto n = particleEmitterComponent["MaxParticles"])
				p.MaxParticles = n.as<uint32_t>();
			if (auto n = particleEmitterComponent["Looping"])
				p.Looping = n.as<bool>();
			if (auto n = particleEmitterComponent["Duration"])
				p.Duration = n.as<float>();
			if (auto n = particleEmitterComponent["PlayOnStart"])
				p.PlayOnStart = n.as<bool>();

			if (auto n = particleEmitterComponent["LifetimeMin"])
				p.LifetimeMin = n.as<float>();
			if (auto n = particleEmitterComponent["LifetimeMax"])
				p.LifetimeMax = n.as<float>();
			if (auto n = particleEmitterComponent["SpeedMin"])
				p.SpeedMin = n.as<float>();
			if (auto n = particleEmitterComponent["SpeedMax"])
				p.SpeedMax = n.as<float>();
			if (auto n = particleEmitterComponent["ConeAngle"])
				p.ConeAngle = n.as<float>();
			if (auto n = particleEmitterComponent["StartSizeMin"])
				p.StartSizeMin = n.as<float>();
			if (auto n = particleEmitterComponent["StartSizeMax"])
				p.StartSizeMax = n.as<float>();
			if (auto n = particleEmitterComponent["StartRotationMin"])
				p.StartRotationMin = n.as<float>();
			if (auto n = particleEmitterComponent["StartRotationMax"])
				p.StartRotationMax = n.as<float>();
			if (auto n = particleEmitterComponent["RotationSpeedMin"])
				p.RotationSpeedMin = n.as<float>();
			if (auto n = particleEmitterComponent["RotationSpeedMax"])
				p.RotationSpeedMax = n.as<float>();
			if (auto n = particleEmitterComponent["GravityModifier"])
				p.GravityModifier = n.as<float>();
			if (auto n = particleEmitterComponent["WorldSpace"])
				p.WorldSpace = n.as<bool>();
			if (auto n = particleEmitterComponent["Seed"])
				p.Seed = n.as<uint32_t>();

			if (auto n = particleEmitterComponent["SizeCurve"])
				p.SizeCurve = n.as<FloatCurve>();
			if (auto n = particleEmitterComponent["ColorOverLifetime"])
				p.ColorOverLifetime = n.as<ColorGradient>();

			if (auto n = particleEmitterComponent["RenderMode"])
				p.RenderMode = (ParticleEmitterComponent::Mode)n.as<int>();
			if (auto n = particleEmitterComponent["Texture"])
				p.Texture = AssetRef<Texture2D>(AssetHandle(n.as<uint64_t>()));
			if (auto n = particleEmitterComponent["Blend"])
				p.Blend = (ParticleBlend)n.as<int>();
			if (auto n = particleEmitterComponent["Mesh"])
				p.Mesh = AssetRef<Mesh>(AssetHandle(n.as<uint64_t>()));
			if (auto n = particleEmitterComponent["Material"])
				p.Material = AssetRef<Material>(AssetHandle(n.as<uint64_t>()));
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
