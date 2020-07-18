#pragma once

extern GanymedE::Application* GanymedE::CreateApplication();

#ifdef GE_PLATFORM_WINDOWS
int main() {
	GanymedE::Log::Init();
	GE_INFO("GanymedEngine initialized!");

	GanymedE::Application* app = GanymedE::CreateApplication();
	app->Run();
	delete app;
	return 0;
}
#endif