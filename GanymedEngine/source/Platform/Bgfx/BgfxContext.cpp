#include "gepch.h"
#include "Platform/Bgfx/BgfxContext.h"

#include "GanymedE/Renderer/RenderPassIDs.h"
#include "GanymedE/Renderer/Renderer.h"
#include "GanymedE/main/Application.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

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
		if (!m_Initialized)
			return;

		// Lower this BEFORE shutting bgfx down: any resource destroyed from here
		// on (including statics torn down after main) must not touch bgfx.
		Renderer::SetGpuAlive(false);
		bgfx::shutdown();
	}

	namespace {

		// ---- bgfx diagnostics ------------------------------------------------------------
		//
		// Until this existed, `init.callback` was null, so bgfx used its own stub: on Windows that
		// writes traces and fatals to the DEBUGGER via OutputDebugString and nowhere else. Every
		// bgfx warning the engine ever produced was invisible in `GanymedE.log`, which is how a
		// backend could render an empty frame and still look clean in the log.
		//
		// Everything here forwards into the engine logger instead. Traces are TRACE rather than
		// INFO because bgfx is chatty in Debug (it is compiled with BX_CONFIG_DEBUG=1, see
		// extern/bgfx.lua) and this is diagnostic detail, not boot narration.
		class BgfxCallback final : public bgfx::CallbackI
		{
		public:
			~BgfxCallback() override = default;

			void fatal(const char* filePath, uint16_t line, bgfx::Fatal::Enum code,
				const char* str) override
			{
				GE_CORE_ERROR("bgfx FATAL [{0}] {1}:{2}: {3}", (int)code,
					filePath ? filePath : "?", line, str ? str : "");

				// DeviceLost is recoverable in principle and bgfx keeps going; the rest are not,
				// and bgfx's own stub aborts on them. Assert so a Debug run stops at the cause
				// rather than at whatever fails next.
				//
				// Braced because GE_CORE_ASSERT compiles to nothing outside Debug, which would
				// otherwise leave `if (...) ;` and the warning that goes with it.
				if (code != bgfx::Fatal::DeviceLost)
				{
					GE_CORE_ASSERT(false, "bgfx reported a fatal error - see the log");
				}
			}

			void traceVargs(const char* filePath, uint16_t line, const char* format,
				va_list argList) override
			{
				char message[2048];

				// va_list is single-use; bgfx may pass one already partially consumed, so copy.
				va_list args;
				va_copy(args, argList);
				const int written = std::vsnprintf(message, sizeof(message), format, args);
				va_end(args);

				if (written <= 0)
					return;

				// bgfx terminates most traces with a newline; the logger adds its own.
				std::string text(message);
				while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
					text.pop_back();

				if (text.empty())
					return;

				GE_CORE_TRACE("bgfx {0}:{1}: {2}", filePath ? filePath : "?", line, text);
			}

			// No shader/texture cache: bgfx asks, the engine declines, and compiled shader
			// bytecode is already cached on disk by the shaderc step.
			uint32_t cacheReadSize(uint64_t) override { return 0; }
			bool cacheRead(uint64_t, void*, uint32_t) override { return false; }
			void cacheWrite(uint64_t, const void*, uint32_t) override {}

			// Frame capture (bgfx's video-capture hook) is not wired to anything.
			void captureBegin(uint32_t, uint32_t, uint32_t, bgfx::TextureFormat::Enum, bool) override {}
			void captureEnd() override {}
			void captureFrame(const void*, uint32_t) override {}

			// Profiler scopes: deliberately empty. The job system already feeds the Instrumentor
			// (see core.md) and routing bgfx's per-draw scopes into a mutex-guarded Chrome-trace
			// writer would measure the tracer. Named rather than removed so the hook is visible
			// to whoever adopts a real frame profiler.
			void profilerBegin(const char*, uint32_t, const char*, uint16_t) override {}
			void profilerBeginLiteral(const char*, uint32_t, const char*, uint16_t) override {}
			void profilerEnd() override {}

			// Taking over the callback means taking over `bgfx::requestScreenShot`, which the
			// stub used to service. Uncompressed 32-bit TGA, which is what the stub wrote.
			void screenShot(const char* filePath, uint32_t width, uint32_t height, uint32_t pitch,
				bgfx::TextureFormat::Enum format, const void* data, uint32_t /*size*/,
				bool yflip) override
			{
				if (format != bgfx::TextureFormat::BGRA8 && format != bgfx::TextureFormat::RGBA8)
				{
					GE_CORE_WARN("Screenshot '{0}' skipped: unexpected format {1}",
						filePath ? filePath : "?", (int)format);
					return;
				}

				// bgfx hands over the name exactly as `requestScreenShot` received it, and its own
				// stub appends the extension - so a caller that passed "shot" expects "shot.tga".
				// Taking over the callback means taking over that convention too.
				std::string path(filePath ? filePath : "screenshot");
				if (path.size() < 4 || path.compare(path.size() - 4, 4, ".tga") != 0)
					path += ".tga";

				std::ofstream out(path, std::ios::binary);
				if (!out)
				{
					GE_CORE_ERROR("Screenshot '{0}' could not be opened for writing", path);
					return;
				}

				uint8_t header[18] = {};
				header[2] = 2;                              // uncompressed true-colour
				header[12] = (uint8_t)(width & 0xFF);
				header[13] = (uint8_t)((width >> 8) & 0xFF);
				header[14] = (uint8_t)(height & 0xFF);
				header[15] = (uint8_t)((height >> 8) & 0xFF);
				header[16] = 32;                            // bits per pixel
				header[17] = 0x28;                          // 8 alpha bits, top-left origin
				out.write((const char*)header, sizeof(header));

				const uint8_t* source = (const uint8_t*)data;
				std::vector<uint8_t> row(width * 4);

				for (uint32_t y = 0; y < height; y++)
				{
					// TGA rows are written top-down here (descriptor 0x20), so a bottom-left
					// source is read in reverse rather than re-flagged.
					const uint32_t sourceRow = yflip ? (height - 1 - y) : y;
					std::memcpy(row.data(), source + (size_t)sourceRow * pitch, width * 4);

					// TGA stores BGRA for 32-bit, so BGRA8 goes out untouched.
					if (format == bgfx::TextureFormat::RGBA8)
					{
						for (uint32_t x = 0; x < width; x++)
							std::swap(row[x * 4 + 0], row[x * 4 + 2]);
					}

					out.write((const char*)row.data(), row.size());
				}
			}
		};

		BgfxCallback s_Callback;

		// ---- Backend selection: `--renderer=<name>` -------------------------------------
		//
		// BGFX_MIGRATION.md §9.2. bgfx picks a backend itself when `init.type` is `Count`, and
		// that default is right for shipping - but "does this reproduce on Vulkan?" is a question
		// you have to be able to ask without rebuilding, and it is the only way to reach the
		// `[-1,1]` clip-depth path that §9.3 built (see Projection:: in Renderer.h).
		//
		// A launch-time switch rather than a config key: which backend to debug on belongs to the
		// run, not to the build. That is also why it is not in `runtime.yaml`, whose own header
		// reserves that file for what "belongs to the build rather than to the launch".
		struct BackendName
		{
			const char* Name;
			bgfx::RendererType::Enum Type;
		};

		// bgfx's own spellings plus the short ones people actually type. `auto` is spelled out
		// rather than left implicit so that `--renderer=auto` overrides a launcher's default
		// instead of erroring.
		constexpr BackendName kBackendNames[] = {
			{ "auto",       bgfx::RendererType::Count      },
			{ "noop",       bgfx::RendererType::Noop       },
			{ "d3d11",      bgfx::RendererType::Direct3D11 },
			{ "dx11",       bgfx::RendererType::Direct3D11 },
			{ "direct3d11", bgfx::RendererType::Direct3D11 },
			{ "d3d12",      bgfx::RendererType::Direct3D12 },
			{ "dx12",       bgfx::RendererType::Direct3D12 },
			{ "direct3d12", bgfx::RendererType::Direct3D12 },
			{ "vulkan",     bgfx::RendererType::Vulkan     },
			{ "vk",         bgfx::RendererType::Vulkan     },
			{ "metal",      bgfx::RendererType::Metal      },
			{ "gl",         bgfx::RendererType::OpenGL     },
			{ "opengl",     bgfx::RendererType::OpenGL     },
			{ "gles",       bgfx::RendererType::OpenGLES   },
		};

		std::string ToLower(const char* text)
		{
			std::string lowered(text ? text : "");
			for (char& c : lowered)
				c = (char)std::tolower((unsigned char)c);
			return lowered;
		}

		std::string SupportedBackendList()
		{
			bgfx::RendererType::Enum supported[bgfx::RendererType::Count];
			const uint8_t count = bgfx::getSupportedRenderers(bgfx::RendererType::Count, supported);

			std::string list;
			for (uint8_t i = 0; i < count; i++)
			{
				if (supported[i] == bgfx::RendererType::Noop)
					continue;   // always "supported" and never what anyone means

				if (!list.empty())
					list += ", ";
				list += bgfx::getRendererName(supported[i]);
			}

			return list;
		}

		bool IsBackendSupported(bgfx::RendererType::Enum type)
		{
			bgfx::RendererType::Enum supported[bgfx::RendererType::Count];
			const uint8_t count = bgfx::getSupportedRenderers(bgfx::RendererType::Count, supported);

			for (uint8_t i = 0; i < count; i++)
			{
				if (supported[i] == type)
					return true;
			}

			return false;
		}

		// `bgfx::RendererType::Count` means "let bgfx choose", which is also what every failure
		// path here returns: an unusable --renderer should cost a warning and a working window,
		// not a black screen.
		bgfx::RendererType::Enum RequestedBackend()
		{
			const ApplicationCommandLineArgs& args = Application::GetCommandLineArgs();

			const char* kFlag = "--renderer=";
			const std::size_t flagLength = std::strlen(kFlag);

			const char* requested = nullptr;
			for (int i = 1; i < args.Count; i++)
			{
				if (args.Args[i] && std::strncmp(args.Args[i], kFlag, flagLength) == 0)
					requested = args.Args[i] + flagLength;   // last one wins
			}

			if (!requested)
				return bgfx::RendererType::Count;

			const std::string name = ToLower(requested);

			const BackendName* match = nullptr;
			for (const BackendName& candidate : kBackendNames)
			{
				if (name == candidate.Name)
				{
					match = &candidate;
					break;
				}
			}

			if (!match)
			{
				GE_CORE_WARN("--renderer='{0}' is not a backend name. Known: auto, d3d11, d3d12, "
					"vulkan, metal, gl, gles. Letting bgfx choose.", requested);
				return bgfx::RendererType::Count;
			}

			if (match->Type == bgfx::RendererType::Count)
				return bgfx::RendererType::Count;

			// Asked for before bgfx::init, which is legal: getSupportedRenderers reports what this
			// BUILD of bgfx can do, and it does not need an initialised context.
			if (!IsBackendSupported(match->Type))
			{
				GE_CORE_WARN("--renderer='{0}' resolves to {1}, which this build does not support. "
					"Supported: {2}. Letting bgfx choose.",
					requested, bgfx::getRendererName(match->Type), SupportedBackendList());
				return bgfx::RendererType::Count;
			}

			GE_CORE_INFO("Backend requested on the command line: {0}",
				bgfx::getRendererName(match->Type));
			return match->Type;
		}

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

		const bgfx::RendererType::Enum requested = RequestedBackend();

		bgfx::Init init;
		init.type = requested;   // Count == let bgfx choose; see RequestedBackend above
		init.callback = &s_Callback;   // or bgfx talks to the debugger and nowhere else
		init.vendorId = BGFX_PCI_ID_NONE;
		init.platformData.nwh = NativeWindowHandle(m_WindowHandle);
		init.platformData.ndt = NativeDisplayHandle();
		init.resolution.width = m_Width;
		init.resolution.height = m_Height;
		init.resolution.reset = m_VSync ? BGFX_RESET_VSYNC : BGFX_RESET_NONE;

		if (!bgfx::init(init))
		{
			// A backend can be supported by the build and still fail to start here - no driver,
			// no device, a headless session. Retry on auto rather than dying, because "Vulkan is
			// not installed" should not look like "the engine is broken".
			if (requested != bgfx::RendererType::Count)
			{
				GE_CORE_ERROR("bgfx failed to initialise on {0}. Retrying with the backend bgfx "
					"chooses itself.", bgfx::getRendererName(requested));

				init.type = bgfx::RendererType::Count;
				if (!bgfx::init(init))
				{
					GE_CORE_ASSERT(false, "Failed to initialize bgfx!");
					return;
				}
			}
			else
			{
				GE_CORE_ASSERT(false, "Failed to initialize bgfx!");
				return;
			}
		}

		m_Initialized = true;
		Renderer::SetGpuAlive(true);

		const bgfx::Caps* caps = bgfx::getCaps();
		GE_CORE_INFO("bgfx Info:");
		// Reported from what bgfx ACTUALLY selected, not from what was asked. bgfx substitutes a
		// working backend silently when the requested one fails to start - asking for Vulkan on a
		// machine without a Vulkan driver returns success and a D3D11 context - so labelling the
		// line from the request alone would print "Vulkan (requested)" over a D3D11 frame.
		const bgfx::RendererType::Enum actual = bgfx::getRendererType();
		if (requested == bgfx::RendererType::Count)
			GE_CORE_INFO("  Renderer: {0} (auto)", bgfx::getRendererName(actual));
		else if (actual == requested)
			GE_CORE_INFO("  Renderer: {0} (requested)", bgfx::getRendererName(actual));
		else
		{
			GE_CORE_WARN("  Renderer: {0} - bgfx substituted it for the requested {1}, which could "
				"not be started on this machine", bgfx::getRendererName(actual),
				bgfx::getRendererName(requested));
		}

		GE_CORE_INFO("  Available: {0}", SupportedBackendList());
		GE_CORE_INFO("  Max texture size: {0}", caps->limits.maxTextureSize);
		GE_CORE_INFO("  Max FB attachments: {0}", caps->limits.maxFBAttachments);
		GE_CORE_INFO("  Homogeneous depth: {0}", caps->homogeneousDepth);
		GE_CORE_INFO("  Origin bottom left: {0}", caps->originBottomLeft);

		// A [-1,1]-clip-depth backend used to be an error here: the workspace compiles with
		// GLM_FORCE_DEPTH_ZERO_TO_ONE, a compile-time choice that cannot adapt, so such a
		// backend rendered with wrong near-plane clipping and half its depth precision. §9.3
		// replaced every projection on the render path with Projection:: (Renderer.h), which
		// reads this same cap, and the shaders answer it per profile - so this is now a fact
		// worth logging rather than a problem.
		//
		// Note what is still NOT verified: no [-1,1] backend has been run. The code paths exist
		// and the [0,1] ones are unchanged by construction; see docs/ToDo/rendering.md.

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
