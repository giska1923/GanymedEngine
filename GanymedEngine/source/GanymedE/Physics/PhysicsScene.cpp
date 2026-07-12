#include "gepch.h"
#include "PhysicsScene.h"

#include "GanymedE/Scene/Scene.h"
#include "GanymedE/Scene/Entity.h"
#include "GanymedE/Scene/Components.h"
#include "GanymedE/Renderer/Renderer3D.h"

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/ContactListener.h>

#ifdef JPH_DEBUG_RENDERER
	#include <Jolt/Renderer/DebugRendererSimple.h>
#endif

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtx/norm.hpp>

#include <cstdarg>
#include <mutex>
#include <thread>

JPH_SUPPRESS_WARNINGS

namespace GanymedE {

	namespace Layers
	{
		static constexpr JPH::ObjectLayer NON_MOVING = 0;
		static constexpr JPH::ObjectLayer MOVING = 1;
		static constexpr JPH::ObjectLayer NUM_LAYERS = 2;
	}

	class ObjectLayerPairFilterImpl : public JPH::ObjectLayerPairFilter
	{
	public:
		bool ShouldCollide(JPH::ObjectLayer inObject1, JPH::ObjectLayer inObject2) const override
		{
			switch (inObject1)
			{
				case Layers::NON_MOVING: return inObject2 == Layers::MOVING;
				case Layers::MOVING:     return true;
				default:                 return false;
			}
		}
	};

	namespace BroadPhaseLayers
	{
		static constexpr JPH::BroadPhaseLayer NON_MOVING(0);
		static constexpr JPH::BroadPhaseLayer MOVING(1);
		static constexpr uint32_t NUM_LAYERS = 2;
	}

	class BPLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface
	{
	public:
		BPLayerInterfaceImpl()
		{
			m_ObjectToBroadPhase[Layers::NON_MOVING] = BroadPhaseLayers::NON_MOVING;
			m_ObjectToBroadPhase[Layers::MOVING] = BroadPhaseLayers::MOVING;
		}

		uint32_t GetNumBroadPhaseLayers() const override { return BroadPhaseLayers::NUM_LAYERS; }

		JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer inLayer) const override
		{
			JPH_ASSERT(inLayer < Layers::NUM_LAYERS);
			return m_ObjectToBroadPhase[inLayer];
		}

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
		const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer inLayer) const override
		{
			switch ((JPH::BroadPhaseLayer::Type)inLayer)
			{
				case (JPH::BroadPhaseLayer::Type)BroadPhaseLayers::NON_MOVING: return "NON_MOVING";
				case (JPH::BroadPhaseLayer::Type)BroadPhaseLayers::MOVING:     return "MOVING";
				default: return "INVALID";
			}
		}
#endif
	private:
		JPH::BroadPhaseLayer m_ObjectToBroadPhase[Layers::NUM_LAYERS];
	};

	class ObjectVsBroadPhaseLayerFilterImpl : public JPH::ObjectVsBroadPhaseLayerFilter
	{
	public:
		bool ShouldCollide(JPH::ObjectLayer inLayer1, JPH::BroadPhaseLayer inLayer2) const override
		{
			switch (inLayer1)
			{
				case Layers::NON_MOVING: return inLayer2 == BroadPhaseLayers::MOVING;
				case Layers::MOVING:     return true;
				default:                 return false;
			}
		}
	};

	static void TraceImpl(const char* inFMT, ...)
	{
		va_list list;
		va_start(list, inFMT);
		char buffer[1024];
		vsnprintf(buffer, sizeof(buffer), inFMT, list);
		va_end(list);
		GE_CORE_TRACE("[Jolt] {0}", buffer);
	}

#ifdef JPH_ENABLE_ASSERTS
	static bool AssertFailedImpl(const char* inExpression, const char* inMessage, const char* inFile, uint32_t inLine)
	{
		GE_CORE_ERROR("[Jolt Assert] {0}:{1}: ({2}) {3}", inFile, inLine, inExpression, inMessage ? inMessage : "");
		return true;
	}
#endif

	static glm::vec3 GetTransformScale(const glm::mat4& transform)
	{
		return {
			glm::length(glm::vec3(transform[0])),
			glm::length(glm::vec3(transform[1])),
			glm::length(glm::vec3(transform[2]))
		};
	}

	static glm::quat GetTransformRotation(const glm::mat4& transform)
	{
		glm::vec3 scale = GetTransformScale(transform);
		glm::mat3 rotMat(
			glm::vec3(transform[0]) / glm::max(scale.x, 1e-6f),
			glm::vec3(transform[1]) / glm::max(scale.y, 1e-6f),
			glm::vec3(transform[2]) / glm::max(scale.z, 1e-6f)
		);
		return glm::normalize(glm::quat_cast(rotMat));
	}

	static glm::vec3 QuatToEulerXYZ(const glm::quat& q)
	{
		glm::mat4 m = glm::mat4_cast(q);
		float x, y, z;
		glm::extractEulerAngleXYZ(m, x, y, z);
		return { x, y, z };
	}

	static JPH::EMotionType ToJoltMotionType(RigidBodyType type)
	{
		switch (type)
		{
			case RigidBodyType::Static:    return JPH::EMotionType::Static;
			case RigidBodyType::Kinematic: return JPH::EMotionType::Kinematic;
			case RigidBodyType::Dynamic:
			default:                       return JPH::EMotionType::Dynamic;
		}
	}

	struct PendingBodyPair
	{
		JPH::BodyID Body1;
		JPH::BodyID Body2;
		bool Entered = true;
	};

	class PhysicsContactListener : public JPH::ContactListener
	{
	public:
		JPH::ValidateResult OnContactValidate(const JPH::Body&, const JPH::Body&,
			JPH::RVec3Arg, const JPH::CollideShapeResult&) override
		{
			return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
		}

		void OnContactAdded(const JPH::Body& inBody1, const JPH::Body& inBody2,
			const JPH::ContactManifold&, JPH::ContactSettings&) override
		{
			std::lock_guard lock(m_Mutex);
			m_Pending.push_back({ inBody1.GetID(), inBody2.GetID(), true });
		}

		void OnContactRemoved(const JPH::SubShapeIDPair& inSubShapePair) override
		{
			std::lock_guard lock(m_Mutex);
			m_Pending.push_back({ inSubShapePair.GetBody1ID(), inSubShapePair.GetBody2ID(), false });
		}

		void Drain(std::vector<PendingBodyPair>& out)
		{
			std::lock_guard lock(m_Mutex);
			out.insert(out.end(), m_Pending.begin(), m_Pending.end());
			m_Pending.clear();
		}

	private:
		std::mutex m_Mutex;
		std::vector<PendingBodyPair> m_Pending;
	};

#ifdef JPH_DEBUG_RENDERER
	class JoltDebugRenderer final : public JPH::DebugRendererSimple
	{
	public:
		void DrawLine(JPH::RVec3Arg inFrom, JPH::RVec3Arg inTo, JPH::ColorArg inColor) override
		{
			glm::vec3 p0{ (float)inFrom.GetX(), (float)inFrom.GetY(), (float)inFrom.GetZ() };
			glm::vec3 p1{ (float)inTo.GetX(), (float)inTo.GetY(), (float)inTo.GetZ() };
			JPH::Vec4 c = inColor.ToVec4();
			Renderer3D::DrawLine(p0, p1, { c.GetX(), c.GetY(), c.GetZ(), c.GetW() });
		}

		void DrawText3D(JPH::RVec3Arg, const JPH::string_view&, JPH::ColorArg, float) override
		{
			// No text renderer yet — shapes/velocities are enough for debugging.
		}
	};
#endif

	struct PhysicsScene::Impl
	{
		JPH::PhysicsSystem System;
		JPH::TempAllocatorImpl TempAllocator{ 10 * 1024 * 1024 };
		JPH::JobSystemThreadPool JobSystem{ JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers,
			(int)std::max(1u, std::thread::hardware_concurrency() - 1) };

		BPLayerInterfaceImpl BroadPhaseLayerInterface;
		ObjectVsBroadPhaseLayerFilterImpl ObjectVsBroadphaseLayerFilter;
		ObjectLayerPairFilterImpl ObjectVsObjectLayerFilter;

		Scope<PhysicsContactListener> ContactListener;
#ifdef JPH_DEBUG_RENDERER
		Scope<JoltDebugRenderer> DebugRenderer;
#endif

		std::unordered_map<UUID, JPH::BodyID> EntityToBody;
		std::unordered_map<uint32_t, UUID> BodyToEntity;
	};

	static void EnsureJoltInitialized()
	{
		static std::once_flag s_JoltInitOnce;
		std::call_once(s_JoltInitOnce, []()
		{
			// Must run before any Jolt object that allocates (TempAllocator, JobSystem, etc.)
			JPH::RegisterDefaultAllocator();
			JPH::Trace = TraceImpl;
			JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = AssertFailedImpl;)
			JPH::Factory::sInstance = new JPH::Factory();
			JPH::RegisterTypes();
		});
	}

	PhysicsScene::PhysicsScene() = default;

	PhysicsScene::~PhysicsScene()
	{
		Stop();
	}

	void PhysicsScene::Start(Scene* scene)
	{
		GE_CORE_ASSERT(scene, "PhysicsScene::Start requires a scene");
		Stop();

		EnsureJoltInitialized();

		m_Scene = scene;
		m_Impl = CreateScope<Impl>();
		m_Impl->ContactListener = CreateScope<PhysicsContactListener>();
#ifdef JPH_DEBUG_RENDERER
		m_Impl->DebugRenderer = CreateScope<JoltDebugRenderer>();
#endif

		constexpr uint32_t cMaxBodies = 65536;
		constexpr uint32_t cNumBodyMutexes = 0;
		constexpr uint32_t cMaxBodyPairs = 65536;
		constexpr uint32_t cMaxContactConstraints = 10240;

		m_Impl->System.Init(cMaxBodies, cNumBodyMutexes, cMaxBodyPairs, cMaxContactConstraints,
			m_Impl->BroadPhaseLayerInterface, m_Impl->ObjectVsBroadphaseLayerFilter, m_Impl->ObjectVsObjectLayerFilter);
		m_Impl->System.SetContactListener(m_Impl->ContactListener.get());

		CreateBodies(scene);
		m_Impl->System.OptimizeBroadPhase();

		m_Active = true;
		m_PreviousPoses.clear();
		m_CurrentPoses.clear();
		CapturePoses(m_CurrentPoses);
		m_PreviousPoses = m_CurrentPoses;
	}

	void PhysicsScene::Stop()
	{
		if (!m_Impl)
		{
			m_Scene = nullptr;
			m_Active = false;
			return;
		}

		DestroyBodies();
		m_Impl->System.SetContactListener(nullptr);
		m_Impl->ContactListener.reset();
#ifdef JPH_DEBUG_RENDERER
		m_Impl->DebugRenderer.reset();
#endif
		m_Impl.reset();

		m_PreviousPoses.clear();
		m_CurrentPoses.clear();
		m_CollisionEvents.clear();
		m_Active = false;
		m_Scene = nullptr;
	}

	void PhysicsScene::CreateBodies(Scene* scene)
	{
		auto& registry = scene->Reg();
		auto& bodyInterface = m_Impl->System.GetBodyInterface();

		auto view = registry.view<IDComponent, TransformComponent, RigidBodyComponent>();
		for (auto entityHandle : view)
		{
			Entity entity{ entityHandle, scene };
			UUID uuid = entity.GetUUID();
			auto& rb = entity.GetComponent<RigidBodyComponent>();

			const bool hasBox = entity.HasComponent<BoxColliderComponent>();
			const bool hasSphere = entity.HasComponent<SphereColliderComponent>();
			const bool hasCapsule = entity.HasComponent<CapsuleColliderComponent>();
			if (!hasBox && !hasSphere && !hasCapsule)
			{
				GE_CORE_WARN("Entity '{0}' has RigidBody but no collider — skipped", entity.GetName());
				continue;
			}

			glm::mat4 world = scene->GetWorldSpaceTransform(entity);
			glm::vec3 worldPos = glm::vec3(world[3]);
			glm::quat worldRot = GetTransformRotation(world);
			glm::vec3 worldScale = GetTransformScale(world);

			JPH::RefConst<JPH::Shape> shape;
			PhysicsMaterial material;

			auto makeOffsetShape = [&](const JPH::Shape* inner, const glm::vec3& offset) -> JPH::RefConst<JPH::Shape>
			{
				glm::vec3 scaledOffset = offset * worldScale;
				if (glm::length2(scaledOffset) < 1e-8f)
					return inner;

				JPH::RotatedTranslatedShapeSettings settings(
					JPH::Vec3(scaledOffset.x, scaledOffset.y, scaledOffset.z),
					JPH::Quat::sIdentity(),
					inner);
				auto result = settings.Create();
				if (result.HasError())
				{
					GE_CORE_ERROR("Failed to create offset shape: {0}", result.GetError().c_str());
					return inner;
				}
				return result.Get();
			};

			if (hasBox)
			{
				auto& col = entity.GetComponent<BoxColliderComponent>();
				material = col.Material;
				glm::vec3 he = glm::max(col.HalfExtents * worldScale, glm::vec3(0.001f));
				JPH::BoxShapeSettings boxSettings(JPH::Vec3(he.x, he.y, he.z));
				auto result = boxSettings.Create();
				if (result.HasError())
				{
					GE_CORE_ERROR("BoxShape create failed: {0}", result.GetError().c_str());
					continue;
				}
				shape = makeOffsetShape(result.Get(), col.Offset);
			}
			else if (hasSphere)
			{
				auto& col = entity.GetComponent<SphereColliderComponent>();
				material = col.Material;
				float radius = glm::max(col.Radius * glm::max(worldScale.x, glm::max(worldScale.y, worldScale.z)), 0.001f);
				JPH::SphereShapeSettings sphereSettings(radius);
				auto result = sphereSettings.Create();
				if (result.HasError())
				{
					GE_CORE_ERROR("SphereShape create failed: {0}", result.GetError().c_str());
					continue;
				}
				shape = makeOffsetShape(result.Get(), col.Offset);
			}
			else
			{
				auto& col = entity.GetComponent<CapsuleColliderComponent>();
				material = col.Material;
				float radius = glm::max(col.Radius * glm::max(worldScale.x, worldScale.z), 0.001f);
				float halfHeight = glm::max(col.HalfHeight * worldScale.y, 0.001f);
				JPH::CapsuleShapeSettings capsuleSettings(halfHeight, radius);
				auto result = capsuleSettings.Create();
				if (result.HasError())
				{
					GE_CORE_ERROR("CapsuleShape create failed: {0}", result.GetError().c_str());
					continue;
				}
				shape = makeOffsetShape(result.Get(), col.Offset);
			}

			JPH::EMotionType motionType = ToJoltMotionType(rb.Type);
			JPH::ObjectLayer layer = (motionType == JPH::EMotionType::Static) ? Layers::NON_MOVING : Layers::MOVING;

			JPH::BodyCreationSettings settings(
				shape,
				JPH::RVec3(worldPos.x, worldPos.y, worldPos.z),
				JPH::Quat(worldRot.x, worldRot.y, worldRot.z, worldRot.w),
				motionType,
				layer);

			settings.mFriction = material.Friction;
			settings.mRestitution = material.Restitution;
			settings.mLinearDamping = rb.LinearDamping;
			settings.mAngularDamping = rb.AngularDamping;
			settings.mGravityFactor = rb.UseGravity ? 1.0f : 0.0f;
			settings.mUserData = static_cast<uint64_t>(uuid);

			if (motionType == JPH::EMotionType::Dynamic && rb.Mass > 0.0f)
			{
				settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
				settings.mMassPropertiesOverride.mMass = rb.Mass;
			}

			JPH::BodyID bodyID = bodyInterface.CreateAndAddBody(settings,
				motionType == JPH::EMotionType::Static ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);

			if (bodyID.IsInvalid())
			{
				GE_CORE_ERROR("Failed to create physics body for '{0}'", entity.GetName());
				continue;
			}

			m_Impl->EntityToBody[uuid] = bodyID;
			m_Impl->BodyToEntity[bodyID.GetIndexAndSequenceNumber()] = uuid;
		}
	}

	void PhysicsScene::DestroyBodies()
	{
		if (!m_Impl)
			return;

		auto& bodyInterface = m_Impl->System.GetBodyInterface();
		for (auto& [uuid, bodyID] : m_Impl->EntityToBody)
		{
			(void)uuid;
			if (!bodyID.IsInvalid())
			{
				bodyInterface.RemoveBody(bodyID);
				bodyInterface.DestroyBody(bodyID);
			}
		}
		m_Impl->EntityToBody.clear();
		m_Impl->BodyToEntity.clear();
	}

	void PhysicsScene::CapturePoses(std::unordered_map<UUID, BodyPose>& out)
	{
		out.clear();
		if (!m_Impl)
			return;

		auto& bodyInterface = m_Impl->System.GetBodyInterface();
		for (auto& [uuid, bodyID] : m_Impl->EntityToBody)
		{
			JPH::RVec3 p = bodyInterface.GetPosition(bodyID);
			JPH::Quat r = bodyInterface.GetRotation(bodyID);
			BodyPose pose;
			pose.Position = { (float)p.GetX(), (float)p.GetY(), (float)p.GetZ() };
			pose.Rotation = glm::normalize(glm::quat(r.GetW(), r.GetX(), r.GetY(), r.GetZ()));
			out[uuid] = pose;
		}
	}

	void PhysicsScene::Step(float fixedDeltaTime)
	{
		if (!m_Active || !m_Scene || !m_Impl)
			return;

		{
			auto& registry = m_Scene->Reg();
			auto& bodyInterface = m_Impl->System.GetBodyInterface();
			auto view = registry.view<IDComponent, TransformComponent, RigidBodyComponent>();
			for (auto entityHandle : view)
			{
				Entity entity{ entityHandle, m_Scene };
				auto& rb = entity.GetComponent<RigidBodyComponent>();
				if (rb.Type != RigidBodyType::Kinematic)
					continue;

				UUID uuid = entity.GetUUID();
				auto it = m_Impl->EntityToBody.find(uuid);
				if (it == m_Impl->EntityToBody.end())
					continue;

				glm::mat4 world = m_Scene->GetWorldSpaceTransform(entity);
				glm::vec3 pos = glm::vec3(world[3]);
				glm::quat rot = GetTransformRotation(world);
				bodyInterface.SetPositionAndRotation(it->second,
					JPH::RVec3(pos.x, pos.y, pos.z),
					JPH::Quat(rot.x, rot.y, rot.z, rot.w),
					JPH::EActivation::Activate);
			}
		}

		m_PreviousPoses = m_CurrentPoses;

		constexpr int cCollisionSteps = 1;
		m_Impl->System.Update(fixedDeltaTime, cCollisionSteps, &m_Impl->TempAllocator, &m_Impl->JobSystem);

		CapturePoses(m_CurrentPoses);

		std::vector<PendingBodyPair> pending;
		m_Impl->ContactListener->Drain(pending);
		for (const auto& pair : pending)
		{
			auto it1 = m_Impl->BodyToEntity.find(pair.Body1.GetIndexAndSequenceNumber());
			auto it2 = m_Impl->BodyToEntity.find(pair.Body2.GetIndexAndSequenceNumber());
			if (it1 == m_Impl->BodyToEntity.end() || it2 == m_Impl->BodyToEntity.end())
				continue;

			PhysicsCollisionEvent e;
			e.EntityA = it1->second;
			e.EntityB = it2->second;
			e.Entered = pair.Entered;
			m_CollisionEvents.push_back(e);
		}
	}

	void PhysicsScene::SyncTransforms(Scene* scene, float alpha)
	{
		if (!m_Active || !scene)
			return;

		alpha = glm::clamp(alpha, 0.0f, 1.0f);

		for (auto& [uuid, current] : m_CurrentPoses)
		{
			Entity entity = scene->FindEntityByUUID(uuid);
			if (!entity || !entity.HasComponent<RigidBodyComponent>())
				continue;

			auto& rb = entity.GetComponent<RigidBodyComponent>();
			if (rb.Type != RigidBodyType::Dynamic)
				continue;

			BodyPose previous = current;
			auto prevIt = m_PreviousPoses.find(uuid);
			if (prevIt != m_PreviousPoses.end())
				previous = prevIt->second;

			glm::vec3 pos = glm::mix(previous.Position, current.Position, alpha);
			glm::quat rot = glm::normalize(glm::slerp(previous.Rotation, current.Rotation, alpha));

			glm::mat4 world = glm::translate(glm::mat4(1.0f), pos) * glm::mat4_cast(rot);
			UUID parentID = entity.GetComponent<RelationshipComponent>().Parent;
			if (parentID != UUID{ 0 })
			{
				Entity parent = scene->FindEntityByUUID(parentID);
				if (parent)
					world = glm::inverse(scene->GetWorldSpaceTransform(parent)) * world;
			}

			auto& tc = entity.GetComponent<TransformComponent>();
			tc.Translation = glm::vec3(world[3]);
			tc.Rotation = QuatToEulerXYZ(GetTransformRotation(world));
		}
	}

	void PhysicsScene::DebugDraw(const glm::vec3& cameraPosition, const PhysicsDebugDrawSettings& settings)
	{
#ifndef JPH_DEBUG_RENDERER
		(void)cameraPosition;
		(void)settings;
#else
		if (!m_Active || !m_Impl || !settings.Enabled || !m_Impl->DebugRenderer)
			return;

		m_Impl->DebugRenderer->SetCameraPos(JPH::RVec3(cameraPosition.x, cameraPosition.y, cameraPosition.z));

		JPH::BodyManager::DrawSettings drawSettings;
		drawSettings.mDrawShape = true;
		drawSettings.mDrawShapeWireframe = settings.Wireframe;
		drawSettings.mDrawBoundingBox = settings.BoundingBoxes;
		drawSettings.mDrawVelocity = settings.Velocities;
		drawSettings.mDrawCenterOfMassTransform = settings.CenterOfMass;

		m_Impl->System.DrawBodies(drawSettings, m_Impl->DebugRenderer.get());
		if (settings.Constraints)
			m_Impl->System.DrawConstraints(m_Impl->DebugRenderer.get());

		m_Impl->DebugRenderer->NextFrame();
#endif
	}

}
