#include "RuntimeConfig.h"

#include "GanymedE/Core/Log.h"

#include <yaml-cpp/yaml.h>

namespace GanymedE {

	namespace {

		// Read a key if present, leave the default alone otherwise. Every field is
		// optional on purpose: a config that only overrides Fullscreen should not have
		// to restate the scene path, and a typo'd key must not zero a field.
		template<typename T>
		void ReadInto(const YAML::Node& root, const char* key, T& out)
		{
			if (const YAML::Node node = root[key])
				out = node.as<T>();
		}

	}

	RuntimeConfig RuntimeConfig::Load(const std::filesystem::path& path)
	{
		RuntimeConfig config;

		if (!std::filesystem::exists(path))
		{
			GE_WARN("Runtime config '{0}' not found - booting with defaults", path.string());
			return config;
		}

		try
		{
			YAML::Node root = YAML::LoadFile(path.string());

			ReadInto(root, "AssetRoot", config.AssetRoot);
			ReadInto(root, "StartScene", config.StartScene);
			ReadInto(root, "UIDocument", config.UIDocument);
			ReadInto(root, "Title", config.Title);
			ReadInto(root, "Width", config.Width);
			ReadInto(root, "Height", config.Height);
			ReadInto(root, "Fullscreen", config.Fullscreen);
		}
		catch (const YAML::Exception& e)
		{
			// yaml-cpp throws on malformed input and on a type mismatch in as<T>(). Caught
			// here rather than left to terminate: a mistyped config should degrade to
			// defaults with an explanation, and the partially-applied fields above are
			// harmless because each one is independent.
			GE_ERROR("Runtime config '{0}' failed to parse ({1}) - booting with defaults",
				path.string(), e.what());
			return RuntimeConfig{};
		}

		// A zero-sized window is a bgfx framebuffer error, not a valid request.
		if (config.Width == 0 || config.Height == 0)
		{
			GE_WARN("Runtime config asked for a {0}x{1} window - falling back to 1600x900",
				config.Width, config.Height);
			config.Width = 1600;
			config.Height = 900;
		}

		return config;
	}

	void RuntimeConfig::ApplyCommandLine(int argc, const char* const* argv)
	{
		if (argc <= 1)
			return;

		// Skip options the engine reads itself (--renderer=, --root). `--connect` and
		// the net flags are this app's, and their values are not a scene path.
		const char* positional = nullptr;
		for (int i = 1; i < argc; i++)
		{
			const char* arg = argv[i];
			if (!arg)
				continue;

			const auto take = [&](const char* name, std::string& dest) -> bool
			{
				const std::string dashed = std::string("--") + name;
				const std::string single = std::string("-") + name;
				const std::string text = arg;
				if (text == dashed || text == single)
				{
					if (i + 1 < argc && argv[i + 1])
						dest = argv[++i];
					return true;
				}
				const std::string eq = dashed + "=";
				if (text.rfind(eq, 0) == 0)
				{
					dest = text.substr(eq.size());
					return true;
				}
				return false;
			};

			if (take("connect", Connect) || take("name", Name) || take("net-sim", NetSim) || take("net-test", NetTest))
				continue;
			if (arg[0] == '-')
				continue;
			if (!positional)
				positional = arg;
		}

		if (!positional)
			return;

		std::filesystem::path scene = positional;
		if (scene.extension() != ".ganymede")
		{
			GE_WARN("Ignoring command-line argument '{0}': not a .ganymede scene", scene.string());
			return;
		}

		GE_INFO("Scene overridden on the command line: {0}", scene.string());
		StartScene = scene.generic_string();
	}

	void RuntimeConfig::Log() const
	{
		GE_INFO("Runtime config: root='{0}' scene='{1}' ui='{2}' title='{3}' {4}x{5} fullscreen={6}",
			AssetRoot, StartScene, UIDocument.empty() ? "<none>" : UIDocument, Title,
			Width, Height, Fullscreen);
	}

}
