#include "gepch.h"
#include "ParticleRenderer.h"

#include "Buffer.h"
#include "FrameUniforms.h"
#include "RenderCommand.h"
#include "Renderer3D.h"
#include "Shader.h"
#include "Texture.h"

#include "GanymedE/Assets/AssetManager.h"
#include "GanymedE/Assets/AssetTypes.h"

#include <bgfx/bgfx.h>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace GanymedE {

	namespace {

		struct ParticleVertex
		{
			float Position[3];
			float Color[4];
			float TexCoord[2];
			float EntityID;
		};

		static_assert(sizeof(ParticleVertex) == 40, "layout must match BufferLayout stride");

		struct PendingEmitter
		{
			const ParticleEmitterComponent* Emitter = nullptr;
			glm::mat4 World{ 1.0f };
			int EntityID = -1;
			glm::vec3 Origin{ 0.0f };
			float SortKey = 0.0f;
		};

		struct ParticleRendererData
		{
			Ref<Shader> Shader;
			Ref<Texture2D> WhiteTexture;
			bgfx::VertexLayout Layout;

			glm::vec3 CameraPosition{ 0.0f };
			glm::vec3 CameraRight{ 1.0f, 0.0f, 0.0f };
			glm::vec3 CameraUp{ 0.0f, 1.0f, 0.0f };

			std::vector<PendingEmitter> Pending;
			std::vector<uint32_t> ParticleOrder;
			std::vector<ParticleVertex> Vertices;
			std::vector<uint16_t> Indices;

			bool TruncationWarned = false;
			uint32_t DebugMaxQuads = std::numeric_limits<uint32_t>::max();
		};

		ParticleRendererData s_Data;

		glm::vec3 WorldPosition(const ParticleEmitterComponent& emitter, const glm::mat4& world,
			const glm::vec3& local)
		{
			return emitter.WorldSpace ? local : glm::vec3(world * glm::vec4(local, 1.0f));
		}

		void WriteCorner(ParticleVertex& v, const glm::vec3& p, const glm::vec4& color,
			float u, float vCoord, float entityID)
		{
			v.Position[0] = p.x;
			v.Position[1] = p.y;
			v.Position[2] = p.z;
			v.Color[0] = color.r;
			v.Color[1] = color.g;
			v.Color[2] = color.b;
			v.Color[3] = color.a;
			v.TexCoord[0] = u;
			v.TexCoord[1] = vCoord;
			v.EntityID = entityID;
		}

		struct RenderStateGuard
		{
			RenderState Saved;
			RenderStateGuard() : Saved(RenderCommand::GetState()) {}
			~RenderStateGuard() { RenderCommand::GetState() = Saved; }
		};

	}

	void ParticleRenderer::Init()
	{
		s_Data.Shader = Shader::Create("assets/shaders/Particle.glsl");

		s_Data.WhiteTexture = Texture2D::Create(1, 1);
		uint32_t white = 0xffffffff;
		s_Data.WhiteTexture->SetData(&white, sizeof(white));

		const BufferLayout layout = {
			{ ShaderDataType::Float3, "a_Position" },
			{ ShaderDataType::Float4, "a_Color" },
			{ ShaderDataType::Float2, "a_TexCoord" },
			{ ShaderDataType::Int, "a_EntityID" }
		};
		s_Data.Layout = layout.ToBgfx();

		s_Data.Pending.reserve(32);
		s_Data.TruncationWarned = false;
		s_Data.DebugMaxQuads = std::numeric_limits<uint32_t>::max();
	}

	void ParticleRenderer::Shutdown()
	{
		s_Data.Pending.clear();
		s_Data.ParticleOrder.clear();
		s_Data.Vertices.clear();
		s_Data.Indices.clear();
		s_Data.Shader = nullptr;
		s_Data.WhiteTexture = nullptr;
	}

	void ParticleRenderer::SetView(const glm::vec3& position, const glm::vec3& right, const glm::vec3& up)
	{
		s_Data.CameraPosition = position;
		const float rl = glm::length(right);
		const float ul = glm::length(up);
		s_Data.CameraRight = rl > 0.0f ? right / rl : glm::vec3(1.0f, 0.0f, 0.0f);
		s_Data.CameraUp = ul > 0.0f ? up / ul : glm::vec3(0.0f, 1.0f, 0.0f);
	}

	void ParticleRenderer::Submit(int entityID, const ParticleEmitterComponent& emitter, const glm::mat4& world)
	{
		if (emitter.RenderMode != ParticleEmitterComponent::Mode::Billboard)
			return;

		if (!Renderer3D::FrustumIntersects(emitter.WorldBounds))
		{
			Renderer3D::AddCulledParticleEmitter();
			return;
		}

		if (emitter.Pool.empty())
			return;

		PendingEmitter pending;
		pending.Emitter = &emitter;
		pending.World = world;
		pending.EntityID = entityID;
		pending.Origin = glm::vec3(world[3]);
		const glm::vec3 delta = pending.Origin - s_Data.CameraPosition;
		pending.SortKey = glm::dot(delta, delta);
		s_Data.Pending.push_back(pending);
	}

	void ParticleRenderer::DebugLimitTransientQuads(uint32_t maxQuads)
	{
		s_Data.DebugMaxQuads = maxQuads;
	}

	void ParticleRenderer::Flush()
	{
		if (s_Data.Pending.empty())
		{
			s_Data.DebugMaxQuads = std::numeric_limits<uint32_t>::max();
			return;
		}

		// Capture before any mutation so Additive cannot leak into grid/2D/next frame,
		// including the transient-buffer early-out path.
		RenderStateGuard restore;

		if (!s_Data.Shader || !s_Data.Shader->IsValid())
		{
			s_Data.Pending.clear();
			s_Data.DebugMaxQuads = std::numeric_limits<uint32_t>::max();
			return;
		}

		std::stable_sort(s_Data.Pending.begin(), s_Data.Pending.end(),
			[](const PendingEmitter& a, const PendingEmitter& b) { return a.SortKey > b.SortKey; });

		RenderCommand::SetDepthTest(true);
		RenderCommand::SetDepthWrite(false);
		RenderCommand::SetBlend(true);
		RenderCommand::SetCullFace(true);

		s_Data.Shader->Bind();

		uint32_t drawnEmitters = 0;
		uint32_t drawnBillboards = 0;
		uint32_t drawCalls = 0;
		bool truncated = false;

		for (const PendingEmitter& pending : s_Data.Pending)
		{
			const ParticleEmitterComponent& emitter = *pending.Emitter;
			const uint32_t poolCount = (uint32_t)emitter.Pool.size();
			if (poolCount == 0)
				continue;

			s_Data.ParticleOrder.resize(poolCount);
			for (uint32_t i = 0; i < poolCount; i++)
				s_Data.ParticleOrder[i] = i;

			// Pool order is the determinism instrument — sort a scratch index array.
			if (emitter.Blend == ParticleBlend::Alpha)
			{
				std::sort(s_Data.ParticleOrder.begin(), s_Data.ParticleOrder.end(),
					[&](uint32_t a, uint32_t b)
					{
						const glm::vec3 pa = WorldPosition(emitter, pending.World, emitter.Pool[a].Position);
						const glm::vec3 pb = WorldPosition(emitter, pending.World, emitter.Pool[b].Position);
						const glm::vec3 da = pa - s_Data.CameraPosition;
						const glm::vec3 db = pb - s_Data.CameraPosition;
						return glm::dot(da, da) > glm::dot(db, db);
					});
			}

			uint32_t availVerts = bgfx::getAvailTransientVertexBuffer(poolCount * 4, s_Data.Layout);
			uint32_t availIndices = bgfx::getAvailTransientIndexBuffer(poolCount * 6);
			uint32_t maxQuads = std::min(availVerts / 4, availIndices / 6);
			maxQuads = std::min(maxQuads, s_Data.DebugMaxQuads);
			maxQuads = std::min(maxQuads, 16383u); // 16-bit indices, 4 verts per quad
			maxQuads = std::min(maxQuads, poolCount);

			if (maxQuads < poolCount)
				truncated = true;

			if (maxQuads == 0)
			{
				truncated = true;
				break;
			}

			const uint32_t vertCount = maxQuads * 4;
			const uint32_t indexCount = maxQuads * 6;

			bgfx::TransientVertexBuffer tvb;
			bgfx::TransientIndexBuffer tib;
			bgfx::allocTransientVertexBuffer(&tvb, vertCount, s_Data.Layout);
			bgfx::allocTransientIndexBuffer(&tib, indexCount);

			s_Data.Vertices.resize(vertCount);
			s_Data.Indices.resize(indexCount);

			const float entityID = (float)pending.EntityID;
			const glm::vec3& camRight = s_Data.CameraRight;
			const glm::vec3& camUp = s_Data.CameraUp;

			for (uint32_t q = 0; q < maxQuads; q++)
			{
				const Particle& p = emitter.Pool[s_Data.ParticleOrder[q]];
				const float life = p.Lifetime > 0.0f ? glm::clamp(p.Age / p.Lifetime, 0.0f, 1.0f) : 1.0f;
				const float size = p.StartSize * emitter.SizeCurve.Sample(life);
				const glm::vec4 color = emitter.ColorOverLifetime.Sample(life);
				const glm::vec3 center = WorldPosition(emitter, pending.World, p.Position);

				const float rad = glm::radians(p.Rotation);
				const float c = glm::cos(rad);
				const float s = glm::sin(rad);
				const glm::vec3 right = (camRight * c + camUp * s) * (size * 0.5f);
				const glm::vec3 up = (-camRight * s + camUp * c) * (size * 0.5f);

				ParticleVertex* v = &s_Data.Vertices[q * 4];
				WriteCorner(v[0], center - right - up, color, 0.0f, 0.0f, entityID);
				WriteCorner(v[1], center + right - up, color, 1.0f, 0.0f, entityID);
				WriteCorner(v[2], center + right + up, color, 1.0f, 1.0f, entityID);
				WriteCorner(v[3], center - right + up, color, 0.0f, 1.0f, entityID);

				const uint16_t base = (uint16_t)(q * 4);
				uint16_t* idx = &s_Data.Indices[q * 6];
				idx[0] = base + 0;
				idx[1] = base + 1;
				idx[2] = base + 2;
				idx[3] = base + 2;
				idx[4] = base + 3;
				idx[5] = base + 0;
			}

			memcpy(tvb.data, s_Data.Vertices.data(), vertCount * sizeof(ParticleVertex));
			memcpy(tib.data, s_Data.Indices.data(), indexCount * sizeof(uint16_t));

			// Unset, or a texture that failed to load, falls back to white - the emitter's
			// vertex colour then carries the whole look.
			const Ref<Texture2D>& assigned = emitter.Texture.Get();
			const Ref<Texture2D>& texture = assigned ? assigned : s_Data.WhiteTexture;

			s_Data.Shader->SetTexture("s_tex0", 0, texture);

			RenderCommand::SetBlendMode(emitter.Blend == ParticleBlend::Additive
				? RenderState::BlendMode::Additive
				: RenderState::BlendMode::Alpha);

			bgfx::setVertexBuffer(0, &tvb);
			bgfx::setIndexBuffer(&tib);
			bgfx::setState(RenderCommand::GetState().ToBgfx());
			FrameUniforms::Apply();
			bgfx::submit(RenderCommand::GetViewId(), s_Data.Shader->GetProgram());

			drawnEmitters++;
			drawnBillboards += maxQuads;
			drawCalls++;

			if (maxQuads < poolCount)
				break;
		}

		if (truncated && !s_Data.TruncationWarned)
		{
			s_Data.TruncationWarned = true;
			GE_CORE_WARN("ParticleRenderer: transient buffer exhausted — dropping billboards. "
				"Stats.ParticleBillboards is the count that actually drew.");
		}

		Renderer3D::AddParticleStats(drawnEmitters, drawnBillboards, drawCalls);

		s_Data.Pending.clear();
		s_Data.DebugMaxQuads = std::numeric_limits<uint32_t>::max();
	}

}
