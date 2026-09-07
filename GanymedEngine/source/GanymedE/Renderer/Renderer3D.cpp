#include "gepch.h"
#include "Renderer3D.h"

#include "FrameUniforms.h"
#include "Shader.h"
#include "RenderCommand.h"
#include "RenderPassIDs.h"
#include "Buffer.h"
#include "Framebuffer.h"
#include "Environment.h"
#include "ParticleRenderer.h"
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

		// Skinned draws index into Renderer3DData::PaletteStorage instead of holding
		// a vector: the palette is copied at submit, so nothing here depends on the
		// AnimatorComponent's storage still being alive (or unchanged) at flush time,
		// exactly as instance data is already staged CPU-side.
		uint32_t PaletteOffset = 0;
		bool IsSkinned = false;
	};

	struct Renderer3DData
	{
		CameraUBO CameraBuffer{};
		Frustum CameraFrustum;

		LightsUBO LightBuffer{};

		std::vector<DrawCommand> DrawList;

		// One MaxBones-sized block per skinned submit, identity-padded. bgfx fixes an
		// array uniform's size at creation, so the upload is always the whole array
		// however few joints a rig has - 8 KB per skinned entity per frame.
		std::vector<glm::mat4> PaletteStorage;

		// vs_PhongSkinned + fs_Phong, and vs_ShadowDepthSkinned + fs_ShadowDepth:
		// both reuse the static fragment stage unchanged.
		Ref<Shader> SkinnedShader;
		Ref<Shader> ShadowDepthSkinnedShader;

		Ref<Shader> GridShader;
		Geometry GridGeometry;

		Ref<Shader> SkyboxShader;
		Ref<Shader> SkyboxCubeShader;
		Geometry FullscreenQuad;

		// HDR image-based lighting (optional). Refreshed by SubmitEnvironment each
		// frame; caching by path is AssetManager's job.
		Ref<Environment> ActiveEnvironment;
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

		s_Data.SkinnedShader = Shader::CreateFromStages("PhongSkinned", "PhongSkinned", "Phong");
		s_Data.ShadowDepthSkinnedShader = Shader::CreateFromStages("ShadowDepthSkinned",
			"ShadowDepthSkinned", "ShadowDepth");

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

		ParticleRenderer::Init();
	}

	void Renderer3D::Shutdown()
	{
		ParticleRenderer::Shutdown();

		s_Data.DrawList.clear();
		s_Data.GridGeometry = {};
		s_Data.GridShader = nullptr;
		s_Data.SkyboxShader = nullptr;
		s_Data.SkyboxCubeShader = nullptr;
		s_Data.FullscreenQuad = {};
		s_Data.ShadowDepthShader = nullptr;
		s_Data.PaletteStorage.clear();
		s_Data.SkinnedShader = nullptr;
		s_Data.ShadowDepthSkinnedShader = nullptr;
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
		s_Data.PaletteStorage.clear();
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

	// One place decides which material a submesh draws with, so the static and skinned paths
	// cannot drift apart.
	static Ref<Material> ResolveMaterial(const Ref<Mesh>& mesh, uint32_t materialIndex,
		const Ref<Material>* overrides, uint32_t overrideCount)
	{
		if (overrides && materialIndex < overrideCount && overrides[materialIndex])
			return overrides[materialIndex];

		return mesh->GetMaterial(materialIndex);
	}

	void Renderer3D::SubmitMesh(const Ref<Mesh>& mesh, const glm::mat4& transform, int entityID,
		const Ref<Material>* materialOverrides, uint32_t overrideCount)
	{
		if (!mesh)
			return;

		const auto& submeshes = mesh->GetSubmeshes();
		for (uint32_t i = 0; i < (uint32_t)submeshes.size(); i++)
		{
			Ref<Material> material = ResolveMaterial(mesh, submeshes[i].MaterialIndex,
				materialOverrides, overrideCount);
			SubmitMesh(mesh, i, material, transform, entityID);
		}
	}

	// Returns the pushed command so a skinned submit can attach its palette. Only
	// valid until the next push, which is all the one caller needs.
	static DrawCommand* PushDrawCommand(const Ref<Mesh>& mesh, uint32_t submeshIndex,
		const Ref<Material>& material, const glm::mat4& transform, int entityID)
	{
		if (!mesh || submeshIndex >= mesh->GetSubmeshes().size())
			return nullptr;

		DrawCommand cmd;
		cmd.Mesh = mesh;
		cmd.SubmeshIndex = submeshIndex;
		cmd.Material = material;

		// LocalTransform applies to skinned submeshes too, which reads wrong against
		// glTF's "the skinned mesh node's transform MUST be ignored" until you notice
		// that Skeleton::RootTransform carries the inverse of that same node's world
		// matrix (see BuildSkeleton). The two cancel, and the bounds below ride the
		// same matrix as the vertices. Drop either one alone and a Y-up-corrected
		// character renders on its side.
		cmd.Transform = transform * mesh->GetSubmeshes()[submeshIndex].LocalTransform;
		cmd.EntityID = entityID;
		cmd.WorldBounds = mesh->GetSubmeshes()[submeshIndex].Bounds.Transformed(cmd.Transform);

		glm::vec3 center = (cmd.WorldBounds.Min + cmd.WorldBounds.Max) * 0.5f;
		cmd.SortKey = glm::dot(center - s_Data.CameraBuffer.CameraPosition, center - s_Data.CameraBuffer.CameraPosition);

		s_Data.DrawList.push_back(cmd);
		s_Data.Stats.MeshCount++;
		return &s_Data.DrawList.back();
	}

	void Renderer3D::SubmitMesh(const Ref<Mesh>& mesh, uint32_t submeshIndex, const Ref<Material>& material,
		const glm::mat4& transform, int entityID)
	{
		PushDrawCommand(mesh, submeshIndex, material, transform, entityID);
	}

	// Copies a palette into the frame's storage, identity-padded to MaxBones so the
	// upload can hand bgfx one contiguous array of the fixed size it was created at.
	static uint32_t StagePalette(const glm::mat4* palette, uint32_t jointCount)
	{
		const uint32_t offset = (uint32_t)s_Data.PaletteStorage.size();
		s_Data.PaletteStorage.resize(offset + Skeleton::MaxBones, glm::mat4(1.0f));
		std::copy(palette, palette + jointCount, s_Data.PaletteStorage.begin() + offset);
		return offset;
	}

	void Renderer3D::SubmitSkinnedMesh(const Ref<Mesh>& mesh, const glm::mat4& transform,
		const glm::mat4* palette, uint32_t jointCount, int entityID,
		const Ref<Material>* materialOverrides, uint32_t overrideCount)
	{
		if (!mesh)
			return;

		// Degrade to the static path rather than dropping the entity: a rig whose
		// palette has not been built yet, or whose program failed to compile, should
		// still draw in its bind pose instead of vanishing.
		if (!palette || jointCount == 0 || !mesh->GetSkinVertexBuffer()
			|| !s_Data.SkinnedShader || !s_Data.SkinnedShader->IsValid())
		{
			SubmitMesh(mesh, transform, entityID, materialOverrides, overrideCount);
			return;
		}

		// Import already warns about oversized rigs; clamping here keeps the upload
		// in bounds, and the joints past the limit simply do not deform.
		if (jointCount > Skeleton::MaxBones)
			jointCount = Skeleton::MaxBones;

		// Staged once per entity - every skinned submesh of this mesh shares it.
		const uint32_t paletteOffset = StagePalette(palette, jointCount);

		const auto& submeshes = mesh->GetSubmeshes();
		for (uint32_t i = 0; i < (uint32_t)submeshes.size(); i++)
		{
			Ref<Material> material = ResolveMaterial(mesh, submeshes[i].MaterialIndex,
				materialOverrides, overrideCount);
			DrawCommand* cmd = PushDrawCommand(mesh, i, material, transform, entityID);

			// A file can mix rigged and rigid primitives under one skin; only the
			// rigged ones need the palette, the rest keep instancing.
			if (cmd && submeshes[i].IsSkinned)
			{
				cmd->IsSkinned = true;
				cmd->PaletteOffset = paletteOffset;
			}
		}
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
				s_Instances[i].EntityID = glm::vec4((float)cmds[offset + i]->EntityID);
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

	// One skinned submesh, one draw call. Batching is off the table: the palette is a
	// uniform, not per-instance data, so two characters in different poses cannot
	// share a draw. Instance count stays 1 so the vertex-input convention (i_data0..4)
	// is the same everywhere and the entity ID still reaches the picking attachment.
	//
	// program replaces whatever the material bound. bgfx uniforms are global by name,
	// so the material's values are already queued for this draw and only the program
	// consuming them changes.
	static void DrawSkinnedCommand(const DrawCommand& cmd, const Ref<Shader>& program)
	{
		const Ref<VertexBuffer>& skinStream = cmd.Mesh->GetSkinVertexBuffer();
		if (!skinStream || !program || !program->IsValid())
		{
			// Discard, don't just return: the caller has already queued this draw's
			// uniforms and textures, and bgfx treats setting one of them twice with no
			// submit in between as fatal, not as an overwrite.
			bgfx::discard();
			return;
		}

		program->SetMat4Array("u_Bones", &s_Data.PaletteStorage[cmd.PaletteOffset], Skeleton::MaxBones);
		program->Bind();

		const Submesh& submesh = cmd.Mesh->GetSubmeshes()[cmd.SubmeshIndex];

		MeshInstanceData instance;
		instance.Transform = cmd.Transform;
		instance.EntityID = glm::vec4((float)cmd.EntityID);

		RenderCommand::DrawIndexedInstancedSkinned(cmd.Mesh->GetGeometry(), skinStream,
			submesh.IndexCount, submesh.BaseIndex, (int32_t)submesh.BaseVertex,
			&instance, 1, (uint16_t)sizeof(MeshInstanceData));
		s_Data.Stats.DrawCalls++;
		s_Data.Stats.SkinnedDraws++;
	}

	static void RenderShadowPass(std::vector<const DrawCommand*>& casters)
	{
		if (!s_Data.HasShadowLight || !s_Data.ShadowDepthShader || casters.empty())
			return;

		ComputeCascades(s_Data.ShadowLightDir);

		// Skinned casters leave the instanced list: they need the deforming depth
		// shader. Skipping this would put a bind-pose shadow under a moving
		// character, which reads as a bug even though nothing errored.
		std::vector<const DrawCommand*> skinnedCasters;
		for (const DrawCommand* cmd : casters)
		{
			if (cmd->IsSkinned)
				skinnedCasters.push_back(cmd);
		}
		casters.erase(std::remove_if(casters.begin(), casters.end(),
			[](const DrawCommand* cmd) { return cmd->IsSkinned; }), casters.end());

		// Group instances by mesh + submesh (material is irrelevant for depth)
		std::sort(casters.begin(), casters.end(), [](const DrawCommand* a, const DrawCommand* b)
		{
			if (a->Mesh != b->Mesh)
				return a->Mesh < b->Mesh;
			return a->SubmeshIndex < b->SubmeshIndex;
		});

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

			// Depth-only target: it has no colour attachment, so colour writes
			// must be off. bgfx will not service a draw whose write mask targets
			// attachments the framebuffer does not have.
			RenderCommand::GetState().WriteRGB = false;
			RenderCommand::GetState().WriteAlpha = false;

			// Cull front faces while rendering caster depth to curb acne / peter-panning
			RenderCommand::SetCullFace(true);
			RenderCommand::SetCullMode(RenderState::CullMode::Front);

			// Set once for the whole cascade, covering the skinned draws too: a uniform
			// value persists across submits, but setting it twice with no submit in
			// between is fatal - and with no static casters, that is exactly what a
			// second set for the skinned pass would be.
			s_Data.ShadowDepthShader->SetMat4("u_LightSpaceMatrix", s_Data.CascadeLightSpace[c]);

			// Rebound per cascade because the skinned draws below replace the program.
			s_Data.ShadowDepthShader->Bind();

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

			// Every cascade, not just the one the camera happens to sample: a
			// character casting into cascade 2 while standing in cascade 0 is normal.
			for (const DrawCommand* cmd : skinnedCasters)
				DrawSkinnedCommand(*cmd, s_Data.ShadowDepthSkinnedShader);

			RenderCommand::SetCullMode(RenderState::CullMode::Back);
			RenderCommand::GetState().WriteRGB = true;
			RenderCommand::GetState().WriteAlpha = true;
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

		// IBL textures bind per-material, because a sampler uniform belongs to a
		// shader - there is no global "bind to unit 9" under bgfx.
		if (s_Data.UseIBL && s_Data.ActiveEnvironment && s_Data.ActiveEnvironment->IsValid())
		{
			const Ref<Environment>& env = s_Data.ActiveEnvironment;
			shader->SetTexture("u_IrradianceMap", (uint8_t)kIrradianceSlot, env->GetIrradiance(), BGFX_SAMPLER_UVW_CLAMP);
			shader->SetTexture("u_PrefilterMap",  (uint8_t)kPrefilterSlot,  env->GetPrefilter(),  BGFX_SAMPLER_UVW_CLAMP);
			shader->SetTexture("u_BRDFLUT",       (uint8_t)kBRDFLutSlot,    env->GetBRDFLut(),    BGFX_SAMPLER_UVW_CLAMP);
		}
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

		// RenderShadowPass leaves the current view pointing at the last cascade.
		// Every colour draw below belongs to the scene view - without this they
		// are submitted into the shadow framebuffer and simply never appear.
		RenderCommand::SetViewId(RenderPass::SceneHDR);

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

		const Material* boundMaterial = nullptr;
		for (size_t i = 0; i < opaque.size(); )
		{
			// A skinned command never joins a run - its palette is per-draw uniform
			// state. With no skinned commands present these clauses are always true,
			// so a static scene batches exactly as it did before.
			size_t end = i + 1;
			while (!opaque[i]->IsSkinned
				&& end < opaque.size() && !opaque[end]->IsSkinned
				&& opaque[end]->Material == opaque[i]->Material
				&& opaque[end]->Mesh == opaque[i]->Mesh
				&& opaque[end]->SubmeshIndex == opaque[i]->SubmeshIndex)
				end++;

			// A skinned draw always rebinds: bgfx discards texture bindings at submit,
			// so it cannot ride on a cache entry left by an earlier draw.
			if (opaque[i]->Material && (opaque[i]->IsSkinned || opaque[i]->Material.get() != boundMaterial))
			{
				BindMaterialForColorPass(opaque[i]->Material);
				boundMaterial = opaque[i]->Material.get();
			}

			if (opaque[i]->IsSkinned)
			{
				DrawSkinnedCommand(*opaque[i], s_Data.SkinnedShader);

				// The skinned program is bound now, so the next static draw must not
				// inherit "material already bound" - it would render rigid geometry
				// through a vertex shader reading a stream 1 that is not there.
				boundMaterial = nullptr;
			}
			else
			{
				DrawInstancedGroup(&opaque[i], end - i);
			}
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
				// Only merge neighbours that stayed adjacent after the depth sort, and
				// never merge a skinned command (see the opaque loop).
				size_t end = i + 1;
				while (!transparent[i]->IsSkinned
					&& end < transparent.size() && !transparent[end]->IsSkinned
					&& transparent[end]->Material == transparent[i]->Material
					&& transparent[end]->Mesh == transparent[i]->Mesh
					&& transparent[end]->SubmeshIndex == transparent[i]->SubmeshIndex)
					end++;

				if (transparent[i]->Material
					&& (transparent[i]->IsSkinned || transparent[i]->Material.get() != boundMaterial))
				{
					BindMaterialForColorPass(transparent[i]->Material);
					boundMaterial = transparent[i]->Material.get();
				}

				if (transparent[i]->IsSkinned)
				{
					DrawSkinnedCommand(*transparent[i], s_Data.SkinnedShader);
					boundMaterial = nullptr;
				}
				else
				{
					DrawInstancedGroup(&transparent[i], end - i);
				}
				i = end;
			}

			// Depth-write stays off into the particle flush. Restored after.
		}

		// After transparent meshes, before the depth-write restore. Billboards
		// inherit Sequential view 5, depth-test LESS, depth-write off. Restore
		// of RenderState is ParticleRenderer's job (unconditional, including
		// transient-buffer early-out).
		ParticleRenderer::Flush();

		RenderCommand::SetDepthWrite(true);

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
			s_Data.SkyboxCubeShader->Bind();
			s_Data.SkyboxCubeShader->SetTexture("u_EnvironmentMap", (uint8_t)kSkyboxCubemapSlot,
				s_Data.ActiveEnvironment->GetSkybox(), BGFX_SAMPLER_UVW_CLAMP);
			s_Data.SkyboxCubeShader->SetMat4("u_InverseViewProjection", invViewProj);
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

		// Scale grid quad so the shader's world-XZ fade covers a large area.
		// This goes on bgfx's transform stack rather than a u_Transform uniform,
		// which is what makes the predefined u_model / u_modelViewProj correct.
		glm::mat4 transform = glm::scale(glm::mat4(1.0f), glm::vec3(100.0f));
		bgfx::setTransform(&transform[0][0]);

		// u_CameraPosition deliberately NOT set here: FrameUniforms already
		// supplies it every draw, and bgfx asserts if one uniform is set twice
		// before a submit.

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

	bool Renderer3D::FrustumIntersects(const AABB& bounds)
	{
		return s_Data.CameraFrustum.Intersects(bounds);
	}

	void Renderer3D::AddParticleStats(uint32_t emitters, uint32_t billboards, uint32_t drawCalls)
	{
		s_Data.Stats.ParticleEmitters += emitters;
		s_Data.Stats.ParticleBillboards += billboards;
		s_Data.Stats.ParticleDrawCalls += drawCalls;
		s_Data.Stats.DrawCalls += drawCalls;
	}

	void Renderer3D::AddCulledParticleEmitter()
	{
		s_Data.Stats.ParticleCulledEmitters++;
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
