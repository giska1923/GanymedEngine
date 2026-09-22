#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "SceneCamera.h"
#include "ScriptableEntity.h"
#include "GanymedE/Core/UUID.h"
#include "GanymedE/Core/Core.h"
#include "GanymedE/Core/Random.h"
#include "GanymedE/Assets/AssetRef.h"
#include "GanymedE/Assets/AssetTypes.h"
#include "GanymedE/Audio/AudioTypes.h"
#include "GanymedE/Math/BoundingVolumes.h"
#include "GanymedE/Math/Curve.h"

#include <cstdint>
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
		// Qualified because the member name shadows the class name for the rest of this scope -
		// same for Material and Environment below.
		AssetRef<GanymedE::Mesh> Mesh;

		// Per renderer slot, parallel to the mesh's own material list (the index is
		// Submesh::MaterialIndex). InvalidAssetHandle - or an index past the end - means "use
		// the material that came with the mesh", so an entity with no overrides renders exactly
		// as it did before this existed.
		//
		// This is the additive half of the material model: the mesh asset keeps its imported
		// materials untouched inside its own cache, and .gmat is a layer on top. See
		// docs/engine/assets.md for why that shape was chosen over meshes referencing .gmat
		// directly the way Unreal does.
		std::vector<AssetRef<GanymedE::Material>> MaterialOverrides;

		StaticMeshComponent() = default;
		StaticMeshComponent(const StaticMeshComponent&) = default;
		StaticMeshComponent(AssetHandle mesh)
			: Mesh(mesh) {}
	};

	// Marks an entity as the root of a linked prefab instance.
	//
	// Only the root carries it - the descendants are ordinary entities, which is what makes
	// structural editing inside an instance free: add, remove and re-parent children at will,
	// because nothing tracks divergence. "Apply to prefab" captures whatever the subtree is now
	// and "Revert instance" discards it. Per-field overrides would need a serialization-diff
	// engine, which is a milestone of its own rather than a feature.
	//
	// Inert at runtime: it exists so the editor can find the source file again.
	// "Which object of the prefab did this entity come from."
	//
	// Present on **every** entity instantiated from a `.gprefab`, root included, where
	// PrefabInstanceComponent marks only the root - keeping "is this an instance root" the same
	// question it has always been.
	//
	// `CanonicalID` is the entity's id *inside the prefab file*, which PrefabSerializer assigns as
	// 1..N in DFS order when the prefab is written. That is what makes a stable per-property
	// override possible at all: an instance's entities get fresh UUIDs, so without this link
	// nothing can say which prefab object a given instance entity corresponds to, and an override
	// would have nothing to key on. Structural edits inside an instance stay free - an entity
	// added by hand simply has no PrefabMemberComponent and is not part of any diff.
	struct PrefabMemberComponent
	{
		UUID CanonicalID{ 0 };

		PrefabMemberComponent() = default;
		PrefabMemberComponent(const PrefabMemberComponent&) = default;
		PrefabMemberComponent(UUID canonicalID)
			: CanonicalID(canonicalID) {}
	};

	struct PrefabInstanceComponent
	{
		AssetHandle Source = InvalidAssetHandle;

		PrefabInstanceComponent() = default;
		PrefabInstanceComponent(const PrefabInstanceComponent&) = default;
		PrefabInstanceComponent(AssetHandle source)
			: Source(source) {}
	};

	// Folder entity for Map-panel scatter. Inert at runtime: identity for the eraser
	// (children of this entity, matching Source) and the last stroke seed so a noted
	// seed can be typed back into the brush. Not a foliage instance array — each child
	// is an ordinary entity. See docs/ToDo/MAP_EDITOR.md M3.
	struct ScatterGroupComponent
	{
		AssetHandle Source = InvalidAssetHandle;
		uint32_t LastSeed = 0;

		ScatterGroupComponent() = default;
		ScatterGroupComponent(const ScatterGroupComponent&) = default;
	};

	// Gameplay marker: spawn points, patrol nodes, triggers. Kind is a string so a game can
	// invent "Patrol" without an engine change (the branch policy forbids the game from
	// touching this header). Color / Size / DrawForward are editor visualization, not gameplay
	// data — wait times and teams stay on ScriptComponent. Inert at runtime except as a query
	// target for Scene.FindMarkers. See docs/engine/scene.md.
	struct MarkerComponent
	{
		std::string Kind = "Spawn";
		glm::vec4 Color{ 0.2f, 0.9f, 0.35f, 1.0f };
		float Size = 0.5f;
		bool DrawForward = true;

		MarkerComponent() = default;
		MarkerComponent(const MarkerComponent&) = default;
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

	// Pins this entity to a joint of a skinned mesh. The joint transform is recovered from
	// AnimatorComponent::Palette each frame via TryGetJointFrame rather than stored beside it:
	// most entities never attach anything, and keeping a second per-joint array would add
	// 2-8 KB per animator for Scene::Copy to shuffle on every play.
	//
	// Writes WorldTransformComponent directly, after TransformSystem, because feeding a joint
	// quaternion through TransformComponent's Euler storage is lossy. Local Translation and
	// Rotation are ignored while the socket resolves — Offset and Rotation on *this* component
	// replace them, as the authored rest pose in joint space. Local **Scale** is kept: there is
	// no counterpart here, and using the same field on both paths means a socket that fails to
	// resolve shows a correctly sized prop rather than a compensated one.
	//
	// Offset is in the target mesh's own units — metres for any mesh authored that way — whatever
	// unit the *rig* uses: TryGetJointFrame folds in the skinned submesh's LocalTransform and
	// divides the bind pose's basis scale back out (see Mesh.h). What the *clip* does to the
	// joint chain still carries, scale included — a constant scale on Hips will grow the attached
	// entity for as long as that clip plays, which is a clip bug, not something this component
	// papers over.
	struct BoneAttachmentComponent
	{
		// The entity carrying the skinned mesh and its AnimatorComponent. Zero means "my parent",
		// which is the common case and keeps simple setups from having to name anything. This is
		// an *entity* UUID, not an asset handle: DuplicateEntity / ResolveHierarchy remap it
		// through the entity map, which asset handles will not be keys of.
		UUID Target{ 0 };

		// By name, like AnimatorComponent::Clip — and for the same reason: a rename in the DCC
		// should fail loudly rather than silently attach to whatever joint 7 happens to be.
		std::string Joint;

		glm::vec3 Offset{ 0.0f };
		glm::vec3 Rotation{ 0.0f };   // Euler radians, X·Y·Z, matching TransformComponent

		// Runtime. Not serialized; re-resolved when the target's mesh or this Joint name changes.
		int32_t Resolved = -1;

		BoneAttachmentComponent() = default;
		BoneAttachmentComponent(const BoneAttachmentComponent&) = default;
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
		AssetRef<GanymedE::Environment> Environment;
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

	// A min/max pair authored as one thing.
	//
	// Introduced so the particle emitter's five ranges could go through the generic inspector.
	// The blocker was never the drawing - it was the **clamp direction**: the hand-written panel
	// pushes Max up when Min passes it and pulls Min down when Max drops below, and a generic
	// drawer that sees one field at a time cannot know which half the author just moved. One
	// drawer owning both halves does know, which is the whole reason this is a type rather than a
	// naming convention over two floats.
	//
	// **Layout-identical to the two floats it replaced** (`float Min, Max;` in that order), and
	// both the YAML keys and the Lua binding names were deliberately kept as they were - see
	// SceneSerializer and ScriptBindings. Nothing on disk or in a script had to change.
	struct RangeF
	{
		float Min = 0.0f;
		float Max = 0.0f;

		RangeF() = default;
		RangeF(float min, float max) : Min(min), Max(max) {}
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

		// Forbid rotation entirely, keeping the body's authored orientation while it still
		// translates and collides. This is what a walking character needs: a dynamic capsule
		// driven by velocity torques itself over the moment it brushes anything, and lies down.
		//
		// Implemented with Jolt's mAllowedDOFs rather than by cranking AngularDamping, which
		// only slows a fall down, or by zeroing the inertia tensor by hand, which Jolt then
		// recomputes. Dynamic bodies only - Static does not move and Kinematic is already
		// driven entirely by its transform.
		//
		// Read at body creation, so toggling it during play does nothing until the body is
		// rebuilt. That is a limitation, not a design: it is a per-frame push to Jolt whenever
		// anyone needs it. See docs/engine/physics.md.
		bool LockRotation = false;

		// A trigger volume: it reports contacts and causes none. Jolt calls this a sensor, so
		// this does too rather than inventing a synonym.
		//
		// **Static is the right motion type for one**, and the cheapest: a static sensor costs
		// nothing in the broadphase and still detects every active Dynamic or Kinematic body
		// that enters it. A Dynamic or Kinematic sensor additionally sees *sleeping* bodies,
		// which is rarely what a pickup wants and always costs more.
		//
		// This is a body-level flag because a body here is one compound shape. Unity puts
		// `isTrigger` on the collider because a Unity body can own several colliders with
		// different roles; ours cannot, so per-collider would be a lie.
		bool IsSensor = false;

		RigidBodyComponent() = default;
		RigidBodyComponent(const RigidBodyComponent&) = default;
	};

	// A walking character, as opposed to a thing that falls over.
	//
	// Not a RigidBodyComponent with extra fields, and not a flag on one, because a
	// CharacterVirtual is not a body: it has no mass in the solver, nothing pushes it, and it
	// moves by collide-and-slide rather than by integration. Authoring it as its own component
	// is what Unity and Unreal both do, for the same reason.
	//
	// Takes its shape from a CapsuleColliderComponent, the same rule rigid bodies follow. An
	// entity with both this and a RigidBodyComponent is a contradiction; the character wins and
	// the body is skipped, with one warning naming the entity.
	struct CharacterControllerComponent
	{
		// Steeper than this and the character slides rather than climbing. 50 deg is Jolt's
		// default and roughly the games convention.
		float MaxSlopeAngle = 50.0f;

		// How high a ledge the character steps onto rather than stopping at. This is the thing a
		// velocity-driven rigid body cannot do at all; a capsule's bottom hemisphere happens to
		// ride small kerbs, but nothing above them.
		float StepHeight = 0.4f;

		// Pushes the character back down onto the floor after a step, so walking off a shallow
		// rise does not launch it into a ballistic arc. Off means it leaves the ground on every
		// bump.
		bool StickToFloor = true;

		// Only used against *dynamic* bodies the character pushes. It has no bearing on how the
		// character itself moves - nothing accelerates it but its own velocity.
		float Mass = 70.0f;

		CharacterControllerComponent() = default;
		CharacterControllerComponent(const CharacterControllerComponent&) = default;
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

	// Component-local, firewalled: RenderState.h packs BGFX_STATE_* and must not reach
	// Components.h (the AudioTypes.h split is the precedent). The renderer maps this to
	// RenderState::BlendMode at draw time (Phase 3).
	enum class ParticleBlend : uint8_t { Alpha = 0, Additive = 1 };

	struct Particle
	{
		glm::vec3 Position{ 0.0f };
		glm::vec3 Velocity{ 0.0f };
		float Rotation = 0.0f;      // degrees
		float RotationSpeed = 0.0f; // deg/sec, constant for the particle's life
		float Age = 0.0f;
		float Lifetime = 1.0f;
		float StartSize = 0.1f;
	};

	// CPU particle emitter. Authored fields live on the component and serialize with the
	// scene; there is no .gparticle asset (prefabs are the reuse vehicle).
	//
	// The pool is component-owned, like AnimatorComponent::Palette, with the honest
	// difference that a pool persists across frames. Scene::Copy therefore resets pool,
	// accumulator, timer, Playing, bounds, and RNG together — play mode starts empty and
	// emitters warm up, the Unity-without-prewarm cost. Runtime fields are not serialized.
	struct ParticleEmitterComponent
	{
		// Emission
		float    RateOverTime = 10.0f;
		uint32_t MaxParticles = 1000;
		bool     Looping = true;
		float    Duration = 5.0f;
		bool     PlayOnStart = true;

		// Initial state (cone axis = entity local +Y; rotate the entity to aim)
		RangeF   Lifetime{ 1.0f, 1.0f };
		RangeF   Speed{ 1.0f, 1.0f };
		float    ConeAngle = 25.0f;
		RangeF   StartSize{ 0.1f, 0.1f };
		RangeF   StartRotation{ 0.0f, 0.0f };
		RangeF   RotationSpeed{ 0.0f, 0.0f };
		float    GravityModifier = 0.0f;
		bool     WorldSpace = false;
		uint32_t Seed = 0; // 0 = derive from entity UUID when playback starts

		FloatCurve    SizeCurve;
		ColorGradient ColorOverLifetime;

		enum class Mode : uint8_t { Billboard = 0, Mesh = 1 };
		Mode          RenderMode = Mode::Billboard;
		AssetRef<Texture2D>            Texture;
		ParticleBlend Blend    = ParticleBlend::Alpha;
		AssetRef<GanymedE::Mesh>       Mesh;
		AssetRef<GanymedE::Material>   Material;

		// Runtime-only. Serializer skips; Scene::Copy resets via ResetRuntime().
		bool Playing = false;
		float Time = 0.0f;
		float EmitAccumulator = 0.0f;
		uint32_t BurstPending = 0; // Lua EmitBurst; consumed this tick while Playing
		Random Rng{ 0 };
		std::vector<Particle> Pool;
		AABB WorldBounds;

		void ResetRuntime()
		{
			Playing = false;
			Time = 0.0f;
			EmitAccumulator = 0.0f;
			BurstPending = 0;
			Rng = Random{ 0 };
			Pool.clear();
			WorldBounds = {};
		}

		bool IsFresh() const { return Time == 0.0f && Pool.empty(); }

		void SeedRng(UUID uuid)
		{
			if (Seed != 0)
			{
				Rng = Random(Seed);
				return;
			}

			const uint64_t u = static_cast<uint64_t>(uuid);
			Rng = Random(static_cast<uint32_t>(u ^ (u >> 32)));
		}

		// Inspector / Lua playback. Play is a no-op on an already-playing emitter (the
		// PlayAnimation trap: a per-frame call must not restart). RNG reseeds only from
		// a fresh state — Stop then Play resumes the same stream; Restart seeds itself
		// because ParticleSystem has already run this frame and will not see a rising edge.
		void PlayPreview(UUID uuid)
		{
			if (Playing)
				return;
			Playing = true;
			if (IsFresh())
				SeedRng(uuid);
		}

		void StopPreview() { Playing = false; }

		void RestartPreview(UUID uuid)
		{
			ResetRuntime();
			Playing = true;
			SeedRng(uuid);
		}

		ParticleEmitterComponent() = default;
		ParticleEmitterComponent(const ParticleEmitterComponent&) = default;
	};
}
