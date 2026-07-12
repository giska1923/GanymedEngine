#include "gepch.h"
#include "Renderer3D.h"

#include "UniformBuffer.h"
#include "Shader.h"
#include "RenderCommand.h"
#include "VertexArray.h"
#include "Buffer.h"
#include "Framebuffer.h"
#include "Environment.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>

namespace GanymedE {

	static constexpr uint32_t kMaxLights = 32;
	static constexpr uint32_t kShadowMapSize = 2048;
	static constexpr uint32_t kCascadeCount = 4;
	static constexpr uint32_t kShadowMapSlot0 = 5; // cascades occupy slots 5..8
	static constexpr uint32_t kIrradianceSlot = 9;
	static constexpr uint32_t kPrefilterSlot = 10;
	static constexpr uint32_t kBRDFLutSlot = 11;
	static constexpr uint32_t kSkyboxCubemapSlot = 12;

	struct CameraUBO
	{
		glm::mat4 ViewProjection;
		glm::mat4 View;
		glm::mat4 Projection;
		glm::vec3 CameraPosition;
		float Padding = 0.0f;
	};

	// std140 layout — every member is a vec4 so alignment is trivial
	struct GPULight
	{
		glm::vec4 Position;   // xyz = world position, w = range/radius
		glm::vec4 Direction;  // xyz = direction (spot), w = type (0 = point, 1 = spot)
		glm::vec4 Color;      // rgb = color, w = intensity
		glm::vec4 SpotParams; // x = inner cone cos, y = outer cone cos, z = falloff
	};

	struct LightsUBO
	{
		glm::vec4 DirLightDirection{ 0.0f, -1.0f, 0.0f, 0.0f }; // xyz dir, w = intensity
		glm::vec4 DirLightColor{ 1.0f, 1.0f, 1.0f, 0.0f };      // rgb color, w = castShadows
		glm::vec4 AmbientSky{ 0.0f, 0.0f, 0.0f, 0.0f };         // rgb sky color, w = ambient intensity
		glm::vec4 AmbientGround{ 0.0f, 0.0f, 0.0f, 0.0f };      // rgb ground color, w = hasSkyLight
		glm::ivec4 Counts{ 0, 0, 0, 0 };                        // x = point/spot light count
		GPULight Lights[kMaxLights]{};
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

		LightsUBO LightBuffer{};
		Ref<UniformBuffer> LightUniformBuffer;

		std::vector<DrawCommand> DrawList;

		Ref<Shader> GridShader;
		Ref<VertexArray> GridVertexArray;

		Ref<Shader> SkyboxShader;
		Ref<Shader> SkyboxCubeShader;
		Ref<VertexArray> FullscreenQuad;

		// HDR image-based lighting (optional)
		Ref<Environment> ActiveEnvironment;
		std::unordered_map<std::string, Ref<Environment>> EnvironmentCache;
		bool UseIBL = false;

		// Directional cascaded shadow maps
		Ref<Shader> ShadowDepthShader;
		Ref<Framebuffer> ShadowFramebuffers[kCascadeCount];
		glm::mat4 CascadeLightSpace[kCascadeCount];
		float CascadeSplits[kCascadeCount]{}; // view-space far distance per cascade
		glm::vec3 ShadowLightDir{ 0.0f, -1.0f, 0.0f };
		bool HasShadowLight = false;

		// Environment
		glm::vec3 SkyColor{ 0.0f };
		glm::vec3 GroundColor{ 0.0f };
		float SkyIntensity = 0.0f;
		bool HasSkyLight = false;
		bool DrawSkyboxFlag = false;

		Renderer3D::Statistics Stats;
	};

	static Renderer3DData s_Data;

	void Renderer3D::Init()
	{
		GE_PROFILE_FUNCTION();

		s_Data.CameraUniformBuffer = UniformBuffer::Create(sizeof(CameraUBO), 0);
		s_Data.LightUniformBuffer = UniformBuffer::Create(sizeof(LightsUBO), 1);

		s_Data.GridShader = Shader::Create("assets/shaders/Grid.glsl");
		s_Data.SkyboxShader = Shader::Create("assets/shaders/Skybox.glsl");
		s_Data.SkyboxCubeShader = Shader::Create("assets/shaders/SkyboxCube.glsl");
		s_Data.ShadowDepthShader = Shader::Create("assets/shaders/ShadowDepth.glsl");

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

		// Fullscreen quad in NDC (used by skybox + reusable for post effects)
		float quadVertices[] = {
			-1.0f, -1.0f,
			 1.0f, -1.0f,
			 1.0f,  1.0f,
			-1.0f,  1.0f
		};
		uint32_t quadIndices[] = { 0, 1, 2, 2, 3, 0 };

		s_Data.FullscreenQuad = VertexArray::Create();
		Ref<VertexBuffer> quadVB = VertexBuffer::Create(quadVertices, sizeof(quadVertices));
		quadVB->SetLayout({ { ShaderDataType::Float2, "a_Position" } });
		s_Data.FullscreenQuad->AddVertexBuffer(quadVB);
		Ref<IndexBuffer> quadIB = IndexBuffer::Create(quadIndices, 6);
		s_Data.FullscreenQuad->SetIndexBuffer(quadIB);

		// Depth-only shadow map framebuffer, one per directional cascade
		FramebufferSpecification shadowSpec;
		shadowSpec.Attachments = { FramebufferTextureFormat::DEPTH32F };
		shadowSpec.Width = kShadowMapSize;
		shadowSpec.Height = kShadowMapSize;
		for (uint32_t i = 0; i < kCascadeCount; i++)
			s_Data.ShadowFramebuffers[i] = Framebuffer::Create(shadowSpec);
	}

	void Renderer3D::Shutdown()
	{
		s_Data.DrawList.clear();
		s_Data.GridVertexArray = nullptr;
		s_Data.GridShader = nullptr;
		s_Data.SkyboxShader = nullptr;
		s_Data.SkyboxCubeShader = nullptr;
		s_Data.FullscreenQuad = nullptr;
		s_Data.ShadowDepthShader = nullptr;
		for (uint32_t i = 0; i < kCascadeCount; i++)
			s_Data.ShadowFramebuffers[i] = nullptr;
		s_Data.CameraUniformBuffer = nullptr;
		s_Data.LightUniformBuffer = nullptr;
	}

	static void ResetFrameState()
	{
		s_Data.DrawList.clear();

		s_Data.LightBuffer = LightsUBO{};
		s_Data.HasShadowLight = false;
		s_Data.HasSkyLight = false;
		s_Data.DrawSkyboxFlag = false;
		s_Data.SkyIntensity = 0.0f;
		s_Data.UseIBL = false;
		s_Data.ActiveEnvironment = nullptr;
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
		ResetFrameState();
	}

	void Renderer3D::BeginScene(const EditorCamera& camera)
	{
		GE_PROFILE_FUNCTION();

		UploadCamera(camera.GetProjection(), camera.GetViewMatrix(), camera.GetPosition());
		ResetFrameState();
	}

	void Renderer3D::SubmitDirectionalLight(const glm::vec3& direction, const glm::vec3& color, float intensity, bool castShadows)
	{
		glm::vec3 dir = glm::length(direction) > 0.0001f ? glm::normalize(direction) : glm::vec3(0.0f, -1.0f, 0.0f);
		s_Data.LightBuffer.DirLightDirection = glm::vec4(dir, intensity);
		s_Data.LightBuffer.DirLightColor = glm::vec4(color, castShadows ? 1.0f : 0.0f);

		if (castShadows)
		{
			s_Data.HasShadowLight = true;
			s_Data.ShadowLightDir = dir;
		}
	}

	void Renderer3D::SubmitPointLight(const glm::vec3& position, const glm::vec3& color, float intensity, float radius, float falloff)
	{
		int& count = s_Data.LightBuffer.Counts.x;
		if (count >= (int)kMaxLights)
			return;

		GPULight& light = s_Data.LightBuffer.Lights[count++];
		light.Position = glm::vec4(position, radius);
		light.Direction = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f); // type 0 = point
		light.Color = glm::vec4(color, intensity);
		light.SpotParams = glm::vec4(0.0f, 0.0f, falloff, 0.0f);
	}

	void Renderer3D::SubmitSpotLight(const glm::vec3& position, const glm::vec3& direction, const glm::vec3& color,
		float intensity, float range, float innerConeCos, float outerConeCos, float falloff)
	{
		int& count = s_Data.LightBuffer.Counts.x;
		if (count >= (int)kMaxLights)
			return;

		glm::vec3 dir = glm::length(direction) > 0.0001f ? glm::normalize(direction) : glm::vec3(0.0f, -1.0f, 0.0f);

		GPULight& light = s_Data.LightBuffer.Lights[count++];
		light.Position = glm::vec4(position, range);
		light.Direction = glm::vec4(dir, 1.0f); // type 1 = spot
		light.Color = glm::vec4(color, intensity);
		light.SpotParams = glm::vec4(innerConeCos, outerConeCos, falloff, 0.0f);
	}

	void Renderer3D::SubmitSkyLight(const glm::vec3& skyColor, const glm::vec3& groundColor, float intensity, bool drawSkybox)
	{
		s_Data.HasSkyLight = true;
		s_Data.UseIBL = false;
		s_Data.ActiveEnvironment = nullptr;
		s_Data.SkyColor = skyColor;
		s_Data.GroundColor = groundColor;
		s_Data.SkyIntensity = intensity;
		s_Data.DrawSkyboxFlag = drawSkybox;

		s_Data.LightBuffer.AmbientSky = glm::vec4(skyColor, intensity);
		s_Data.LightBuffer.AmbientGround = glm::vec4(groundColor, 1.0f);
	}

	void Renderer3D::SubmitEnvironment(const Ref<Environment>& environment, float intensity, bool drawSkybox)
	{
		if (!environment || !environment->IsValid())
			return;

		s_Data.HasSkyLight = true;
		s_Data.UseIBL = true;
		s_Data.ActiveEnvironment = environment;
		s_Data.SkyIntensity = intensity;
		s_Data.DrawSkyboxFlag = drawSkybox;

		// w = intensity for the shader; w > 0.5 flags real IBL (not procedural hemispheric)
		s_Data.LightBuffer.AmbientSky = glm::vec4(0.0f, 0.0f, 0.0f, intensity);
		s_Data.LightBuffer.AmbientGround = glm::vec4(0.0f, 0.0f, 0.0f, 2.0f);
	}

	Ref<Environment> Renderer3D::LoadEnvironment(const std::string& path)
	{
		if (path.empty())
			return nullptr;

		auto it = s_Data.EnvironmentCache.find(path);
		if (it != s_Data.EnvironmentCache.end())
			return it->second;

		Ref<Environment> environment = Environment::Create(path);
		s_Data.EnvironmentCache[path] = environment;
		return environment;
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

	// Fits one orthographic light frustum to a slice of the camera frustum (stable, texel-snapped).
	static glm::mat4 FitCascade(const glm::mat4& view, float fov, float aspect, float nearSplit, float farSplit, const glm::vec3& lightDir)
	{
		glm::mat4 cascadeProj = glm::perspective(fov, aspect, nearSplit, farSplit);
		glm::mat4 invViewProj = glm::inverse(cascadeProj * view);

		glm::vec3 corners[8];
		glm::vec3 center(0.0f);
		int idx = 0;
		for (int x = 0; x < 2; x++)
			for (int y = 0; y < 2; y++)
				for (int z = 0; z < 2; z++)
				{
					glm::vec4 pt = invViewProj * glm::vec4(2.0f * x - 1.0f, 2.0f * y - 1.0f, 2.0f * z - 1.0f, 1.0f);
					corners[idx] = glm::vec3(pt) / pt.w;
					center += corners[idx];
					idx++;
				}
		center /= 8.0f;

		float radius = 0.0f;
		for (int c = 0; c < 8; c++)
			radius = glm::max(radius, glm::length(corners[c] - center));
		radius = std::ceil(radius * 16.0f) / 16.0f;

		glm::vec3 up = glm::abs(lightDir.y) > 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
		glm::mat4 lightView = glm::lookAt(center - lightDir * radius, center, up);

		const float zMult = 6.0f;
		glm::mat4 lightProj = glm::ortho(-radius, radius, -radius, radius, -radius * zMult, radius * zMult);

		// Snap the frustum to shadow-map texels to eliminate edge shimmering when the camera moves
		glm::mat4 shadowMatrix = lightProj * lightView;
		glm::vec4 shadowOrigin = shadowMatrix * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
		shadowOrigin *= (float)kShadowMapSize / 2.0f;
		glm::vec4 roundedOrigin = glm::round(shadowOrigin);
		glm::vec4 roundOffset = roundedOrigin - shadowOrigin;
		roundOffset *= 2.0f / (float)kShadowMapSize;
		lightProj[3][0] += roundOffset.x;
		lightProj[3][1] += roundOffset.y;

		return lightProj * lightView;
	}

	static void ComputeCascades(const glm::vec3& lightDir)
	{
		const glm::mat4& proj = s_Data.CameraBuffer.Projection;
		const glm::mat4& view = s_Data.CameraBuffer.View;

		bool perspective = glm::abs(proj[3][3]) < 0.0001f;

		float nearP, farP, fov, aspect;
		if (perspective)
		{
			nearP = proj[3][2] / (proj[2][2] - 1.0f);
			farP = proj[3][2] / (proj[2][2] + 1.0f);
			fov = 2.0f * std::atan(1.0f / proj[1][1]);
			aspect = proj[1][1] / proj[0][0];
		}
		else
		{
			// Orthographic fallback: reasonable defaults, cascades still fit the view slices
			nearP = 0.1f; farP = 200.0f; fov = glm::radians(50.0f); aspect = 1.7778f;
		}

		// Keep cascades tight — shadows past this distance are rarely useful
		float shadowFar = glm::min(farP, 200.0f);

		const float lambda = 0.7f; // blend between uniform and logarithmic splits
		float lastSplit = nearP;
		for (uint32_t i = 0; i < kCascadeCount; i++)
		{
			float p = (float)(i + 1) / (float)kCascadeCount;
			float logSplit = nearP * std::pow(shadowFar / nearP, p);
			float linSplit = nearP + (shadowFar - nearP) * p;
			float splitFar = lambda * logSplit + (1.0f - lambda) * linSplit;

			s_Data.CascadeLightSpace[i] = FitCascade(view, fov, aspect, lastSplit, splitFar, lightDir);
			s_Data.CascadeSplits[i] = splitFar;
			lastSplit = splitFar;
		}
	}

	static void RenderShadowPass()
	{
		if (!s_Data.HasShadowLight || !s_Data.ShadowDepthShader)
			return;

		ComputeCascades(s_Data.ShadowLightDir);

		s_Data.ShadowDepthShader->Bind();

		for (uint32_t c = 0; c < kCascadeCount; c++)
		{
			if (!s_Data.ShadowFramebuffers[c])
				continue;

			s_Data.ShadowFramebuffers[c]->Bind();
			RenderCommand::SetDepthTest(true);
			RenderCommand::SetDepthWrite(true);
			RenderCommand::Clear();

			// Cull front faces while rendering caster depth to curb acne / peter-panning
			RenderCommand::SetCullFace(true);
			RenderCommand::SetCullMode(RendererAPI::CullMode::Front);

			s_Data.ShadowDepthShader->SetMat4("u_LightSpaceMatrix", s_Data.CascadeLightSpace[c]);

			for (const DrawCommand& cmd : s_Data.DrawList)
			{
				const Submesh& submesh = cmd.Mesh->GetSubmeshes()[cmd.SubmeshIndex];
				s_Data.ShadowDepthShader->SetMat4("u_Transform", cmd.Transform);
				RenderCommand::DrawIndexed(cmd.Mesh->GetVertexArray(), submesh.IndexCount, submesh.BaseIndex, (int32_t)submesh.BaseVertex);
				s_Data.Stats.DrawCalls++;
			}

			RenderCommand::SetCullMode(RendererAPI::CullMode::Back);
			s_Data.ShadowFramebuffers[c]->Unbind();
		}
	}

	void Renderer3D::EndScene()
	{
		GE_PROFILE_FUNCTION();

		// Push all shared frame data to the GPU
		s_Data.LightUniformBuffer->SetData(&s_Data.LightBuffer, sizeof(LightsUBO));

		// Shadow depth pass (renders the same draw list from the light's POV)
		RenderShadowPass();

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

		// Bind all cascade depth maps once for the whole pass (slots 5..8)
		const float shadowTexelSize = 1.0f / (float)kShadowMapSize;
		for (uint32_t c = 0; c < kCascadeCount; c++)
		{
			if (s_Data.ShadowFramebuffers[c])
				s_Data.ShadowFramebuffers[c]->BindDepthTexture(kShadowMapSlot0 + c);
		}

		// Bind IBL textures once for the pass (slots 9..11)
		if (s_Data.UseIBL && s_Data.ActiveEnvironment)
			s_Data.ActiveEnvironment->BindIBL(kIrradianceSlot, kPrefilterSlot, kBRDFLutSlot);

		for (const DrawCommand& cmd : s_Data.DrawList)
		{
			const Submesh& submesh = cmd.Mesh->GetSubmeshes()[cmd.SubmeshIndex];

			if (cmd.Material)
			{
				RenderCommand::SetCullFace(!cmd.Material->IsTwoSided());
				cmd.Material->Bind();

				const Ref<Shader>& shader = cmd.Material->GetShader();
				shader->SetMat4("u_Transform", cmd.Transform);
				shader->SetInt("u_EntityID", cmd.EntityID);

				shader->SetInt("u_UseShadows", s_Data.HasShadowLight ? 1 : 0);
				shader->SetInt("u_CascadeCount", (int)kCascadeCount);
				shader->SetFloat("u_ShadowTexelSize", shadowTexelSize);
				for (uint32_t c = 0; c < kCascadeCount; c++)
				{
					std::string idx = std::to_string(c);
					shader->SetMat4("u_LightSpaceMatrices[" + idx + "]", s_Data.CascadeLightSpace[c]);
					shader->SetInt("u_ShadowMaps[" + idx + "]", (int)(kShadowMapSlot0 + c));
					shader->SetFloat("u_CascadeSplits[" + idx + "]", s_Data.CascadeSplits[c]);
				}

				shader->SetInt("u_UseIBL", s_Data.UseIBL ? 1 : 0);
				shader->SetInt("u_IrradianceMap", (int)kIrradianceSlot);
				shader->SetInt("u_PrefilterMap", (int)kPrefilterSlot);
				shader->SetInt("u_BRDFLUT", (int)kBRDFLutSlot);
				shader->SetFloat("u_MaxReflectionLod",
					s_Data.ActiveEnvironment ? s_Data.ActiveEnvironment->GetMaxReflectionLod() : 0.0f);
			}

			RenderCommand::DrawIndexed(cmd.Mesh->GetVertexArray(), submesh.IndexCount, submesh.BaseIndex, (int32_t)submesh.BaseVertex);
			s_Data.Stats.DrawCalls++;
		}

		RenderCommand::SetCullFace(true);
		s_Data.DrawList.clear();
	}

	void Renderer3D::DrawSkybox()
	{
		if (!s_Data.DrawSkyboxFlag || !s_Data.FullscreenQuad)
			return;

		RenderCommand::SetDepthTest(false);
		RenderCommand::SetDepthWrite(false);
		RenderCommand::SetCullFace(false);

		glm::mat4 invViewProj = glm::inverse(s_Data.CameraBuffer.ViewProjection);

		if (s_Data.UseIBL && s_Data.ActiveEnvironment && s_Data.SkyboxCubeShader)
		{
			s_Data.ActiveEnvironment->BindSkybox(kSkyboxCubemapSlot);

			s_Data.SkyboxCubeShader->Bind();
			s_Data.SkyboxCubeShader->SetMat4("u_InverseViewProjection", invViewProj);
			s_Data.SkyboxCubeShader->SetInt("u_EnvironmentMap", (int)kSkyboxCubemapSlot);
			s_Data.SkyboxCubeShader->SetFloat("u_Intensity", s_Data.SkyIntensity);
		}
		else if (s_Data.SkyboxShader)
		{
			s_Data.SkyboxShader->Bind();
			s_Data.SkyboxShader->SetMat4("u_InverseViewProjection", invViewProj);
			s_Data.SkyboxShader->SetFloat3("u_SkyColor", s_Data.SkyColor);
			s_Data.SkyboxShader->SetFloat3("u_GroundColor", s_Data.GroundColor);
			s_Data.SkyboxShader->SetFloat("u_SkyIntensity", s_Data.SkyIntensity);
			s_Data.SkyboxShader->SetFloat3("u_SunDirection", glm::vec3(s_Data.LightBuffer.DirLightDirection));
		}
		else
		{
			RenderCommand::SetDepthTest(true);
			RenderCommand::SetDepthWrite(true);
			RenderCommand::SetCullFace(true);
			return;
		}

		RenderCommand::DrawIndexed(s_Data.FullscreenQuad);
		s_Data.Stats.DrawCalls++;

		RenderCommand::SetDepthTest(true);
		RenderCommand::SetDepthWrite(true);
		RenderCommand::SetCullFace(true);
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
