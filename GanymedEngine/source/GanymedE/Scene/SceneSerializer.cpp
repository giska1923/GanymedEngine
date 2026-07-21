#include "gepch.h"
#include "SceneSerializer.h"

#include "Entity.h"
#include "Components.h"

#include "GanymedE/Assets/AssetManager.h"

#include <fstream>

#include <yaml-cpp/yaml.h>

namespace YAML {

	template<>
	struct convert<glm::vec3>
	{
		static Node encode(const glm::vec3& rhs)
		{
			Node node;
			node.push_back(rhs.x);
			node.push_back(rhs.y);
			node.push_back(rhs.z);
			return node;
		}

		static bool decode(const Node& node, glm::vec3& rhs)
		{
			if (!node.IsSequence() || node.size() != 3)
				return false;

			rhs.x = node[0].as<float>();
			rhs.y = node[1].as<float>();
			rhs.z = node[2].as<float>();
			return true;
		}
	};

	template<>
	struct convert<glm::vec4>
	{
		static Node encode(const glm::vec4& rhs)
		{
			Node node;
			node.push_back(rhs.x);
			node.push_back(rhs.y);
			node.push_back(rhs.z);
			node.push_back(rhs.w);
			return node;
		}

		static bool decode(const Node& node, glm::vec4& rhs)
		{
			if (!node.IsSequence() || node.size() != 4)
				return false;

			rhs.x = node[0].as<float>();
			rhs.y = node[1].as<float>();
			rhs.z = node[2].as<float>();
			rhs.w = node[3].as<float>();
			return true;
		}
	};

}
namespace GanymedE {

	YAML::Emitter& operator<<(YAML::Emitter& out, const glm::vec3& v)
	{
		out << YAML::Flow;
		out << YAML::BeginSeq << v.x << v.y << v.z << YAML::EndSeq;
		return out;
	}

	YAML::Emitter& operator<<(YAML::Emitter& out, const glm::vec4& v)
	{
		out << YAML::Flow;
		out << YAML::BeginSeq << v.x << v.y << v.z << v.w << YAML::EndSeq;
		return out;
	}

	SceneSerializer::SceneSerializer(const Ref<Scene>& scene)
		: m_Scene(scene)
	{
	}

	static void SerializeEntity(YAML::Emitter& out, Entity entity)
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

		if (entity.HasComponent<SpriteRendererComponent>())
		{
			out << YAML::Key << "SpriteRendererComponent";
			out << YAML::BeginMap; // SpriteRendererComponent

			auto& spriteRendererComponent = entity.GetComponent<SpriteRendererComponent>();
			out << YAML::Key << "Color" << YAML::Value << spriteRendererComponent.Color;

			out << YAML::EndMap; // SpriteRendererComponent
		}

		if (entity.HasComponent<StaticMeshComponent>())
		{
			out << YAML::Key << "StaticMeshComponent";
			out << YAML::BeginMap;

			auto& smc = entity.GetComponent<StaticMeshComponent>();
			if (IsAssetHandleValid(smc.Mesh))
				out << YAML::Key << "Mesh" << YAML::Value << static_cast<uint64_t>(smc.Mesh);

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
			out << YAML::Key << "DirectionalLightComponent";
			out << YAML::BeginMap;

			auto& dlc = entity.GetComponent<DirectionalLightComponent>();
			out << YAML::Key << "Color" << YAML::Value << dlc.Color;
			out << YAML::Key << "Intensity" << YAML::Value << dlc.Intensity;
			out << YAML::Key << "CastShadows" << YAML::Value << dlc.CastShadows;

			out << YAML::EndMap;
		}

		if (entity.HasComponent<PointLightComponent>())
		{
			out << YAML::Key << "PointLightComponent";
			out << YAML::BeginMap;

			auto& plc = entity.GetComponent<PointLightComponent>();
			out << YAML::Key << "Color" << YAML::Value << plc.Color;
			out << YAML::Key << "Intensity" << YAML::Value << plc.Intensity;
			out << YAML::Key << "Radius" << YAML::Value << plc.Radius;
			out << YAML::Key << "Falloff" << YAML::Value << plc.Falloff;

			out << YAML::EndMap;
		}

		if (entity.HasComponent<SpotLightComponent>())
		{
			out << YAML::Key << "SpotLightComponent";
			out << YAML::BeginMap;

			auto& slc = entity.GetComponent<SpotLightComponent>();
			out << YAML::Key << "Color" << YAML::Value << slc.Color;
			out << YAML::Key << "Intensity" << YAML::Value << slc.Intensity;
			out << YAML::Key << "Range" << YAML::Value << slc.Range;
			out << YAML::Key << "InnerConeAngle" << YAML::Value << slc.InnerConeAngle;
			out << YAML::Key << "OuterConeAngle" << YAML::Value << slc.OuterConeAngle;
			out << YAML::Key << "Falloff" << YAML::Value << slc.Falloff;

			out << YAML::EndMap;
		}

		if (entity.HasComponent<SkyLightComponent>())
		{
			out << YAML::Key << "SkyLightComponent";
			out << YAML::BeginMap;

			auto& skc = entity.GetComponent<SkyLightComponent>();
			if (IsAssetHandleValid(skc.Environment))
				out << YAML::Key << "Environment" << YAML::Value << static_cast<uint64_t>(skc.Environment);
			out << YAML::Key << "SkyColor" << YAML::Value << skc.SkyColor;
			out << YAML::Key << "GroundColor" << YAML::Value << skc.GroundColor;
			out << YAML::Key << "Intensity" << YAML::Value << skc.Intensity;
			out << YAML::Key << "DrawSkybox" << YAML::Value << skc.DrawSkybox;

			out << YAML::EndMap;
		}

		if (entity.HasComponent<RigidBodyComponent>())
		{
			out << YAML::Key << "RigidBodyComponent";
			out << YAML::BeginMap;
			auto& rb = entity.GetComponent<RigidBodyComponent>();
			out << YAML::Key << "Type" << YAML::Value << (int)rb.Type;
			out << YAML::Key << "Mass" << YAML::Value << rb.Mass;
			out << YAML::Key << "LinearDamping" << YAML::Value << rb.LinearDamping;
			out << YAML::Key << "AngularDamping" << YAML::Value << rb.AngularDamping;
			out << YAML::Key << "UseGravity" << YAML::Value << rb.UseGravity;
			out << YAML::EndMap;
		}

		auto serializePhysicsMaterial = [](YAML::Emitter& emitter, const PhysicsMaterial& mat)
		{
			emitter << YAML::Key << "Friction" << YAML::Value << mat.Friction;
			emitter << YAML::Key << "Restitution" << YAML::Value << mat.Restitution;
		};

		if (entity.HasComponent<BoxColliderComponent>())
		{
			out << YAML::Key << "BoxColliderComponent";
			out << YAML::BeginMap;
			auto& col = entity.GetComponent<BoxColliderComponent>();
			out << YAML::Key << "HalfExtents" << YAML::Value << col.HalfExtents;
			out << YAML::Key << "Offset" << YAML::Value << col.Offset;
			serializePhysicsMaterial(out, col.Material);
			out << YAML::EndMap;
		}

		if (entity.HasComponent<SphereColliderComponent>())
		{
			out << YAML::Key << "SphereColliderComponent";
			out << YAML::BeginMap;
			auto& col = entity.GetComponent<SphereColliderComponent>();
			out << YAML::Key << "Radius" << YAML::Value << col.Radius;
			out << YAML::Key << "Offset" << YAML::Value << col.Offset;
			serializePhysicsMaterial(out, col.Material);
			out << YAML::EndMap;
		}

		if (entity.HasComponent<CapsuleColliderComponent>())
		{
			out << YAML::Key << "CapsuleColliderComponent";
			out << YAML::BeginMap;
			auto& col = entity.GetComponent<CapsuleColliderComponent>();
			out << YAML::Key << "Radius" << YAML::Value << col.Radius;
			out << YAML::Key << "HalfHeight" << YAML::Value << col.HalfHeight;
			out << YAML::Key << "Offset" << YAML::Value << col.Offset;
			serializePhysicsMaterial(out, col.Material);
			out << YAML::EndMap;
		}

		out << YAML::EndMap; // Entity
	}

	void SceneSerializer::Serialize(const std::string& filepath)
	{
		YAML::Emitter out;
		out << YAML::BeginMap;
		out << YAML::Key << "Scene" << YAML::Value << "Untitled";
		out << YAML::Key << "Entities" << YAML::Value << YAML::BeginSeq;
		auto view = m_Scene->m_Registry.view<IDComponent>();
		for (auto entityID : view)
		{
			Entity entity = { entityID, m_Scene.get() };
			if (!entity)
				continue;

			SerializeEntity(out, entity);
		}
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

			for (auto entity : entities)
			{
				uint64_t uuid = entity["Entity"].as<uint64_t>();

				// Older scenes serialized a hardcoded ID for every entity — mint a fresh UUID on collision
				if (uuid == 0 || usedUUIDs.find(uuid) != usedUUIDs.end())
					uuid = static_cast<uint64_t>(UUID());
				usedUUIDs.insert(uuid);

				std::string name;
				auto tagComponent = entity["TagComponent"];
				if (tagComponent)
					name = tagComponent["Tag"].as<std::string>();

				GE_CORE_TRACE("Deserialized entity with ID = {0}, name = {1}", uuid, name);

				Entity deserializedEntity = m_Scene->CreateEntityWithUUID(uuid, name);

				auto transformComponent = entity["TransformComponent"];
				if (transformComponent)
				{
					// Entities always have transforms
					auto& tc = deserializedEntity.GetComponent<TransformComponent>();
					tc.Translation = transformComponent["Translation"].as<glm::vec3>();
					tc.Rotation = transformComponent["Rotation"].as<glm::vec3>();
					tc.Scale = transformComponent["Scale"].as<glm::vec3>();
				}

				auto relationshipComponent = entity["RelationshipComponent"];
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

				auto cameraComponent = entity["CameraComponent"];
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

				auto spriteRendererComponent = entity["SpriteRendererComponent"];
				if (spriteRendererComponent)
				{
					auto& src = deserializedEntity.AddComponent<SpriteRendererComponent>();
					src.Color = spriteRendererComponent["Color"].as<glm::vec4>();
				}

				auto staticMeshComponent = entity["StaticMeshComponent"];
				if (staticMeshComponent)
				{
					auto& smc = deserializedEntity.AddComponent<StaticMeshComponent>();

					auto meshHandle = staticMeshComponent["Mesh"];
					if (meshHandle)
					{
						smc.Mesh = meshHandle.as<uint64_t>();
						AssetManager::GetAsset<Mesh>(smc.Mesh);
					}
					else
					{
						// Backward compatibility with path-based scenes
						auto meshPath = staticMeshComponent["MeshPath"];
						if (meshPath)
							smc.Mesh = AssetManager::ImportAsset(meshPath.as<std::string>());
					}
				}

				auto scriptComponent = entity["ScriptComponent"];
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

				auto directionalLightComponent = entity["DirectionalLightComponent"];
				if (directionalLightComponent)
				{
					auto& dlc = deserializedEntity.AddComponent<DirectionalLightComponent>();
					dlc.Color = directionalLightComponent["Color"].as<glm::vec3>();
					dlc.Intensity = directionalLightComponent["Intensity"].as<float>();
					dlc.CastShadows = directionalLightComponent["CastShadows"].as<bool>();
				}

				auto pointLightComponent = entity["PointLightComponent"];
				if (pointLightComponent)
				{
					auto& plc = deserializedEntity.AddComponent<PointLightComponent>();
					plc.Color = pointLightComponent["Color"].as<glm::vec3>();
					plc.Intensity = pointLightComponent["Intensity"].as<float>();
					plc.Radius = pointLightComponent["Radius"].as<float>();
					plc.Falloff = pointLightComponent["Falloff"].as<float>();
				}

				auto spotLightComponent = entity["SpotLightComponent"];
				if (spotLightComponent)
				{
					auto& slc = deserializedEntity.AddComponent<SpotLightComponent>();
					slc.Color = spotLightComponent["Color"].as<glm::vec3>();
					slc.Intensity = spotLightComponent["Intensity"].as<float>();
					slc.Range = spotLightComponent["Range"].as<float>();
					slc.InnerConeAngle = spotLightComponent["InnerConeAngle"].as<float>();
					slc.OuterConeAngle = spotLightComponent["OuterConeAngle"].as<float>();
					slc.Falloff = spotLightComponent["Falloff"].as<float>();
				}

				auto skyLightComponent = entity["SkyLightComponent"];
				if (skyLightComponent)
				{
					auto& skc = deserializedEntity.AddComponent<SkyLightComponent>();

					auto envHandle = skyLightComponent["Environment"];
					if (envHandle)
						skc.Environment = envHandle.as<uint64_t>();
					else
					{
						// Backward compatibility with path-based scenes
						auto envPath = skyLightComponent["EnvironmentPath"];
						if (envPath)
							skc.Environment = AssetManager::ImportAsset(envPath.as<std::string>());
					}
					skc.SkyColor = skyLightComponent["SkyColor"].as<glm::vec3>();
					skc.GroundColor = skyLightComponent["GroundColor"].as<glm::vec3>();
					skc.Intensity = skyLightComponent["Intensity"].as<float>();
					skc.DrawSkybox = skyLightComponent["DrawSkybox"].as<bool>();
				}

				auto rigidBodyComponent = entity["RigidBodyComponent"];
				if (rigidBodyComponent)
				{
					auto& rb = deserializedEntity.AddComponent<RigidBodyComponent>();
					rb.Type = (RigidBodyType)rigidBodyComponent["Type"].as<int>();
					rb.Mass = rigidBodyComponent["Mass"].as<float>();
					rb.LinearDamping = rigidBodyComponent["LinearDamping"].as<float>();
					rb.AngularDamping = rigidBodyComponent["AngularDamping"].as<float>();
					rb.UseGravity = rigidBodyComponent["UseGravity"].as<bool>();
				}

				auto readPhysicsMaterial = [](const YAML::Node& node, PhysicsMaterial& mat)
				{
					if (node["Friction"])
						mat.Friction = node["Friction"].as<float>();
					if (node["Restitution"])
						mat.Restitution = node["Restitution"].as<float>();
				};

				auto boxColliderComponent = entity["BoxColliderComponent"];
				if (boxColliderComponent)
				{
					auto& col = deserializedEntity.AddComponent<BoxColliderComponent>();
					col.HalfExtents = boxColliderComponent["HalfExtents"].as<glm::vec3>();
					col.Offset = boxColliderComponent["Offset"].as<glm::vec3>();
					readPhysicsMaterial(boxColliderComponent, col.Material);
				}

				auto sphereColliderComponent = entity["SphereColliderComponent"];
				if (sphereColliderComponent)
				{
					auto& col = deserializedEntity.AddComponent<SphereColliderComponent>();
					col.Radius = sphereColliderComponent["Radius"].as<float>();
					col.Offset = sphereColliderComponent["Offset"].as<glm::vec3>();
					readPhysicsMaterial(sphereColliderComponent, col.Material);
				}

				auto capsuleColliderComponent = entity["CapsuleColliderComponent"];
				if (capsuleColliderComponent)
				{
					auto& col = deserializedEntity.AddComponent<CapsuleColliderComponent>();
					col.Radius = capsuleColliderComponent["Radius"].as<float>();
					col.HalfHeight = capsuleColliderComponent["HalfHeight"].as<float>();
					col.Offset = capsuleColliderComponent["Offset"].as<glm::vec3>();
					readPhysicsMaterial(capsuleColliderComponent, col.Material);
				}
			}
		}

		return true;
	}

	bool SceneSerializer::DeserializeRuntime(const std::string& filepath)
	{
		// Not implemented
		GE_CORE_ASSERT(false, "not implemented!");
		return false;
	}

}
