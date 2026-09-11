#pragma once

#include "GanymedE/Core/Core.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>

namespace GanymedE {

	// Three tiers, mapped 1:1 onto enkiTS's default ENKITS_TASK_PRIORITIES_NUM == 3.
	// High is frame work, Normal is import/compile, Low is speculative and background I/O.
	// Raising the tier count means recompiling enkiTS with a different macro, so treat
	// these as fixed rather than as a knob.
	enum class JobPriority : uint8_t
	{
		High = 0,
		Normal = 1,
		Low = 2
	};

	namespace Detail {

		// The block a Future shares with its in-flight task.
		//
		// Deliberately enkiTS-free so it can live in this header: Task is a void* that
		// JobSystem.cpp casts back to enki::ITaskSet*. Same firewall AudioEngine gets by
		// keeping miniaudio out of its header, and it matters more here - the engine is a
		// static lib consumed by the editor, the runtime and Sandbox, and none of them
		// should need enkiTS on their include path to hold a Future.
		struct JobState
		{
			// Type-erased at submission: the caller's callable plus the store into its
			// result slot, collapsed into one std::function. One allocation per job, which
			// is noise next to any work worth submitting.
			std::function<void()> Work;

			// Cooperative, and note what it does NOT mean. enkiTS has no way to pull a
			// queued task back out of the pipe, so a cancelled job still runs - it just
			// returns immediately without touching the result slot. A body already past
			// that check polls JobSystem::IsCurrentJobCancelled at coarse boundaries.
			std::atomic<bool> Cancelled{ false };

			// enki::ITaskSet*, allocated by Dispatch and freed by Release. Raw on purpose:
			// enkiTS holds this pointer itself and does not own it, so exactly one side
			// may free it and that side is JobSystem, after the task is provably done.
			// The task holds a JobStatePtr back, so the state cannot die underneath it.
			void* Task = nullptr;
		};

		using JobStatePtr = std::shared_ptr<JobState>;

		// Result storage, separate from JobState so JobState stays non-template.
		template<typename T>
		struct ResultSlot { std::optional<T> Value; };
		template<>
		struct ResultSlot<void> {};
	}

	// A handle to one submitted job.
	//
	// Move-only, and **the destructor cancels and waits**. That is the load-bearing
	// property of this whole subsystem, not a convenience: a dropped Future must never
	// leave a task running against state its caller has already destroyed. Scene swaps in
	// the asset layer will drop Futures by the dozen, and a detached task writing into a
	// freed resource is the bug you cannot debug - it reproduces on one machine, in
	// Release, once a week. BlankEngine's ThreadService reaches the same conclusion from the
	// other direction, with explicit m_cancelledFutures vectors that exist only to hold
	// futures until their tasks drain.
	//
	// The cost is stated plainly: destroying a Future can block. If that matters at a call
	// site, move the Future somewhere that outlives the work instead of reaching for a
	// detach that does not exist.
	template<typename T>
	class Future
	{
	public:
		Future() = default;
		~Future() { Reset(); }

		Future(const Future&) = delete;
		Future& operator=(const Future&) = delete;

		Future(Future&& other) noexcept
			: m_State(std::move(other.m_State)), m_Result(std::move(other.m_Result))
		{
		}

		Future& operator=(Future&& other) noexcept
		{
			if (this != &other)
			{
				// Cancel-and-wait on whatever we were holding before adopting the new job.
				Reset();
				m_State = std::move(other.m_State);
				m_Result = std::move(other.m_Result);
			}
			return *this;
		}

		bool IsValid() const { return static_cast<bool>(m_State); }
		bool IsCancelled() const { return m_State && m_State->Cancelled.load(std::memory_order_relaxed); }

		// Non-blocking. Authoritative: it asks enkiTS whether the task retired, not
		// whether the body finished running - those are not the same instant.
		bool IsReady() const;

		// Request only; see JobState::Cancelled for what it actually does.
		void Cancel()
		{
			if (m_State)
				m_State->Cancelled.store(true, std::memory_order_relaxed);
		}

		// Blocks. Pumps other queued jobs while waiting, so a job waiting on a job cannot
		// deadlock the pool - and only jobs at this priority or higher, so waiting on frame
		// work never drags the waiter into background I/O.
		void Wait();

		// Blocks, then hands over the result. nullopt when the job was cancelled before
		// its body ran. Only present for non-void jobs.
		template<typename U = T>
		std::enable_if_t<!std::is_void_v<U>, std::optional<U>> Get()
		{
			Wait();
			return m_Result ? std::move(m_Result->Value) : std::nullopt;
		}

		// Cancel, wait, release. What the destructor does; call it early to reclaim the
		// slot at a point you choose rather than at end of scope.
		void Reset();

	private:
		friend class JobSystem;

		Detail::JobStatePtr m_State;
		std::shared_ptr<Detail::ResultSlot<T>> m_Result;
	};

	// The engine's scheduler: a parallel-for and cancellable background jobs, over enkiTS.
	//
	// Static facade for the same reason Renderer and AudioEngine are - there is one
	// scheduler per process and threading it through call sites buys nothing. Lifetime is
	// tied to Application: Init in the ctor, Shutdown in the dtor body.
	//
	// ---- The main thread, and why it is thread 0 ----
	//
	// enkiTS numbers the thread that calls Initialize as 0 and creates workers 1..N, so
	// initialising from the Application ctor makes the main thread - which is the thread
	// that owns bgfx submission - thread 0 by construction. No RegisterExternalTaskThread
	// is needed, and IsMainThread is the assert every bgfx-touching path should carry: a
	// bgfx call from a worker is a corruption bug that manifests far from its cause.
	//
	// ---- Two shapes, deliberately, and no third ----
	//
	// ParallelFor for data-parallel work (blocking, splits a range across workers) and
	// Submit for independent background work (returns a Future). There is no task graph
	// and no per-frame system scheduling here; see docs/history/THREADING_ROADMAP.md for
	// why that was ruled out rather than deferred.
	//
	// ---- Degrading when uninitialised ----
	//
	// Every entry point works before Init and after Shutdown by running the work inline on
	// the calling thread. That is not politeness: the asset compiler is expected to run
	// from tools and tests that never build an Application, and a scheduler that must
	// exist is a scheduler that gets a null check at every call site instead.
	class GE_API JobSystem
	{
	public:
		// Creates hardware_concurrency()-1 workers, so worker count plus the main thread
		// equals the hardware thread count. Note that Jolt still constructs its own pool
		// of the same size (Physics/PhysicsScene.cpp) - that oversubscription is real and
		// is resolved by consolidating Jolt onto this scheduler, not by shrinking this one
		// on a guess.
		static void Init();

		// Waits for all outstanding work, then joins. Never ShutdownNow(): enkiTS
		// documents that as leaving queued tasks "in an undefined state in which should
		// not be re-launched", which is a crash-exit path, not shutdown.
		static void Shutdown();

		static bool IsInitialized();

		// Threads able to run jobs, including the main thread. 1 when uninitialised.
		static uint32_t ThreadCount();

		// The thread that called Init. True before Init and after Shutdown only for the
		// thread that did, or would have, initialised - it is captured once and not cleared.
		static bool IsMainThread();

		// Drains main-thread jobs. Once per frame from Application::Run.
		static void OnUpdate();

		// begin/end is a half-open range; threadIndex is 0..ThreadCount()-1 and is meant
		// for indexing per-thread output buckets, not for changing what the work does.
		using RangeFn = std::function<void(uint32_t begin, uint32_t end, uint32_t threadIndex)>;

		// Blocking data-parallel loop over [0, count).
		//
		// minRange is the grain size, and choosing it badly is the classic way to make a
		// system slower and conclude threading does not help. enkiTS's own guidance is a
		// grain worth at least ~10k cycles; minRange == 1 over cheap per-item work spends
		// more on scheduling than on the work. There is no default for that reason.
		static void ParallelFor(uint32_t count, uint32_t minRange, const RangeFn& fn);

		// Runs fn on a worker. The returned Future must be kept alive until the job
		// finishes or explicitly reset - dropping it cancels and waits.
		template<typename Fn>
		static auto Submit(JobPriority priority, Fn&& fn) -> Future<std::invoke_result_t<Fn>>;

		template<typename Fn>
		static auto Submit(Fn&& fn) -> Future<std::invoke_result_t<Fn>>
		{
			return Submit(JobPriority::Normal, std::forward<Fn>(fn));
		}

		// Queues fn for the main thread. This is the bgfx handoff: a worker parses bytes,
		// then hands the "turn it into a GPU object" half back here.
		//
		// Drained by OnUpdate with a swap-under-lock, so work queued from a main-thread job
		// lands next frame by construction rather than extending the current drain. When
		// called from the main thread while uninitialised it runs inline - otherwise a tool
		// with no Application would queue into a drain that never happens.
		static void SubmitToMainThread(std::function<void()> fn);

		// Outstanding main-thread jobs. For asserts and editor status, not for polling.
		static size_t PendingMainThreadJobs();

		// Call from inside a job body to bail out of long work early. False anywhere else.
		//
		// This exists because Future::Cancel cannot unqueue anything: a body that has
		// already started is only stoppable if it asks. Backed by a thread_local pointing
		// at the running job, which is BlankEngine's thisThreadTaskIsCancelled with the
		// same rationale - the alternative is threading a token parameter through every
		// callable signature, and that is how you end up with eight SFINAE overloads.
		static bool IsCurrentJobCancelled();

		// Exercises the three properties that are worth catching at boot rather than in a
		// scene swap: a ParallelFor result matches the serial one, a Future dropped mid
		// flight does not touch freed state, and cancellation is observed. Runs from Init
		// in Debug builds only; it sleeps ~20ms on purpose, see the note at the call site.
		static bool RunSelfTest();

	private:
		template<typename T> friend class Future;

		static Detail::JobStatePtr CreateJob();
		static void Dispatch(const Detail::JobStatePtr& state, JobPriority priority);
		static bool IsJobReady(const Detail::JobStatePtr& state);
		static void WaitForJob(const Detail::JobStatePtr& state);
		static void ReleaseJob(const Detail::JobStatePtr& state);
	};

	template<typename T>
	inline bool Future<T>::IsReady() const
	{
		return m_State && JobSystem::IsJobReady(m_State);
	}

	template<typename T>
	inline void Future<T>::Wait()
	{
		if (m_State)
			JobSystem::WaitForJob(m_State);
	}

	template<typename T>
	inline void Future<T>::Reset()
	{
		if (!m_State)
			return;

		Cancel();
		JobSystem::WaitForJob(m_State);
		JobSystem::ReleaseJob(m_State);

		m_State.reset();
		m_Result.reset();
	}

	template<typename Fn>
	auto JobSystem::Submit(JobPriority priority, Fn&& fn) -> Future<std::invoke_result_t<Fn>>
	{
		using T = std::invoke_result_t<Fn>;

		Future<T> future;
		future.m_State = CreateJob();
		future.m_Result = std::make_shared<Detail::ResultSlot<T>>();

		// Captures the result slot, never the JobState: state -> Work -> state would be a
		// shared_ptr cycle and the job would leak. The cancellation check lives on the
		// task side, which already holds the state.
		future.m_State->Work = [result = future.m_Result, work = std::forward<Fn>(fn)]() mutable
		{
			if constexpr (std::is_void_v<T>)
			{
				(void)result;
				work();
			}
			else
			{
				result->Value.emplace(work());
			}
		};

		Dispatch(future.m_State, priority);
		return future;
	}
}
