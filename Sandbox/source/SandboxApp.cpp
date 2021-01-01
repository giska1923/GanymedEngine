#include <GanymedE.h>

class ExampleLayer :public GanymedE::Layer {
public:
	ExampleLayer() : Layer("Example") {}

	void OnUpdate() override {
		if (GanymedE::Input::IsKeyPressed(GE_KEY_TAB))
			GE_TRACE("Tab key is pressed (poll)!");
	}

	void OnEvent(GanymedE::Event& event) override {
		if (event.GetEventType() == GanymedE::EventType::KeyPressed) {
			GanymedE::KeyPressedEvent& e = (GanymedE::KeyPressedEvent&)event;
			if(e.GetKeyCode() == GE_KEY_TAB)
				GE_TRACE("Tab key is pressed (event)!");
			GE_TRACE("{0}", (char)e.GetKeyCode());
		}
	}
};


class Sandbox : public GanymedE::Application {
public:
	Sandbox() {
		PushLayer(new ExampleLayer());
		PushOverlay(new GanymedE::ImGuiLayer());
	}
	~Sandbox() {}

};

GanymedE::Application* GanymedE::CreateApplication() {
	return new Sandbox();
}