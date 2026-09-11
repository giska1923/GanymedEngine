#include "gepch.h"
#include "GanymedE/Assets/TextureCompiler.h"

#include "GanymedE/Assets/TextureImporter.h"
#include "GanymedE/Core/JobSystem.h"

#include "Platform/Bimg/TextureEncode.h"

namespace GanymedE {

	namespace {

		struct FormatChoice
		{
			EncodedFormat Format = EncodedFormat::Auto;
			bool NormalMap = false;
		};

		FormatChoice ChooseFormat(const AssetConfig& config, const std::string& assetPath)
		{
			const std::string requested = ConfigString(config, "Format", "auto");

			// `auto` leaves the choice to the encoder, which is the side holding the pixels and
			// can therefore see whether the alpha channel is used. Everything else is taken at
			// its word.
			//
			// The roadmap wanted the format inferred from the asset's *role* - a material's
			// normal-map slot picking BC5 - but the compiler runs on the texture, and the role
			// lives in whichever `.gmat` references it. Resolving that needs a reverse index the
			// engine deliberately does not have (decision 11), and BC5 needs a shader change
			// besides. `Format:` in the `.meta` is the way in until then.
			if (requested == "auto")   return { EncodedFormat::Auto, false };
			if (requested == "BC7")    return { EncodedFormat::BC7, false };
			if (requested == "BC5")    return { EncodedFormat::BC5, true };
			if (requested == "BC4")    return { EncodedFormat::BC4, false };
			if (requested == "BC3")    return { EncodedFormat::BC3, false };
			if (requested == "BC1")    return { EncodedFormat::BC1, false };
			if (requested == "RGBA8")  return { EncodedFormat::RGBA8, false };

			GE_CORE_WARN("Texture '{0}' asks for an unknown Format '{1}' - using auto",
				assetPath, requested);
			return { EncodedFormat::Auto, false };
		}

	}

	bool TextureCompiler::Compile(const CompileInput& input, CompileOutput& output) const
	{
		GE_PROFILE_FUNCTION();

		const std::string& assetPath = input.Metadata->FilePath;
		const AssetConfig& config = *input.Config;

		// stb, through the same decoder every other texture path in the engine uses - so the set
		// of source formats that work is exactly what it was before compilation existed.
		DecodedImage decoded = TextureImporter::DecodeFromMemory(
			input.SourceBytes->data(), input.SourceBytes->size(), /*flipVertically=*/false);

		if (!decoded)
		{
			GE_CORE_ERROR("Could not decode texture '{0}'", assetPath);
			return false;
		}

		const FormatChoice choice = ChooseFormat(config, assetPath);

		EncodeOptions options;
		options.Format = choice.Format;
		options.NormalMap = choice.NormalMap;
		options.GenerateMips = ConfigBool(config, "GenerateMips", true);
		options.MaxSize = (uint32_t)std::max(0, ConfigInt(config, "MaxSize", 0));

		// The threading milestone's first consumer (THREADING_ROADMAP.md T3), and the reason it
		// is the right one: this is offline work with no frame budget and no lifetime hazards,
		// so a bug costs import time rather than a corrupted frame. The encoder takes the
		// dispatch as a parameter because it is a separate C++20 library and cannot include
		// JobSystem.h - see Platform/Bimg/TextureEncode.h.
		options.ParallelFor = [](uint32_t count, uint32_t minRange, const EncodeOptions::RangeFn& fn)
		{
			JobSystem::ParallelFor(count, minRange, [&fn](uint32_t begin, uint32_t end, uint32_t)
			{
				fn(begin, end);
			});
		};

		// Coarse cancellation, per the threading milestone's contract: enkiTS cannot dequeue a
		// started task, so a body that wants to stop early has to ask.
		options.ShouldCancel = [] { return JobSystem::IsCurrentJobCancelled(); };

		EncodeResult encoded;
		if (!EncodeTexture(decoded.Pixels.get(), decoded.Width, decoded.Height, options, encoded))
		{
			GE_CORE_ERROR("Could not compile texture '{0}': {1}", assetPath,
				encoded.Message.empty() ? "unknown failure" : encoded.Message);
			return false;
		}

		if (!encoded.Message.empty())
			GE_CORE_WARN("Texture '{0}': {1}", assetPath, encoded.Message);

		GE_CORE_TRACE("Texture '{0}': {1}x{2}, {3} mips, {4}", assetPath,
			encoded.Width, encoded.Height, encoded.MipCount,
			encoded.Compressed ? "block compressed" : "uncompressed");

		output.Bytes = std::move(encoded.Dds);
		return true;
	}

}
