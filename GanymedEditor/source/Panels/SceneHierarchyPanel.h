#pragma once

#include "GanymedE/Core/Core.h"
#include "GanymedE/Core/Log.h"
#include "GanymedE/Scene/Scene.h"
#include "GanymedE/Scene/Entity.h"

namespace GanymedE {

	class SceneHierarchyPanel
	{
	public:
		SceneHierarchyPanel() = default;
		SceneHierarchyPanel(const Ref<Scene>& scene);

		void SetContext(const Ref<Scene>& scene);

		void OnImGuiRender();
	private:
		void DrawEntityNode(Entity entity);
		void DrawComponents(Entity entity);
	private:
		Ref<Scene> m_Context;
		Entity m_SelectionContext;
	};
}
