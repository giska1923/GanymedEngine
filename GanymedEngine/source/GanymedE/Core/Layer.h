#pragma once

#include "GanymedE/Core/Core.h"
#include "GanymedE/Core/Timestep.h"
#include "GanymedE/events/Event.h"

namespace GanymedE {

	class GE_API Layer {
	public:
		Layer(const std::string& name = "Layer");
		virtual ~Layer();

		virtual void OnAttach() {}
		virtual void OnDetach() {}
		virtual void OnUpdate(Timestep ts) {}
		virtual void OnImGuiRender() {}
		virtual void OnEvent(Event& event) {}

		inline const std::string& GetName() const { return m_DebugName; }

		// OnDetach tears down what OnAttach built, so it must never run against
		// a layer that was never attached. Application owns this flag because it
		// is what decides whether attaching is possible at all.
		inline bool IsAttached() const { return m_Attached; }
		inline void SetAttached(bool attached) { m_Attached = attached; }
	protected:
		std::string m_DebugName;
	private:
		bool m_Attached = false;
	};
}