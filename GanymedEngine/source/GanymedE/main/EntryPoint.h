#pragma once

extern GanymedE::Application* GanymedE::CreateApplication();

#ifdef GE_PLATFORM_WINDOWS
int main() {
	GanymedE::Application* app = GanymedE::CreateApplication();
	app->Run();
	delete app;
	return 0;
}
#endif