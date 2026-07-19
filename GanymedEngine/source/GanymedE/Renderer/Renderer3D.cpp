#include "gepch.h"
#include "Renderer3D.h"

#include "FrameUniforms.h"
#include "Shader.h"
#include "RenderCommand.h"
#include "RenderPassIDs.h"
#include "Buffer.h"
#include "Framebuffer.h"
#include "Environment.h"
#include "GanymedE/Math/BoundingVolumes.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

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

	// Aliases keep the member names from shadowing the type names (GCC rejects that)
	using MeshRef = Ref<Mesh>;
	using MaterialRef = Ref<Material>;

	struct DrawCommand
	{
		MeshRef Mesh;
		uint32_t SubmeshIndex = 0;
		MaterialRef Material;
		glm::mat4 Transform{ 1.0f };
		int EntityID = -1;
		float SortKey = 0.0f;  // squared distance from camera
		AABB WorldBounds;      // submesh bounds in world space, for frustum culling
	};

	struct Renderer3DData
	{
		CameraUBO CameraBuffer{};
		Frustum CameraFrustum;

		LightsUBO LightBuffer{};

		std::vector<DrawCommand> DrawList;

		Ref<Shader> GridShader;
		Geometry GridGeometry;

		Ref<Shader> SkyboxShader;
		Ref<Shader> SkyboxCubeShader;
		Geometry FullscreenQuad;

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

		// Debug lines
		struct LineVertex
		{
			glm::vec3 Position;
			glm::vec4 Color;
		};
		std::vector<LineVertex> LineVertices;
		Ref<Shader> LineShader;
		Geometry LineGeometry;
		Ref<VertexBuffer> LineVertexBuffer;
		static constexpr uint32_t MaxLineVertices = 20000;

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

		FrameUniforms::Init();

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

		s_Data.GridGeometry.Vertices = VertexBuffer::Create(gridVertices, sizeof(gridVertices),
			{ { ShaderDataType::Float3, "a_Position" } });
		s_Data.GridGeometry.Indices = IndexBuffer::Create(gridIndices, 6);

		// Fullscreen quad in NDC (used by skybox + reusable for post effects)
		float quadVertices[] = {
			-1.0f, -1.0f,
			 1.0f, -1.0f,
			 1.0f,  1.0f,
			-1.0f,  1.0f
		};
		uint32_t quadIndices[] = { 0, 1, 2, 2, 3, 0 };

		s_Data.FullscreenQuad.Vertices = VertexBuffer::Create(quadVertices, sizeof(quadVertices),
			{ { ShaderDataType::Float2, "a_Position" } });
		s_Data.FullscreenQuad.Indices = IndexBuffer::Create(quadIndices, 6);

		// Depth-only shadow map framebuffer, one per directional cascade
		FramebufferSpecification shadowSpec;
		shadowSpec.Attachments = { FramebufferTextureFormat::DEPTH32F };
		shadowSpec.Width = kShadowMapSize;
		shadowSpec.Height = kShadowMapSize;
		for (uint32_t i = 0; i < kCascadeCount; i++)
			s_Data.ShadowFramebuffers[i] = Framebuffer::Create(shadowSpec);

		s_Data.LineShader = Shader::Create("assets/shaders/Line.glsl");
		s_Data.LineVertexBuffer = VertexBuffer::Create(s_Data.MaxLineVertices * sizeof(Renderer3DData::LineVertex), {
			{ ShaderDataType::Float3, "a_Position" },
			{ ShaderDataType::Float4, "a_Color" }
		});
		s_Data.LineGeometry.Vertices = s_Data.LineVertexBuffer;
		s_Data.LineVertices.reserve(s_Data.MaxLineVertices);
	}

	void Renderer3D::Shutdown()
	{
		s_Data.DrawList.clear();
		s_Data.GridGeometry = {};
		s_Data.GridShader = nullptr;
		s_Data.SkyboxShader = nullptr;
		s_Data.SkyboxCubeShader = nullptr;
		s_Data.FullscreenQuad = {};
		s_Data.ShadowDepthShader = nullptr;
		for (uint32_t i = 0; i < kCascadeCount; i++)
			s_Data.ShadowFramebuffers[i] = nullptr;
		FrameUniforms::Shutdown();
		s_Data.LineShader = nullptr;
		s_Data.LineGeometry = {};
		s_Data.LineVertexBuffer = nullptr;
		s_Data.LineVertices.clear();
	}

	static void ResetFrameState()
	{
		s_Data.DrawList.clear();
		s_Data.LineVertices.clear();

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

		// The matrices ride on the view rather than in a uniform block; only the
		// camera position still needs uploading. Culling keeps using the CPU-side
		// copy, which is why CameraBuffer survives the UBO removal.
		FrameUniforms::SetCamera(RenderPass::SceneHDR, view, projection, cameraPos);

		s_Data.CameraFrustum = Frustum::FromViewProjection(s_Data.CameraBuffer.ViewProjection);
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
		cmd.WorldBounds = mesh->GetSubmeshes()[submeshIndex].Bounds.Transformed(cmd.Transform);

		glm::vec3 center = (cmd.WorldBounds.Min + cmd.WorldBounds.Max) * 0.5f;
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

	// Draws a contiguous run of commands sharing (mesh, submesh) as instanced chunks.
	static void DrawInstancedGroup(const DrawCommand* const* cmds, size_t count)
	{
		static std::vector<MeshInstanceData> s_Instances;

		const Ref<Mesh>& mesh = cmds[0]->Mesh;
		const Submesh& submesh = mesh->GetSubmeshes()[cmds[0]->SubmeshIndex];

		for (size_t offset = 0; offset < count; offset += Mesh::MaxInstancesPerDraw)
		{
			size_t chunk = std::min((size_t)Mesh::MaxInstancesPerDraw, count - offset);
			s_Instances.resize(chunk);
			for (size_t i = 0; i < chunk; i++)
			{
				s_Instances[i].Transform = cmds[offset + i]->Transform;
				s_Instances[i].EntityID = cmds[offset + i]->EntityID;
			}

			mesh->SetInstanceData(s_Instances.data(), (uint32_t)chunk);

			// bgfx copies instance data into its own transient buffer at submit
			// time, so it is handed over per draw rather than pre-uploaded.
			const auto& instances = mesh->GetInstanceData();
			RenderCommand::DrawIndexedInstanced(mesh->GetGeometry(), submesh.IndexCount,
				submesh.BaseIndex, (int32_t)submesh.BaseVertex,
				instances.data(), (uint32_t)instances.size(), (uint16_t)sizeof(MeshInstanceData));
			s_Data.Stats.DrawCalls++;
			if (chunk > 1)
				s_Data.Stats.InstancedDraws++;
		}
	}

	static void RenderShadowPass(std::vector<const DrawCommand*>& casters)
	{
		if (!s_Data.HasShadowLight || !s_Data.ShadowDepthShader || casters.empty())
			return;

		ComputeCascades(s_Data.ShadowLightDir);

		// Group instances by mesh + submesh (material is irrelevant for depth)
		std::sort(casters.begin(), casters.end(), [](const DrawCommand* a, const DrawCommand* b)
		{
			if (a->Mesh != b->Mesh)
				return a->Mesh < b->Mesh;
			return a->SubmeshIndex < b->SubmeshIndex;
		});

		s_Data.ShadowDepthShader->Bind();

		for (uint32_t c = 0; c < kCascadeCount; c++)
		{
			if (!s_Data.ShadowFramebuffers[c])
				continue;

			// One view per cascade, so bgfx renders them in a defined order.
			const uint16_t view = RenderPass::Shadow + (uint16_t)c;
			s_Data.ShadowFramebuffers[c]->BindToView(view);
			RenderCommand::SetViewId(view);
			bgfx::setViewClear(view, BGFX_CLEAR_DEPTH, 0x00000000, 1.0f, 0);

			RenderCommand::SetDepthTest(true);
			RenderCommand::SetDepthWrite(true);

			// Cull front faces while rendering caster depth to curb acne / peter-panning
			RenderCommand::SetCullFace(true);
			RenderCommand::SetCullMode(RenderState::CullMode::Front);

			s_Data.ShadowDepthShader->SetMat4("u_LightSpaceMatrix", s_Data.CascadeLightSpace[c]);

			for (size_t i = 0; i < casters.size(); )
			{
				size_t end = i + 1;
				while (end < casters.size()
					&& casters[end]->Mesh == casters[i]->Mesh
					&& casters[end]->SubmeshIndex == casters[i]->SubmeshIndex)
					end++;

				DrawInstancedGroup(&casters[i], end - i);
				i = end;
			}

			RenderCommand::SetCullMode(RenderState::CullMode::Back);
		}
	}

	// Bind a material plus the per-pass shadow/IBL uniforms its shader needs
	static void BindMaterialForColorPass(const Ref<Material>& material)
	{
		RenderCommand::SetCullFace(!material->IsTwoSided());
		material->Bind();

		const Ref<Shader>& shader = material->GetShader();
		const float shadowTexelSize = 1.0f / (float)kShadowMapSize;

		shader->SetInt("u_UseShadows", s_Data.HasShadowLight ? 1 : 0);
		shader->SetInt("u_CascadeCount", (int)kCascadeCount);
		shader->SetFloat("u_ShadowTexelSize", shadowTexelSize);

		// bgfx addresses an array uniform as a whole; "u_Name[i]" is not a name it
		// knows. The four cascade matrices go up as one mat4[4], and the four
		// split distances fit in a single vec4.
		shader->SetMat4Array("u_LightSpaceMatrices", s_Data.CascadeLightSpace, kCascadeCount);
		shader->SetFloat4("u_CascadeSplits", glm::vec4(
			s_Data.CascadeSplits[0], s_Data.CascadeSplits[1],
			s_Data.CascadeSplits[2], s_Data.CascadeSplits[3]));

		for (uint32_t c = 0; c < kCascadeCount; c++)
		{
			std::string idx = std::to_string(c);

			// Shadow lookups must clamp: a sample off the edge of a cascade would
			// otherwise wrap and shadow the opposite side of the scene.
			if (s_Data.ShadowFramebuffers[c])
			{
				shader->SetTexture("s_shadowMap" + idx,
					(uint8_t)(kShadowMapSlot0 + c),
					s_Data.ShadowFramebuffers[c]->GetDepthAttachment(),
					BGFX_SAMPLER_UVW_CLAMP);
			}
		}

		shader->SetInt("u_UseIBL", s_Data.UseIBL ? 1 : 0);
		shader->SetInt("u_IrradianceMap", (int)kIrradianceSlot);
		shader->SetInt("u_PrefilterMap", (int)kPrefilterSlot);
		shader->SetInt("u_BRDFLUT", (int)kBRDFLutSlot);
		shader->SetFloat("u_MaxReflectionLod",
			s_Data.ActiveEnvironment ? s_Data.ActiveEnvironment->GetMaxReflectionLod() : 0.0f);
	}

	void Renderer3D::EndScene()
	{
		GE_PROFILE_FUNCTION();

		// Push all shared frame data to the GPU. The old LightsUBO was already
		// vec4-shaped, so its members map straight onto vec4 uniforms and the
		// GPULight array uploads as one flat vec4[] - no repacking needed.
		FrameUniforms::SetDirectionalLight(s_Data.LightBuffer.DirLightDirection,
			s_Data.LightBuffer.DirLightColor);
		FrameUniforms::SetAmbient(s_Data.LightBuffer.AmbientSky,
			s_Data.LightBuffer.AmbientGround);
		FrameUniforms::SetLights(
			reinterpret_cast<const glm::vec4*>(s_Data.LightBuffer.Lights),
			(uint32_t)s_Data.LightBuffer.Counts.x,
			kMaxLights);

		// Partition the draw list: shadow casters skip camera culling (an off-screen
		// mesh still casts a visible shadow); the color passes are frustum-culled.
		std::vector<const DrawCommand*> shadowCasters;
		std::vector<const DrawCommand*> opaque;
		std::vector<const DrawCommand*> transparent;
		shadowCasters.reserve(s_Data.DrawList.size());
		opaque.reserve(s_Data.DrawList.size());

		for (const DrawCommand& cmd : s_Data.DrawList)
		{
			bool isTransparent = cmd.Material && cmd.Material->IsTransparent();
			if (!isTransparent)
				shadowCasters.push_back(&cmd);

			if (!s_Data.CameraFrustum.Intersects(cmd.WorldBounds))
			{
				s_Data.Stats.CulledMeshes++;
				continue;
			}

			if (isTransparent)
			{
				transparent.push_back(&cmd);
				s_Data.Stats.TransparentMeshes++;
			}
			else
			{
				opaque.push_back(&cmd);
			}
		}

		// Shadow depth pass (re-renders the caster list from the light's POV)
		RenderShadowPass(shadowCasters);

		// Opaque: group by material -> mesh -> submesh (instancing batches within a
		// group), front-to-back inside each group for early-z
		std::sort(opaque.begin(), opaque.end(), [](const DrawCommand* a, const DrawCommand* b)
		{
			if (a->Material != b->Material)
				return a->Material < b->Material;
			if (a->Mesh != b->Mesh)
				return a->Mesh < b->Mesh;
			if (a->SubmeshIndex != b->SubmeshIndex)
				return a->SubmeshIndex < b->SubmeshIndex;
			return a->SortKey < b->SortKey;
		});

		RenderCommand::SetDepthTest(true);
		RenderCommand::SetDepthWrite(true);
		RenderCommand::SetBlend(false);

		// Bind IBL textures once for the pass (slots 9..11)
		if (s_Data.UseIBL && s_Data.ActiveEnvironment)
			s_Data.ActiveEnvironment->BindIBL(kIrradianceSlot, kPrefilterSlot, kBRDFLutSlot);

		const Material* boundMaterial = nullptr;
		for (size_t i = 0; i < opaque.size(); )
		{
			size_t end = i + 1;
			while (end < opaque.size()
				&& opaque[end]->Material == opaque[i]->Material
				&& opaque[end]->Mesh == opaque[i]->Mesh
				&& opaque[end]->SubmeshIndex == opaque[i]->SubmeshIndex)
				end++;

			if (opaque[i]->Material && opaque[i]->Material.get() != boundMaterial)
			{
				BindMaterialForColorPass(opaque[i]->Material);
				boundMaterial = opaque[i]->Material.get();
			}

			DrawInstancedGroup(&opaque[i], end - i);
			i = end;
		}

		// Transparent: back-to-front after opaque, depth-tested but not depth-written
		if (!transparent.empty())
		{
			std::sort(transparent.begin(), transparent.end(), [](const DrawCommand* a, const DrawCommand* b)
			{
				return a->SortKey > b->SortKey;
			});

			RenderCommand::SetBlend(true);
			RenderCommand::SetDepthWrite(false);

			for (size_t i = 0; i < transparent.size(); )
			{
				// Only merge neighbours that stayed adjacent after the depth sort
				size_t end = i + 1;
				while (end < transparent.size()
					&& transparent[end]->Material == transparent[i]->Material
					&& transparent[end]->Mesh == transparent[i]->Mesh
					&& transparent[end]->SubmeshIndex == transparent[i]->SubmeshIndex)
					end++;

				if (transparent[i]->Material && transparent[i]->Material.get() != boundMaterial)
				{
					BindMaterialForColorPass(transparent[i]->Material);
					boundMaterial = transparent[i]->Material.get();
				}

				DrawInstancedGroup(&transparent[i], end - i);
				i = end;
			}

			RenderCommand::SetDepthWrite(true);
		}

		// Leave blending on: the 2D renderer, grid, and debug lines rely on it
		RenderCommand::SetBlend(true);
		RenderCommand::SetCullFace(true);
		s_Data.DrawList.clear();

		// Debug line overlay (after opaque, depth-tested)
		if (!s_Data.LineVertices.empty() && s_Data.LineShader && s_Data.LineVertexBuffer)
		{
			uint32_t count = (uint32_t)s_Data.LineVertices.size();
			if (count > s_Data.MaxLineVertices)
				count = s_Data.MaxLineVertices;

			s_Data.LineVertexBuffer->SetData(s_Data.LineVertices.data(), count * sizeof(Renderer3DData::LineVertex));
			s_Data.LineShader->Bind();
			RenderCommand::SetDepthTest(true);
			RenderCommand::SetDepthWrite(false);
			RenderCommand::DrawLines(s_Data.LineGeometry, count);
			RenderCommand::SetDepthWrite(true);
			s_Data.Stats.DrawCalls++;
			s_Data.LineVertices.clear();
		}
	}

	void Renderer3D::DrawSkybox()
	{
		if (!s_Data.DrawSkyboxFlag || !s_Data.FullscreenQuad.IsValid())
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
		if (!s_Data.GridShader || !s_Data.GridGeometry.IsValid())
			return;

		RenderCommand::SetDepthTest(true);
		RenderCommand::SetDepthWrite(false);
		RenderCommand::SetCullFace(false);

		s_Data.GridShader->Bind();
		// Scale grid quad so the shader's world-XZ fade covers a large area
		glm::mat4 transform = glm::scale(glm::mat4(1.0f), glm::vec3(100.0f));
		s_Data.GridShader->SetMat4("u_Transform", transform);
		s_Data.GridShader->SetFloat3("u_CameraPosition", s_Data.CameraBuffer.CameraPosition);

		RenderCommand::DrawIndexed(s_Data.GridGeometry);
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

	void Renderer3D::DrawLine(const glm::vec3& p0, const glm::vec3& p1, const glm::vec4& color)
	{
		if (s_Data.LineVertices.size() + 2 > s_Data.MaxLineVertices)
			return;

		s_Data.LineVertices.push_back({ p0, color });
		s_Data.LineVertices.push_back({ p1, color });
	}

	void Renderer3D::DrawWireBox(const glm::mat4& transform, const glm::vec4& color)
	{
		glm::vec3 corners[8] = {
			{ -0.5f, -0.5f, -0.5f }, {  0.5f, -0.5f, -0.5f },
			{  0.5f,  0.5f, -0.5f }, { -0.5f,  0.5f, -0.5f },
			{ -0.5f, -0.5f,  0.5f }, {  0.5f, -0.5f,  0.5f },
			{  0.5f,  0.5f,  0.5f }, { -0.5f,  0.5f,  0.5f }
		};
		for (int i = 0; i < 8; i++)
			corners[i] = glm::vec3(transform * glm::vec4(corners[i], 1.0f));

		auto edge = [&](int a, int b) { DrawLine(corners[a], corners[b], color); };
		edge(0, 1); edge(1, 2); edge(2, 3); edge(3, 0);
		edge(4, 5); edge(5, 6); edge(6, 7); edge(7, 4);
		edge(0, 4); edge(1, 5); edge(2, 6); edge(3, 7);
	}

	void Renderer3D::DrawWireSphere(const glm::vec3& center, float radius, const glm::vec4& color, int segments)
	{
		segments = glm::max(segments, 8);
		auto ring = [&](const glm::vec3& axisA, const glm::vec3& axisB)
		{
			for (int i = 0; i < segments; i++)
			{
				float t0 = (float)i / (float)segments * glm::two_pi<float>();
				float t1 = (float)(i + 1) / (float)segments * glm::two_pi<float>();
				glm::vec3 p0 = center + (axisA * glm::cos(t0) + axisB * glm::sin(t0)) * radius;
				glm::vec3 p1 = center + (axisA * glm::cos(t1) + axisB * glm::sin(t1)) * radius;
				DrawLine(p0, p1, color);
			}
		};
		ring({ 1, 0, 0 }, { 0, 1, 0 });
		ring({ 1, 0, 0 }, { 0, 0, 1 });
		ring({ 0, 1, 0 }, { 0, 0, 1 });
	}

	void Renderer3D::DrawWireCapsule(const glm::vec3& center, const glm::quat& rotation, float radius, float halfHeight,
		const glm::vec4& color, int segments)
	{
		segments = glm::max(segments, 8);
		glm::vec3 up = rotation * glm::vec3(0.0f, 1.0f, 0.0f);
		glm::vec3 right = rotation * glm::vec3(1.0f, 0.0f, 0.0f);
		glm::vec3 forward = rotation * glm::vec3(0.0f, 0.0f, 1.0f);

		glm::vec3 top = center + up * halfHeight;
		glm::vec3 bottom = center - up * halfHeight;

		for (int i = 0; i < segments; i++)
		{
			float t0 = (float)i / (float)segments * glm::two_pi<float>();
			float t1 = (float)(i + 1) / (float)segments * glm::two_pi<float>();
			glm::vec3 d0 = (right * glm::cos(t0) + forward * glm::sin(t0)) * radius;
			glm::vec3 d1 = (right * glm::cos(t1) + forward * glm::sin(t1)) * radius;
			DrawLine(top + d0, top + d1, color);
			DrawLine(bottom + d0, bottom + d1, color);
			DrawLine(top + d0, bottom + d0, color);
		}

		// Hemisphere arcs in two planes
		for (int i = 0; i < segments / 2; i++)
		{
			float t0 = (float)i / (float)(segments / 2) * glm::half_pi<float>();
			float t1 = (float)(i + 1) / (float)(segments / 2) * glm::half_pi<float>();
			auto arc = [&](const glm::vec3& base, const glm::vec3& a, const glm::vec3& b, float sign)
			{
				glm::vec3 p0 = base + (a * glm::cos(t0) + b * glm::sin(t0) * sign) * radius;
				glm::vec3 p1 = base + (a * glm::cos(t1) + b * glm::sin(t1) * sign) * radius;
				DrawLine(p0, p1, color);
			};
			arc(top, right, up, 1.0f);
			arc(top, forward, up, 1.0f);
			arc(bottom, right, up, -1.0f);
			arc(bottom, forward, up, -1.0f);
		}
	}

}
