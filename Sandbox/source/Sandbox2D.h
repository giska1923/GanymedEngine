#pragma once

#include "GanymedE.h"

class Sandbox2D : public GanymedE::Layer
{
public:
	Sandbox2D();
	virtual ~Sandbox2D() = default;

	virtual void OnAttach() override;
	virtual void OnDetach() override;

	virtual void OnUpdate(GanymedE::Timestep ts) override;
	virtual void OnImGuiRender() override;
	virtual void OnEvent(GanymedE::Event& e) override;
private:
	GanymedE::OrthographicCameraController m_CameraController;

	GanymedE::Ref<GanymedE::VertexArray> m_SquareVA;
	GanymedE::Ref<GanymedE::Shader> m_FlatColorShader;

	GanymedE::Ref<GanymedE::Texture2D> m_CheckerboardTexture;

	glm::vec4 m_SquareColor = { 0.2f, 0.3f, 0.8f, 1.f };
};