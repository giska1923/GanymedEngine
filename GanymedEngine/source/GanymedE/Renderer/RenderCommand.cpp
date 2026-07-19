#include "gepch.h"
#include "RenderCommand.h"
#include "FrameUniforms.h"

#include "GanymedE/Renderer/RenderPassIDs.h"

namespace GanymedE {

	namespace {

		RenderState s_State;
		uint16_t s_ViewId = RenderPass::Backbuffer;
		bgfx::ProgramHandle s_Program = BGFX_INVALID_HANDLE;

		// Applies the shared per-draw setup. Returns false when the draw cannot
		// go through, which is the normal case until Phase 3 supplies programs.
		//
		// Bailing out has to DISCARD, not just return: uniforms, textures and
		// buffers set by the caller are pending draw-call state that only a
		// submit consumes. Leaving them queued makes the next setUniform of the
		// same name assert with "was already set for this draw call".
		bool BeginSubmit(const Geometry& geometry)
		{
			if (!geometry.IsValid() || !bgfx::isValid(s_Program))
			{
				bgfx::discard();
				return false;
			}

			if (geometry.Vertices->IsDynamic())
				bgfx::setVertexBuffer(0, geometry.Vertices->GetDynamicHandle());
			else
				bgfx::setVertexBuffer(0, geometry.Vertices->GetStaticHandle());

			return true;
		}

	}

	void RenderCommand::Init()
	{
		s_State = RenderState{};
		s_ViewId = RenderPass::Backbuffer;
		s_Program = BGFX_INVALID_HANDLE;
	}

	void RenderCommand::SetViewId(uint16_t viewId) { s_ViewId = viewId; }
	uint16_t RenderCommand::GetViewId() { return s_ViewId; }

	void RenderCommand::SetViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
	{
		bgfx::setViewRect(s_ViewId, (uint16_t)x, (uint16_t)y, (uint16_t)width, (uint16_t)height);
	}

	void RenderCommand::SetClearColor(const glm::vec4& color)
	{
		const uint32_t rgba =
			  (uint32_t(glm::clamp(color.r, 0.0f, 1.0f) * 255.0f) << 24)
			| (uint32_t(glm::clamp(color.g, 0.0f, 1.0f) * 255.0f) << 16)
			| (uint32_t(glm::clamp(color.b, 0.0f, 1.0f) * 255.0f) << 8)
			|  uint32_t(glm::clamp(color.a, 0.0f, 1.0f) * 255.0f);

		bgfx::setViewClear(s_ViewId, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, rgba, 1.0f, 0);
	}

	void RenderCommand::Clear()
	{
		// bgfx clears as part of processing a view rather than on demand; touch
		// guarantees the view is processed even with nothing submitted to it.
		bgfx::touch(s_ViewId);
	}

	void RenderCommand::SetProgram(bgfx::ProgramHandle program) { s_Program = program; }
	bgfx::ProgramHandle RenderCommand::GetProgram() { return s_Program; }

	void RenderCommand::SetDepthTest(bool enabled) { s_State.DepthTest = enabled; }
	void RenderCommand::SetDepthWrite(bool enabled) { s_State.DepthWrite = enabled; }
	void RenderCommand::SetDepthFunc(RenderState::DepthFunc func) { s_State.Depth = func; }
	void RenderCommand::SetCullFace(bool enabled) { s_State.CullFace = enabled; }
	void RenderCommand::SetCullMode(RenderState::CullMode mode) { s_State.Cull = mode; }
	void RenderCommand::SetBlend(bool enabled) { s_State.Blend = enabled; }
	void RenderCommand::SetBlendMode(RenderState::BlendMode mode) { s_State.Blending = mode; }

	RenderState& RenderCommand::GetState() { return s_State; }

	void RenderCommand::DrawIndexed(const Geometry& geometry, uint32_t indexCount)
	{
		if (!BeginSubmit(geometry))
			return;

		const uint32_t count = indexCount ? indexCount : geometry.Indices->GetCount();

		bgfx::setIndexBuffer(geometry.Indices->GetHandle(), 0, count);
		bgfx::setState(s_State.ToBgfx());
		FrameUniforms::Apply();
		bgfx::submit(s_ViewId, s_Program);
	}

	void RenderCommand::DrawIndexed(const Geometry& geometry, uint32_t indexCount, uint32_t baseIndex, int32_t baseVertex)
	{
		if (!BeginSubmit(geometry))
			return;

		// bgfx has no baseVertex on setIndexBuffer; the offset is applied to the
		// vertex buffer binding instead.
		if (geometry.Vertices->IsDynamic())
			bgfx::setVertexBuffer(0, geometry.Vertices->GetDynamicHandle(), baseVertex, UINT32_MAX);
		else
			bgfx::setVertexBuffer(0, geometry.Vertices->GetStaticHandle(), baseVertex, UINT32_MAX);

		bgfx::setIndexBuffer(geometry.Indices->GetHandle(), baseIndex, indexCount);
		bgfx::setState(s_State.ToBgfx());
		FrameUniforms::Apply();
		bgfx::submit(s_ViewId, s_Program);
	}

	void RenderCommand::DrawIndexedInstanced(const Geometry& geometry, uint32_t indexCount, uint32_t baseIndex, int32_t baseVertex,
		const void* instanceData, uint32_t instanceCount, uint16_t instanceStride)
	{
		if (!BeginSubmit(geometry))
			return;

		// bgfx has no attribute divisor: per-instance data goes through a
		// dedicated buffer the shader reads as i_data0..4.
		if (bgfx::getAvailInstanceDataBuffer(instanceCount, instanceStride) < instanceCount)
		{
			// BeginSubmit already bound buffers for this draw; discard so the
			// pending state does not leak into the next one.
			bgfx::discard();
			return;
		}

		bgfx::InstanceDataBuffer idb;
		bgfx::allocInstanceDataBuffer(&idb, instanceCount, instanceStride);
		memcpy(idb.data, instanceData, size_t(instanceCount) * instanceStride);
		bgfx::setInstanceDataBuffer(&idb);

		if (geometry.Vertices->IsDynamic())
			bgfx::setVertexBuffer(0, geometry.Vertices->GetDynamicHandle(), baseVertex, UINT32_MAX);
		else
			bgfx::setVertexBuffer(0, geometry.Vertices->GetStaticHandle(), baseVertex, UINT32_MAX);

		bgfx::setIndexBuffer(geometry.Indices->GetHandle(), baseIndex, indexCount);
		bgfx::setState(s_State.ToBgfx());
		FrameUniforms::Apply();
		bgfx::submit(s_ViewId, s_Program);
	}

	void RenderCommand::DrawLines(const Geometry& geometry, uint32_t vertexCount)
	{
		if (!geometry.Vertices || !geometry.Vertices->IsValid() || !bgfx::isValid(s_Program))
		{
			bgfx::discard();
			return;
		}

		if (geometry.Vertices->IsDynamic())
			bgfx::setVertexBuffer(0, geometry.Vertices->GetDynamicHandle(), 0, vertexCount);
		else
			bgfx::setVertexBuffer(0, geometry.Vertices->GetStaticHandle(), 0, vertexCount);

		RenderState lineState = s_State;
		lineState.Primitive = RenderState::Topology::Lines;

		bgfx::setState(lineState.ToBgfx());
		FrameUniforms::Apply();
		bgfx::submit(s_ViewId, s_Program);
	}

}
