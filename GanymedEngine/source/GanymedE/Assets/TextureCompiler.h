#pragma once

#include "GanymedE/Assets/AssetCompiler.h"

namespace GanymedE {

	// PNG/JPEG source -> a mipped, block-compressed DDS the GPU can consume directly.
	//
	// This is the single largest *runtime* win the asset milestone has: textures used to be
	// stb-decoded to RGBA8 on the main thread at load and uploaded uncompressed, so a 2048x2048
	// albedo cost 16 MB of VRAM and no mips. BC7 makes that 4 MB with a full mip chain, at a
	// quality loss that is not visible on albedo.
	//
	// `.meta` Config keys, all optional:
	//
	//   Format:       auto | BC1 | BC3 | BC4 | BC5 | BC7 | RGBA8   (default auto)
	//   GenerateMips: true | false                                 (default true)
	//   MaxSize:      <int>                                        (default 0 = no limit)
	//
	// **`auto` is BC1, or BC3 when the source actually uses its alpha channel** - not BC7. BC7
	// is the better format on quality, but bimg's encoder for it is NVIDIA's AVPCL reference
	// implementation and runs at roughly 10k pixels/second; see Platform/Bimg/TextureEncode.h
	// for the measurement. BC1/BC3 go through libsquish, which is two orders of magnitude
	// faster and has a real quality setting. `Format: BC7` is there for the texture that is
	// worth the wait.
	//
	// **Normal maps are not automatically BC5, and that is deliberate.** BC5 would be both
	// smaller and visibly better on normals, but it stores two channels and needs the shader to
	// reconstruct Z - and Phong.glsl reads `.xyz` straight out of the map. Making it the
	// automatic choice for a normal-map slot is a rendering change, not an asset-pipeline one.
	//
	// **There is deliberately no sRGB key.** The engine has no sRGB pipeline: albedo is sampled
	// as raw RGBA8, lit in that space, and gamma is applied once at tonemap. Adding a colour-space
	// flag here would either do nothing or change the look of every scene, and neither belongs in
	// an asset-pipeline change - see docs/engine/rendering.md.
	class TextureCompiler : public IAssetCompiler
	{
	public:
		const char* Name() const override { return "TextureCompiler"; }

		// 1: BC7/BC5/BC4/BC1 + mips, DDS container.
		uint32_t Version() const override { return 1; }

		bool Compile(const CompileInput& input, CompileOutput& output) const override;
	};

}
