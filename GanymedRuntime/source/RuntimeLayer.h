#pragma once

#include <GanymedE.h>

#include "RuntimeConfig.h"

namespace GanymedE {

	// The whole standalone game: one layer, no editor chrome, no edit mode.
	//
	// Deliberately a thin cousin of EditorLayer rather than a shared base class. The two
	// overlap in about fifteen lines of boot sequence and diverge everywhere else - there
	// is no viewport panel, no picking consumer, no play/stop transition, no Scene::Copy,
	// no gizmos - and a common base would have to be parameterized on all of that to save
	// those fifteen lines. The overlap is documented instead: docs/runtime/runtime.md
	// lists the boot steps and why each one is in that order.
	class RuntimeLayer : public Layer
	{
	public:
		explicit RuntimeLayer(RuntimeConfig config);

		void OnAttach() override;
		void OnDetach() override;
		void OnUpdate(Timestep ts) override;
		void OnEvent(Event& e) override;
	private:
		bool OnWindowResize(WindowResizeEvent& e);
		bool OnKeyPressed(KeyPressedEvent& e);
	private:
		RuntimeConfig m_Config;

		Ref<SceneRenderer> m_SceneRenderer;
		Ref<Scene> m_Scene;

		// False when the scene never loaded. The layer still renders (clear colour, HUD)
		// so the window is not a lie about being alive, but nothing is stepped.
		bool m_RuntimeStarted = false;
	};

}
