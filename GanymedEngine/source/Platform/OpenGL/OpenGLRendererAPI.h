#pragma once

#include "GanymedE/Renderer/RendererAPI.h"

namespace GanymedE {

	class OpenGLRendererAPI : public RendererAPI
	{
	public:
		virtual void Init() override;
		virtual void SetViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height) override;

		virtual void SetClearColor(const glm::vec4& color) override;
		virtual void Clear() override;

		virtual void SetDepthTest(bool enabled) override;
		virtual void SetDepthWrite(bool enabled) override;
		virtual void SetDepthFunc(DepthFunc func) override;
		virtual void SetCullFace(bool enabled) override;
		virtual void SetCullMode(CullMode mode) override;

		virtual void DrawIndexed(const Ref<VertexArray>& vertexArray, uint32_t indexCount = 0) override;
		virtual void DrawIndexed(const Ref<VertexArray>& vertexArray, uint32_t indexCount, uint32_t baseIndex, int32_t baseVertex) override;
		virtual void DrawLines(const Ref<VertexArray>& vertexArray, uint32_t vertexCount) override;
	};

}
