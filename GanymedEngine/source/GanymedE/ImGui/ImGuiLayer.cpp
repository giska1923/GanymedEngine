#include "gepch.h"
#include "ImGuiLayer.h"

#include "imgui.h"
#include "backends/imgui_impl_glfw.h"

#include "GanymedE/main/Application.h"

#include <filesystem>
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

		// AddFontFromFileTTF asserts hard on a missing file, which takes the whole
		// app down. Not every app ships the editor's fonts (Sandbox does not), so
		// check first and fall back to ImGui's built-in font.
		auto addFont = [&io](const char* path) -> ImFont*
		{
			if (!std::filesystem::exists(path))
			{
				GE_CORE_WARN("Font '{0}' not found; falling back to the default ImGui font", path);
				return nullptr;
			}
			return io.Fonts->AddFontFromFileTTF(path, 18.0f);
		};

		addFont("assets/fonts/montserrat/Montserrat-Bold.ttf");
		if (ImFont* regular = addFont("assets/fonts/montserrat/Montserrat-Regular.ttf"))
			io.FontDefault = regular;

		// Setup Dear ImGui style
		ImGui::StyleColorsDark();
		//ImGui::StyleColorsClassic();

		// When viewports are enabled we tweak WindowRounding/WindowBg so platform windows can look identical to regular ones.
		ImGuiStyle& style = ImGui::GetStyle();
		if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
		{
			style.WindowRounding = 0.0f;
			style.Colors[ImGuiCol_WindowBg].w = 1.0f;
		}

		SetDarkThemeColors();

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

	void ImGuiLayer::SetDarkThemeColors()
	{
		auto& colors = ImGui::GetStyle().Colors;
		colors[ImGuiCol_WindowBg] = ImVec4{ 0.1f, 0.105f, 0.11f, 1.0f };

		// Headers
		colors[ImGuiCol_Header] = ImVec4{ 0.2f, 0.205f, 0.21f, 1.0f };
		colors[ImGuiCol_HeaderHovered] = ImVec4{ 0.3f, 0.305f, 0.31f, 1.0f };
		colors[ImGuiCol_HeaderActive] = ImVec4{ 0.15f, 0.1505f, 0.151f, 1.0f };

		// Buttons
		colors[ImGuiCol_Button] = ImVec4{ 0.2f, 0.205f, 0.21f, 1.0f };
		colors[ImGuiCol_ButtonHovered] = ImVec4{ 0.3f, 0.305f, 0.31f, 1.0f };
		colors[ImGuiCol_ButtonActive] = ImVec4{ 0.15f, 0.1505f, 0.151f, 1.0f };

		// Frame BG
		colors[ImGuiCol_FrameBg] = ImVec4{ 0.2f, 0.205f, 0.21f, 1.0f };
		colors[ImGuiCol_FrameBgHovered] = ImVec4{ 0.3f, 0.305f, 0.31f, 1.0f };
		colors[ImGuiCol_FrameBgActive] = ImVec4{ 0.15f, 0.1505f, 0.151f, 1.0f };

		// Tabs
		colors[ImGuiCol_Tab] = ImVec4{ 0.15f, 0.1505f, 0.151f, 1.0f };
		colors[ImGuiCol_TabHovered] = ImVec4{ 0.38f, 0.3805f, 0.381f, 1.0f };
		colors[ImGuiCol_TabActive] = ImVec4{ 0.28f, 0.2805f, 0.281f, 1.0f };
		colors[ImGuiCol_TabUnfocused] = ImVec4{ 0.15f, 0.1505f, 0.151f, 1.0f };
		colors[ImGuiCol_TabUnfocusedActive] = ImVec4{ 0.2f, 0.205f, 0.21f, 1.0f };

		// Title
		colors[ImGuiCol_TitleBg] = ImVec4{ 0.15f, 0.1505f, 0.151f, 1.0f };
		colors[ImGuiCol_TitleBgActive] = ImVec4{ 0.15f, 0.1505f, 0.151f, 1.0f };
		colors[ImGuiCol_TitleBgCollapsed] = ImVec4{ 0.15f, 0.1505f, 0.151f, 1.0f };
	}
}
