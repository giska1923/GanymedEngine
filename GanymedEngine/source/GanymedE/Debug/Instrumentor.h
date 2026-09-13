#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "GanymedE/Core/Log.h"

namespace GanymedE {

	// One closed scope, and the only thing the hot path stores. POD and 24 bytes on purpose:
	// anything here with a constructor is an allocation per scope, and at frame granularity
	// there are hundreds of those per frame.
	//
	// `Name` is **borrowed and never owned**. Every producer passes either a string literal or
	// the `static constexpr` buffer `GE_PROFILE_SCOPE` builds, both of which outlive the
	// process. The previous design stored a `std::string` here, which allocated once per scope
	// to copy a pointer that was already immortal.
	//
	// **No default member initialisers, deliberately.** They would make this non-trivial, and a
	// non-trivial element type turns the buffer allocation below into a value-initialising one -
	// i.e. a 6 MB memset per thread on its first scope. Measured before this was fixed: 14 ms on
	// the first recording thread and 109 ms across sixteen contending for the allocator, which is
	// both a frame stall and a fabricated span at the very start of every trace. Nothing reads a
	// record before the producer has written all three fields.
	struct ProfileRecord
	{
		const char* Name;
		int64_t StartNs;     // steady_clock epoch
		int64_t ElapsedNs;
	};

	// Records per thread, allocated on that thread's first scope and never grown.
	//
	// 1<<18 records is 6 MB per *recording* thread - worker threads only emit a handful of wait
	// spans, so in practice it is the main thread's buffer that matters. At a few hundred scopes
	// per frame that holds roughly fifteen seconds at 60 fps, which is the right shape for a
	// capture rather than a session. Overflow is counted and reported by name at `EndSession`,
	// never silently truncated: a trace that quietly stops halfway is worse than no trace.
	inline constexpr size_t kProfileRecordsPerThread = 1 << 18;

	// The chrome://tracing writer.
	//
	// ---- Why the hot path looks like this ----
	//
	// The previous version formatted a `std::stringstream`, allocated a `std::string` for the
	// name, took a process-wide `std::mutex` and **flushed the `ofstream` per record**.
	//
	// Measured against 20,000 scopes per thread, before and after, steady state:
	//
	//       threads |     before |   after
	//       --------|------------|--------
	//             1 |   2,976 ns |   74 ns
	//             4 |   6,809 ns |   81 ns
	//            16 |   5,245 ns |   72 ns
	//
	// Read the "before" column downwards: it cost *more* per scope as threads were added, which
	// is the signature of the shared lock rather than of the work. That is the part that made it
	// unusable here rather than merely slow - every scope on every thread serialised on one
	// mutex, so instrumenting the frame loop would have serialised the job system the trace
	// exists to look at. The parallel systems would have rendered as a staircase and the
	// profiler would have been measuring itself.
	//
	// So: a fixed-size buffer per thread, a POD record, and no lock, no allocation and no I/O
	// between `BeginSession` and `EndSession`.
	//
	// ---- The concurrency, stated exactly ----
	//
	// Each buffer has **one producer** (the thread that owns it) and **one consumer** (whoever
	// calls `EndSession`, always the main thread). That is what makes the publication safe
	// without a lock: the producer writes `Records[Written]` and then release-stores
	// `Written + 1` into `Published`; the consumer acquire-loads `Published` and reads strictly
	// below it. A record is therefore never read before the store that filled it is visible.
	//
	// Buffers are owned by the Instrumentor and never freed, so a worker thread that exits
	// mid-session does not take its records with it, and the `thread_local` pointer into it
	// cannot dangle.
	//
	// **Buffers are not reset between sessions, deliberately.** enkiTS's workers are alive
	// across every `BeginSession` boundary and emit wait spans continuously while idle, so
	// rewinding a producer's cursor from the main thread would be a plain data race. Instead
	// each session writes the records that appeared since the last one (`Flushed`), and
	// capacity is a whole-process budget rather than a per-session one.
	class Instrumentor
	{
	public:
		Instrumentor(const Instrumentor&) = delete;
		Instrumentor(Instrumentor&&) = delete;

		struct ThreadBuffer
		{
			std::unique_ptr<ProfileRecord[]> Records;

			// Producer-only cursor, and the count the consumer is allowed to read up to.
			size_t Written = 0;
			std::atomic<size_t> Published{ 0 };

			// Consumer-only.
			size_t Flushed = 0;
			bool NameEmitted = false;

			std::atomic<size_t> Dropped{ 0 };

			// Copied, not borrowed: the job system builds "GE Worker 3" into a stack buffer.
			char Name[32] = {};
			uint32_t Tid = 0;
		};

		static Instrumentor& Get()
		{
			static Instrumentor instance;
			return instance;
		}

		void BeginSession(const std::string& name, const std::string& filepath = "results.json")
		{
			std::lock_guard lock(m_Mutex);

			if (m_CurrentSession)
			{
				// Closing the open one first is better than interleaving two sessions into one
				// malformed file. Records already produced and not yet written go to the file
				// being closed, which is where they happened.
				if (Log::GetCoreLogger())
				{
					GE_CORE_ERROR("Instrumentor::BeginSession('{0}') when session '{1}' already open.",
						name, m_CurrentSession->Name);
				}
				InternalEndSession();
			}

			m_OutputStream.open(filepath);
			if (!m_OutputStream.is_open())
			{
				if (Log::GetCoreLogger())
					GE_CORE_ERROR("Instrumentor could not open results file '{0}'.", filepath);
				return;
			}

			m_CurrentSession = std::make_unique<Session>(Session{ name });
			m_OutputStream << "{\"otherData\": {},\"traceEvents\":[{}";

			// Consumer-side state only, so this touches nothing a producer reads.
			for (auto& buffer : m_Buffers)
				buffer->NameEmitted = false;
		}

		void EndSession()
		{
			std::lock_guard lock(m_Mutex);
			InternalEndSession();
		}

		// Names the **calling** thread's lane in the trace. Without this a viewer shows raw
		// thread ids: the OS-level names the job system sets (`SetThreadDescription`,
		// `pthread_setname_np`) are visible to a debugger and to nothing that reads this JSON.
		void SetThreadName(const char* name)
		{
			ThreadBuffer* buffer = t_Buffer ? t_Buffer : AcquireBuffer();

			std::lock_guard lock(m_Mutex);
			std::snprintf(buffer->Name, sizeof(buffer->Name), "%s", name);
		}

		// The hot path. No lock, no allocation, no I/O.
		void Record(const char* name, int64_t startNs, int64_t elapsedNs)
		{
			ThreadBuffer* buffer = t_Buffer ? t_Buffer : AcquireBuffer();

			if (buffer->Written == kProfileRecordsPerThread)
			{
				buffer->Dropped.fetch_add(1, std::memory_order_relaxed);
				return;
			}

			buffer->Records[buffer->Written] = { name, startNs, elapsedNs };

			// Release, paired with the consumer's acquire in Flush: publishes the record above.
			buffer->Published.store(++buffer->Written, std::memory_order_release);
		}

	private:
		struct Session
		{
			std::string Name;
		};

		Instrumentor() = default;

		~Instrumentor()
		{
			EndSession();
		}

		ThreadBuffer* AcquireBuffer()
		{
			std::lock_guard lock(m_Mutex);

			auto owned = std::make_unique<ThreadBuffer>();

			// `new T[n]`, not `make_unique<T[]>(n)`: the latter value-initialises. See the note
			// on ProfileRecord. Pages are then faulted in as records are written, one per ~170
			// records, instead of all of them up front.
			owned->Records.reset(new ProfileRecord[kProfileRecordsPerThread]);
			owned->Tid = static_cast<uint32_t>(m_Buffers.size());

			ThreadBuffer* buffer = owned.get();
			m_Buffers.push_back(std::move(owned));

			t_Buffer = buffer;
			return buffer;
		}

		// Caller owns m_Mutex.
		void InternalEndSession()
		{
			if (!m_CurrentSession)
				return;

			Flush();

			m_OutputStream << "]}";
			m_OutputStream.close();

			size_t dropped = 0;
			for (const auto& buffer : m_Buffers)
				dropped += buffer->Dropped.load(std::memory_order_relaxed);

			if (dropped > 0 && Log::GetCoreLogger())
			{
				GE_CORE_WARN("Instrumentor dropped {0} scope(s): a thread filled its {1}-record "
					"buffer. The trace is complete up to that point and stops there. Raise "
					"kProfileRecordsPerThread in Instrumentor.h to capture more.",
					dropped, kProfileRecordsPerThread);
			}

			m_CurrentSession.reset();
		}

		// Caller owns m_Mutex, and the session is open.
		void Flush()
		{
			char line[1024];

			for (const auto& buffer : m_Buffers)
			{
				const size_t published = buffer->Published.load(std::memory_order_acquire);

				if (!buffer->NameEmitted)
				{
					char named[32];
					if (buffer->Name[0] != '\0')
						std::snprintf(named, sizeof(named), "%s", buffer->Name);
					else
						std::snprintf(named, sizeof(named), "Thread %u", buffer->Tid);

					const int n = std::snprintf(line, sizeof(line),
						",{\"ph\":\"M\",\"pid\":0,\"tid\":%u,\"name\":\"thread_name\","
						"\"args\":{\"name\":\"%s\"}}", buffer->Tid, named);
					m_OutputStream.write(line, n);

					buffer->NameEmitted = true;
				}

				for (size_t i = buffer->Flushed; i < published; i++)
				{
					const ProfileRecord& record = buffer->Records[i];

					// Both in microseconds, fractional. The previous writer emitted `dur` as an
					// integer count of microseconds, so every scope shorter than 1 us - which is
					// most of them once the frame loop is instrumented - was written as 0.
					const int n = std::snprintf(line, sizeof(line),
						",{\"cat\":\"function\",\"dur\":%.3f,\"name\":\"%s\",\"ph\":\"X\","
						"\"pid\":0,\"tid\":%u,\"ts\":%.3f}",
						record.ElapsedNs / 1000.0, record.Name ? record.Name : "<null>",
						buffer->Tid, record.StartNs / 1000.0);
					m_OutputStream.write(line, n);
				}

				buffer->Flushed = published;
			}

			m_OutputStream.flush();
		}

		std::mutex m_Mutex;   // buffer registration and flush only - never the hot path
		std::unique_ptr<Session> m_CurrentSession;
		std::ofstream m_OutputStream;
		std::vector<std::unique_ptr<ThreadBuffer>> m_Buffers;

		inline static thread_local ThreadBuffer* t_Buffer = nullptr;
	};

	class InstrumentationTimer
	{
	public:
		explicit InstrumentationTimer(const char* name)
			: m_Name(name), m_StartTimepoint(std::chrono::steady_clock::now())
		{
		}

		~InstrumentationTimer()
		{
			if (!m_Stopped)
				Stop();
		}

		void Stop()
		{
			const auto end = std::chrono::steady_clock::now();

			Instrumentor::Get().Record(m_Name,
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					m_StartTimepoint.time_since_epoch()).count(),
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					end - m_StartTimepoint).count());

			m_Stopped = true;
		}

	private:
		const char* m_Name;
		std::chrono::steady_clock::time_point m_StartTimepoint;
		bool m_Stopped = false;
	};

	namespace InstrumentorUtils {
		template <size_t N>
		struct ChangeResult
		{
			char Data[N];
		};

		template <size_t N, size_t K>
		constexpr auto CleanupOutputString(const char(&expr)[N], const char(&remove)[K])
		{
			ChangeResult<N> result = {};

			size_t srcIndex = 0;
			size_t dstIndex = 0;
			while (srcIndex < N)
			{
				size_t matchIndex = 0;
				while (matchIndex < K - 1 && srcIndex + matchIndex < N - 1 && expr[srcIndex + matchIndex] == remove[matchIndex])
					matchIndex++;
				if (matchIndex == K - 1)
					srcIndex += matchIndex;
				result.Data[dstIndex++] = expr[srcIndex] == '"' ? '\'' : expr[srcIndex];
				srcIndex++;
			}
			return result;
		}
	}
}

#define GE_PROFILE 0
#if GE_PROFILE
	// Resolve which function signature macro will be used. Note that this only
	// is resolved when the (pre)compiler starts, so the syntax highlighting
	// could mark the wrong one in your editor!
	#if defined(__GNUC__) || (defined(__MWERKS__) && (__MWERKS__ >= 0x3000)) || (defined(__ICC) && (__ICC >= 600)) || defined(__ghs__)
		#define GE_FUNC_SIG __PRETTY_FUNCTION__
	#elif defined(__DMC__) && (__DMC__ >= 0x810)
		#define GE_FUNC_SIG __PRETTY_FUNCTION__
	#elif (defined(__FUNCSIG__) || (_MSC_VER))
		#define GE_FUNC_SIG __FUNCSIG__
	#elif (defined(__INTEL_COMPILER) && (__INTEL_COMPILER >= 600)) || (defined(__IBMCPP__) && (__IBMCPP__ >= 500))
		#define GE_FUNC_SIG __FUNCTION__
	#elif defined(__BORLANDC__) && (__BORLANDC__ >= 0x550)
		#define GE_FUNC_SIG __FUNC__
	#elif defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 199901)
		#define GE_FUNC_SIG __func__
	#elif defined(__cplusplus) && (__cplusplus >= 201103)
		#define GE_FUNC_SIG __func__
	#else
		#define GE_FUNC_SIG "GE_FUNC_SIG unknown!"
	#endif

	#define GE_PROFILE_BEGIN_SESSION(name, filepath) ::GanymedE::Instrumentor::Get().BeginSession(name, filepath)
	#define GE_PROFILE_END_SESSION() ::GanymedE::Instrumentor::Get().EndSession()
	#define GE_PROFILE_THREAD(name) ::GanymedE::Instrumentor::Get().SetThreadName(name)
	// `static` is load-bearing, not tidiness: the record borrows this buffer and outlives the
	// scope, so an automatic `constexpr` would leave the writer holding a pointer into a dead
	// stack frame. It was automatic while the record copied the name into a std::string.
	#define GE_PROFILE_SCOPE_LINE2(name, line) static constexpr auto fixedName##line = ::GanymedE::InstrumentorUtils::CleanupOutputString(name, "__cdecl ");\
												   ::GanymedE::InstrumentationTimer timer##line(fixedName##line.Data)
	#define GE_PROFILE_SCOPE_LINE(name, line) GE_PROFILE_SCOPE_LINE2(name, line)
	#define GE_PROFILE_SCOPE(name) GE_PROFILE_SCOPE_LINE(name, __LINE__)
	#define GE_PROFILE_FUNCTION() GE_PROFILE_SCOPE(GE_FUNC_SIG)

	// For a name that is a `const char*` rather than a literal - a system's Name(), a pass name.
	// **The pointer is borrowed and must outlive the process**, so this takes string literals
	// reached indirectly, never a std::string's c_str() or a local buffer.
	#define GE_PROFILE_SCOPE_DYNAMIC_LINE2(name, line) ::GanymedE::InstrumentationTimer timer##line(name)
	#define GE_PROFILE_SCOPE_DYNAMIC_LINE(name, line) GE_PROFILE_SCOPE_DYNAMIC_LINE2(name, line)
	#define GE_PROFILE_SCOPE_DYNAMIC(name) GE_PROFILE_SCOPE_DYNAMIC_LINE(name, __LINE__)
#else
	#define GE_PROFILE_BEGIN_SESSION(name, filepath)
	#define GE_PROFILE_END_SESSION()
	#define GE_PROFILE_THREAD(name)
	#define GE_PROFILE_SCOPE(name)
	#define GE_PROFILE_SCOPE_DYNAMIC(name)
	#define GE_PROFILE_FUNCTION()
#endif
