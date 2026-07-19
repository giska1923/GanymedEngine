#include <GanymedE.h>
//-------------ENTRY POINT-----------------
#include <GanymedE/main/EntryPoint.h>
//-----------------------------------------

#include "Sandbox2D.h"

class Sandbox : public GanymedE::Application {
public:
	Sandbox() {
		PushLayer(new Sandbox2D());
	}
	~Sandbox() {}

};

GanymedE::Application* GanymedE::CreateApplication() {
	return new Sandbox();
}
