#pragma once

#include "GanymedE/Core/Core.h"

struct GLFWwindow;

namespace GanymedE {

	// Owns bgfx's lifetime and the backbuffer swapchain for one window.
	//
	// Deliberately not a GraphicsContext subclass: with bgfx there is only ever
	// one backend, so the virtual indirection buys nothing. GraphicsContext and
	// OpenGLContext are removed together in Phase 7.
	// See docs/history/BGFX_MIGRATION.md.
	class BgfxContext
	{
	public:
		// `headless` is the only way a null window is legal. A windowed app whose window
		// failed to open still asserts, including in the constructor, rather than falling
		// through to Noop.
		BgfxContext(GLFWwindow* windowHandle, bool headless = false);
		~BgfxContext();

		void Init(uint32_t width, uint32_t height);

		// Ends the frame and presents. bgfx has no separate swap step.
		void Frame();

		// Recreates the swapchain. Cheap to call with unchanged values - bgfx
		// ignores a reset that does not change resolution or flags.
		void Resize(uint32_t width, uint32_t height);

		void SetVSync(bool enabled);
		bool IsVSync() const { return m_VSync; }

		uint32_t GetWidth() const { return m_Width; }
		uint32_t GetHeight() const { return m_Height; }

	private:
		void Reset();
		uint32_t ResetFlags() const;

	private:
		GLFWwindow* m_WindowHandle;
		bool m_Headless = false;

		uint32_t m_Width = 0;
		uint32_t m_Height = 0;
		bool m_VSync = true;
		bool m_Initialized = false;
	};

}
