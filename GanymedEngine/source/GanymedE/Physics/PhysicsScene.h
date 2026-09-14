#pragma once

#include "GanymedE/Core/Core.h"
#include "GanymedE/Core/UUID.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace GanymedE {

	class Scene;

	struct PhysicsCollisionEvent
	{
		UUID EntityA{ 0 };
		UUID EntityB{ 0 };
		bool Entered = true;
	};

	struct PhysicsDebugDrawSettings
	{
		bool Enabled = false;
		bool Wireframe = true;
		bool BoundingBoxes = false;
		bool Velocities = false;
		bool Constraints = false;
		bool CenterOfMass = false;
	};

	// Owns the Jolt world. Jolt types stay in the .cpp (pimpl) so engine headers stay clean.
	class PhysicsScene
	{
	public:
		PhysicsScene();
		~PhysicsScene();

		PhysicsScene(const PhysicsScene&) = delete;
		PhysicsScene& operator=(const PhysicsScene&) = delete;

		void Start(Scene* scene);
		void Stop();
		bool IsActive() const { return m_Active; }

		// Reconcile Jolt's bodies with the registry: create one for anything that has a
		// RigidBodyComponent and no body, destroy any whose entity or component is gone.
		//
		// **Once per frame, before stepping.** Body creation used to happen only in Start, which
		// was fine while nothing could create an entity mid-run; a prefab spawned by a script
		// would otherwise render and never simulate. Costs one hash lookup per rigid body when
		// nothing has changed.
		void SyncBodies(Scene* scene);

		void Step(float fixedDeltaTime);

		// alpha in [0,1]: blend previous→current physics poses into TransformComponents
		void SyncTransforms(Scene* scene, float alpha);

		// Submit Jolt debug geometry into Renderer3D's line batch (call between BeginScene/EndScene)
		void DebugDraw(const glm::vec3& cameraPosition, const PhysicsDebugDrawSettings& settings);

		const std::vector<PhysicsCollisionEvent>& GetCollisionEvents() const { return m_CollisionEvents; }
		void ClearCollisionEvents() { m_CollisionEvents.clear(); }

		// ---- Runtime body control, for gameplay scripts ----
		//
		// This is the route scripts must take to move a physics body. Writing the
		// TransformComponent instead fights the simulation: PhysicsScene::SyncTransforms
		// overwrites it from the body every step, so the write appears to do nothing for
		// dynamic bodies. All of these no-op on an entity with no body (static geometry,
		// or a call made outside play) rather than asserting - a script poking at the
		// wrong entity should not take the editor down.
		//
		// Jolt puts idle bodies to sleep, so each of these wakes the body; a velocity set
		// on a sleeping body would otherwise be quietly discarded.
		// Each of these accepts a character controller as well as a rigid body, because a script
		// should not have to know which one it is driving. AddForce is the exception: a
		// CharacterVirtual has no mass in the solver for a force to act on, so it warns once per
		// entity and does nothing. AddImpulse *is* meaningful - it becomes a velocity change of
		// impulse/Mass, which is what a jump or a knockback wants.
		void SetLinearVelocity(UUID entity, const glm::vec3& velocity);
		glm::vec3 GetLinearVelocity(UUID entity) const;
		void AddImpulse(UUID entity, const glm::vec3& impulse);
		void AddForce(UUID entity, const glm::vec3& force);
		bool HasBody(UUID entity) const;

		// True when a character controller is standing on ground it can walk on. False for a
		// steep slope it is sliding down, for freefall, and for every entity that is not a
		// character - a rigid body has no such concept and answering for one would invite
		// gameplay to ask the wrong question.
		bool IsGrounded(UUID entity) const;

		// ---- Queries ----

		// What a ray hit, or Hit == false. Entity is the UUID behind the body, so a caller
		// never sees a Jolt type; Distance is in world units along Direction, not Jolt's
		// [0,1] fraction, because a fraction is only meaningful next to the length that
		// produced it and call sites lose that.
		struct RaycastHit
		{
			bool Hit = false;
			UUID Entity = 0;
			glm::vec3 Point{ 0.0f };
			glm::vec3 Normal{ 0.0f };
			float Distance = 0.0f;
		};

		// Closest hit along the ray. `direction` need not be normalised; `maxDistance` is what
		// sets the reach either way.
		//
		// `ignore` exists because the overwhelmingly common caller is an entity casting from
		// its own position - a weapon, an eye - and its own collider is the first thing in the
		// way. Passing 0 ignores nothing.
		//
		// Hits static and dynamic bodies alike: a line-of-sight test that could not see walls
		// would be useless, and a weapon that could not hit scenery would be worse. Sensors
		// would be the thing to filter out here, and there are none yet.
		//
		// **Not safe to call while the simulation is stepping.** Scripts run outside the step,
		// so this is a rule about future engine code rather than about gameplay.
		RaycastHit CastRay(const glm::vec3& origin, const glm::vec3& direction,
			float maxDistance, UUID ignore = 0) const;

	private:
		struct BodyPose
		{
			glm::vec3 Position{ 0.0f };
			glm::quat Rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
		};

		void CreateBodies(Scene* scene);
		void CreateCharacters(Scene* scene);
		void RemoveDeadBodies(Scene* scene);
		void StepCharacters(float fixedDeltaTime);
		void DestroyBodies();
		void CapturePoses(std::unordered_map<UUID, BodyPose>& out);

	private:
		struct Impl;
		Scope<Impl> m_Impl;

		Scene* m_Scene = nullptr;
		bool m_Active = false;
		std::vector<PhysicsCollisionEvent> m_CollisionEvents;

		// Entities warned about for having a RigidBody and no collider. CreateBodies runs every
		// frame now, so without this the warning would scroll.
		std::unordered_set<UUID> m_WarnedNoCollider;

		std::unordered_map<UUID, BodyPose> m_PreviousPoses;
		std::unordered_map<UUID, BodyPose> m_CurrentPoses;
	};

}
