#pragma once

#include <cstdio>
#include <string>

extern GanymedE::Application* GanymedE::CreateApplication();

#if defined(GE_PLATFORM_WINDOWS) || defined(GE_PLATFORM_LINUX) || defined(GE_PLATFORM_MACOS)
int main(int argc, char** argv) {
	// Before Init: a dedicated server's --root has to apply before GanymedE.log would be
	// truncated, and before CreateApplication reads assets/dedicated.yaml.
	std::string error;
	if (!GanymedE::Log::Prepare(argc, argv, error))
	{
		std::fprintf(stderr, "%s\n", error.c_str());
		return 2;
	}
	
#ifdef GE_PLATFORM_WINDOWS
	GE_INFO("GanymedEngine initialized on Windows!");
#elif defined(GE_PLATFORM_LINUX)
	GE_INFO("GanymedEngine initialized on Linux!");
#elif defined(GE_PLATFORM_MACOS)
	GE_INFO("GanymedEngine initialized on macOS!");
#endif

	GE_PROFILE_BEGIN_SESSION("Startup", "GanymedEProfile-Startup.json");
	GanymedE::Application::SetCommandLineArgs({ argc, argv });
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