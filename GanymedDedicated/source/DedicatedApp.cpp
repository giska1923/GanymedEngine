#include <GanymedE.h>
#include <GanymedE/main/EntryPoint.h>

#include "DedicatedLayer.h"

#include <cstdlib>

namespace GanymedE {

	class GanymedDedicated : public Application
	{
	public:
		GanymedDedicated(const ApplicationSpecification& spec, DedicatedConfig config)
			: Application(spec)
		{
			PushLayer(new DedicatedLayer(std::move(config)));
		}
	};

	Application* CreateApplication()
	{
		DedicatedConfig config = DedicatedConfig::Load("assets/dedicated.yaml");

		const auto& args = Application::GetCommandLineArgs();
		std::string error;
		bool ok = false;
		config = ConfigFromCommandLine(std::move(config), args.Count, args.Args, error, ok);
		if (!ok)
		{
			GE_CORE_ERROR("{0}", error);
			std::exit(2);
		}

		ApplicationSpecification spec;
		spec.Name = "GanymedDedicated";
		spec.Width = 1280;
		spec.Height = 720;
		spec.EnableImGui = false;
		spec.Headless = true;
		spec.PlayerBackend = false;

		return new GanymedDedicated(spec, std::move(config));
	}

}
