#pragma once

#include "RendererAPI.h"

namespace GanymedE {

	class RenderCommand
	{
	public:
		inline static void Init()
		{
			s_RendererAPI->Init();
		}

		inline static void SetViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
		{
			s_RendererAPI->SetViewport(x, y, width, height);
		}

		inline static void SetClearColor(const glm::vec4& color)
		{
			s_RendererAPI->SetClearColor(color);
		}

		inline static void Clear()
		{
			s_RendererAPI->Clear();
		}

		inline static void SetDepthTest(bool enabled)
		{
			s_RendererAPI->SetDepthTest(enabled);
		}

		inline static void SetDepthWrite(bool enabled)
		{
			s_RendererAPI->SetDepthWrite(enabled);
		}

		inline static void SetDepthFunc(RendererAPI::DepthFunc func)
		{
			s_RendererAPI->SetDepthFunc(func);
		}

		inline static void SetCullFace(bool enabled)
		{
			s_RendererAPI->SetCullFace(enabled);
		}

		inline static void SetCullMode(RendererAPI::CullMode mode)
		{
			s_RendererAPI->SetCullMode(mode);
		}

		inline static void SetBlend(bool enabled)
		{
			s_RendererAPI->SetBlend(enabled);
		}

		inline static void SetBlendMode(RendererAPI::BlendMode mode)
		{
			s_RendererAPI->SetBlendMode(mode);
		}

		inline static void DrawIndexed(const Ref<VertexArray>& vertexArray, uint32_t count = 0)
		{
			s_RendererAPI->DrawIndexed(vertexArray, count);
		}

		inline static void DrawIndexed(const Ref<VertexArray>& vertexArray, uint32_t indexCount, uint32_t baseIndex, int32_t baseVertex)
		{
			s_RendererAPI->DrawIndexed(vertexArray, indexCount, baseIndex, baseVertex);
		}

		inline static void DrawIndexedInstanced(const Ref<VertexArray>& vertexArray, uint32_t indexCount, uint32_t baseIndex, int32_t baseVertex, uint32_t instanceCount)
		{
			s_RendererAPI->DrawIndexedInstanced(vertexArray, indexCount, baseIndex, baseVertex, instanceCount);
		}

		inline static void DrawLines(const Ref<VertexArray>& vertexArray, uint32_t vertexCount)
		{
			s_RendererAPI->DrawLines(vertexArray, vertexCount);
		}
	private:
		static Scope<RendererAPI> s_RendererAPI;
	};

}
