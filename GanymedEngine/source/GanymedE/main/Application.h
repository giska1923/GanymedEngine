#pragma once
#include "GanymedE/Core.h"
#include "GanymedE/events/Event.h"

namespace GanymedE {
	class GE_API Application {
	public:
		Application();
		virtual ~Application();

		void Run();
	};

	// THIS FUNCTION NEEDS TO BE IMPLEMENTED ON CLIENT IN ORDER TO START THE ENGINE
	Application* CreateApplication();
}