#pragma once

#include <entt/entt.hpp>
#include <unordered_map>

#include "GanymedE/Core/Core.h"

namespace GanymedE::ECS {

	// One-frame storage for components that were just removed.
	//
	// FiniView (Phase 6) has to *read* a component that no longer exists. entt destroys the
	// instance during remove/destroy, but its on_destroy signal fires **before** removal, so a copy
	// can be taken at that moment and kept alive for exactly one more frame.
	//
	// Lifetime rule: graveyards are cleared at the top of FrameBegin, *before* the command flush
	// that refills them. Anything removed during last frame's update (or by the editor between
	// frames) therefore stays readable for exactly one update.

	class GraveyardBase
	{
	public:
		virtual ~GraveyardBase() = default;
		virtual void Clear() = 0;
	};

	template<typename T>
	class Graveyard : public GraveyardBase
	{
	public:
		// insert_or_assign rather than operator[], so T need not be default-constructible.
		void Bury(entt::entity entity, const T& component) { m_Dead.insert_or_assign(entity, component); }

		const T* Find(entt::entity entity) const
		{
			auto it = m_Dead.find(entity);
			return it != m_Dead.end() ? &it->second : nullptr;
		}

		// Non-const so a FiniView can hand out a writeable slot into the corpse — script teardown
		// nulls the instance pointer on the copy it is given.
		T* Find(entt::entity entity)
		{
			auto it = m_Dead.find(entity);
			return it != m_Dead.end() ? &it->second : nullptr;
		}

		bool Empty() const { return m_Dead.empty(); }
		size_t Size() const { return m_Dead.size(); }

		void Clear() override { m_Dead.clear(); }

	private:
		std::unordered_map<entt::entity, T> m_Dead;
	};
}
