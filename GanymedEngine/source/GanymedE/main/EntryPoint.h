#pragma once

extern GanymedE::Application* GanymedE::CreateApplication();

#if defined(GE_PLATFORM_WINDOWS) || defined(GE_PLATFORM_LINUX) || defined(GE_PLATFORM_MACOS)
int main(int argc, char** argv) {
	GanymedE::Log::Init();
	
#ifdef GE_PLATFORM_WINDOWS
	GE_INFO("GanymedEngine initialized on Windows!");
#elif defined(GE_PLATFORM_LINUX)
	GE_INFO("GanymedEngine initialized on Linux!");
#elif defined(GE_PLATFORM_MACOS)
	GE_INFO("GanymedEngine initialized on macOS!");
#endif

	GE_PROFILE_BEGIN_SESSION("Startup", "GanymedEProfile-Startup.json");
	GanymedE::Application* app = GanymedE::CreateApplication();
	GE_PROFILE_END_SESSION();

	GE_PROFILE_BEGIN_SESSION("Runtime", "GanymedEProfile-Runtime.json");
	app->Run();
	GE_PROFILE_END_SESSION();

	GE_PROFILE_BEGIN_SESSION("Shutdown", "GanymedEProfile-Shutdown.json");
	delete app;
	GE_PROFILE_END_SESSION();

	return 0;
}
#endif