#pragma once

extern GanymedE::Application* GanymedE::CreateApplication();

#ifdef GE_PLATFORM_WINDOWS
int main(int argc, char** argv) {
	GanymedE::Log::Init();
	GE_INFO("GanymedEngine initialized!");

	GE_PROFILE_BEGIN_SESSION("Startup", "GanymedEProfile-Startup.json");
	GanymedE::Application* app = GanymedE::CreateApplication();
	GE_PROFILE_END_SESSION();

	GE_PROFILE_BEGIN_SESSION("Runtime", "GanymedEProfile-Runtime.json");
	app->Run();
	GE_PROFILE_END_SESSION();

	GE_PROFILE_BEGIN_SESSION("Startup", "GanymedEProfile-Shutdown.json");
	delete app;
	GE_PROFILE_END_SESSION();

	return 0;
}
#endif