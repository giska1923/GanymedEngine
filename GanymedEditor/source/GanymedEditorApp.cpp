#include <GanymedE.h>
//-------------ENTRY POINT-----------------
#include <GanymedE/main/EntryPoint.h>
//-----------------------------------------

#include "Platform/OpenGL/OpenGLShader.h"

#include "imgui.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "EditorLayer.h"

namespace GanymedE {
	class GanymedEditor : public Application {
	public:
		GanymedEditor() : Application("GanymedEditor")
		{
			PushLayer(new EditorLayer());
		}
		~GanymedEditor() {}

	};

	Application* CreateApplication() {
		return new GanymedEditor();
	}
}