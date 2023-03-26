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

		void Begin();
		void End();
	private:
		float m_Time = 0.0f;
	};
}