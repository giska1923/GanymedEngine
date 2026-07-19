#pragma once

#include "GanymedE/Renderer/Buffer.h"
#include "GanymedE/Renderer/RenderState.h"

#include <glm/glm.hpp>

namespace GanymedE {

	// Thin namespace over bgfx, kept in place of the old virtual RendererAPI so
	// the existing call sites keep their shape.
	//
	// The setters no longer touch the GPU: they mutate one RenderState that the
	// next Draw* call packs into bgfx::setState. Draws also need a program,
	// which Shader::Bind will supply via SetProgram once Phase 3 lands - until
	// then the program handle is invalid and draws are skipped.
	class RenderCommand
	{
	public:
		static void Init();

		// The view a submit lands in. bgfx orders the frame by view ID, so this
		// replaces "whichever framebuffer happens to be bound".
		static void SetViewId(uint16_t viewId);
		static uint16_t GetViewId();

		static void SetViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height);
		static void SetClearColor(const glm::vec4& color);
		static void Clear();

		static void SetProgram(bgfx::ProgramHandle program);
		static bgfx::ProgramHandle GetProgram();

		static void SetDepthTest(bool enabled);
		static void SetDepthWrite(bool enabled);
		static void SetDepthFunc(RenderState::DepthFunc func);
		static void SetCullFace(bool enabled);
		static void SetCullMode(RenderState::CullMode mode);
		static void SetBlend(bool enabled);
		static void SetBlendMode(RenderState::BlendMode mode);

		static RenderState& GetState();

		// indexCount 0 means "all of them".
		static void DrawIndexed(const Geometry& geometry, uint32_t indexCount = 0);
		static void DrawIndexed(const Geometry& geometry, uint32_t indexCount, uint32_t baseIndex, int32_t baseVertex);
		static void DrawIndexedInstanced(const Geometry& geometry, uint32_t indexCount, uint32_t baseIndex, int32_t baseVertex,
			const void* instanceData, uint32_t instanceCount, uint16_t instanceStride);
		static void DrawLines(const Geometry& geometry, uint32_t vertexCount);
	};

}
