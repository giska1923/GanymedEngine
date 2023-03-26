#include "Sandbox2D.h"
#include "imgui.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

Sandbox2D::Sandbox2D()
	: Layer("Sandbox2D"), m_CameraController((float)DEFAULT_WINDOW_WIDTH / (float)DEFAULT_WINDOW_HEIGHT)
{
}

void Sandbox2D::OnAttach()
{
	m_CheckerboardTexture = GanymedE::Texture2D::Create("assets/textures/Checkerboard.png");
}

void Sandbox2D::OnDetach()
{
}

void Sandbox2D::OnUpdate(GanymedE::Timestep ts)
{
	GE_PROFILE_FUNCTION();

	// Update
	{
		GE_PROFILE_SCOPE("CameraController::OnUpdate");
		m_CameraController.OnUpdate(ts);
	}

	// Render
	{
		GE_PROFILE_SCOPE("Renderer Prep");
		GanymedE::RenderCommand::SetClearColor({ 0.1f, 0.1f, 0.1f, 1 });
		GanymedE::RenderCommand::Clear();
	}

	{
		GE_PROFILE_SCOPE("Renderer Draw");
		GanymedE::Renderer2D::BeginScene(m_CameraController.GetCamera());
		GanymedE::Renderer2D::DrawQuad({ -1.f, 0.f }, { 0.8f, 0.8f }, { 0.8f, 0.2f, 0.3f, 1.f });
		GanymedE::Renderer2D::DrawQuad({ 0.5f, -0.5f }, { 0.5f, 0.75f }, { 0.2f, 0.3f, 0.8f, 1.f });
		GanymedE::Renderer2D::DrawQuad({ 0.f, 0.f, -0.1f }, { 10.f, 10.f }, m_CheckerboardTexture);
		GanymedE::Renderer2D::EndScene();
	}
}

void Sandbox2D::OnImGuiRender()
{
	ImGui::Begin("Settings");
	ImGui::ColorEdit4("Square Color", glm::value_ptr(m_SquareColor));
	ImGui::End();
}

void Sandbox2D::OnEvent(GanymedE::Event& e)
{
	m_CameraController.OnEvent(e);
}