#pragma once

#include <GanymedE.h>

#include "DedicatedConfig.h"
#include "GanymedE/Online/ServerLifecycle.h"

#include <chrono>

namespace GanymedE {

	class DedicatedLayer : public Layer
	{
	public:
		explicit DedicatedLayer(DedicatedConfig config);
		~DedicatedLayer() override = default;

		void OnAttach() override;
		void OnDetach() override;
		void OnUpdate(Timestep ts) override;

	private:
		DedicatedConfig m_Config;
		ServerLifecycle m_Server;
		Ref<Scene> m_Scene;
		bool m_RuntimeStarted = false;
		bool m_Simulating = false;
		std::chrono::steady_clock::time_point m_MatchEnd{};
	};

}
