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
		: Shader(name, name, name)
	{
	}

	Shader::Shader(const std::string& name, const std::string& vertexStage, const std::string& fragmentStage)
		: m_Name(name)
	{
		const char* profile = ProfileDirectory();
		if (!profile)
		{
			GE_CORE_ERROR("No compiled shader profile for the active bgfx backend");
			return;
		}

		const std::string directory = std::string("assets/shaders/compiled/") + profile;

		bgfx::ShaderHandle vs = LoadShaderStage(directory, "vs_", vertexStage);
		bgfx::ShaderHandle fs = LoadShaderStage(directory, "fs_", fragmentStage);

		if (!bgfx::isValid(vs) || !bgfx::isValid(fs))
		{
			GE_CORE_ERROR("Failed to load shader '{0}' - run scripts/compile_shaders.bat", name);
			if (bgfx::isValid(vs)) bgfx::destroy(vs);
			if (bgfx::isValid(fs)) bgfx::destroy(fs);
			return;
		}

		// destroyShaders = false, and the stages are kept: the program has to be relinkable.
		// See RelinkProgram for why.
		m_VertexStage = vs;
		m_FragmentStage = fs;
		m_Program = bgfx::createProgram(vs, fs, false);

		if (!bgfx::isValid(m_Program))
			GE_CORE_ERROR("Failed to link shader program '{0}'", name);
		else
			GE_CORE_TRACE("Loaded shader '{0}' ({1})", name, profile);
	}

	Shader::~Shader()
	{
		// See Renderer::IsGpuAlive - this object may outlive bgfx.
		if (!Renderer::IsGpuAlive())
			return;

		for (auto& entry : m_Uniforms)
		{
			if (bgfx::isValid(entry.second))
				bgfx::destroy(entry.second);
		}

		if (bgfx::isValid(m_Program))
			bgfx::destroy(m_Program);

		// Owned here because createProgram was told not to take them - see RelinkProgram.
		if (bgfx::isValid(m_VertexStage))
			bgfx::destroy(m_VertexStage);
		if (bgfx::isValid(m_FragmentStage))
			bgfx::destroy(m_FragmentStage);
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

	void Shader::RelinkProgram()
	{
		if (!bgfx::isValid(m_VertexStage) || !bgfx::isValid(m_FragmentStage))
			return;

		const bool wasBound = bgfx::isValid(m_Program)
			&& RenderCommand::GetProgram().idx == m_Program.idx;

		// **The old program has to be released first.** bgfx caches programs by their (vertex,
		// fragment) stage pair and hands back the existing handle for a repeat request, so
		// calling createProgram again on the same stages relinks nothing - it returns the very
		// program whose uniform bindings are the problem, with one more reference.
		//
		// Destroying mid-frame is safe: bgfx defers the actual teardown, and a relink only ever
		// happens during the Set* calls that precede this shader's first draw of the frame.
		if (bgfx::isValid(m_Program))
			bgfx::destroy(m_Program);

		m_Program = bgfx::createProgram(m_VertexStage, m_FragmentStage, false);

		// Bind() runs before the Set* calls that trigger a relink, so the draw about to be
		// submitted is still holding the handle we just replaced.
		if (wasBound && bgfx::isValid(m_Program))
			RenderCommand::SetProgram(m_Program);
	}

	bgfx::UniformHandle Shader::GetUniform(const std::string& name, bgfx::UniformType::Enum type, uint16_t num)
	{
		auto it = m_Uniforms.find(name);
		if (it != m_Uniforms.end())
			return it->second;

		bgfx::UniformHandle handle = bgfx::createUniform(name.c_str(), type, num);
		m_Uniforms[name] = handle;

		// **bgfx binds a program's uniforms by name when the program is LINKED.** A uniform that
		// did not exist at that moment is not wired to the program, and on OpenGL it then reads
		// as ZERO in the shader forever - `bgfx WARN User defined uniform 'u_Exposure' is not
		// found, it won't be set`. Direct3D resolves per draw instead and tolerates it, which is
		// why this only ever showed on GL: the post-process chain multiplied by an exposure of 0
		// and the viewport rendered black while the scene target held the correct image.
		//
		// Uniforms are created lazily by the Set* calls above, always after the constructor
		// linked the program, so the fix is to relink once a new name appears. It settles after
		// the first frames that exercise a shader - a uniform is only ever new once.
		//
		// The obvious alternative, creating everything up front, cannot work here:
		// getShaderUniforms reports nothing for GLSL binaries (their uniform table is empty,
		// unlike the D3D ones), and some names are built at runtime (`s_shadowMap0..3`), so
		// there is no complete list to pre-register from.
		RelinkProgram();

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

	void Shader::SetFloat4Array(const std::string& name, const glm::vec4* values, uint32_t count)
	{
		if (count == 0)
			return;

		bgfx::setUniform(GetUniform(name, bgfx::UniformType::Vec4, (uint16_t)count),
			values, (uint16_t)count);
	}

	void Shader::SetMat4Array(const std::string& name, const glm::mat4* values, uint32_t count)
	{
		if (count == 0)
			return;

		bgfx::setUniform(GetUniform(name, bgfx::UniformType::Mat4, (uint16_t)count),
			values, (uint16_t)count);
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

	Ref<Shader> Shader::CreateFromStages(const std::string& name, const std::string& vertexStage,
		const std::string& fragmentStage)
	{
		return CreateRef<Shader>(name, vertexStage, fragmentStage);
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
