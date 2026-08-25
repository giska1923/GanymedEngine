#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace GanymedE {

	// What the game boots as, read from assets/runtime.yaml.
	//
	// Deliberately app-side rather than an engine type: the engine hosts front-ends and
	// knows nothing about this file's schema, the same way it knows nothing about the
	// editor's imgui.ini. A project-settings asset is the production norm (Unity's Build
	// Settings, Unreal's DefaultGame.ini); the divergence here is scale - one scene, one
	// document, six keys - so it stays a file the app parses, not an asset type.
	struct RuntimeConfig
	{
		std::string StartScene = "assets/scenes/Demo.ganymede";

		// Empty means "no HUD". The editor hard-codes assets/ui/hud.rml at play; that
		// hard-coding is precisely the editor-ism this app exists to shed.
		std::string UIDocument = "assets/ui/hud.rml";

		std::string Title = "GanymedEngine Runtime";
		uint32_t Width = 1600;
		uint32_t Height = 900;
		bool Fullscreen = false;

		// Missing or malformed file: every field keeps its default and a warning is logged.
		// A game that boots to something diagnosable beats one that refuses to start,
		// because the log then tells you which step failed rather than nothing at all.
		static RuntimeConfig Load(const std::filesystem::path& path);

		// Overrides StartScene from the command line: GanymedRuntime <path/to/scene.ganymede>.
		// Only the scene: window geometry and the HUD belong to the build, not to how it
		// happened to be launched.
		void ApplyCommandLine(int argc, const char* const* argv);

		void Log() const;
	};

}
