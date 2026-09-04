#include "gepch.h"
#include "ParticleSystem.h"

#include "GanymedE/Scene/Entity.h"
#include "GanymedE/Scene/Scene.h"

#include <algorithm>
#include <glm/gtc/constants.hpp>

namespace GanymedE {

	namespace {

		constexpr float kGravity = 9.81f;

		glm::mat3 OrthonormalRotation(const glm::mat4& world)
		{
			glm::vec3 x = glm::vec3(world[0]);
			glm::vec3 y = glm::vec3(world[1]);
			glm::vec3 z = glm::vec3(world[2]);
			const float xl = glm::length(x);
			const float yl = glm::length(y);
			const float zl = glm::length(z);
			x = xl > 0.0f ? x / xl : glm::vec3(1.0f, 0.0f, 0.0f);
			y = yl > 0.0f ? y / yl : glm::vec3(0.0f, 1.0f, 0.0f);
			z = zl > 0.0f ? z / zl : glm::vec3(0.0f, 0.0f, 1.0f);
			return glm::mat3(x, y, z);
		}

		// Uniform in solid angle inside a cone whose axis is local +Y.
		// Consumes two Float01 draws (u, v) — part of the spawn draw-order contract.
		glm::vec3 DirectionInCone(Random& rng, float coneAngleDegrees)
		{
			const float coneRad = glm::radians(glm::clamp(coneAngleDegrees, 0.0f, 180.0f));
			const float u = rng.Float01();
			const float v = rng.Float01();
			const float cosMax = glm::cos(coneRad);
			const float cosTheta = glm::mix(cosMax, 1.0f, u);
			const float sinTheta = glm::sqrt(glm::max(0.0f, 1.0f - cosTheta * cosTheta));
			const float phi = v * glm::two_pi<float>();
			return { sinTheta * glm::cos(phi), cosTheta, sinTheta * glm::sin(phi) };
		}

		void RebuildBounds(ParticleEmitterComponent& emitter, const glm::mat4& world)
		{
			float maxMul = 0.0f;
			for (const FloatKey& key : emitter.SizeCurve.Keys())
				maxMul = glm::max(maxMul, key.Value);
			const float maxStart = glm::max(emitter.StartSizeMin, emitter.StartSizeMax);
			const float maxSize = maxStart * maxMul;

			const float maxScale = glm::max(glm::length(glm::vec3(world[0])),
				glm::max(glm::length(glm::vec3(world[1])), glm::length(glm::vec3(world[2]))));
			const float margin = emitter.WorldSpace ? maxSize : maxSize * maxScale;

			const glm::vec3 origin = glm::vec3(world[3]);
			if (emitter.Pool.empty())
			{
				emitter.WorldBounds = AABB(origin - glm::vec3(margin), origin + glm::vec3(margin));
				return;
			}

			auto toWorld = [&](const glm::vec3& p) {
				return emitter.WorldSpace ? p : glm::vec3(world * glm::vec4(p, 1.0f));
			};

			glm::vec3 first = toWorld(emitter.Pool.front().Position);
			AABB bounds(first, first);
			for (size_t i = 1; i < emitter.Pool.size(); i++)
				bounds.Grow(toWorld(emitter.Pool[i].Position));

			bounds.Min -= glm::vec3(margin);
			bounds.Max += glm::vec3(margin);
			emitter.WorldBounds = bounds;
		}

	}

	void ParticleSystem::OnUpdate(Timestep ts)
	{
		Tick(ts);
	}

	void ParticleSystem::OnUpdateEditor(Timestep ts)
	{
		Tick(ts);
	}

	void ParticleSystem::Tick(Timestep ts)
	{
		const float dt = ts.GetSeconds();
		if (dt <= 0.0f)
			return;

		for (auto [entity, emitter, worldTransform] : View<EmitterView>())
			TickEmitter(entity, emitter, worldTransform.World, dt);
	}

	void ParticleSystem::TickEmitter(entt::entity entity, ParticleEmitterComponent& emitter,
		const glm::mat4& world, float dt)
	{
		// Sub-step order is the determinism contract. Age/retire before integrate before
		// spawn: a particle born this tick is not aged or moved until the next, and
		// retirement cannot see this tick's newborns.

		const bool wasPlaying = emitter.Playing;
		if (!emitter.Playing && emitter.PlayOnStart && emitter.IsFresh())
			emitter.Playing = true;

		// Seed only from a fresh start. A Stop→Play resume must keep the stream; reseeding
		// on every Playing rising edge would reshuffle a mid-effect preview. Inspector
		// Restart seeds itself — this tick already saw Playing==true after ResetRuntime.
		if (emitter.Playing && !wasPlaying && emitter.IsFresh())
			emitter.SeedRng(Entity{ entity, &m_Scene }.GetUUID());

		if (!emitter.Playing)
		{
			RebuildBounds(emitter, world);
			return;
		}

		emitter.Time += dt;

		for (Particle& p : emitter.Pool)
			p.Age += dt;

		// Stable compaction: remaining particles keep their relative order. Swap-erase
		// would reorder the pool by retirement pattern and break exact replay.
		auto retired = std::remove_if(emitter.Pool.begin(), emitter.Pool.end(),
			[](const Particle& p) { return p.Age >= p.Lifetime; });
		emitter.Pool.erase(retired, emitter.Pool.end());

		const glm::mat3 rotation = OrthonormalRotation(world);
		const glm::vec3 gravityWorld(0.0f, -kGravity, 0.0f);
		const glm::vec3 gravity = emitter.WorldSpace
			? gravityWorld
			: glm::transpose(rotation) * gravityWorld;
		const glm::vec3 gravityAccel = gravity * emitter.GravityModifier;

		for (Particle& p : emitter.Pool)
		{
			p.Velocity += gravityAccel * dt;
			p.Position += p.Velocity * dt;
			p.Rotation += p.RotationSpeed * dt;
		}

		const bool emitting = emitter.Looping || emitter.Time <= emitter.Duration;
		if (emitting && emitter.Pool.size() < emitter.MaxParticles)
			emitter.EmitAccumulator += emitter.RateOverTime * dt;

		while (emitter.EmitAccumulator >= 1.0f && emitter.Pool.size() < emitter.MaxParticles)
		{
			emitter.EmitAccumulator -= 1.0f;

			// Draw order is the determinism contract. Reordering these silently changes
			// every effect that uses this seed.
			Particle p;
			p.Lifetime = emitter.Rng.Range(emitter.LifetimeMin, emitter.LifetimeMax);
			const float speed = emitter.Rng.Range(emitter.SpeedMin, emitter.SpeedMax);
			const glm::vec3 localDir = DirectionInCone(emitter.Rng, emitter.ConeAngle);
			p.StartSize = emitter.Rng.Range(emitter.StartSizeMin, emitter.StartSizeMax);
			p.Rotation = emitter.Rng.Range(emitter.StartRotationMin, emitter.StartRotationMax);
			p.RotationSpeed = emitter.Rng.Range(emitter.RotationSpeedMin, emitter.RotationSpeedMax);
			p.Age = 0.0f;

			if (emitter.WorldSpace)
			{
				p.Position = glm::vec3(world[3]);
				p.Velocity = (rotation * localDir) * speed;
			}
			else
			{
				p.Position = glm::vec3(0.0f);
				p.Velocity = localDir * speed;
			}

			emitter.Pool.push_back(p);
		}

		if (emitter.Pool.size() >= emitter.MaxParticles)
			emitter.EmitAccumulator = glm::min(emitter.EmitAccumulator, 0.999f);

		RebuildBounds(emitter, world);
	}

}
