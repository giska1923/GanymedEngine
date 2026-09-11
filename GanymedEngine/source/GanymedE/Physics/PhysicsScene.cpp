#include "gepch.h"
#include "PhysicsScene.h"

#include "GanymedE/Scene/Scene.h"
#include "GanymedE/Scene/Entity.h"
#include "GanymedE/Scene/Components.h"
#include "GanymedE/Renderer/Renderer3D.h"
#include "GanymedE/Core/JobSystem.h"

#include <TaskScheduler.h>

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemWithBarrier.h>
#include <Jolt/Core/FixedSizeFreeList.h>
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
#include <glm/gtc/epsilon.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtx/norm.hpp>

#include <atomic>
#include <cstdarg>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

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

	// ---------------------------------------------------------------------------------------
	// Jolt's job system, backed by the engine's scheduler instead of its own thread pool.
	//
	// THREADING_ROADMAP.md decision 4. Jolt's `JobSystemThreadPool` creates
	// `hardware_concurrency() - 1` threads of its own, and `Core/JobSystem` already created that
	// many - so the process ran two full-width pools for one machine. Measured on a 16-thread
	// box: 49 threads in the editor process, 16 of them the engine's. Oversubscription of that
	// shape does not show up as a clean regression; it shows up as both subsystems getting
	// slower whenever they happen to peak together.
	//
	// What is NOT changed: Jolt still drives its own job graph, dependencies and barriers. Only
	// the threads underneath are shared. `JobSystemWithBarrier` supplies the barrier half
	// (including the property this whole design leans on - a thread waiting on a barrier executes
	// jobs from that barrier itself, so a job waiting on a job cannot deadlock the pool).
	//
	// Four functions are all that is left to implement: GetMaxConcurrency, CreateJob, FreeJob and
	// QueueJob(s).
	class JoltJobSystem final : public JPH::JobSystemWithBarrier
	{
	public:
		JoltJobSystem(JPH::uint maxJobs, JPH::uint maxBarriers, JPH::uint maxConcurrentTasks)
			: JPH::JobSystemWithBarrier(maxBarriers)
		{
			m_Jobs.Init(maxJobs, maxJobs);

			m_Slots.reserve(maxConcurrentTasks);
			for (JPH::uint i = 0; i < maxConcurrentTasks; i++)
				m_Slots.push_back(std::make_unique<TaskSlot>());
		}

		~JoltJobSystem() override
		{
			// Nothing may be mid-flight when the slots die. Jolt's Update is synchronous, so by
			// the time a scene tears down every job has run - but a task can still be between
			// "job executed" and enkiTS retiring the task object, and that object is a member
			// here.
			if (enki::TaskScheduler* scheduler = Detail::NativeScheduler())
			{
				for (auto& slot : m_Slots)
				{
					if (!slot->Task.GetIsComplete())
						scheduler->WaitforTask(&slot->Task);
				}
			}
		}

		// Jolt sizes its work by this: it splits each stage into this many chunks. The engine's
		// worker count plus the calling thread is exactly the number of threads that can be
		// running a Jolt job at once, because the waiting thread runs jobs too.
		int GetMaxConcurrency() const override
		{
			return static_cast<int>(GanymedE::JobSystem::ThreadCount());
		}

		JPH::JobHandle CreateJob(const char* name, JPH::ColorArg color,
			const JobFunction& function, JPH::uint32 numDependencies = 0) override
		{
			JPH::uint32 index;
			for (;;)
			{
				index = m_Jobs.ConstructObject(name, color, this, function, numDependencies);
				if (index != AvailableJobs::cInvalidObjectIndex)
					break;

				// Same posture as JobSystemThreadPool: this means maxJobs was sized too small,
				// which is a configuration bug rather than a runtime condition to handle.
				GE_CORE_ASSERT(false, "Jolt job pool exhausted");
				std::this_thread::sleep_for(std::chrono::microseconds(100));
			}

			Job* job = &m_Jobs.Get(index);

			// The handle takes the reference. Queue only after it exists - a job with no
			// dependencies can complete before this function returns.
			JPH::JobHandle handle(job);
			if (numDependencies == 0)
				QueueJob(job);

			return handle;
		}

	protected:
		void QueueJob(Job* job) override
		{
			enki::TaskScheduler* scheduler = Detail::NativeScheduler();

			// enkiTS is only usable from the thread that initialised it and its own workers.
			// Anywhere else - a tool with no Application, or physics stepped from an unregistered
			// thread - fall through to running it here.
			if (!scheduler || scheduler->GetThreadNum() == enki::NO_THREAD_NUM)
			{
				RunInline(job);
				return;
			}

			TaskSlot* slot = ClaimSlot();
			if (!slot)
			{
				// Every slot busy. Running the job on this thread is always legal (it is ready -
				// its dependencies are what triggered this call), and it is self-limiting: the
				// thread that would have queued work instead performs it.
				RunInline(job);
				return;
			}

			// The reference the task will release once the job has run. Jolt guarantees the job
			// is alive for the duration of this call and no longer.
			job->AddRef();
			slot->Task.TargetJob = job;
			slot->Task.Owner = slot;

			scheduler->AddTaskSetToPipe(&slot->Task);
		}

		void QueueJobs(Job** jobs, JPH::uint count) override
		{
			for (JPH::uint i = 0; i < count; i++)
				QueueJob(jobs[i]);
		}

		void FreeJob(Job* job) override
		{
			m_Jobs.DestructObject(job);
		}

	private:
		struct TaskSlot;

		// One enkiTS task per in-flight Jolt job.
		class JobTask final : public enki::ITaskSet
		{
		public:
			void ExecuteRange(enki::TaskSetPartition, uint32_t) override;

			Job* TargetJob = nullptr;
			TaskSlot* Owner = nullptr;
		};

		// A task object plus the flag that says whether anyone owns it.
		//
		// Two conditions gate reuse, and BOTH are needed. `InUse` is cleared at the end of
		// ExecuteRange, but enkiTS touches the task once more after that to retire it - and it
		// documents that the task must not be accessed after its running count reaches zero.
		// `GetIsComplete()` is exactly that signal. Claiming on `InUse` alone would hand a slot
		// out while enkiTS was still finishing with it; claiming on `GetIsComplete()` alone would
		// let two threads claim the same idle slot.
		struct TaskSlot
		{
			JobTask Task;
			std::atomic<bool> InUse{ false };
		};

		TaskSlot* ClaimSlot()
		{
			// A rotating start so threads spread across the ring instead of contending on slot 0.
			const std::size_t count = m_Slots.size();
			const std::size_t start = m_NextSlot.fetch_add(1, std::memory_order_relaxed) % count;

			for (std::size_t i = 0; i < count; i++)
			{
				TaskSlot& slot = *m_Slots[(start + i) % count];

				if (!slot.Task.GetIsComplete())
					continue;

				bool expected = false;
				if (slot.InUse.compare_exchange_strong(expected, true, std::memory_order_acquire))
					return &slot;
			}

			return nullptr;
		}

		// Executes on the calling thread and releases nothing: the caller still holds the
		// reference Jolt handed it.
		static void RunInline(Job* job)
		{
			job->Execute();
		}

		using AvailableJobs = JPH::FixedSizeFreeList<Job>;
		AvailableJobs m_Jobs;

		// unique_ptr because ITaskSet is neither copyable nor movable, so the slots cannot live
		// in a vector directly.
		std::vector<std::unique_ptr<TaskSlot>> m_Slots;
		std::atomic<std::size_t> m_NextSlot{ 0 };
	};

	void JoltJobSystem::JobTask::ExecuteRange(enki::TaskSetPartition, uint32_t)
	{
		Job* job = TargetJob;
		TaskSlot* owner = Owner;

		TargetJob = nullptr;
		Owner = nullptr;

		job->Execute();
		job->Release();

		// Released last, and deliberately after Release: until this store the slot cannot be
		// reclaimed, so nothing can overwrite TargetJob underneath the lines above.
		owner->InUse.store(false, std::memory_order_release);
	}

	struct PhysicsScene::Impl
	{
		JPH::PhysicsSystem System;
		JPH::TempAllocatorImpl TempAllocator{ 10 * 1024 * 1024 };
		// Backed by Core/JobSystem rather than a second pool of its own - see JoltJobSystem
		// above. The slot count bounds how many Jolt jobs can be in the engine's pipe at once;
		// anything beyond it runs on the queueing thread, which is legal and self-limiting.
		// 256 is far above what a step actually queues (Jolt splits its stages by
		// GetMaxConcurrency) and costs a few KB.
		JoltJobSystem JobSystem{ JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, 256 };

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

			const glm::vec3 translation = glm::vec3(world[3]);
			const glm::vec3 rotation = QuatToEulerXYZ(GetTransformRotation(world));

			auto& tc = entity.GetComponent<TransformComponent>();

			// Only report an actual movement. Writing every dynamic body every frame regardless
			// would dirty the world-transform cache for bodies that are asleep or resting, which
			// is exactly the per-frame recompute this cache exists to avoid.
			constexpr float epsilon = 1e-6f;
			const bool moved = glm::any(glm::epsilonNotEqual(tc.Translation, translation, epsilon))
				|| glm::any(glm::epsilonNotEqual(tc.Rotation, rotation, epsilon));

			tc.Translation = translation;
			tc.Rotation = rotation;

			if (moved)
				scene->MarkChanged<TransformComponent>(entity);
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

	// ---- Runtime body control ----

	bool PhysicsScene::HasBody(UUID entity) const
	{
		if (!m_Active || !m_Impl)
			return false;

		return m_Impl->EntityToBody.find(entity) != m_Impl->EntityToBody.end();
	}

	void PhysicsScene::SetLinearVelocity(UUID entity, const glm::vec3& velocity)
	{
		if (!m_Active || !m_Impl)
			return;

		auto it = m_Impl->EntityToBody.find(entity);
		if (it == m_Impl->EntityToBody.end())
			return;

		auto& bodyInterface = m_Impl->System.GetBodyInterface();
		// Activate first: setting a velocity on a sleeping body is silently dropped.
		bodyInterface.ActivateBody(it->second);
		bodyInterface.SetLinearVelocity(it->second, JPH::Vec3(velocity.x, velocity.y, velocity.z));
	}

	glm::vec3 PhysicsScene::GetLinearVelocity(UUID entity) const
	{
		if (!m_Active || !m_Impl)
			return glm::vec3(0.0f);

		auto it = m_Impl->EntityToBody.find(entity);
		if (it == m_Impl->EntityToBody.end())
			return glm::vec3(0.0f);

		const JPH::Vec3 velocity = m_Impl->System.GetBodyInterface().GetLinearVelocity(it->second);
		return glm::vec3(velocity.GetX(), velocity.GetY(), velocity.GetZ());
	}

	void PhysicsScene::AddImpulse(UUID entity, const glm::vec3& impulse)
	{
		if (!m_Active || !m_Impl)
			return;

		auto it = m_Impl->EntityToBody.find(entity);
		if (it == m_Impl->EntityToBody.end())
			return;

		auto& bodyInterface = m_Impl->System.GetBodyInterface();
		bodyInterface.ActivateBody(it->second);
		bodyInterface.AddImpulse(it->second, JPH::Vec3(impulse.x, impulse.y, impulse.z));
	}

	void PhysicsScene::AddForce(UUID entity, const glm::vec3& force)
	{
		if (!m_Active || !m_Impl)
			return;

		auto it = m_Impl->EntityToBody.find(entity);
		if (it == m_Impl->EntityToBody.end())
			return;

		auto& bodyInterface = m_Impl->System.GetBodyInterface();
		bodyInterface.ActivateBody(it->second);
		// Force accumulates only until the next Step consumes it, so this is meant to
		// be called every frame while the push lasts - unlike AddImpulse, which is a
		// one-shot change in momentum.
		bodyInterface.AddForce(it->second, JPH::Vec3(force.x, force.y, force.z));
	}

}
