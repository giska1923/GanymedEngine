#include <GanymedE.h>

#include "imgui.h"

class ExampleLayer :public GanymedE::Layer {
public:
	ExampleLayer()
		: Layer("Example"), m_Camera(-1.6f, 1.6f, -0.9f, 0.9f), m_CameraPosition(0.f)
	{
		m_VertexArray.reset(GanymedE::VertexArray::Create());

		float vertices[3 * 7] = {
			-0.5f, -0.5f, 0.0f, 0.8f, 0.2f, 0.8f, 1.0f,
			0.5f, -0.5f, 0.0f, 0.2f, 0.3f, 0.8f, 1.0f,
			0.0f, 0.5f, 0.0f, 0.8f, 0.8f, 0.2f, 1.0f
		};

		std::shared_ptr<GanymedE::VertexBuffer> vertexBuffer;
		vertexBuffer.reset(GanymedE::VertexBuffer::Create(vertices, sizeof(vertices)));
		GanymedE::BufferLayout layout = {
			{ GanymedE::ShaderDataType::Float3, "a_Position" },
			{ GanymedE::ShaderDataType::Float4, "a_Color" }
		};

		vertexBuffer->SetLayout(layout);
		m_VertexArray->AddVertexBuffer(vertexBuffer);

		uint32_t indices[3] = { 0, 1, 2 };
		std::shared_ptr<GanymedE::IndexBuffer> indexBuffer;
		indexBuffer.reset(GanymedE::IndexBuffer::Create(indices, std::size(indices)));
		m_VertexArray->SetIndexBuffer(indexBuffer);

		m_SquareVA.reset(GanymedE::VertexArray::Create());

		float squareVertices[3 * 4] = {
			-0.75f, -0.75f, 0.0f,
			 0.75f, -0.75f, 0.0f,
			 0.75f,  0.75f, 0.0f,
			-0.75f,  0.75f, 0.0f
		};

		std::shared_ptr<GanymedE::VertexBuffer> squareVB;
		squareVB.reset(GanymedE::VertexBuffer::Create(squareVertices, sizeof(squareVertices)));
		squareVB->SetLayout({
			{ GanymedE::ShaderDataType::Float3, "a_Position" }
		});
		m_SquareVA->AddVertexBuffer(squareVB);

		uint32_t squareIndices[6] = { 0, 1, 2, 2, 3, 0 };
		std::shared_ptr<GanymedE::IndexBuffer> squareIB;
		squareIB.reset(GanymedE::IndexBuffer::Create(squareIndices, std::size(squareIndices)));
		m_SquareVA->SetIndexBuffer(squareIB);

		std::string vertexSrc = R"(
			#version 330 core

			layout(location = 0) in vec3 a_Position;
			layout(location = 1) in vec4 a_Color;

			uniform mat4 u_ViewProjection;

			out vec3 v_Position;
			out vec4 v_Color;

			void main()
			{
				v_Position = a_Position;
				v_Color = a_Color;
				gl_Position = u_ViewProjection * vec4(a_Position, 1.0);
			}		
		)";

		std::string fragmentSrc = R"(
			#version 330 core

			layout(location = 0) out vec4 color;

			in vec3 v_Position;
			in vec4 v_Color;

			void main()
			{
				color = vec4(v_Position * 0.5 + 0.5, 1.0);
				color = v_Color;
			}		
		)";

		m_Shader.reset(new GanymedE::Shader(vertexSrc, fragmentSrc));

		std::string blueShaderVertexSrc = R"(
			#version 330 core

			layout(location = 0) in vec3 a_Position;

			uniform mat4 u_ViewProjection;

			out vec3 v_Position;

			void main()
			{
				v_Position = a_Position;
				gl_Position = u_ViewProjection * vec4(a_Position, 1.0);
			}		
		)";

		std::string blueShaderFragmentSrc = R"(
			#version 330 core

			layout(location = 0) out vec4 color;

			in vec3 v_Position;

			void main()
			{
				color = vec4(0.2, 0.3, 0.8, 1.0);
			}		
		)";

		m_BlueShader.reset(new GanymedE::Shader(blueShaderVertexSrc, blueShaderFragmentSrc));
	}

	void OnUpdate(GanymedE::Timestep ts) override
	{
		if (GanymedE::Input::IsKeyPressed(GE_KEY_LEFT))
		{
			m_CameraPosition.x -= m_CameraMoveSpeed * ts;
		}
		else if (GanymedE::Input::IsKeyPressed(GE_KEY_RIGHT))
		{
			m_CameraPosition.x += m_CameraMoveSpeed * ts;
		}

		if (GanymedE::Input::IsKeyPressed(GE_KEY_UP))
		{
			m_CameraPosition.y += m_CameraMoveSpeed * ts;
		}
		else if (GanymedE::Input::IsKeyPressed(GE_KEY_DOWN))
		{
			m_CameraPosition.y -= m_CameraMoveSpeed * ts;
		}

		if (GanymedE::Input::IsKeyPressed(GE_KEY_A))
		{
			m_CameraRotation += m_CameraRotationSpeed * ts;
		}
		if (GanymedE::Input::IsKeyPressed(GE_KEY_D))
		{
			m_CameraRotation -= m_CameraRotationSpeed * ts;
		}

		GanymedE::RenderCommand::SetClearColor({ 0.1f, 0.1f, 0.1f, 1 });
		GanymedE::RenderCommand::Clear();

		m_Camera.SetPosition(m_CameraPosition);
		m_Camera.SetRotation(m_CameraRotation);

		GanymedE::Renderer::BeginScene(m_Camera);

		GanymedE::Renderer::Submit(m_BlueShader, m_SquareVA);
		GanymedE::Renderer::Submit(m_Shader, m_VertexArray);

		GanymedE::Renderer::EndScene();
	}

	virtual void OnImGuiRender() override
	{

	}

	void OnEvent(GanymedE::Event& event) override
	{

	}

private:
	std::shared_ptr<GanymedE::Shader> m_Shader;
	std::shared_ptr<GanymedE::VertexArray> m_VertexArray;

	std::shared_ptr<GanymedE::Shader> m_BlueShader;
	std::shared_ptr<GanymedE::VertexArray> m_SquareVA;

	GanymedE::OrthographicCamera m_Camera;
	glm::vec3 m_CameraPosition;
	float m_CameraMoveSpeed = 5.f;

	float m_CameraRotation = 0.f;
	float m_CameraRotationSpeed = 180.f;
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