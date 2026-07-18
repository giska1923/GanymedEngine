#pragma once

#include <entt/entt.hpp>

#include <cstdint>
#include <vector>

#include "GanymedE/Core/Core.h"
#include "GanymedE/Core/Log.h"

namespace GanymedE::ECS {

	// Per-component-type log of "which entities changed", keeping exactly two frames of history.
	//
	// Entries are addressed by a monotonically increasing *virtual index* rather than a position in
	// a vector, so any number of consumers can each hold their own independent read cursor without
	// coordinating. A consumer that reads every frame always finds its cursor still in range; one
	// that skips a frame has lost events and is asserted rather than silently given a partial answer.
	//
	// Indices start at 1 so that cursor == 0 unambiguously means "never read" (a first-ever read is
	// defined to mean "the whole matching world", which is a different thing from "no changes").
	class ChangeBuffer
	{
	public:
		using VirtualIndex = uint64_t;

		void Add(entt::entity entity) { m_Current.push_back(entity); }

		// Shift current -> previous, dropping what was previous. Call once per frame, before systems run.
		void NextFrame()
		{
			// NOTE: advance by the size of the frame being retired (m_Current), not m_Previous —
			// m_Previous is the frame being dropped and its size is unrelated to where the new
			// current frame starts.
			m_PrevFrameStart = m_CurrFrameStart;
			m_CurrFrameStart += m_Current.size();

			// Swap rather than move-assign so both allocations get recycled instead of freed.
			m_Previous.swap(m_Current);
			m_Current.clear();
		}

		// Appends every change logged after 'cursor' to out; returns the new cursor.
		// Entries may repeat (an entity modified twice logs twice) — callers dedup.
		VirtualIndex CollectSince(VirtualIndex cursor, std::vector<entt::entity>& out) const
		{
			GE_CORE_ASSERT(cursor == 0 || cursor >= m_PrevFrameStart,
				"Reactive view skipped a frame - changes were lost. Read every reactive view every frame.");

			const VirtualIndex end = Head();
			VirtualIndex first = cursor < m_PrevFrameStart ? m_PrevFrameStart : cursor;
			if (first > end)   // stale cursor (e.g. carried across scenes); collect nothing
				first = end;

			out.reserve(out.size() + (size_t)(end - first));
			for (VirtualIndex i = first; i < end; i++)
				out.push_back(At(i));

			return end;
		}

		// One past the newest entry. A cursor set to Head() collects nothing until something changes.
		VirtualIndex Head() const { return m_CurrFrameStart + m_Current.size(); }

	private:
		entt::entity At(VirtualIndex i) const
		{
			return i < m_CurrFrameStart
				? m_Previous[(size_t)(i - m_PrevFrameStart)]
				: m_Current[(size_t)(i - m_CurrFrameStart)];
		}

		std::vector<entt::entity> m_Previous, m_Current;

		// Invariant: m_CurrFrameStart == m_PrevFrameStart + m_Previous.size()
		VirtualIndex m_PrevFrameStart = 1, m_CurrFrameStart = 1;
	};
}
