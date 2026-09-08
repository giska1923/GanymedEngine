#pragma once

#include "GanymedE/Assets/AssetTypes.h"
#include "GanymedE/Core/Core.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace GanymedE {

	// Per-asset import settings, straight out of the `.meta` sidecar's Config block. Flat
	// scalar->scalar, because that is the shape import settings actually take (format, quality,
	// generate-mips, max size) - see AssetMeta.h for why nested config is not representable.
	using AssetConfig = std::map<std::string, std::string>;

	// Read helpers, so every compiler reads a missing or malformed key the same way: fall back,
	// never throw. A `.meta` is hand-editable and committed, so a typo in one is a normal event.
	inline std::string ConfigString(const AssetConfig& config, const char* key, const char* fallback)
	{
		auto it = config.find(key);
		return it == config.end() ? std::string(fallback) : it->second;
	}

	inline bool ConfigBool(const AssetConfig& config, const char* key, bool fallback)
	{
		auto it = config.find(key);
		if (it == config.end())
			return fallback;

		return it->second == "true" || it->second == "1";
	}

	inline int ConfigInt(const AssetConfig& config, const char* key, int fallback)
	{
		auto it = config.find(key);
		if (it == config.end())
			return fallback;

		try { return std::stoi(it->second); }
		catch (...) { return fallback; }
	}

	struct CompileInput
	{
		const AssetMetadata* Metadata = nullptr;
		std::filesystem::path SourceFullPath;

		// The whole source file. Read once by CompiledCache so a compiler never has to think
		// about IO, and so the same bytes feed the content hash that decides staleness.
		const std::vector<uint8_t>* SourceBytes = nullptr;

		const AssetConfig* Config = nullptr;
	};

	struct CompileOutput
	{
		std::vector<uint8_t> Bytes;

		// Other *source* assets this output was built from, asset-root-relative. Recorded in the
		// `.dep` so a change to one of them invalidates this output. Not a general dependency
		// graph - see ASSET_PIPELINE_ROADMAP.md decision 11.
		std::vector<std::string> Dependencies;

		// Reported back for the log line, and the data a statistics panel would need if one is
		// ever wanted. BlankEngine returns these through an `rttr::variant`; a plain struct does
		// the same job without the reflection dependency.
		double CompileMs = 0.0;
	};

	// Source bytes in, compiled bytes out.
	//
	// **A compiler must be a pure function of its input**, because the epoch record decides
	// staleness from exactly what CompileInput carries: the compiler version, the source bytes,
	// the config, and the declared dependencies. Reading anything else - an environment variable,
	// a file it did not declare - produces an output the cache cannot know is stale.
	class IAssetCompiler
	{
	public:
		virtual ~IAssetCompiler() = default;

		virtual const char* Name() const = 0;

		// **Bump this whenever the output bytes could change for identical input.** It is the
		// property that makes editing an importer safe: every output the previous version
		// produced becomes stale at once, with no cache to clear by hand.
		virtual uint32_t Version() const = 0;

		virtual bool Compile(const CompileInput& input, CompileOutput& output) const = 0;
	};

}
