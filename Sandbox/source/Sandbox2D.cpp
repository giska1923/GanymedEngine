#include "Sandbox2D.h"
#include "imgui.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "Platform/OpenGL/OpenGLShader.h"

Sandbox2D::Sandbox2D()
	: Layer("Sandbox2D"), m_CameraController((float)DEFAULT_WINDOW_WIDTH / (float)DEFAULT_WINDOW_HEIGHT)
{
}

void Sandbox2D::OnAttach()
{
	m_SquareVA = GanymedE::VertexArray::Create();

	float squareVertices[5 * 4] = {
		-0.5f, -0.5f, 0.0f,
		 0.5f, -0.5f, 0.0f,
		 0.5f,  0.5f, 0.0f,
		-0.5f,  0.5f, 0.0f
	};

	GanymedE::Ref<GanymedE::VertexBuffer> squareVB;
	squareVB = GanymedE::VertexBuffer::Create(squareVertices, sizeof(squareVertices));
	squareVB->SetLayout({
		{ GanymedE::ShaderDataType::Float3, "a_Position" }
		});
	m_SquareVA->AddVertexBuffer(squareVB);

	uint32_t squareIndices[6] = { 0, 1, 2, 2, 3, 0 };
	GanymedE::Ref<GanymedE::IndexBuffer> squareIB;
	squareIB = GanymedE::IndexBuffer::Create(squareIndices, std::size(squareIndices));
	m_SquareVA->SetIndexBuffer(squareIB);

	m_FlatColorShader = GanymedE::Shader::Create("assets/shaders/FlatColor.glsl");
}

void Sandbox2D::OnDetach()
{
}

void Sandbox2D::OnUpdate(GanymedE::Timestep ts)
{
	// Update
	m_CameraController.OnUpdate(ts);

	// Render
	GanymedE::RenderCommand::SetClearColor({ 0.1f, 0.1f, 0.1f, 1 });
	GanymedE::RenderCommand::Clear();

	GanymedE::Renderer::BeginScene(m_CameraController.GetCamera());

	std::dynamic_pointer_cast<GanymedE::OpenGLShader>(m_FlatColorShader)->Bind();
	std::dynamic_pointer_cast<GanymedE::OpenGLShader>(m_FlatColorShader)->UploadUniformFloat4("u_Color", m_SquareColor);
	
	GanymedE::Renderer::Submit(m_FlatColorShader, m_SquareVA, glm::scale(glm::mat4(1.f), glm::vec3(1.5f)));

	GanymedE::Renderer::EndScene();
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