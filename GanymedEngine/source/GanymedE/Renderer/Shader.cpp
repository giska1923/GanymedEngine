#include "gepch.h"
#include "Shader.h"

#include "Renderer.h"
#include "RenderCommand.h"
#include "Texture.h"

#include <filesystem>
#include <fstream>

namespace GanymedE {

	namespace {

		// Must match the profile folders written by scripts/compile_shaders.bat.
		const char* ProfileDirectory()
		{
			switch (bgfx::getRendererType())
			{
				case bgfx::RendererType::Direct3D11:
				case bgfx::RendererType::Direct3D12: return "dx11";
				case bgfx::RendererType::Vulkan:     return "spirv";
				case bgfx::RendererType::OpenGL:
				case bgfx::RendererType::OpenGLES:   return "glsl";
				case bgfx::RendererType::Metal:      return "metal";
				default:                             return nullptr;
			}
		}

		// Call sites still say "assets/shaders/Texture.glsl"; the compiled
		// bytecode is keyed on the bare name, so reduce the path to its stem.
		std::string ShaderNameFromPath(const std::string& filepath)
		{
			return std::filesystem::path(filepath).stem().string();
		}

		// bgfx takes ownership of the memory block and frees it once the shader
		// is created, so this must not be a stack buffer.
		const bgfx::Memory* ReadShaderBlob(const std::string& path)
		{
			std::ifstream file(path, std::ios::binary | std::ios::ate);
			if (!file)
				return nullptr;

			const std::streamsize size = file.tellg();
			if (size <= 0)
				return nullptr;

			file.seekg(0, std::ios::beg);

			const bgfx::Memory* mem = bgfx::alloc((uint32_t)size + 1);
			if (!file.read((char*)mem->data, size))
				return nullptr;

			mem->data[size] = '\0';
			return mem;
		}

		bgfx::ShaderHandle LoadShaderStage(const std::string& directory, const std::string& prefix, const std::string& name)
		{
			const std::string path = directory + "/" + prefix + name + ".bin";

			const bgfx::Memory* mem = ReadShaderBlob(path);
			if (!mem)
			{
				GE_CORE_ERROR("Could not read compiled shader '{0}'", path);
				return BGFX_INVALID_HANDLE;
			}

			bgfx::ShaderHandle handle = bgfx::createShader(mem);
			if (bgfx::isValid(handle))
				bgfx::setName(handle, (prefix + name).c_str());

			return handle;
		}

	}

	Shader::Shader(const std::string& name)
		: m_Name(name)
	{
		const char* profile = ProfileDirectory();
		if (!profile)
		{
			GE_CORE_ERROR("No compiled shader profile for the active bgfx backend");
			return;
		}

		const std::string directory = std::string("assets/shaders/compiled/") + profile;

		bgfx::ShaderHandle vs = LoadShaderStage(directory, "vs_", name);
		bgfx::ShaderHandle fs = LoadShaderStage(directory, "fs_", name);

		if (!bgfx::isValid(vs) || !bgfx::isValid(fs))
		{
			GE_CORE_ERROR("Failed to load shader '{0}' - run scripts/compile_shaders.bat", name);
			if (bgfx::isValid(vs)) bgfx::destroy(vs);
			if (bgfx::isValid(fs)) bgfx::destroy(fs);
			return;
		}

		// destroyShaders = true: the program owns the stages from here on.
		m_Program = bgfx::createProgram(vs, fs, true);

		if (!bgfx::isValid(m_Program))
			GE_CORE_ERROR("Failed to link shader program '{0}'", name);
		else
			GE_CORE_TRACE("Loaded shader '{0}' ({1})", name, profile);
	}

	Shader::~Shader()
	{
		for (auto& entry : m_Uniforms)
		{
			if (bgfx::isValid(entry.second))
				bgfx::destroy(entry.second);
		}

		if (bgfx::isValid(m_Program))
			bgfx::destroy(m_Program);
	}

	void Shader::Bind() const
	{
		// Nothing is bound on the GPU here - bgfx takes the program as an
		// argument to submit, so this just records which one the next draw uses.
		RenderCommand::SetProgram(m_Program);
	}

	void Shader::Unbind() const
	{
		RenderCommand::SetProgram(BGFX_INVALID_HANDLE);
	}

	bgfx::UniformHandle Shader::GetUniform(const std::string& name, bgfx::UniformType::Enum type, uint16_t num)
	{
		auto it = m_Uniforms.find(name);
		if (it != m_Uniforms.end())
			return it->second;

		bgfx::UniformHandle handle = bgfx::createUniform(name.c_str(), type, num);
		m_Uniforms[name] = handle;
		return handle;
	}

	void Shader::SetTexture(const std::string& samplerName, uint8_t slot, const Ref<Texture2D>& texture)
	{
		if (!texture || !texture->IsValid())
			return;

		SetTexture(samplerName, slot, texture->GetHandle(), texture->GetSamplerFlags());
	}

	void Shader::SetTexture(const std::string& samplerName, uint8_t slot, bgfx::TextureHandle texture, uint32_t samplerFlags)
	{
		if (!bgfx::isValid(texture))
			return;

		// The sampler uniform carries no value of its own - it exists so bgfx can
		// associate this texture unit with the sampler declared in the shader.
		bgfx::setTexture(slot, GetUniform(samplerName, bgfx::UniformType::Sampler), texture, samplerFlags);
	}

	void Shader::SetInt(const std::string& name, int value)
	{
		// Sampler uniforms are set through bgfx::setTexture, so the usual
		// "SetInt(u_Texture, slot)" carries no information here. Numeric ints
		// still need to travel, and bgfx has no int uniform: pad into a vec4.
		const glm::vec4 packed((float)value, 0.0f, 0.0f, 0.0f);
		bgfx::setUniform(GetUniform(name, bgfx::UniformType::Vec4), &packed[0]);
	}

	void Shader::SetIntArray(const std::string& name, int* values, uint32_t count)
	{
		// Each int occupies its own vec4 slot - bgfx array uniforms are vec4[].
		std::vector<glm::vec4> packed(count);
		for (uint32_t i = 0; i < count; i++)
			packed[i] = glm::vec4((float)values[i], 0.0f, 0.0f, 0.0f);

		bgfx::setUniform(GetUniform(name, bgfx::UniformType::Vec4, (uint16_t)count), packed.data(), (uint16_t)count);
	}

	void Shader::SetFloat(const std::string& name, float value)
	{
		const glm::vec4 packed(value, 0.0f, 0.0f, 0.0f);
		bgfx::setUniform(GetUniform(name, bgfx::UniformType::Vec4), &packed[0]);
	}

	void Shader::SetFloat2(const std::string& name, const glm::vec2& value)
	{
		const glm::vec4 packed(value.x, value.y, 0.0f, 0.0f);
		bgfx::setUniform(GetUniform(name, bgfx::UniformType::Vec4), &packed[0]);
	}

	void Shader::SetFloat3(const std::string& name, const glm::vec3& value)
	{
		const glm::vec4 packed(value.x, value.y, value.z, 0.0f);
		bgfx::setUniform(GetUniform(name, bgfx::UniformType::Vec4), &packed[0]);
	}

	void Shader::SetFloat4(const std::string& name, const glm::vec4& value)
	{
		bgfx::setUniform(GetUniform(name, bgfx::UniformType::Vec4), &value[0]);
	}

	void Shader::SetMat4(const std::string& name, const glm::mat4& value)
	{
		bgfx::setUniform(GetUniform(name, bgfx::UniformType::Mat4), &value[0][0]);
	}

	Ref<Shader> Shader::Create(const std::string& filepath)
	{
		return CreateRef<Shader>(ShaderNameFromPath(filepath));
	}

	Ref<Shader> Shader::Create(const std::string& name, const std::string& vertexSrc, const std::string& fragmentSrc)
	{
		// Runtime source compilation has no bgfx equivalent - shaderc runs
		// offline. The sources are ignored; the name selects the compiled blob.
		GE_CORE_WARN("Shader::Create with inline sources is unsupported under bgfx; "
			"loading compiled '{0}' instead", name);
		return CreateRef<Shader>(name);
	}

	void ShaderLibrary::Add(const std::string& name, const Ref<Shader>& shader)
	{
		GE_CORE_ASSERT(!Exists(name), "Shader already exists!");
		m_Shaders[name] = shader;
	}

	void ShaderLibrary::Add(const Ref<Shader>& shader)
	{
		auto& name = shader->GetName();
		Add(name, shader);
	}

	Ref<Shader> ShaderLibrary::Load(const std::string& filepath)
	{
		auto shader = Shader::Create(filepath);
		Add(shader);
		return shader;
	}

	Ref<Shader> ShaderLibrary::Load(const std::string& name, const std::string& filepath)
	{
		auto shader = Shader::Create(filepath);
		Add(name, shader);
		return shader;
	}

	Ref<Shader> ShaderLibrary::Get(const std::string & name)
	{
		GE_CORE_ASSERT(Exists(name), "Shader not found!");
		return m_Shaders[name];
	}

	bool ShaderLibrary::Exists(const std::string& name) const
	{
		return m_Shaders.find(name) != m_Shaders.end();
	}
}
