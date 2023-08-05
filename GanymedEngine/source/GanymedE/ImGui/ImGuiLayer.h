#pragma once

#include "GanymedE/Core/Layer.h"

#include "GanymedE/events/ApplicationEvent.h"
#include "GanymedE/events/KeyEvent.h"
#include "GanymedE/events/MouseEvent.h"

namespace GanymedE {
	class GE_API ImGuiLayer : public Layer {
	public:
		ImGuiLayer();
		~ImGuiLayer();

		virtual void OnAttach() override;
		virtual void OnDetach() override;
		virtual void OnEvent(Event& e) override;

		void Begin();
		void End();

		void BlockEvents(bool block) { m_BlockEvents = block; }
	private:
		bool m_BlockEvents = true;
		float m_Time = 0.0f;
	};
}