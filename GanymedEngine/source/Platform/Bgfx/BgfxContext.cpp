#include "gepch.h"
#include "Platform/Bgfx/BgfxContext.h"

#include "GanymedE/Renderer/RenderPassIDs.h"
#include "GanymedE/Renderer/Renderer.h"

#if defined(GE_PLATFORM_WINDOWS)
	#define GLFW_EXPOSE_NATIVE_WIN32
#elif defined(GE_PLATFORM_LINUX)
	#define GLFW_EXPOSE_NATIVE_X11
#elif defined(GE_PLATFORM_MACOS)
	#define GLFW_EXPOSE_NATIVE_COCOA
#endif

#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

// PlatformData and renderFrame() used to live in <bgfx/platform.h>; upstream
// folded that header into bgfx.h.
#include <bgfx/bgfx.h>

namespace GanymedE {

	namespace {

		// The OS window bgfx renders into, plus the display connection it needs
		// on X11. bgfx does not use GLFW, so it has to be handed raw handles.
		void* NativeWindowHandle(GLFWwindow* window)
		{
#if defined(GE_PLATFORM_WINDOWS)
			return glfwGetWin32Window(window);
#elif defined(GE_PLATFORM_LINUX)
			return reinterpret_cast<void*>(glfwGetX11Window(window));
#elif defined(GE_PLATFORM_MACOS)
			return glfwGetCocoaWindow(window);
#else
			return nullptr;
#endif
		}

		void* NativeDisplayHandle()
		{
#if defined(GE_PLATFORM_LINUX)
			return glfwGetX11Display();
#else
			return nullptr;
#endif
		}

	}

	BgfxContext::BgfxContext(GLFWwindow* windowHandle)
		: m_WindowHandle(windowHandle)
	{
		GE_CORE_ASSERT(windowHandle, "Window handle is null!");
	}

	BgfxContext::~BgfxContext()
	{
		if (m_Initialized)
			bgfx::shutdown();
	}

	void BgfxContext::Init(uint32_t width, uint32_t height)
	{
		GE_PROFILE_FUNCTION();

		m_Width = width;
		m_Height = height;

		// Calling renderFrame() before init() puts bgfx in single-threaded mode:
		// the calling thread becomes the render thread. Dropping this (and going
		// multithreaded) is an optional Phase 7 optimisation.
		bgfx::renderFrame();

		bgfx::Init init;
		init.type = bgfx::RendererType::Count; // auto-pick; configurable in Phase 7
		init.vendorId = BGFX_PCI_ID_NONE;
		init.platformData.nwh = NativeWindowHandle(m_WindowHandle);
		init.platformData.ndt = NativeDisplayHandle();
		init.resolution.width = m_Width;
		init.resolution.height = m_Height;
		init.resolution.reset = m_VSync ? BGFX_RESET_VSYNC : BGFX_RESET_NONE;

		if (!bgfx::init(init))
		{
			GE_CORE_ASSERT(false, "Failed to initialize bgfx!");
			return;
		}

		m_Initialized = true;

		const bgfx::Caps* caps = bgfx::getCaps();
		GE_CORE_INFO("bgfx Info:");
		GE_CORE_INFO("  Renderer: {0}", bgfx::getRendererName(bgfx::getRendererType()));
		GE_CORE_INFO("  Max texture size: {0}", caps->limits.maxTextureSize);
		GE_CORE_INFO("  Max FB attachments: {0}", caps->limits.maxFBAttachments);
		GE_CORE_INFO("  Homogeneous depth: {0}", caps->homogeneousDepth);
		GE_CORE_INFO("  Origin bottom left: {0}", caps->originBottomLeft);

		// The whole project is compiled with GLM_FORCE_DEPTH_ZERO_TO_ONE (see the
		// workspace defines in premake5.lua). That is a compile-time choice, so a
		// backend wanting OpenGL's [-1,1] clip depth would not fail loudly - it
		// would just render with wrong near-plane clipping and half the depth
		// precision. Say so instead.
		if (caps->homogeneousDepth)
		{
			GE_CORE_ERROR("Backend '{0}' expects [-1,1] clip depth, but glm is built for [0,1]. "
				"Near geometry will clip incorrectly - projection matrices need a caps-driven "
				"path (see docs/BGFX_MIGRATION.md §9.3).",
				bgfx::getRendererName(bgfx::getRendererType()));
		}

		// BGFX_DEBUG_TEXT is the baseline the stats overlay draws on top of.
		bgfx::setDebug(BGFX_DEBUG_TEXT);

		bgfx::setViewClear(RenderPass::Backbuffer,
			BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,
			0x1a1a2eff, // matches the old GL clear colour
			1.0f, 0);
		bgfx::setViewRect(RenderPass::Backbuffer, 0, 0, uint16_t(m_Width), uint16_t(m_Height));
	}

	void BgfxContext::Frame()
	{
		GE_PROFILE_FUNCTION();

		if (!m_Initialized)
			return;

		// A view with no draw calls is skipped entirely, so its clear would never
		// happen. touch() submits an empty primitive to force it.
		// Phase 4 hands view management to SceneRenderer.
		bgfx::touch(RenderPass::Backbuffer);

		// Anything waiting on GPU results (entity-ID readback) polls against this.
		Renderer::OnFrameSubmitted(bgfx::frame());
	}

	void BgfxContext::Resize(uint32_t width, uint32_t height)
	{
		if (m_Width == width && m_Height == height)
			return;

		m_Width = width;
		m_Height = height;
		Reset();
	}

	void BgfxContext::SetVSync(bool enabled)
	{
		if (m_VSync == enabled)
			return;

		m_VSync = enabled;

		// Before init this only records the preference; Init() folds it into
		// bgfx::Init::resolution.reset.
		if (m_Initialized)
			Reset();
	}

	void BgfxContext::Reset()
	{
		if (!m_Initialized)
			return;

		const uint32_t flags = m_VSync ? BGFX_RESET_VSYNC : BGFX_RESET_NONE;
		bgfx::reset(m_Width, m_Height, flags);
		bgfx::setViewRect(RenderPass::Backbuffer, 0, 0, uint16_t(m_Width), uint16_t(m_Height));
	}

}
