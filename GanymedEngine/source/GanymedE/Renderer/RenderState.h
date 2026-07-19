#pragma once

#include <bgfx/bgfx.h>

#include <cstdint>

namespace GanymedE {

	// bgfx has no persistent pipeline state: depth/cull/blend/write flags are
	// packed into a single uint64 handed to bgfx::setState on every submit.
	//
	// This mirrors the old RendererAPI setters so the call sites that say
	// "SetDepthTest(false)" keep reading the same way; RenderCommand owns one of
	// these and folds it into each draw.
	struct RenderState
	{
		enum class DepthFunc
		{
			Never = 0,
			Less,
			Equal,
			LessEqual,
			Greater,
			NotEqual,
			GreaterEqual,
			Always
		};

		enum class CullMode
		{
			Front = 0,
			Back,
			FrontAndBack
		};

		enum class BlendMode
		{
			Alpha = 0, // src_alpha, one_minus_src_alpha
			Additive   // one, one
		};

		enum class Topology
		{
			Triangles = 0,
			Lines
		};

		bool DepthTest = true;
		bool DepthWrite = true;
		DepthFunc Depth = DepthFunc::Less;

		bool CullFace = true;
		CullMode Cull = CullMode::Back;

		bool Blend = false;
		BlendMode Blending = BlendMode::Alpha;

		bool WriteRGB = true;
		bool WriteAlpha = true;

		Topology Primitive = Topology::Triangles;

		uint64_t ToBgfx() const
		{
			uint64_t state = 0;

			if (WriteRGB)
				state |= BGFX_STATE_WRITE_RGB;
			if (WriteAlpha)
				state |= BGFX_STATE_WRITE_A;

			if (DepthWrite)
				state |= BGFX_STATE_WRITE_Z;

			if (DepthTest)
			{
				switch (Depth)
				{
					case DepthFunc::Never:        state |= BGFX_STATE_DEPTH_TEST_NEVER;    break;
					case DepthFunc::Less:         state |= BGFX_STATE_DEPTH_TEST_LESS;     break;
					case DepthFunc::Equal:        state |= BGFX_STATE_DEPTH_TEST_EQUAL;    break;
					case DepthFunc::LessEqual:    state |= BGFX_STATE_DEPTH_TEST_LEQUAL;   break;
					case DepthFunc::Greater:      state |= BGFX_STATE_DEPTH_TEST_GREATER;  break;
					case DepthFunc::NotEqual:     state |= BGFX_STATE_DEPTH_TEST_NOTEQUAL; break;
					case DepthFunc::GreaterEqual: state |= BGFX_STATE_DEPTH_TEST_GEQUAL;   break;
					case DepthFunc::Always:       state |= BGFX_STATE_DEPTH_TEST_ALWAYS;   break;
				}
			}
			// No DEPTH_TEST bits at all == depth testing disabled in bgfx.

			if (CullFace)
			{
				// bgfx culls by winding order, not by face. The engine's meshes
				// are counter-clockwise front-facing, so "cull back" is CW.
				// Verify against real geometry once something is on screen
				// (Phase 3) - a whole-model invisibility is the tell.
				switch (Cull)
				{
					case CullMode::Back:  state |= BGFX_STATE_CULL_CW;  break;
					case CullMode::Front: state |= BGFX_STATE_CULL_CCW; break;
					case CullMode::FrontAndBack:
						// No bgfx equivalent; the caller wants nothing drawn.
						state |= BGFX_STATE_CULL_CW | BGFX_STATE_CULL_CCW;
						break;
				}
			}

			if (Blend)
			{
				switch (Blending)
				{
					case BlendMode::Alpha:    state |= BGFX_STATE_BLEND_ALPHA;    break;
					case BlendMode::Additive: state |= BGFX_STATE_BLEND_ADD;      break;
				}
			}

			if (Primitive == Topology::Lines)
				state |= BGFX_STATE_PT_LINES;

			return state;
		}
	};

}
