#pragma once

#include "GanymedE/Layer.h"

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
		virtual void OnImGuiRender() override;

		void Begin();
		void End();
	private:
		float m_Time = 0.0f;
	};
}