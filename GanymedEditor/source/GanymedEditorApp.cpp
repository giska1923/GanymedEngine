#include <GanymedE.h>
//-------------ENTRY POINT-----------------
#include <GanymedE/main/EntryPoint.h>
//-----------------------------------------

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
