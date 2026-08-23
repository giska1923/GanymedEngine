#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "SceneCamera.h"
#include "ScriptableEntity.h"
#include "GanymedE/Core/UUID.h"
#include "GanymedE/Core/Core.h"
#include "GanymedE/Assets/AssetTypes.h"
#include "GanymedE/Audio/AudioTypes.h"

#include <unordered_map>
#include <variant>

namespace GanymedE {

	struct IDComponent
	{
		UUID ID;

		IDComponent() = default;
		IDComponent(const IDComponent&) = default;
		IDComponent(UUID uuid)
			: ID(uuid) {}
	};

	struct TagComponent
	{
		std::string Tag;

		TagComponent() = default;
		TagComponent(const TagComponent&) = default;
		TagComponent(const std::string& tag)
			: Tag(tag) {}
	};

	struct TransformComponent
	{
		glm::vec3 Translation = { 0.0f, 0.0f, 0.0f };
		glm::vec3 Rotation = { 0.0f, 0.0f, 0.0f };
		glm::vec3 Scale = { 1.0f, 1.0f, 1.0f };

		TransformComponent() = default;
		TransformComponent(const TransformComponent&) = default;
		TransformComponent(const glm::vec3& translation)
			: Translation(translation) {}

		glm::mat4 GetLocalTransform() const
		{
			glm::mat4 rotation = glm::rotate(glm::mat4(1.0f), Rotation.x, { 1, 0, 0 })
				* glm::rotate(glm::mat4(1.0f), Rotation.y, { 0, 1, 0 })
				* glm::rotate(glm::mat4(1.0f), Rotation.z, { 0, 0, 1 });

			return glm::translate(glm::mat4(1.0f), Translation)
				* rotation
				* glm::scale(glm::mat4(1.0f), Scale);
		}

		// Back-compat alias used throughout the engine/editor
		glm::mat4 GetTransform() const { return GetLocalTransform(); }
	};

	// Cached world-space transform, maintained by TransformSystem from TransformComponent and
	// RelationshipComponent. Derived data: never authored, never serialized, and only recomputed
	// for entities whose local transform or parenting actually changed.
	//
	// Because rendering reads this instead of walking the parent chain, anything that writes a
	// TransformComponent directly (rather than through a view's Modify()) must call
	// Scene::MarkChanged<TransformComponent>() or this cache goes silently stale.
	struct WorldTransformComponent
	{
		glm::mat4 World{ 1.0f };

		WorldTransformComponent() = default;
		WorldTransformComponent(const WorldTransformComponent&) = default;
	};

	struct RelationshipComponent
	{
		UUID Parent{ 0 };
		std::vector<UUID> Children;

		RelationshipComponent() = default;
		RelationshipComponent(const RelationshipComponent&) = default;
	};

	struct SpriteRendererComponent
	{
		glm::vec4 Color{ 1.0f, 1.0f, 1.0f, 1.0f };

		SpriteRendererComponent() = default;
		SpriteRendererComponent(const SpriteRendererComponent&) = default;
		SpriteRendererComponent(const glm::vec4& color)
			: Color(color) {}
	};

	struct StaticMeshComponent
	{
		AssetHandle Mesh = InvalidAssetHandle;

		StaticMeshComponent() = default;
		StaticMeshComponent(const StaticMeshComponent&) = default;
		StaticMeshComponent(AssetHandle mesh)
			: Mesh(mesh) {}
	};

	// Plays one of the clips carried by the entity's StaticMeshComponent mesh. There is no
	// SkinnedMeshComponent: an entity is skinned iff its mesh asset HasSkeleton() and it has an
	// animator, so a second mesh component would duplicate drag-drop, serialization, inspector and
	// RenderSystem plumbing to express something the asset already knows.
	//
	// Clips are referenced by name rather than index because indices shift whenever a DCC
	// reorders or adds a clip on re-export. A name that no longer resolves warns once and falls
	// back to the bind pose - see AnimationSystem.
	struct AnimatorComponent
	{
		std::string Clip;
		float Speed = 1.0f;
		bool Playing = true;
		bool Loop = true;

		// Not serialized: a scene should load at the start of its clip, not wherever it was saved.
		float Time = 0.0f;

		// Runtime-only, rebuilt every frame by AnimationSystem and cleared by the Scene::Copy
		// fixup. Lives here rather than in system-owned storage so its lifetime is the entity's
		// and RenderSystem can reach it through declared access instead of another system's map.
		std::vector<glm::mat4> Palette;

		AnimatorComponent() = default;
		AnimatorComponent(const AnimatorComponent&) = default;
	};

	struct CameraComponent
	{
		SceneCamera Camera;
		bool Primary = true; // TODO: think about moving to Scene
		bool FixedAspectRatio = false;

		CameraComponent() = default;
		CameraComponent(const CameraComponent&) = default;
	};

	// The light "direction" is the entity's forward vector (-Z of its world transform).
	struct DirectionalLightComponent
	{
		glm::vec3 Color{ 1.0f, 1.0f, 1.0f };
		float Intensity = 1.0f;
		bool CastShadows = true;

		DirectionalLightComponent() = default;
		DirectionalLightComponent(const DirectionalLightComponent&) = default;
	};

	struct PointLightComponent
	{
		glm::vec3 Color{ 1.0f, 1.0f, 1.0f };
		float Intensity = 1.0f;
		float Radius = 10.0f;
		float Falloff = 1.0f;

		PointLightComponent() = default;
		PointLightComponent(const PointLightComponent&) = default;
	};

	struct SpotLightComponent
	{
		glm::vec3 Color{ 1.0f, 1.0f, 1.0f };
		float Intensity = 1.0f;
		float Range = 10.0f;
		// Cone half-angles stored in radians
		float InnerConeAngle = glm::radians(20.0f);
		float OuterConeAngle = glm::radians(30.0f);
		float Falloff = 1.0f;

		SpotLightComponent() = default;
		SpotLightComponent(const SpotLightComponent&) = default;
	};

	// Environment / image-based lighting. When Environment is set, a baked HDR
	// cubemap drives the skybox and IBL; otherwise the procedural hemispheric colors are used.
	struct SkyLightComponent
	{
		AssetHandle Environment = InvalidAssetHandle;
		glm::vec3 SkyColor{ 0.45f, 0.62f, 0.9f };
		glm::vec3 GroundColor{ 0.28f, 0.26f, 0.22f };
		float Intensity = 1.0f;
		bool DrawSkybox = true;

		SkyLightComponent() = default;
		SkyLightComponent(const SkyLightComponent&) = default;
	};

	struct NativeScriptComponent
	{
		ScriptableEntity* Instance = nullptr;

		ScriptableEntity*(*InstantiateScript)() = nullptr;
		void(*DestroyScript)(NativeScriptComponent*) = nullptr;

		template<typename T>
		void Bind()
		{
			InstantiateScript = []() { return static_cast<ScriptableEntity*>(new T()); };
			DestroyScript = [](NativeScriptComponent* nsc) { delete nsc->Instance; nsc->Instance = nullptr; };
		}
	};

	// One tunable value on a script, as stored per entity.
	//
	// The alternative shapes: a raw string (loses type, and the editor could not pick a widget) or
	// a sol::object (would drag sol2 into this header, which the whole ScriptComponent design
	// exists to avoid). A closed variant keeps both the type and the header boundary.
	//
	// Deliberately NOT every Lua type: tables and functions are not tunable data, and permitting
	// them would mean serializing arbitrary graphs.
	//
	// Every number is a double, even though Lua 5.4 does distinguish integers from floats. That
	// distinction cannot survive the TypeScript path - TS has a single number type, so TSTL emits
	// `3` for a declared `3.0` - and honouring it would make the same property behave differently
	// depending on which language authored the script. The failure is asymmetric, too: a float
	// field wrongly given an integer widget can never be set to 3.5, while an integer field given
	// a float widget is merely untidy. So: one number type.
	using ScriptFieldValue = std::variant<bool, double, std::string, glm::vec3>;

	// A Lua gameplay script attached to this entity.
	//
	// Every sol2 object (the class table, the per-entity instance table) still lives inside
	// ScriptEngine, keyed by UUID - that is what keeps sol2 out of this header, which is included
	// almost everywhere and would otherwise pay for sol2's templates everywhere.
	//
	// `Fields` holds **only the values this entity overrides**, never a full copy of the script's
	// defaults. That is the load-bearing choice: editing a default in the .lua then propagates to
	// every entity that did not explicitly change it, which is what anyone tuning a script expects.
	// A full snapshot would freeze each entity at whatever the defaults were when it was created.
	struct ScriptComponent
	{
		AssetHandle Script = InvalidAssetHandle;   // a .lua asset
		std::unordered_map<std::string, ScriptFieldValue> Fields;

		ScriptComponent() = default;
		ScriptComponent(const ScriptComponent&) = default;
		ScriptComponent(AssetHandle script)
			: Script(script) {}
	};

	// A sound emitter. Everything here is AUTHORED state - there is deliberately no VoiceId
	// and no "is playing" flag on the component, because the live voice is a foreign
	// resource with a lifecycle, not data. AudioSystem owns it in a map keyed by entity,
	// the way PhysicsSystem owns Jolt bodies; a component carrying one would leak runtime
	// state into the serializer's field of view, need a Scene::Copy fixup, and let a copied
	// scene double-drive one sound. With none here, Scene::Copy is trivially correct.
	//
	// Clip, Spatialize and Stream are read once, when the voice is created. Changing them
	// during play does nothing until the voice is rebuilt; Volume, Pitch and Loop are pushed
	// every frame.
	struct AudioSourceComponent
	{
		AssetHandle Clip = InvalidAssetHandle;   // a .wav/.mp3/.flac asset

		float Volume = 1.0f;
		float Pitch = 1.0f;

		bool Loop = false;
		bool PlayOnStart = false;
		bool Spatialize = true;
		bool Stream = false;   // decode on the fly (music) instead of into memory (SFX)

		AudioGroup Group = AudioGroup::SFX;

		AudioSourceComponent() = default;
		AudioSourceComponent(const AudioSourceComponent&) = default;
	};

	// The ear. Unity's model: a component you place, normally on the camera, exactly one
	// active. Explicit rather than implicit-on-the-camera because third-person games put the
	// listener between the camera and the character; a scene with no listener at all falls
	// back to the primary camera's pose, so the common case still needs no authoring.
	struct AudioListenerComponent
	{
		bool Primary = true;

		AudioListenerComponent() = default;
		AudioListenerComponent(const AudioListenerComponent&) = default;
	};

	struct PhysicsMaterial
	{
		float Friction = 0.5f;
		float Restitution = 0.0f;
	};

	enum class RigidBodyType : uint8_t
	{
		Static = 0,
		Dynamic,
		Kinematic
	};

	struct RigidBodyComponent
	{
		RigidBodyType Type = RigidBodyType::Dynamic;
		float Mass = 1.0f;
		float LinearDamping = 0.05f;
		float AngularDamping = 0.05f;
		bool UseGravity = true;

		RigidBodyComponent() = default;
		RigidBodyComponent(const RigidBodyComponent&) = default;
	};

	struct BoxColliderComponent
	{
		glm::vec3 HalfExtents{ 0.5f, 0.5f, 0.5f };
		glm::vec3 Offset{ 0.0f, 0.0f, 0.0f };
		PhysicsMaterial Material;

		BoxColliderComponent() = default;
		BoxColliderComponent(const BoxColliderComponent&) = default;
	};

	struct SphereColliderComponent
	{
		float Radius = 0.5f;
		glm::vec3 Offset{ 0.0f, 0.0f, 0.0f };
		PhysicsMaterial Material;

		SphereColliderComponent() = default;
		SphereColliderComponent(const SphereColliderComponent&) = default;
	};

	struct CapsuleColliderComponent
	{
		float Radius = 0.5f;
		float HalfHeight = 0.5f; // half-length of the cylindrical section
		glm::vec3 Offset{ 0.0f, 0.0f, 0.0f };
		PhysicsMaterial Material;

		CapsuleColliderComponent() = default;
		CapsuleColliderComponent(const CapsuleColliderComponent&) = default;
	};
}
