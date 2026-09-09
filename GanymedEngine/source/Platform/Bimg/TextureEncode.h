#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace GanymedE {

	// Block-compression, isolated behind a header that mentions neither bimg nor bx.
	//
	// **This module is its own static library, compiled as C++20, and the reason is a hard
	// constraint rather than a preference:** `bx/platform.h` refuses to compile below C++20, and
	// the engine is C++17 (see AGENTS.md). `Platform/Bgfx/` exists for the same shape of reason,
	// and bgfx.lua's own comment records the matching rule for bgfx - `<bgfx/bgfx.h>` was chosen
	// precisely because it pulls in no bx headers. bimg's public headers only *forward-declare*
	// the bx types, but its API takes them by pointer, so any caller needs the real definitions.
	//
	// The alternative was raising the whole engine to C++20 for one file. That is one line and a
	// project-wide language change touching every vendored header; this is a build file and a
	// narrow API. If the engine moves to C++20 for its own reasons, fold this back in.

	enum class EncodedFormat : uint8_t
	{
		Auto,    // BC1, or BC3 if the source actually uses its alpha channel
		BC1,     // colour, 1-bit alpha - 4 bpp
		BC3,     // colour + interpolated alpha - 8 bpp
		BC4,     // one channel - 4 bpp
		BC5,     // two channels, the normal-map format - 8 bpp
		BC7,     // best quality colour + alpha - 8 bpp, and see the warning below
		RGBA8,   // uncompressed, the fallback
	};

	// **BC7 is opt-in, not the default, and the reason is measured rather than assumed.** bimg's
	// BC7 encoder is NVIDIA's AVPCL *reference* implementation - an exhaustive mode and partition
	// search written to be correct, not fast. Measured here on an optimised build: a 256x256
	// texture with mips takes 30 s serial and 6.4 s across 15 workers, i.e. ~10k pixels/second.
	// A 2048x2048 albedo would be minutes. BC1 and BC3 go through libsquish instead, which is a
	// production encoder with a real quality setting.
	//
	// AVPCL also keeps four mutable globals that `compressBC7` writes per block, so BC7 is
	// encoded serially here. They are set to the same constant every time, which makes the race
	// benign in practice - "benign in practice" is not a thing to build a thread on.

	struct EncodeOptions
	{
		EncodedFormat Format = EncodedFormat::Auto;

		// Tells the encoder to optimise for angular error rather than RGB error. Only meaningful
		// for BC5, which is what a normal map compresses to.
		bool NormalMap = false;

		bool GenerateMips = true;

		// Longest edge, or 0 for no limit. Applied by starting the chain at a smaller mip rather
		// than resampling - the chain already holds every halving of the source.
		uint32_t MaxSize = 0;

		// The engine's ParallelFor, injected. This module cannot call JobSystem directly: the
		// engine links *it*, so including an engine header here would be a cycle, and the two
		// are compiled at different language levels. Left empty, encoding is serial.
		using RangeFn = std::function<void(uint32_t begin, uint32_t end)>;
		std::function<void(uint32_t count, uint32_t minRange, const RangeFn&)> ParallelFor;

		// Polled between mips. A cancelled encode still has to run to *some* boundary - the
		// scheduler cannot pull a started task back - so the question is only how coarse the
		// boundary is. Between mips is the natural one here: mip 0 is most of the work, so this
		// bounds the wait at roughly one mip rather than the whole chain. Left empty, nothing
		// cancels and the encode always runs to completion.
		std::function<bool()> ShouldCancel;
	};

	struct EncodeResult
	{
		std::vector<uint8_t> Dds;

		uint32_t Width = 0;
		uint32_t Height = 0;
		uint8_t MipCount = 0;
		bool Compressed = false;

		// Why the result is not what was asked for - an unencodable size, a failed decode. The
		// caller logs it; this module has no logger, deliberately.
		std::string Message;
	};

	// Decoded RGBA8 pixels, tightly packed, to a DDS container.
	//
	// Pixels rather than file bytes on purpose: `bimg::imageParse` would be the obvious front
	// door, but upstream's `bimg_decode` project drags in dav1d and libavif for AV1 support -
	// megabytes of C for formats nothing here loads. The engine already vendors stb_image and
	// decodes PNG/JPEG with it, so the source format support stays exactly what it was.
	bool EncodeTexture(const uint8_t* rgba8, uint32_t width, uint32_t height,
		const EncodeOptions& options, EncodeResult& result);

}
