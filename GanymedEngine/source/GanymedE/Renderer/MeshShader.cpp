#include "gepch.h"
#include "GanymedE/Renderer/MeshShader.h"

#include "GanymedE/Renderer/Shader.h"

namespace GanymedE {

	namespace MeshShader {

		namespace { Ref<Shader> s_Shader; }

		Ref<Shader> Get()
		{
			if (!s_Shader)
				s_Shader = Shader::Create("assets/shaders/Phong.glsl");

			return s_Shader;
		}

		void Release()
		{
			s_Shader = nullptr;
		}

	}

}
