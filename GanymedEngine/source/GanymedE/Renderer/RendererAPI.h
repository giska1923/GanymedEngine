#pragma once

namespace GanymedE {

	// Once the migration finishes this is nothing but a backend tag. The virtual
	// dispatch layer that used to live here (DrawIndexed, SetDepthTest, ...) is
	// gone: bgfx is the only backend, so RenderCommand calls it directly and
	// pipeline state is packed by RenderState.
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

		inline static API GetAPI() { return s_API; }
	private:
		static API s_API;
	};

}
