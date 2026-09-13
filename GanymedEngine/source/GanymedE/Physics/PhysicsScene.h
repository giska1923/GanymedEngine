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
		void SetLinearVelocity(UUID entity, const glm::vec3& velocity);
		glm::vec3 GetLinearVelocity(UUID entity) const;
		void AddImpulse(UUID entity, const glm::vec3& impulse);
		void AddForce(UUID entity, const glm::vec3& force);
		bool HasBody(UUID entity) const;

	private:
		struct BodyPose
		{
			glm::vec3 Position{ 0.0f };
			glm::quat Rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
		};

		void CreateBodies(Scene* scene);
		void RemoveDeadBodies(Scene* scene);
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
