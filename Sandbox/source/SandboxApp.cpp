#include <GanymedE.h>

class Sandbox : public GanymedE::Application {
public:
	Sandbox() {}
	~Sandbox() {}

};

GanymedE::Application* GanymedE::CreateApplication() {
	return new Sandbox();
}