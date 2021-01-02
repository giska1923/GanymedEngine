#include <GanymedE.h>

#include "imgui.h"

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

	virtual void OnImGuiRender() override {
		ImGui::Begin("Test");
		ImGui::Text("Hello World");
		ImGui::End();
	}
};


class Sandbox : public GanymedE::Application {
public:
	Sandbox() {
		PushLayer(new ExampleLayer());
	}
	~Sandbox() {}

};

GanymedE::Application* GanymedE::CreateApplication() {
	return new Sandbox();
}