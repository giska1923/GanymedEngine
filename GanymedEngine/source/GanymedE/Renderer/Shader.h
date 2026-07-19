#pragma once

#include <string>
#include <unordered_map>

#include <glm/glm.hpp>
#include <bgfx/bgfx.h>

namespace GanymedE {

	class Texture2D;

	// Concrete wrapper over a bgfx program (a linked vertex + fragment shader).
	//
	// Shaders are no longer compiled at runtime: shaderc produces bytecode
	// offline (scripts/compile_shaders.bat) and this loads the blob matching the
	// active backend. Editing a shader now means re-running that script.
	//
	// Uniforms differ from GL in two ways that leak into this API:
	//   - bgfx uniforms are vec4/mat4 only, so scalars are padded into a vec4.
	//   - "binding a texture to a slot" is a property of the draw call, not of
	//     the shader, so SetTexture records it and the next submit applies it.
	class Shader
	{
	public:
		Shader(const std::string& name);
		~Shader();

		Shader(const Shader&) = delete;
		Shader& operator=(const Shader&) = delete;

		// Makes this the program the next RenderCommand draw submits with.
		void Bind() const;
		void Unbind() const;

		bool IsValid() const { return bgfx::isValid(m_Program); }
		bgfx::ProgramHandle GetProgram() const { return m_Program; }

		// Replaces "texture->Bind(slot); shader->SetInt(name, slot)". Under bgfx
		// a texture binding belongs to the draw call and must name the sampler
		// uniform it feeds, so the texture alone cannot bind itself.
		// Takes effect on the next submit.
		void SetTexture(const std::string& samplerName, uint8_t slot, const Ref<Texture2D>& texture);
		void SetTexture(const std::string& samplerName, uint8_t slot, bgfx::TextureHandle texture, uint32_t samplerFlags);

		// SetInt on a sampler uniform is a no-op under bgfx - use SetTexture.
		// It is kept so existing "SetInt(u_Texture, 0)" call sites stay valid.
		void SetInt(const std::string& name, int value);
		void SetIntArray(const std::string& name, int* values, uint32_t count);
		void SetFloat(const std::string& name, float value);
		void SetFloat2(const std::string& name, const glm::vec2& value);
		void SetFloat3(const std::string& name, const glm::vec3& value);
		void SetFloat4(const std::string& name, const glm::vec4& value);
		void SetMat4(const std::string& name, const glm::mat4& value);

		const std::string& GetName() const { return m_Name; }

		static Ref<Shader> Create(const std::string& filepath);
		static Ref<Shader> Create(const std::string& name, const std::string& vertexSrc, const std::string& fragmentSrc);
	private:
		// Uniform handles are created on first use and live for the program's
		// lifetime; bgfx dedupes by name internally but this avoids the lookup.
		bgfx::UniformHandle GetUniform(const std::string& name, bgfx::UniformType::Enum type, uint16_t num = 1);
	private:
		std::string m_Name;
		bgfx::ProgramHandle m_Program = BGFX_INVALID_HANDLE;
		std::unordered_map<std::string, bgfx::UniformHandle> m_Uniforms;
	};

	class ShaderLibrary
	{
	public:
		void Add(const std::string& name, const Ref<Shader>& shader);
		void Add(const Ref<Shader>& shader);
		Ref<Shader> Load(const std::string& filepath);
		Ref<Shader> Load(const std::string& name, const std::string& filepath);

		Ref<Shader> Get(const std::string & name);

		bool Exists(const std::string& name) const;
	private:
		std::unordered_map<std::string, Ref<Shader>> m_Shaders;
	};
}
