#include "gepch.h"
#include "JobSystem.h"

#include <TaskScheduler.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <numeric>
#include <thread>
#include <vector>

#if defined(GE_PLATFORM_LINUX) || defined(GE_PLATFORM_MACOS)
	#include <pthread.h>
#endif

namespace GanymedE {

	namespace {

		// enkiTS holds raw ITaskSet pointers it does not own, so every submitted task is
		// heap-allocated here and freed by ReleaseJob once the task is provably retired.
		// The JobStatePtr member is what stops the state dying underneath a running task.
		class JobTask final : public enki::ITaskSet
		{
		public:
			explicit JobTask(Detail::JobStatePtr state)
				: m_State(std::move(state))
			{
			}

			void ExecuteRange(enki::TaskSetPartition, uint32_t) override;

		private:
			Detail::JobStatePtr m_State;
		};

		std::unique_ptr<enki::TaskScheduler> s_Scheduler;

		// Seeded at static init so IsMainThread answers sensibly before Init, then
		// overwritten by Init with the thread enkiTS will know as 0. Never cleared: after
		// Shutdown, "the main thread" is still a meaningful question for asserts.
		std::thread::id s_MainThreadId = std::this_thread::get_id();

		std::mutex s_MainThreadMutex;
		std::vector<std::function<void()>> s_MainThreadJobs;

		// Diagnostics only: Dispatch increments, ReleaseJob decrements, Shutdown complains.
		// A non-zero count at shutdown means a Future was leaked rather than reset, which
		// leaks its task object too.
		std::atomic<int32_t> s_JobsInFlight{ 0 };

		// The running job on this thread, for IsCurrentJobCancelled. Raw and non-owning -
		// JobTask::ExecuteRange owns the scope it is valid in.
		thread_local Detail::JobState* t_CurrentJob = nullptr;

		// JobPriority is declared to be a 1:1 mapping, so assert that rather than writing a
		// switch that would silently keep compiling if enkiTS's tier count ever changed.
		static_assert(ENKITS_TASK_PRIORITIES_NUM == 3,
			"JobPriority assumes enkiTS's three default tiers; rebuilding enkiTS with a different "
			"ENKITS_TASK_PRIORITIES_NUM needs JobPriority and ToEnkiPriority revisited");
		static_assert(static_cast<int>(JobPriority::High) == enki::TASK_PRIORITY_HIGH);
		static_assert(static_cast<int>(JobPriority::Normal) == enki::TASK_PRIORITY_MED);
		static_assert(static_cast<int>(JobPriority::Low) == enki::TASK_PRIORITY_LOW);

		enki::TaskPriority ToEnkiPriority(JobPriority priority)
		{
			return static_cast<enki::TaskPriority>(priority);
		}

		// ---- Thread naming --------------------------------------------------------------
		//
		// Names the CALLING thread. That restriction is why this is invoked from enkiTS's
		// threadStart callback rather than from a loop after Initialize: macOS's
		// pthread_setname_np only accepts the current thread, and Windows' debugger-facing
		// name has always been a property a thread sets on itself by convention.
		//
		// Worth having even with the profiler compiled out, which is the usual reason this
		// gets skipped: an unnamed pool is fifteen identical "Worker Thread" rows in the
		// debugger, and a hang or a crash dump is exactly when you need to know which of them
		// is the one blocked in a bgfx call it should never have made.
		void SetCurrentThreadName(const char* name)
		{
#if defined(GE_PLATFORM_WINDOWS)
			// SetThreadDescription is Windows 10 1607+. Resolved dynamically rather than
			// linked, so a binary built against a current SDK still starts on an older
			// Windows instead of failing to load over a diagnostic nicety.
			//
			// This is the modern API, not the legacy RaiseException(0x406D1388) trick: that
			// one is only observed by a debugger that happens to be attached at the moment
			// the thread starts, where a description is stored by the OS and so also reaches
			// ETW, WPA, Task Manager and post-mortem dumps.
			using SetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);

			static const SetThreadDescriptionFn setThreadDescription = []() -> SetThreadDescriptionFn
			{
				if (HMODULE kernel = GetModuleHandleW(L"kernel32.dll"))
				{
					return reinterpret_cast<SetThreadDescriptionFn>(
						reinterpret_cast<void*>(GetProcAddress(kernel, "SetThreadDescription")));
				}

				return nullptr;
			}();

			if (!setThreadDescription)
				return;

			// Names here are ASCII literals built below, so a widening copy is exact.
			const std::string narrow(name);
			const std::wstring wide(narrow.begin(), narrow.end());
			setThreadDescription(GetCurrentThread(), wide.c_str());
#elif defined(GE_PLATFORM_LINUX)
			// Linux caps this at 16 bytes INCLUDING the terminator and fails the call
			// outright (ERANGE) if the name is longer, so truncate rather than lose the name.
			// The precision is written out rather than left to the buffer size so the bound is
			// visible to a reader and to -Wformat-truncation, which otherwise reports the
			// worst case of the "GE Worker %u" below. Every name this is actually called with
			// fits: "GE Main" is 7 and "GE Worker 31" is 12.
			char truncated[16];
			std::snprintf(truncated, sizeof(truncated), "%.15s", name);
			pthread_setname_np(pthread_self(), truncated);
#elif defined(GE_PLATFORM_MACOS)
			pthread_setname_np(name);   // current thread only, hence the shape of this function
#else
			(void)name;
#endif
		}

		// enkiTS's numbering, deliberately: "GE Worker 3" is the thread that a ParallelFor
		// body sees as threadIndex 3, and the main thread is 0 in both. A separate numbering
		// would make a debugger's thread list and a per-thread output bucket disagree about
		// which thread is which, which is precisely when that costs the most.
		void NameSchedulerThread(uint32_t threadNum)
		{
			if (threadNum == 0)
			{
				SetCurrentThreadName("GE Main");
				return;
			}

			char name[24];
			std::snprintf(name, sizeof(name), "GE Worker %u", threadNum);
			SetCurrentThreadName(name);
		}

		// ---- Profiler callbacks ---------------------------------------------------------
		//
		// enkiTS hands these out as plain C function pointers with no user data, so the
		// bridge below is free functions over thread_local state rather than anything
		// captured.
		//
		// Only threadStart is wired unconditionally, because that is where naming happens.
		// The six wait/suspend callbacks are compiled in only with GE_PROFILE on, so with
		// profiling off enkiTS gets null pointers for them and its SafeCallback skips them
		// entirely - a null check per wait, which is free next to the wait itself.

#if GE_PROFILE
		// GE_PROFILE_SCOPE is an RAII scope and cannot span two separate callback functions,
		// so the span is assembled by hand: a start timestamp per thread per category, and
		// Instrumentor::WriteProfile on the matching stop.
		//
		// Three categories rather than one slot, because they nest - a thread inside
		// waitForTaskComplete can go on to suspend, and a single slot would have the inner
		// stop consume the outer start and report a span that never happened.
		enum class WaitKind : size_t
		{
			NewTaskSuspend = 0,
			TaskComplete,
			TaskCompleteSuspend,
			Count
		};

		using ProfileClock = std::chrono::steady_clock;

		thread_local ProfileClock::time_point t_WaitStart[static_cast<size_t>(WaitKind::Count)];
		thread_local ProfileClock::time_point t_ThreadStart;

		void BeginSpan(ProfileClock::time_point& slot)
		{
			slot = ProfileClock::now();
		}

		void EndSpan(ProfileClock::time_point& slot, const char* name)
		{
			// An unpaired stop is not hypothetical: a thread can be inside a wait when the
			// scheduler is told to shut down, and the profiler session can be closed between
			// a start and its stop. Either way a zero start would be written as a span
			// beginning at the epoch, which is worse than no record at all.
			if (slot == ProfileClock::time_point{})
				return;

			const auto end = ProfileClock::now();
			const FloatingPointMicroseconds start{ slot.time_since_epoch() };
			const auto elapsed =
				std::chrono::time_point_cast<std::chrono::microseconds>(end).time_since_epoch()
				- std::chrono::time_point_cast<std::chrono::microseconds>(slot).time_since_epoch();

			Instrumentor::Get().WriteProfile({ name, start, elapsed, std::this_thread::get_id() });
			slot = ProfileClock::time_point{};
		}

		void OnWaitForNewTaskSuspendStart(uint32_t) { BeginSpan(t_WaitStart[(size_t)WaitKind::NewTaskSuspend]); }
		void OnWaitForNewTaskSuspendStop(uint32_t) { EndSpan(t_WaitStart[(size_t)WaitKind::NewTaskSuspend], "JobSystem: idle (suspended)"); }

		void OnWaitForTaskCompleteStart(uint32_t) { BeginSpan(t_WaitStart[(size_t)WaitKind::TaskComplete]); }
		void OnWaitForTaskCompleteStop(uint32_t) { EndSpan(t_WaitStart[(size_t)WaitKind::TaskComplete], "JobSystem: waiting on task"); }

		void OnWaitForTaskCompleteSuspendStart(uint32_t) { BeginSpan(t_WaitStart[(size_t)WaitKind::TaskCompleteSuspend]); }
		void OnWaitForTaskCompleteSuspendStop(uint32_t) { EndSpan(t_WaitStart[(size_t)WaitKind::TaskCompleteSuspend], "JobSystem: waiting on task (suspended)"); }
#endif

		void OnSchedulerThreadStart(uint32_t threadNum)
		{
			// Called ON the starting worker, which is what makes naming from here correct.
			NameSchedulerThread(threadNum);

#if GE_PROFILE
			BeginSpan(t_ThreadStart);
#endif
		}

		void OnSchedulerThreadStop(uint32_t threadNum)
		{
			(void)threadNum;

#if GE_PROFILE
			// One low-frequency record bracketing the whole worker lifetime, which is what
			// gives the trace a lane per worker to hang the wait spans off.
			EndSpan(t_ThreadStart, "JobSystem: worker lifetime");
#endif
		}

		enki::ProfilerCallbacks MakeProfilerCallbacks()
		{
			enki::ProfilerCallbacks callbacks{};
			callbacks.threadStart = &OnSchedulerThreadStart;
			callbacks.threadStop = &OnSchedulerThreadStop;

#if GE_PROFILE
			// Stated plainly, because turning GE_PROFILE on and being surprised by this would
			// waste an afternoon: these fire on every spin-to-suspend transition on every
			// worker, and Instrumentor::WriteProfile takes a process-wide mutex and flushes
			// to disk per record. With an idle pool that is a trace dominated by idleness,
			// and contention the scheduler would not otherwise have. It is wired anyway
			// rather than left as a retrofit, and "is a real frame profiler worth adopting"
			// is the separate question THREADING_ROADMAP.md keeps it separate from.
			callbacks.waitForNewTaskSuspendStart = &OnWaitForNewTaskSuspendStart;
			callbacks.waitForNewTaskSuspendStop = &OnWaitForNewTaskSuspendStop;
			callbacks.waitForTaskCompleteStart = &OnWaitForTaskCompleteStart;
			callbacks.waitForTaskCompleteStop = &OnWaitForTaskCompleteStop;
			callbacks.waitForTaskCompleteSuspendStart = &OnWaitForTaskCompleteSuspendStart;
			callbacks.waitForTaskCompleteSuspendStop = &OnWaitForTaskCompleteSuspendStop;
#endif

			return callbacks;
		}

		// enkiTS's API is valid on the thread that initialised it, on its own workers, and
		// on explicitly registered external threads - nothing else. Anything else reports
		// NO_THREAD_NUM and must not touch the scheduler; it runs its work inline instead.
		bool CanUseScheduler()
		{
			return s_Scheduler && s_Scheduler->GetThreadNum() != enki::NO_THREAD_NUM;
		}

		uint32_t CurrentThreadIndex()
		{
			if (!s_Scheduler)
				return 0;

			const uint32_t threadNum = s_Scheduler->GetThreadNum();
			return threadNum == enki::NO_THREAD_NUM ? 0 : threadNum;
		}

		void JobTask::ExecuteRange(enki::TaskSetPartition, uint32_t)
		{
			GE_PROFILE_FUNCTION();

			if (m_State->Cancelled.load(std::memory_order_relaxed))
				return;

			t_CurrentJob = m_State.get();
			m_State->Work();
			t_CurrentJob = nullptr;
		}
	}

	namespace Detail {

		enki::TaskScheduler* NativeScheduler()
		{
			return s_Scheduler.get();
		}

	}

	void JobSystem::Init()
	{
		if (s_Scheduler)
			return;

		s_MainThreadId = std::this_thread::get_id();

		enki::TaskSchedulerConfig config;
		// Default already, spelled out because it is the load-bearing choice: enkiTS counts
		// the initialising thread as thread 0, so workers + main == hardware threads.
		config.numTaskThreadsToCreate = enki::GetNumHardwareThreads() - 1;

		// Thread naming and the profiler bridge. threadStart fires on each worker as it
		// starts and names it there; see MakeProfilerCallbacks for what the other six do and
		// what they cost.
		config.profilerCallbacks = MakeProfilerCallbacks();

		// Thread 0 is this thread, and enkiTS never calls threadStart for it - it did not
		// create it. Named here so the pool is complete in a debugger rather than fifteen
		// named workers around one anonymous main thread.
		NameSchedulerThread(0);

		s_Scheduler = std::make_unique<enki::TaskScheduler>();
		s_Scheduler->Initialize(config);

		GE_CORE_INFO("JobSystem initialised: {0} worker threads + main ({1} total)",
			config.numTaskThreadsToCreate, s_Scheduler->GetNumTaskThreads());

#ifdef GE_DEBUG
		// Debug only. Its cost is dominated by the deliberate 20ms sleep in check 2, which
		// is what makes "the destructor actually waited" observable rather than a race the
		// test wins by luck. Paying that at boot beats finding those three failures in a
		// scene swap.
		RunSelfTest();
#endif
	}

	void JobSystem::Shutdown()
	{
		if (!s_Scheduler)
			return;

		// Drain queued main-thread work before the workers go: a pinned job may be the only
		// thing holding a resource the caller expects to have been applied.
		OnUpdate();

		const int32_t inFlight = s_JobsInFlight.load(std::memory_order_relaxed);
		if (inFlight != 0)
		{
			GE_CORE_WARN("JobSystem::Shutdown with {0} job(s) still in flight - a Future was leaked "
				"rather than reset, and its task object leaks with it", inFlight);
		}

		// Never ShutdownNow(): enkiTS documents it as leaving queued tasks in an undefined
		// state that must not be re-launched. That is a crash-exit path, not shutdown.
		s_Scheduler->WaitforAllAndShutdown();
		s_Scheduler.reset();

		GE_CORE_INFO("JobSystem shut down");
	}

	bool JobSystem::IsInitialized()
	{
		return static_cast<bool>(s_Scheduler);
	}

	uint32_t JobSystem::ThreadCount()
	{
		return s_Scheduler ? s_Scheduler->GetNumTaskThreads() : 1;
	}

	bool JobSystem::IsMainThread()
	{
		return std::this_thread::get_id() == s_MainThreadId;
	}

	bool JobSystem::IsCurrentJobCancelled()
	{
		return t_CurrentJob && t_CurrentJob->Cancelled.load(std::memory_order_relaxed);
	}

	void JobSystem::OnUpdate()
	{
		GE_PROFILE_FUNCTION();

		GE_CORE_ASSERT(IsMainThread(), "JobSystem::OnUpdate must run on the main thread");

		// Swap under the lock, run outside it. Two reasons, both load-bearing: a job body
		// that queues more main-thread work would deadlock on a held lock, and swapping
		// fixes the drain boundary so that work queued from inside a drain lands next frame
		// instead of extending this one indefinitely.
		std::vector<std::function<void()>> jobs;
		{
			const std::scoped_lock lock(s_MainThreadMutex);
			jobs.swap(s_MainThreadJobs);
		}

		for (auto& job : jobs)
			job();
	}

	void JobSystem::SubmitToMainThread(std::function<void()> fn)
	{
		if (!fn)
			return;

		// With no scheduler there is no Application, so nothing will ever call OnUpdate and
		// queueing would silently drop the work. Running it inline is the only honest
		// outcome, and it is only reachable from the main thread because there are no
		// workers without a scheduler.
		if (!s_Scheduler)
		{
			fn();
			return;
		}

		const std::scoped_lock lock(s_MainThreadMutex);
		s_MainThreadJobs.push_back(std::move(fn));
	}

	size_t JobSystem::PendingMainThreadJobs()
	{
		const std::scoped_lock lock(s_MainThreadMutex);
		return s_MainThreadJobs.size();
	}

	void JobSystem::ParallelFor(uint32_t count, uint32_t minRange, const RangeFn& fn)
	{
		GE_PROFILE_FUNCTION();

		if (count == 0 || !fn)
			return;

		GE_CORE_ASSERT(minRange > 0, "JobSystem::ParallelFor needs a grain size of at least 1");
		const uint32_t grain = minRange > 0 ? minRange : 1;

		// Serial when there is nothing to split across, or nothing to split with. Both
		// paths call fn exactly as the parallel one would, so a caller cannot observe
		// which it got except through timing.
		if (count <= grain || !CanUseScheduler())
		{
			fn(0, count, CurrentThreadIndex());
			return;
		}

		// Stack-allocated, unlike Submit's tasks, because this function does not return
		// until WaitforTask says the task retired. That is the entire difference between
		// the two shapes.
		enki::TaskSet task(count, [&fn](enki::TaskSetPartition range, uint32_t threadIndex)
		{
			fn(range.start, range.end, threadIndex);
		});
		task.m_MinRange = grain;
		task.m_Priority = enki::TASK_PRIORITY_HIGH;

		s_Scheduler->AddTaskSetToPipe(&task);

		// Bounded at the task's own priority: a thread waiting on frame work pumps other
		// frame work, never background I/O.
		s_Scheduler->WaitforTask(&task, task.m_Priority);
	}

	Detail::JobStatePtr JobSystem::CreateJob()
	{
		return std::make_shared<Detail::JobState>();
	}

	void JobSystem::Dispatch(const Detail::JobStatePtr& state, JobPriority priority)
	{
		if (!state)
			return;

		// No scheduler, or a thread enkiTS does not know: run inline. The Future the caller
		// gets back is simply already complete, which is why nothing has to special-case it.
		if (!CanUseScheduler())
		{
			if (!state->Cancelled.load(std::memory_order_relaxed))
			{
				Detail::JobState* previous = t_CurrentJob;
				t_CurrentJob = state.get();
				state->Work();
				t_CurrentJob = previous;
			}
			return;
		}

		auto* task = new JobTask(state);
		task->m_Priority = ToEnkiPriority(priority);
		state->Task = task;

		s_JobsInFlight.fetch_add(1, std::memory_order_relaxed);
		s_Scheduler->AddTaskSetToPipe(task);
	}

	bool JobSystem::IsJobReady(const Detail::JobStatePtr& state)
	{
		if (!state || !state->Task)
			return true;

		return static_cast<JobTask*>(state->Task)->GetIsComplete();
	}

	void JobSystem::WaitForJob(const Detail::JobStatePtr& state)
	{
		if (!state || !state->Task)
			return;

		GE_PROFILE_FUNCTION();

		auto* task = static_cast<JobTask*>(state->Task);

		if (!CanUseScheduler())
		{
			// A thread enkiTS does not know cannot pump the pipe, so all it can do is spin.
			// Unreachable today - nothing moves a Future onto a foreign thread - but the
			// alternative to spinning here is calling WaitforTask from an unregistered
			// thread, which is unsupported.
			while (!task->GetIsComplete())
				std::this_thread::yield();
			return;
		}

		s_Scheduler->WaitforTask(task, task->m_Priority);
	}

	void JobSystem::ReleaseJob(const Detail::JobStatePtr& state)
	{
		if (!state || !state->Task)
			return;

		auto* task = static_cast<JobTask*>(state->Task);

		// WaitForJob runs first in every path that reaches here. Deleting a task enkiTS is
		// still holding is precisely the use-after-free this subsystem exists to prevent,
		// so the ordering gets an assert rather than a comment.
		GE_CORE_ASSERT(task->GetIsComplete(), "JobSystem::ReleaseJob on a task still in flight");

		state->Task = nullptr;
		delete task;

		s_JobsInFlight.fetch_sub(1, std::memory_order_relaxed);
	}

	bool JobSystem::RunSelfTest()
	{
		GE_PROFILE_FUNCTION();

		bool passed = true;

		// 1. A ParallelFor result matches the serial one. Per-thread buckets rather than one
		//    atomic, because the point is to exercise the threadIndex contract too.
		{
			constexpr uint32_t count = 100000;
			std::vector<uint64_t> buckets(ThreadCount(), 0);

			ParallelFor(count, 1024, [&buckets](uint32_t begin, uint32_t end, uint32_t threadIndex)
			{
				uint64_t sum = 0;
				for (uint32_t i = begin; i < end; ++i)
					sum += i;

				buckets[threadIndex] += sum;
			});

			const uint64_t actual = std::accumulate(buckets.begin(), buckets.end(), uint64_t{ 0 });
			constexpr uint64_t expected = uint64_t{ count } * (count - 1) / 2;

			if (actual != expected)
			{
				GE_CORE_ERROR("JobSystem self-test: ParallelFor summed {0}, expected {1}", actual, expected);
				passed = false;
			}
		}

		// 2. A Future dropped mid-flight cancels and waits. The job flips Running true, sleeps
		//    long enough that it cannot have finished by the time the Future dies, then flips
		//    it false. If Reset did not wait, Running is still true after the scope closes -
		//    and in a real caller the state it was writing into would already be freed.
		{
			std::atomic<bool> running{ false };
			{
				Future<void> future = Submit(JobPriority::High, [&running]()
				{
					running.store(true);
					std::this_thread::sleep_for(std::chrono::milliseconds(20));
					running.store(false);
				});

				// Wait for the body to actually start, so the test is about the drain and not
				// about cancelling a job that never ran.
				while (!running.load() && !future.IsReady())
					std::this_thread::yield();
			}

			if (running.load())
			{
				GE_CORE_ERROR("JobSystem self-test: Future destructor returned while its job was still running");
				passed = false;
			}
		}

		// 3. Cancellation is observed by a running body. Without IsCurrentJobCancelled this
		//    job would never end, so a hang here is the failure mode rather than a log line.
		{
			std::atomic<bool> started{ false };
			std::atomic<bool> sawCancel{ false };

			Future<void> future = Submit(JobPriority::High, [&started, &sawCancel]()
			{
				started.store(true);
				while (!IsCurrentJobCancelled())
					std::this_thread::yield();

				sawCancel.store(true);
			});

			while (!started.load())
				std::this_thread::yield();

			future.Cancel();
			future.Wait();

			if (!sawCancel.load())
			{
				GE_CORE_ERROR("JobSystem self-test: a running job did not observe IsCurrentJobCancelled");
				passed = false;
			}
		}

		if (passed)
			GE_CORE_TRACE("JobSystem self-test passed");

		return passed;
	}
}
