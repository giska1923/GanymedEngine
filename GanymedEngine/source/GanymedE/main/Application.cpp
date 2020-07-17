#include "Application.h"
#include "GanymedE/events/ApplicationEvent.h"
#include "GanymedE/Log.h"

namespace GanymedE {
	Application::Application() {

	}

	Application::~Application() {

	}

	void Application::Run() {
		WindowResizeEvent we(640, 480);
		GE_TRACE(we);

		while (1);
	}
}