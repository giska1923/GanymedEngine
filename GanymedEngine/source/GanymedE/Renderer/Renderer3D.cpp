#include "gepch.h"
#include "Renderer3D.h"

#include "UniformBuffer.h"
#include "Shader.h"
#include "RenderCommand.h"
#include "VertexArray.h"
#include "Buffer.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>

namespace GanymedE {

	struct CameraUBO
	{
		glm::mat4 ViewProjection;
		glm::mat4 View;
		glm::mat4 Projection;
		glm::vec3 CameraPosition;
		float Padding = 0.0f;
	};

	struct DrawCommand
	{
		Ref<Mesh> Mesh;
		uint32_t SubmeshIndex = 0;
		Ref<Material> Material;
		glm::mat4 Transform{ 1.0f };
		int EntityID = -1;
		float SortKey = 0.0f; // distance from camera for front-to-back
	};

	struct Renderer3DData
	{
		CameraUBO CameraBuffer{};
		Ref<UniformBuffer> CameraUniformBuffer;

		std::vector<DrawCommand> DrawList;

		Ref<Shader> GridShader;
		Ref<VertexArray> GridVertexArray;

		glm::vec3 LightDirection = glm::normalize(glm::vec3(0.4f, -1.0f, 0.3f));
		glm::vec3 LightColor = { 1.0f, 0.98f, 0.92f };

		Renderer3D::Statistics Stats;
	};

	static Renderer3DData s_Data;

	void Renderer3D::Init()
	{
		GE_PROFILE_FUNCTION();

		s_Data.CameraUniformBuffer = UniformBuffer::Create(sizeof(CameraUBO), 0);

		s_Data.GridShader = Shader::Create("assets/shaders/Grid.glsl");

		// Fullscreen-ish large quad on XZ plane; real infinite look comes from the fragment shader
		float gridVertices[] = {
			-1.0f, 0.0f, -1.0f,
			 1.0f, 0.0f, -1.0f,
			 1.0f, 0.0f,  1.0f,
			-1.0f, 0.0f,  1.0f
		};
		uint32_t gridIndices[] = { 0, 1, 2, 2, 3, 0 };

		s_Data.GridVertexArray = VertexArray::Create();
		Ref<VertexBuffer> gridVB = VertexBuffer::Create(gridVertices, sizeof(gridVertices));
		gridVB->SetLayout({ { ShaderDataType::Float3, "a_Position" } });
		s_Data.GridVertexArray->AddVertexBuffer(gridVB);
		Ref<IndexBuffer> gridIB = IndexBuffer::Create(gridIndices, 6);
		s_Data.GridVertexArray->SetIndexBuffer(gridIB);
	}

	void Renderer3D::Shutdown()
	{
		s_Data.DrawList.clear();
		s_Data.GridVertexArray = nullptr;
		s_Data.GridShader = nullptr;
		s_Data.CameraUniformBuffer = nullptr;
	}

	static void UploadCamera(const glm::mat4& projection, const glm::mat4& view, const glm::vec3& cameraPos)
	{
		s_Data.CameraBuffer.ViewProjection = projection * view;
		s_Data.CameraBuffer.View = view;
		s_Data.CameraBuffer.Projection = projection;
		s_Data.CameraBuffer.CameraPosition = cameraPos;
		s_Data.CameraUniformBuffer->SetData(&s_Data.CameraBuffer, sizeof(CameraUBO));
	}

	void Renderer3D::BeginScene(const Camera& camera, const glm::mat4& transform)
	{
		GE_PROFILE_FUNCTION();

		glm::mat4 view = glm::inverse(transform);
		glm::vec3 cameraPos = glm::vec3(transform[3]);
		UploadCamera(camera.GetProjection(), view, cameraPos);
		s_Data.DrawList.clear();
	}

	void Renderer3D::BeginScene(const EditorCamera& camera)
	{
		GE_PROFILE_FUNCTION();

		UploadCamera(camera.GetProjection(), camera.GetViewMatrix(), camera.GetPosition());
		s_Data.DrawList.clear();
	}

	void Renderer3D::SubmitMesh(const Ref<Mesh>& mesh, const glm::mat4& transform, int entityID)
	{
		if (!mesh)
			return;

		const auto& submeshes = mesh->GetSubmeshes();
		for (uint32_t i = 0; i < (uint32_t)submeshes.size(); i++)
		{
			Ref<Material> material = mesh->GetMaterial(submeshes[i].MaterialIndex);
			SubmitMesh(mesh, i, material, transform, entityID);
		}
	}

	void Renderer3D::SubmitMesh(const Ref<Mesh>& mesh, uint32_t submeshIndex, const Ref<Material>& material,
		const glm::mat4& transform, int entityID)
	{
		if (!mesh || submeshIndex >= mesh->GetSubmeshes().size())
			return;

		DrawCommand cmd;
		cmd.Mesh = mesh;
		cmd.SubmeshIndex = submeshIndex;
		cmd.Material = material;
		cmd.Transform = transform * mesh->GetSubmeshes()[submeshIndex].LocalTransform;
		cmd.EntityID = entityID;

		glm::vec3 center = glm::vec3(cmd.Transform[3]);
		cmd.SortKey = glm::dot(center - s_Data.CameraBuffer.CameraPosition, center - s_Data.CameraBuffer.CameraPosition);

		s_Data.DrawList.push_back(cmd);
		s_Data.Stats.MeshCount++;
	}

	void Renderer3D::EndScene()
	{
		GE_PROFILE_FUNCTION();

		// Opaque front-to-back, then group by material pointer for fewer state changes
		std::sort(s_Data.DrawList.begin(), s_Data.DrawList.end(), [](const DrawCommand& a, const DrawCommand& b)
		{
			if (a.Material != b.Material)
				return a.Material < b.Material;
			if (a.Mesh != b.Mesh)
				return a.Mesh < b.Mesh;
			return a.SortKey < b.SortKey;
		});

		RenderCommand::SetDepthTest(true);
		RenderCommand::SetDepthWrite(true);

		for (const DrawCommand& cmd : s_Data.DrawList)
		{
			const Submesh& submesh = cmd.Mesh->GetSubmeshes()[cmd.SubmeshIndex];

			if (cmd.Material)
			{
				RenderCommand::SetCullFace(!cmd.Material->IsTwoSided());
				cmd.Material->Bind();
				cmd.Material->GetShader()->SetMat4("u_Transform", cmd.Transform);
				cmd.Material->GetShader()->SetFloat3("u_LightDirection", s_Data.LightDirection);
				cmd.Material->GetShader()->SetFloat3("u_LightColor", s_Data.LightColor);
				cmd.Material->GetShader()->SetFloat3("u_CameraPosition", s_Data.CameraBuffer.CameraPosition);
				cmd.Material->GetShader()->SetInt("u_EntityID", cmd.EntityID);
			}

			RenderCommand::DrawIndexed(cmd.Mesh->GetVertexArray(), submesh.IndexCount, submesh.BaseIndex, (int32_t)submesh.BaseVertex);
			s_Data.Stats.DrawCalls++;
		}

		RenderCommand::SetCullFace(true);
		s_Data.DrawList.clear();
	}

	void Renderer3D::DrawGrid()
	{
		if (!s_Data.GridShader || !s_Data.GridVertexArray)
			return;

		RenderCommand::SetDepthTest(true);
		RenderCommand::SetDepthWrite(false);
		RenderCommand::SetCullFace(false);

		s_Data.GridShader->Bind();
		// Scale grid quad so the shader's world-XZ fade covers a large area
		glm::mat4 transform = glm::scale(glm::mat4(1.0f), glm::vec3(100.0f));
		s_Data.GridShader->SetMat4("u_Transform", transform);
		s_Data.GridShader->SetFloat3("u_CameraPosition", s_Data.CameraBuffer.CameraPosition);

		RenderCommand::DrawIndexed(s_Data.GridVertexArray);
		s_Data.Stats.DrawCalls++;

		RenderCommand::SetDepthWrite(true);
		RenderCommand::SetCullFace(true);
	}

	void Renderer3D::ResetStats()
	{
		memset(&s_Data.Stats, 0, sizeof(Statistics));
	}

	Renderer3D::Statistics Renderer3D::GetStats()
	{
		return s_Data.Stats;
	}

}
