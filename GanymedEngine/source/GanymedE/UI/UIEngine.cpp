#include "gepch.h"
#include "GanymedE/UI/UIEngine.h"

#include "GanymedE/Renderer/Framebuffer.h"
#include "GanymedE/Scripting/ScriptEngine.h"
#include "Platform/RmlUi/RmlUiRendererBgfx.h"
#include "Platform/RmlUi/RmlUiSystemInterface.h"

#include <RmlUi/Core.h>
#include <RmlUi/Lua.h>

#ifdef GE_DEBUG
	#include <RmlUi/Debugger.h>
#endif

namespace GanymedE {

	namespace {

		struct UIEngineData
		{
			RmlUiSystemInterface SystemInterface;
			RmlUiRendererBgfx RenderInterface;

			Rml::Context* Context = nullptr;
			Ref<Framebuffer> Target;   // null = backbuffer

			uint32_t Width = 0;
			uint32_t Height = 0;

			bool DebuggerVisible = false;
		};

		UIEngineData* s_Data = nullptr;

		// Fonts are loaded by path relative to the working directory, like every
		// other asset. Regular + Bold covers the default RCSS font-weight values;
		// anything else an .rcss asks for falls back with a warning from RmlUi.
		void LoadDefaultFonts()
		{
			const char* faces[] = {
				"assets/fonts/montserrat/Montserrat-Regular.ttf",
				"assets/fonts/montserrat/Montserrat-Bold.ttf",
				"assets/fonts/montserrat/Montserrat-Italic.ttf"
			};

			for (const char* face : faces)
			{
				if (!Rml::LoadFontFace(face))
					GE_CORE_WARN("UIEngine: failed to load font face '{0}'", face);
			}
		}
	}

	void UIEngine::Init(uint32_t width, uint32_t height)
	{
		GE_CORE_ASSERT(!s_Data, "UIEngine::Init called twice");
		s_Data = new UIEngineData();

		s_Data->Width = width;
		s_Data->Height = height;

		// Interfaces must be set before Initialise: RmlUi captures them there, and
		// the font engine immediately needs the render interface to build atlases.
		Rml::SetSystemInterface(&s_Data->SystemInterface);
		Rml::SetRenderInterface(&s_Data->RenderInterface);

		if (!s_Data->RenderInterface.Init())
		{
			GE_CORE_ERROR("UIEngine: render backend failed to initialise; UI disabled");
			delete s_Data;
			s_Data = nullptr;
			return;
		}

		if (!Rml::Initialise())
		{
			GE_CORE_ERROR("UIEngine: Rml::Initialise failed");
			s_Data->RenderInterface.Shutdown();
			delete s_Data;
			s_Data = nullptr;
			return;
		}

		// Share the gameplay VM, so UI scripts and gameplay scripts see the same
		// globals and can call each other. This is the whole reason there is one
		// Lua state rather than one per subsystem.
		if (lua_State* lua = static_cast<lua_State*>(ScriptEngine::GetLuaState()))
			Rml::Lua::Initialise(lua);
		else
			GE_CORE_WARN("UIEngine: ScriptEngine has no Lua state; UI scripting disabled");

		LoadDefaultFonts();

		s_Data->Context = Rml::CreateContext("main", Rml::Vector2i((int)width, (int)height));
		if (!s_Data->Context)
		{
			GE_CORE_ERROR("UIEngine: failed to create the RmlUi context");
			return;
		}

#ifdef GE_DEBUG
		Rml::Debugger::Initialise(s_Data->Context);
		Rml::Debugger::SetVisible(false);
#endif

		GE_CORE_INFO("UIEngine initialised (RmlUi, {0}x{1})", width, height);
	}

	void UIEngine::Shutdown()
	{
		if (!s_Data)
			return;

		// Rml::Shutdown releases documents, fonts and textures, so it has to run
		// while the GPU context is still alive - and before ScriptEngine::Shutdown,
		// because the Lua plugin holds references into that state.
		Rml::Shutdown();

		s_Data->RenderInterface.Shutdown();

		delete s_Data;
		s_Data = nullptr;
	}

	bool UIEngine::IsInitialized()
	{
		return s_Data != nullptr && s_Data->Context != nullptr;
	}

	Rml::Context* UIEngine::GetContext()
	{
		return s_Data ? s_Data->Context : nullptr;
	}

	void UIEngine::SetTarget(const Ref<Framebuffer>& target)
	{
		if (s_Data)
			s_Data->Target = target;
	}

	Rml::ElementDocument* UIEngine::LoadDocument(const std::filesystem::path& rmlPath)
	{
		if (!IsInitialized())
			return nullptr;

		const std::string path = rmlPath.generic_string();

		Rml::ElementDocument* document = s_Data->Context->LoadDocument(path);
		if (!document)
		{
			GE_CORE_ERROR("UIEngine: failed to load document '{0}'", path);
			return nullptr;
		}

		document->Show();
		return document;
	}

	void UIEngine::CloseAllDocuments()
	{
		if (IsInitialized())
			s_Data->Context->UnloadAllDocuments();
	}

	void UIEngine::SetViewport(uint32_t width, uint32_t height)
	{
		if (!s_Data || width == 0 || height == 0)
			return;

		if (s_Data->Width == width && s_Data->Height == height)
			return;

		s_Data->Width = width;
		s_Data->Height = height;

		if (s_Data->Context)
			s_Data->Context->SetDimensions(Rml::Vector2i((int)width, (int)height));
	}

	void UIEngine::OnUpdate(Timestep ts)
	{
		(void)ts;   // RmlUi animates off SystemInterface::GetElapsedTime, not a delta
		if (IsInitialized())
			s_Data->Context->Update();
	}

	void UIEngine::OnRender()
	{
		if (!IsInitialized())
			return;

		s_Data->RenderInterface.BeginFrame(s_Data->Target, s_Data->Width, s_Data->Height);
		s_Data->Context->Render();
		s_Data->RenderInterface.EndFrame();
	}

	void UIEngine::SetDebuggerVisible(bool visible)
	{
#ifdef GE_DEBUG
		if (!IsInitialized())
			return;

		s_Data->DebuggerVisible = visible;
		Rml::Debugger::SetVisible(visible);
#else
		(void)visible;
#endif
	}

	bool UIEngine::IsDebuggerVisible()
	{
		return s_Data && s_Data->DebuggerVisible;
	}

}
