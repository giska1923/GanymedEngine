#include "gepch.h"
#include "GanymedE/Renderer/FrameUniforms.h"

#include <bgfx/bgfx.h>

#include <vector>

namespace GanymedE {

	namespace FrameUniforms {

		namespace {

			bool s_Initialized = false;

			bgfx::UniformHandle s_CameraPosition = BGFX_INVALID_HANDLE;
			bgfx::UniformHandle s_DirLightDirection = BGFX_INVALID_HANDLE;
			bgfx::UniformHandle s_DirLightColor = BGFX_INVALID_HANDLE;
			bgfx::UniformHandle s_AmbientSky = BGFX_INVALID_HANDLE;
			bgfx::UniformHandle s_AmbientGround = BGFX_INVALID_HANDLE;
			bgfx::UniformHandle s_LightCounts = BGFX_INVALID_HANDLE;
			bgfx::UniformHandle s_Lights = BGFX_INVALID_HANDLE;

			// CPU-side mirror of the old uniform blocks. Uniforms are per-draw, so
			// these are the values Apply() replays before every submit.
			glm::vec4 s_CameraPositionValue{ 0.0f, 0.0f, 0.0f, 1.0f };
			glm::vec4 s_DirLightDirectionValue{ 0.0f, -1.0f, 0.0f, 0.0f };
			glm::vec4 s_DirLightColorValue{ 1.0f, 1.0f, 1.0f, 0.0f };
			glm::vec4 s_AmbientSkyValue{ 0.0f };
			glm::vec4 s_AmbientGroundValue{ 0.0f };
			glm::vec4 s_LightCountsValue{ 0.0f };

			std::vector<glm::vec4> s_LightsValue;

			void Destroy(bgfx::UniformHandle& handle)
			{
				if (bgfx::isValid(handle))
					bgfx::destroy(handle);
				handle = BGFX_INVALID_HANDLE;
			}

		}

		void Init()
		{
			if (s_Initialized)
				return;

			s_CameraPosition    = bgfx::createUniform("u_CameraPosition",    bgfx::UniformType::Vec4);
			s_DirLightDirection = bgfx::createUniform("u_DirLightDirection", bgfx::UniformType::Vec4);
			s_DirLightColor     = bgfx::createUniform("u_DirLightColor",     bgfx::UniformType::Vec4);
			s_AmbientSky        = bgfx::createUniform("u_AmbientSky",        bgfx::UniformType::Vec4);
			s_AmbientGround     = bgfx::createUniform("u_AmbientGround",     bgfx::UniformType::Vec4);
			s_LightCounts       = bgfx::createUniform("u_LightCounts",       bgfx::UniformType::Vec4);

			s_Initialized = true;
		}

		void Shutdown()
		{
			Destroy(s_CameraPosition);
			Destroy(s_DirLightDirection);
			Destroy(s_DirLightColor);
			Destroy(s_AmbientSky);
			Destroy(s_AmbientGround);
			Destroy(s_LightCounts);
			Destroy(s_Lights);

			s_LightsValue.clear();
			s_Initialized = false;
		}

		void SetCamera(uint16_t viewId, const glm::mat4& view, const glm::mat4& projection,
			const glm::vec3& cameraPosition)
		{
			// View transforms are view state, not draw state: applied immediately
			// and persisting across every submit to this view.
			bgfx::setViewTransform(viewId, &view[0][0], &projection[0][0]);

			s_CameraPositionValue = glm::vec4(cameraPosition, 1.0f);
		}

		void SetDirectionalLight(const glm::vec4& direction, const glm::vec4& color)
		{
			s_DirLightDirectionValue = direction;
			s_DirLightColorValue = color;
		}

		void SetAmbient(const glm::vec4& sky, const glm::vec4& ground)
		{
			s_AmbientSkyValue = sky;
			s_AmbientGroundValue = ground;
		}

		void SetLights(const glm::vec4* packed, uint32_t count, uint32_t maxLights)
		{
			const uint32_t capacity = maxLights * 4; // four vec4s per GPULight

			// bgfx fixes an array uniform's size at creation, so it is created once
			// at full capacity and always uploaded whole. Counting the live lights
			// is the shader's job, via u_LightCounts.
			if (!bgfx::isValid(s_Lights) || s_LightsValue.size() != capacity)
			{
				Destroy(s_Lights);
				s_Lights = bgfx::createUniform("u_Lights", bgfx::UniformType::Vec4, (uint16_t)capacity);
				s_LightsValue.assign(capacity, glm::vec4(0.0f));
			}

			s_LightCountsValue = glm::vec4((float)count, 0.0f, 0.0f, 0.0f);

			if (packed)
				std::copy(packed, packed + capacity, s_LightsValue.begin());
		}

		void Apply()
		{
			if (!s_Initialized)
				return;

			bgfx::setUniform(s_CameraPosition, &s_CameraPositionValue[0]);
			bgfx::setUniform(s_DirLightDirection, &s_DirLightDirectionValue[0]);
			bgfx::setUniform(s_DirLightColor, &s_DirLightColorValue[0]);
			bgfx::setUniform(s_AmbientSky, &s_AmbientSkyValue[0]);
			bgfx::setUniform(s_AmbientGround, &s_AmbientGroundValue[0]);
			bgfx::setUniform(s_LightCounts, &s_LightCountsValue[0]);

			if (bgfx::isValid(s_Lights) && !s_LightsValue.empty())
				bgfx::setUniform(s_Lights, s_LightsValue.data(), (uint16_t)s_LightsValue.size());
		}

	}

}
