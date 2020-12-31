#include <GanymedE.h>

class ExampleLayer :public GanymedE::Layer {
public:
	ExampleLayer() : Layer("Example") {}

	void OnUpdate() override {
		GE_INFO("ExampleLayer::Update");
	}

	void OnEvent(GanymedE::Event& event) override {
		GE_TRACE("{0}", event);
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