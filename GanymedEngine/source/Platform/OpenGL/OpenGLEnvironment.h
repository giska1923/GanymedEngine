#pragma once

#include "GanymedE/Renderer/Environment.h"

#include <glad/glad.h>

namespace GanymedE {

	class OpenGLEnvironment : public Environment
	{
	public:
		OpenGLEnvironment(const std::string& filepath);
		virtual ~OpenGLEnvironment();

		virtual void BindIBL(uint32_t irradianceSlot, uint32_t prefilterSlot, uint32_t brdfLutSlot) const override;
		virtual void BindSkybox(uint32_t slot) const override;

		virtual float GetMaxReflectionLod() const override { return (float)(m_PrefilterMipLevels - 1); }
		virtual bool IsValid() const override { return m_Valid; }
		virtual const std::string& GetFilepath() const override { return m_Filepath; }
	private:
		void Bake();
	private:
		std::string m_Filepath;
		bool m_Valid = false;

		GLuint m_EnvCubemap = 0;
		GLuint m_IrradianceMap = 0;
		GLuint m_PrefilterMap = 0;
		GLuint m_BRDFLUT = 0;

		uint32_t m_PrefilterMipLevels = 5;
	};

}
