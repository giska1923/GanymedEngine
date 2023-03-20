#include <GanymedE.h>
//-------------ENTRY POINT-----------------
#include <GanymedE/main/EntryPoint.h>
//-----------------------------------------

#include "Platform/OpenGL/OpenGLShader.h"

#include "imgui.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "Sandbox2D.h"

class ExampleLayer :public GanymedE::Layer {
public:
	ExampleLayer()
		: Layer("Example"), m_CameraController((float)DEFAULT_WINDOW_WIDTH / (float)DEFAULT_WINDOW_HEIGHT)
	{
		m_VertexArray = GanymedE::VertexArray::Create();

		float vertices[3 * 7] = {
			-0.5f, -0.5f, 0.0f, 0.8f, 0.2f, 0.8f, 1.0f,
			0.5f, -0.5f, 0.0f, 0.2f, 0.3f, 0.8f, 1.0f,
			0.0f, 0.5f, 0.0f, 0.8f, 0.8f, 0.2f, 1.0f
		};

		GanymedE::Ref<GanymedE::VertexBuffer> vertexBuffer;
		vertexBuffer = GanymedE::VertexBuffer::Create(vertices, sizeof(vertices));
		GanymedE::BufferLayout layout = {
			{ GanymedE::ShaderDataType::Float3, "a_Position" },
			{ GanymedE::ShaderDataType::Float4, "a_Color" }
		};

		vertexBuffer->SetLayout(layout);
		m_VertexArray->AddVertexBuffer(vertexBuffer);

		uint32_t indices[3] = { 0, 1, 2 };
		GanymedE::Ref<GanymedE::IndexBuffer> indexBuffer;
		indexBuffer = GanymedE::IndexBuffer::Create(indices, std::size(indices));
		m_VertexArray->SetIndexBuffer(indexBuffer);

		m_SquareVA = GanymedE::VertexArray::Create();

		float squareVertices[5 * 4] = {
			-0.5f, -0.5f, 0.0f, 0.f, 0.f,
			 0.5f, -0.5f, 0.0f, 1.f, 0.f,
			 0.5f,  0.5f, 0.0f, 1.f, 1.f,
			-0.5f,  0.5f, 0.0f, 0.f, 1.f
		};

		GanymedE::Ref<GanymedE::VertexBuffer> squareVB;
		squareVB = GanymedE::VertexBuffer::Create(squareVertices, sizeof(squareVertices));
		squareVB->SetLayout({
			{ GanymedE::ShaderDataType::Float3, "a_Position" },
			{ GanymedE::ShaderDataType::Float2, "a_TexCoord" }
		});
		m_SquareVA->AddVertexBuffer(squareVB);

		uint32_t squareIndices[6] = { 0, 1, 2, 2, 3, 0 };
		GanymedE::Ref<GanymedE::IndexBuffer> squareIB;
		squareIB = GanymedE::IndexBuffer::Create(squareIndices, std::size(squareIndices));
		m_SquareVA->SetIndexBuffer(squareIB);

		m_Shader = GanymedE::Shader::Create("assets/shaders/VertexPosColor.glsl");

		m_FlatColorShader = GanymedE::Shader::Create("assets/shaders/FlatColor.glsl");

		m_TextureShader = GanymedE::Shader::Create("assets/shaders/Texture.glsl");

		m_Texture = GanymedE::Texture2D::Create("assets/textures/Checkerboard.png");

		std::dynamic_pointer_cast<GanymedE::OpenGLShader>(m_TextureShader)->Bind();
		std::dynamic_pointer_cast<GanymedE::OpenGLShader>(m_TextureShader)->UploadUniformInt("u_Texture", 0);
	}

	void OnUpdate(GanymedE::Timestep ts) override
	{
		// Update
		m_CameraController.OnUpdate(ts);

		// Render
		GanymedE::RenderCommand::SetClearColor({ 0.1f, 0.1f, 0.1f, 1 });
		GanymedE::RenderCommand::Clear();

		GanymedE::Renderer::BeginScene(m_CameraController.GetCamera());

		static glm::mat4 scale = glm::scale(glm::mat4(1.f), glm::vec3(0.1f));

		std::dynamic_pointer_cast<GanymedE::OpenGLShader>(m_FlatColorShader)->Bind();
		std::dynamic_pointer_cast<GanymedE::OpenGLShader>(m_FlatColorShader)->UploadUniformFloat3("u_Color", m_SquareColor);

		for (int y = 0; y < 20; y++)
		{
			for (int x = 0; x < 20; x++)
			{
				glm::vec3 pos(x * 0.11f, y * 0.11f, 0.f);
				glm::mat4 transform = glm::translate(glm::mat4(1.f), pos) * scale;
				GanymedE::Renderer::Submit(m_FlatColorShader, m_SquareVA, transform);
			}
		}

		m_Texture->Bind();
		GanymedE::Renderer::Submit(m_TextureShader, m_SquareVA, glm::scale(glm::mat4(1.f), glm::vec3(1.5f)));

		// Triangle
		//GanymedE::Renderer::Submit(m_Shader, m_VertexArray);

		GanymedE::Renderer::EndScene();
	}

	virtual void OnImGuiRender() override
	{
		ImGui::Begin("Settings");
		ImGui::ColorEdit3("Square Color", glm::value_ptr(m_SquareColor));
		ImGui::End();
	}

	void OnEvent(GanymedE::Event& e) override
	{
		m_CameraController.OnEvent(e);
	}

private:
	GanymedE::ShaderLibrary m_ShaderLibrary;
	GanymedE::Ref<GanymedE::Shader> m_Shader;
	GanymedE::Ref<GanymedE::VertexArray> m_VertexArray;

	GanymedE::Ref<GanymedE::Shader> m_FlatColorShader, m_TextureShader;
	GanymedE::Ref<GanymedE::VertexArray> m_SquareVA;

	GanymedE::Ref<GanymedE::Texture2D> m_Texture;

	GanymedE::OrthographicCameraController m_CameraController;

	glm::vec3 m_SquareColor = { 0.2f, 0.3f, 0.8f };
};


class Sandbox : public GanymedE::Application {
public:
	Sandbox() {
		// PushLayer(new ExampleLayer());
		PushLayer(new Sandbox2D());
	}
	~Sandbox() {}

};

GanymedE::Application* GanymedE::CreateApplication() {
	return new Sandbox();
}