#include "gepch.h"
#include "GanymedE/UI/UIEngine.h"

#include "GanymedE/Renderer/Framebuffer.h"
#include "GanymedE/Scripting/ScriptEngine.h"
#include "GanymedE/events/Event.h"
#include "GanymedE/events/KeyEvent.h"
#include "GanymedE/events/MouseEvent.h"
#include "Platform/RmlUi/RmlUiInput.h"
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

			float OriginX = 0.0f;
			float OriginY = 0.0f;

			bool DebuggerVisible = false;

			// Backing storage for the "hud" data model. RmlUi binds to these
			// addresses, so they must outlive every document that uses them - which
			// is why they live in the engine data block rather than in a document.
			struct HudData
			{
				float Health = 100.0f;
				int Score = 0;
			} Hud;

			Rml::DataModelHandle HudModel;
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
		{
			Rml::Lua::Initialise(lua);

			// The plugin installs globals of its own, and one of them is `Log`,
			// which shadows the engine's. Re-install ours so engine names win; the
			// plugin's `rmlui` global (documents, elements, events) is untouched.
			ScriptEngine::ReinstallGlobals();
		}
		else
		{
			GE_CORE_WARN("UIEngine: ScriptEngine has no Lua state; UI scripting disabled");
		}

		LoadDefaultFonts();

		s_Data->Context = Rml::CreateContext("main", Rml::Vector2i((int)width, (int)height));
		if (!s_Data->Context)
		{
			GE_CORE_ERROR("UIEngine: failed to create the RmlUi context");
			return;
		}

		// The data model must exist BEFORE any document declaring data-model="hud"
		// is loaded; RmlUi resolves the binding at parse time and a document that
		// arrives first silently renders its {{expressions}} as literal text.
		{
			Rml::DataModelConstructor constructor = s_Data->Context->CreateDataModel("hud");
			if (constructor)
			{
				constructor.Bind("health", &s_Data->Hud.Health);
				constructor.Bind("score", &s_Data->Hud.Score);
				s_Data->HudModel = constructor.GetModelHandle();
			}
			else
			{
				GE_CORE_ERROR("UIEngine: failed to create the 'hud' data model");
			}
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

	void UIEngine::SetViewportOrigin(float x, float y)
	{
		if (s_Data)
		{
			s_Data->OriginX = x;
			s_Data->OriginY = y;
		}
	}

	namespace {

		// Is the pointer over something the UI should actually claim?
		//
		// NOT the same as "Context::Process* returned consumed". A HUD document's
		// <body> fills the whole viewport, so RmlUi reports *every* mouse move as
		// consumed - which would swallow camera orbit and gameplay clicks any time a
		// HUD is on screen. The hover element being the document root means the
		// pointer is over transparent nothing, and the event belongs to the game.
		bool IsPointerOverUI(Rml::Context* ctx)
		{
			Rml::Element* hover = ctx->GetHoverElement();
			if (!hover)
				return false;

			// The document root reports tag "body"; its owner document is itself.
			return hover != hover->GetOwnerDocument();
		}
	}

	void UIEngine::OnEvent(Event& e)
	{
		if (!IsInitialized())
			return;

		Rml::Context* ctx = s_Data->Context;
		const int modifiers = RmlUiInput::GetModifierState();

		EventDispatcher dispatcher(e);

		dispatcher.Dispatch<MouseMovedEvent>([&](MouseMovedEvent& moved)
		{
			// Window coords -> viewport-local, the same translation the picking code
			// does. Like picking, there is no Y flip: RmlUi and the pick request both
			// address from the viewport's top-left.
			const int x = (int)(moved.GetX() - s_Data->OriginX);
			const int y = (int)(moved.GetY() - s_Data->OriginY);
			ctx->ProcessMouseMove(x, y, modifiers);

			// Always forwarded (RmlUi needs it to track hover), but only claimed
			// when it landed on a real widget.
			return IsPointerOverUI(ctx);
		});

		dispatcher.Dispatch<MouseButtonPressedEvent>([&](MouseButtonPressedEvent& pressed)
		{
			ctx->ProcessMouseButtonDown(
				RmlUiInput::ConvertMouseButton(pressed.GetMouseButton()), modifiers);
			return IsPointerOverUI(ctx);
		});

		dispatcher.Dispatch<MouseButtonReleasedEvent>([&](MouseButtonReleasedEvent& released)
		{
			ctx->ProcessMouseButtonUp(
				RmlUiInput::ConvertMouseButton(released.GetMouseButton()), modifiers);
			return IsPointerOverUI(ctx);
		});

		dispatcher.Dispatch<MouseScrolledEvent>([&](MouseScrolledEvent& scrolled)
		{
			// RmlUi's wheel axis points the opposite way to GLFW's: positive scrolls
			// the document down.
			ctx->ProcessMouseWheel(-scrolled.GetYOffset(), modifiers);
			return IsPointerOverUI(ctx);
		});

		// Keyboard deliberately uses the Process* return rather than the hover test:
		// key events belong to whatever has FOCUS, not to whatever the pointer
		// happens to be over. With nothing focused RmlUi reports them unconsumed,
		// so editor shortcuts and gameplay keys keep working while a HUD is up.
		dispatcher.Dispatch<KeyPressedEvent>([&](KeyPressedEvent& key)
		{
			return !ctx->ProcessKeyDown(RmlUiInput::ConvertKey(key.GetKeyCode()), modifiers);
		});

		dispatcher.Dispatch<KeyReleasedEvent>([&](KeyReleasedEvent& key)
		{
			return !ctx->ProcessKeyUp(RmlUiInput::ConvertKey(key.GetKeyCode()), modifiers);
		});

		dispatcher.Dispatch<KeyTypedEvent>([&](KeyTypedEvent& typed)
		{
			return !ctx->ProcessTextInput((Rml::Character)typed.GetKeyCode());
		});
	}

	void UIEngine::SetHudHealth(float health)
	{
		if (!s_Data)
			return;

		s_Data->Hud.Health = health;

		// Binding to the variable is not enough: RmlUi only re-evaluates the
		// expressions that reference a variable once it has been marked dirty.
		if (s_Data->HudModel)
			s_Data->HudModel.DirtyVariable("health");
	}

	void UIEngine::SetHudScore(int score)
	{
		if (!s_Data)
			return;

		s_Data->Hud.Score = score;

		if (s_Data->HudModel)
			s_Data->HudModel.DirtyVariable("score");
	}

	float UIEngine::GetHudHealth() { return s_Data ? s_Data->Hud.Health : 0.0f; }
	int UIEngine::GetHudScore()    { return s_Data ? s_Data->Hud.Score : 0; }

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
