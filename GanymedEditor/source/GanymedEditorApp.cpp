#include <GanymedE.h>
//-------------ENTRY POINT-----------------
#include <GanymedE/main/EntryPoint.h>
//-----------------------------------------

#include "EditorLayer.h"

namespace GanymedE {

	namespace {

		ApplicationSpecification MakeEditorSpec()
		{
			ApplicationSpecification spec;
			spec.Name = "GanymedEditor";
			spec.CustomTitleBar = true;
			return spec;
		}

	}

	class GanymedEditor : public Application {
	public:
		GanymedEditor() : Application(MakeEditorSpec())
		{
			PushLayer(new EditorLayer());
		}
		~GanymedEditor() {}
	};

	Application* CreateApplication() {
		return new GanymedEditor();
	}
}
