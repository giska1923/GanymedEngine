#include "gepch.h"
#include "ImGuiLayer.h"

#include "imgui.h"
#include "backends/imgui_impl_glfw.h"

#include "GanymedE/main/Application.h"

#include "Platform/Bgfx/ImGuiRendererBgfx.h"

#include <GLFW/glfw3.h>

namespace GanymedE {
	ImGuiLayer::ImGuiLayer() : Layer("ImGuiLayer") {}
	ImGuiLayer::~ImGuiLayer() {}

	void ImGuiLayer::OnAttach() {
		GE_PROFILE_FUNCTION();

		// Setup Dear ImGui context
		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO(); (void)io;
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;       // Enable Keyboard Controls
		//io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls
		io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;           // Enable Docking
		// Multi-viewport is off under bgfx: it needs one bgfx framebuffer per OS
		// window (§8.4), which is deferred. Enabling it without that support
		// would spawn platform windows that never draw.
		// io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
		//io.ConfigViewportsNoAutoMerge = true;
		//io.ConfigViewportsNoTaskBarIcon = true;

		// Fonts stay at ImGui's embedded default here. The editor swaps the atlas
		// in EditorFonts::Load after this layer attaches; Sandbox has no assets/fonts
		// and must not warn (or assert) on a missing TTF.

		// Setup Dear ImGui style. The editor overwrites this from ApplyTheme after
		// attach; Sandbox (and any other EnableImGui front-end) keeps the stock dark
		// style. The engine does not own a brand palette.
		ImGui::StyleColorsDark();
		//ImGui::StyleColorsClassic();

		// When viewports are enabled we tweak WindowRounding/WindowBg so platform windows can look identical to regular ones.
		ImGuiStyle& style = ImGui::GetStyle();
		if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
		{
			style.WindowRounding = 0.0f;
			style.Colors[ImGuiCol_WindowBg].w = 1.0f;
		}

		Application& app = Application::Get();
		GLFWwindow* window = static_cast<GLFWwindow*>(app.GetWindow().GetNativeWindow());

		// Platform half stays GLFW; only the render half moved to bgfx.
		// InitForOther (not InitForOpenGL) because the window is created with
		// GLFW_NO_API - there is no GL context for ImGui to assume.
		ImGui_ImplGlfw_InitForOther(window, true);

		if (!ImGuiRendererBgfx::Init())
			GE_CORE_ERROR("ImGui bgfx backend failed to initialise; the editor UI will not draw");
	}

	void ImGuiLayer::OnDetach() {
		GE_PROFILE_FUNCTION();

		ImGuiRendererBgfx::Shutdown();
		ImGui_ImplGlfw_Shutdown();
		ImGui::DestroyContext();
	}

	void ImGuiLayer::OnEvent(Event& e)
	{
		if (m_BlockEvents)
		{
			ImGuiIO& io = ImGui::GetIO();
			e.SetIsHandled(e.IsInCategory(EventCategoryMouse) & io.WantCaptureMouse);
			e.SetIsHandled(e.IsInCategory(EventCategoryKeyboard) & io.WantCaptureKeyboard);
		}
	}

	void ImGuiLayer::Begin() {
		GE_PROFILE_FUNCTION();

		ImGuiRendererBgfx::NewFrame();
		ImGui_ImplGlfw_NewFrame();
		ImGui::NewFrame();
	}

	void ImGuiLayer::End() {
		GE_PROFILE_FUNCTION();

		ImGuiIO& io = ImGui::GetIO();
		Application& app = Application::Get();
		io.DisplaySize = ImVec2((float)app.GetWindow().GetWidth(), (float)app.GetWindow().GetHeight());

		//Rendering
		ImGui::Render();
		ImGuiRendererBgfx::RenderDrawData(ImGui::GetDrawData());

		// Multi-viewport needs one bgfx framebuffer per OS window (§8.4) and is
		// deferred, so the platform-window pass is skipped rather than run
		// against a renderer that cannot service it.
		if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
			ImGui::UpdatePlatformWindows();
		}
	}
}
