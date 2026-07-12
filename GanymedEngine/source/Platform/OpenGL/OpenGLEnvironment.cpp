#include "gepch.h"
#include "OpenGLEnvironment.h"

#include "GanymedE/Renderer/Shader.h"
#include "GanymedE/Renderer/VertexArray.h"
#include "GanymedE/Renderer/Buffer.h"
#include "GanymedE/Renderer/RenderCommand.h"

#include <stb_image.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

namespace GanymedE {

	namespace {

		Ref<VertexArray> CreateCubeVAO()
		{
			float vertices[] = {
				-1.0f, -1.0f, -1.0f,
				 1.0f, -1.0f, -1.0f,
				 1.0f,  1.0f, -1.0f,
				-1.0f,  1.0f, -1.0f,
				-1.0f, -1.0f,  1.0f,
				 1.0f, -1.0f,  1.0f,
				 1.0f,  1.0f,  1.0f,
				-1.0f,  1.0f,  1.0f
			};
			uint32_t indices[] = {
				0, 1, 2, 2, 3, 0, // -Z
				4, 5, 6, 6, 7, 4, // +Z
				0, 4, 7, 7, 3, 0, // -X
				1, 5, 6, 6, 2, 1, // +X
				0, 1, 5, 5, 4, 0, // -Y
				3, 2, 6, 6, 7, 3  // +Y
			};

			Ref<VertexArray> va = VertexArray::Create();
			Ref<VertexBuffer> vb = VertexBuffer::Create(vertices, sizeof(vertices));
			vb->SetLayout({ { ShaderDataType::Float3, "a_Position" } });
			va->AddVertexBuffer(vb);
			Ref<IndexBuffer> ib = IndexBuffer::Create(indices, 36);
			va->SetIndexBuffer(ib);
			return va;
		}

		Ref<VertexArray> CreateQuadVAO()
		{
			float vertices[] = {
				-1.0f, -1.0f,
				 1.0f, -1.0f,
				 1.0f,  1.0f,
				-1.0f,  1.0f
			};
			uint32_t indices[] = { 0, 1, 2, 2, 3, 0 };

			Ref<VertexArray> va = VertexArray::Create();
			Ref<VertexBuffer> vb = VertexBuffer::Create(vertices, sizeof(vertices));
			vb->SetLayout({ { ShaderDataType::Float2, "a_Position" } });
			va->AddVertexBuffer(vb);
			Ref<IndexBuffer> ib = IndexBuffer::Create(indices, 6);
			va->SetIndexBuffer(ib);
			return va;
		}

		GLuint CreateCubemap(uint32_t size, bool mips)
		{
			GLuint id;
			glGenTextures(1, &id);
			glBindTexture(GL_TEXTURE_CUBE_MAP, id);
			for (uint32_t i = 0; i < 6; i++)
				glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, GL_RGB16F, size, size, 0, GL_RGB, GL_FLOAT, nullptr);

			glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, mips ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
			glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

			if (mips)
				glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
			return id;
		}

	}

	OpenGLEnvironment::OpenGLEnvironment(const std::string& filepath)
		: m_Filepath(filepath)
	{
		Bake();
	}

	OpenGLEnvironment::~OpenGLEnvironment()
	{
		if (m_EnvCubemap) glDeleteTextures(1, &m_EnvCubemap);
		if (m_IrradianceMap) glDeleteTextures(1, &m_IrradianceMap);
		if (m_PrefilterMap) glDeleteTextures(1, &m_PrefilterMap);
		if (m_BRDFLUT) glDeleteTextures(1, &m_BRDFLUT);
	}

	void OpenGLEnvironment::Bake()
	{
		GE_PROFILE_FUNCTION();

		// --- Load the equirectangular HDR ---
		stbi_set_flip_vertically_on_load(1);
		int width = 0, height = 0, channels = 0;
		float* data = stbi_loadf(m_Filepath.c_str(), &width, &height, &channels, 3);
		if (!data)
		{
			GE_CORE_ERROR("Failed to load HDR environment '{0}'", m_Filepath);
			return;
		}

		GLuint hdrTexture;
		glGenTextures(1, &hdrTexture);
		glBindTexture(GL_TEXTURE_2D, hdrTexture);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16F, width, height, 0, GL_RGB, GL_FLOAT, data);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		stbi_image_free(data);

		// --- GL setup for the offscreen capture passes ---
		GLint previousViewport[4];
		glGetIntegerv(GL_VIEWPORT, previousViewport);

		GLuint captureFBO, captureRBO;
		glGenFramebuffers(1, &captureFBO);
		glGenRenderbuffers(1, &captureRBO);

		glm::mat4 captureProjection = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 10.0f);
		glm::mat4 captureViews[6] = {
			glm::lookAt(glm::vec3(0.0f), glm::vec3( 1.0f,  0.0f,  0.0f), glm::vec3(0.0f, -1.0f,  0.0f)),
			glm::lookAt(glm::vec3(0.0f), glm::vec3(-1.0f,  0.0f,  0.0f), glm::vec3(0.0f, -1.0f,  0.0f)),
			glm::lookAt(glm::vec3(0.0f), glm::vec3( 0.0f,  1.0f,  0.0f), glm::vec3(0.0f,  0.0f,  1.0f)),
			glm::lookAt(glm::vec3(0.0f), glm::vec3( 0.0f, -1.0f,  0.0f), glm::vec3(0.0f,  0.0f, -1.0f)),
			glm::lookAt(glm::vec3(0.0f), glm::vec3( 0.0f,  0.0f,  1.0f), glm::vec3(0.0f, -1.0f,  0.0f)),
			glm::lookAt(glm::vec3(0.0f), glm::vec3( 0.0f,  0.0f, -1.0f), glm::vec3(0.0f, -1.0f,  0.0f))
		};

		Ref<VertexArray> cube = CreateCubeVAO();
		Ref<VertexArray> quad = CreateQuadVAO();

		glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS); // smooth seams for irradiance/prefilter sampling
		glDisable(GL_CULL_FACE);
		glEnable(GL_DEPTH_TEST);
		glDepthFunc(GL_LEQUAL);

		const uint32_t envSize = 512;
		m_EnvCubemap = CreateCubemap(envSize, true);

		// --- 1. Equirectangular -> cubemap ---
		{
			Ref<Shader> shader = Shader::Create("assets/shaders/Equirect.glsl");
			shader->Bind();
			shader->SetInt("u_EquirectangularMap", 0);
			shader->SetMat4("u_Projection", captureProjection);
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, hdrTexture);

			glBindFramebuffer(GL_FRAMEBUFFER, captureFBO);
			glBindRenderbuffer(GL_RENDERBUFFER, captureRBO);
			glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, envSize, envSize);
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, captureRBO);
			glViewport(0, 0, envSize, envSize);

			for (uint32_t i = 0; i < 6; i++)
			{
				shader->SetMat4("u_View", captureViews[i]);
				glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, m_EnvCubemap, 0);
				glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
				RenderCommand::DrawIndexed(cube);
			}

			glBindTexture(GL_TEXTURE_CUBE_MAP, m_EnvCubemap);
			glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
		}

		// --- 2. Diffuse irradiance convolution ---
		{
			const uint32_t irrSize = 32;
			m_IrradianceMap = CreateCubemap(irrSize, false);

			Ref<Shader> shader = Shader::Create("assets/shaders/Irradiance.glsl");
			shader->Bind();
			shader->SetInt("u_EnvironmentMap", 0);
			shader->SetMat4("u_Projection", captureProjection);
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_CUBE_MAP, m_EnvCubemap);

			glBindFramebuffer(GL_FRAMEBUFFER, captureFBO);
			glBindRenderbuffer(GL_RENDERBUFFER, captureRBO);
			glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, irrSize, irrSize);
			glViewport(0, 0, irrSize, irrSize);

			for (uint32_t i = 0; i < 6; i++)
			{
				shader->SetMat4("u_View", captureViews[i]);
				glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, m_IrradianceMap, 0);
				glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
				RenderCommand::DrawIndexed(cube);
			}
		}

		// --- 3. Pre-filtered specular environment (mip chain by roughness) ---
		{
			const uint32_t preSize = 128;
			m_PrefilterMap = CreateCubemap(preSize, true);
			m_PrefilterMipLevels = 5;

			Ref<Shader> shader = Shader::Create("assets/shaders/Prefilter.glsl");
			shader->Bind();
			shader->SetInt("u_EnvironmentMap", 0);
			shader->SetMat4("u_Projection", captureProjection);
			shader->SetFloat("u_Resolution", (float)envSize);
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_CUBE_MAP, m_EnvCubemap);

			glBindFramebuffer(GL_FRAMEBUFFER, captureFBO);

			for (uint32_t mip = 0; mip < m_PrefilterMipLevels; mip++)
			{
				uint32_t mipSize = (uint32_t)(preSize * std::pow(0.5f, (float)mip));
				glBindRenderbuffer(GL_RENDERBUFFER, captureRBO);
				glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, mipSize, mipSize);
				glViewport(0, 0, mipSize, mipSize);

				float roughness = (float)mip / (float)(m_PrefilterMipLevels - 1);
				shader->SetFloat("u_Roughness", roughness);

				for (uint32_t i = 0; i < 6; i++)
				{
					shader->SetMat4("u_View", captureViews[i]);
					glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, m_PrefilterMap, mip);
					glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
					RenderCommand::DrawIndexed(cube);
				}
			}
		}

		// --- 4. BRDF integration LUT (2D) ---
		{
			const uint32_t lutSize = 512;
			glGenTextures(1, &m_BRDFLUT);
			glBindTexture(GL_TEXTURE_2D, m_BRDFLUT);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RG16F, lutSize, lutSize, 0, GL_RG, GL_FLOAT, nullptr);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

			glBindFramebuffer(GL_FRAMEBUFFER, captureFBO);
			glBindRenderbuffer(GL_RENDERBUFFER, captureRBO);
			glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, lutSize, lutSize);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_BRDFLUT, 0);
			glViewport(0, 0, lutSize, lutSize);

			Ref<Shader> shader = Shader::Create("assets/shaders/BRDFLut.glsl");
			shader->Bind();
			glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
			RenderCommand::DrawIndexed(quad);
		}

		// --- Restore state ---
		glDeleteTextures(1, &hdrTexture);
		glDeleteFramebuffers(1, &captureFBO);
		glDeleteRenderbuffers(1, &captureRBO);

		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glViewport(previousViewport[0], previousViewport[1], previousViewport[2], previousViewport[3]);
		glEnable(GL_CULL_FACE);
		glEnable(GL_DEPTH_TEST);
		glDepthFunc(GL_LESS);

		m_Valid = true;
		GE_CORE_INFO("Baked IBL environment from '{0}'", m_Filepath);
	}

	void OpenGLEnvironment::BindIBL(uint32_t irradianceSlot, uint32_t prefilterSlot, uint32_t brdfLutSlot) const
	{
		glActiveTexture(GL_TEXTURE0 + irradianceSlot);
		glBindTexture(GL_TEXTURE_CUBE_MAP, m_IrradianceMap);
		glActiveTexture(GL_TEXTURE0 + prefilterSlot);
		glBindTexture(GL_TEXTURE_CUBE_MAP, m_PrefilterMap);
		glActiveTexture(GL_TEXTURE0 + brdfLutSlot);
		glBindTexture(GL_TEXTURE_2D, m_BRDFLUT);
	}

	void OpenGLEnvironment::BindSkybox(uint32_t slot) const
	{
		glActiveTexture(GL_TEXTURE0 + slot);
		glBindTexture(GL_TEXTURE_CUBE_MAP, m_EnvCubemap);
	}

}
