#pragma once

#include "ChangeBuffer.h"
#include "ComponentTraits.h"

// Read/write proxies handed out as view tuple slots.
//
// The load-bearing invariant: for a component that is both change-tracked and writeable, reads go
// through operator*/-> (which yield const) and the ONLY way to get a mutable reference is Modify(),
// which logs the entity into that type's change buffer. A tracked+writeable slot must therefore
// never be collapsed to a bare T& anywhere downstream, or changes go unrecorded and every
// ChangeView consumer silently misses them.

namespace GanymedE::ECS {

	// ---- Primary: read-only (tracked or not — reading never logs anything) ----
	template<typename T, bool Optional, bool Writeable,
	         bool Trackable = ComponentTraits<T>::TrackChanges>
	class ComponentAccessor
	{
	public:
		using ComponentType = T;
		static constexpr bool IsOptional = Optional;
		static constexpr bool IsWriteable = false;

		ComponentAccessor() = default;
		ComponentAccessor(const T* component, ChangeBuffer*, entt::entity)
			: m_Component(component) {}

		explicit operator bool() const { return m_Component != nullptr; }
		const T& operator*()  const { return *m_Component; }
		const T* operator->() const { return m_Component; }
		const T* Get()        const { return m_Component; }

	private:
		const T* m_Component = nullptr;
	};

	// ---- Writeable, NOT tracked: plain mutable access, zero overhead ----
	// Modify() exists so call sites can be written uniformly regardless of trackability.
	template<typename T, bool Optional>
	class ComponentAccessor<T, Optional, true, false>
	{
	public:
		using ComponentType = T;
		static constexpr bool IsOptional = Optional;
		static constexpr bool IsWriteable = true;

		ComponentAccessor() = default;
		ComponentAccessor(T* component, ChangeBuffer* buffer, entt::entity)
			: m_Component(component)
		{
			GE_CORE_ASSERT(!buffer, "Non-tracked component was given a change buffer");
			(void)buffer;
		}

		explicit operator bool() const { return m_Component != nullptr; }
		T& operator*()  const { return *m_Component; }
		T* operator->() const { return m_Component; }
		T* Get()        const { return m_Component; }

		T& Modify() const
		{
			GE_CORE_ASSERT(m_Component, "Modify() on a missing optional component");
			return *m_Component;
		}

	private:
		T* m_Component = nullptr;
	};

	// ---- Writeable AND tracked: reads are const, writing requires Modify() ----
	template<typename T, bool Optional>
	class ComponentAccessor<T, Optional, true, true>
	{
	public:
		using ComponentType = T;
		static constexpr bool IsOptional = Optional;
		static constexpr bool IsWriteable = true;

		ComponentAccessor() = default;
		ComponentAccessor(T* component, ChangeBuffer* buffer, entt::entity entity)
			: m_Component(component), m_Buffer(buffer), m_Entity(entity) {}

		explicit operator bool() const { return m_Component != nullptr; }
		const T& operator*()  const { return *m_Component; }
		const T* operator->() const { return m_Component; }
		const T* Get()        const { return m_Component; }

		T& Modify()
		{
			GE_CORE_ASSERT(m_Component, "Modify() on a missing optional component");

			// Log at most once per accessor lifetime: a system that writes ten fields of the same
			// component should not push ten entries. A null buffer means "not logging" — that is
			// the graveyard case (Phase 5), where modifying a corpse is deliberately not a change.
			if (m_Buffer)
			{
				m_Buffer->Add(m_Entity);
				m_Buffer = nullptr;
			}
			return *m_Component;
		}

	private:
		T* m_Component = nullptr;
		ChangeBuffer* m_Buffer = nullptr;
		entt::entity m_Entity = entt::null;
	};
}
