#pragma once

#include <glm/glm.hpp>

#include "VertexArray.h"

namespace GanymedE {

	class RendererAPI
	{
	public:
		enum class API
		{
			None = 0,
			OpenGL = 1,
			// Under bgfx this stops being a dispatch mechanism: there is one
			// backend, and the concrete GPU API is chosen via bgfx::Init::type.
			// It survives only as the migration switch until Platform/OpenGL is
			// deleted in Phase 7. See docs/BGFX_MIGRATION.md.
			Bgfx = 2
		};

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
			Alpha = 0, // src_alpha, one_minus_src_alpha (default)
			Additive   // one, one
		};
	public:
		virtual ~RendererAPI() = default;

		virtual void Init() = 0;
		virtual void SetViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height) = 0;
		virtual void SetClearColor(const glm::vec4& color) = 0;
		virtual void Clear() = 0;

		virtual void SetDepthTest(bool enabled) = 0;
		virtual void SetDepthWrite(bool enabled) = 0;
		virtual void SetDepthFunc(DepthFunc func) = 0;
		virtual void SetCullFace(bool enabled) = 0;
		virtual void SetCullMode(CullMode mode) = 0;
		virtual void SetBlend(bool enabled) = 0;
		virtual void SetBlendMode(BlendMode mode) = 0;

		virtual void DrawIndexed(const Ref<VertexArray>& vertexArray, uint32_t indexCount = 0) = 0;
		virtual void DrawIndexed(const Ref<VertexArray>& vertexArray, uint32_t indexCount, uint32_t baseIndex, int32_t baseVertex) = 0;
		virtual void DrawIndexedInstanced(const Ref<VertexArray>& vertexArray, uint32_t indexCount, uint32_t baseIndex, int32_t baseVertex, uint32_t instanceCount) = 0;
		virtual void DrawLines(const Ref<VertexArray>& vertexArray, uint32_t vertexCount) = 0;

		inline static API GetAPI() { return s_API; }
	private:
		static API s_API;
	};

}
