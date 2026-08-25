#include <GanymedE.h>
//-------------ENTRY POINT-----------------
#include <GanymedE/main/EntryPoint.h>
//-----------------------------------------

#include "RuntimeLayer.h"

namespace GanymedE {

	class GanymedRuntime : public Application
	{
	public:
		GanymedRuntime(const ApplicationSpecification& spec, RuntimeConfig config)
			: Application(spec)
		{
			PushLayer(new RuntimeLayer(std::move(config)));
		}
	};

	Application* CreateApplication()
	{
		// Config first: it decides the window, so it has to be read before the
		// Application ctor creates one. Log::Init already ran in EntryPoint, so a
		// missing or broken config still gets to say so.
		RuntimeConfig config = RuntimeConfig::Load("assets/runtime.yaml");
		const auto& args = Application::GetCommandLineArgs();
		config.ApplyCommandLine(args.Count, args.Args);

		ApplicationSpecification spec;
		spec.Name = config.Title;
		spec.Width = config.Width;
		spec.Height = config.Height;
		spec.Fullscreen = config.Fullscreen;
		spec.EnableImGui = false;

		return new GanymedRuntime(spec, std::move(config));
	}

}
