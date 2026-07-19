#pragma once

#include <bgfx/bgfx.h>

namespace GanymedE {
	enum class ShaderDataType
	{
		None = 0,
		Float,
		Float2,
		Float3,
		Float4,
		Mat3,
		Mat4,
		Int,
		Int2,
		Int3,
		Int4,
		Bool
	};

	static uint32_t ShaderDataTypeSize(ShaderDataType type)
	{
		switch (type)
		{
			case ShaderDataType::Float:    return 4;
			case ShaderDataType::Float2:   return 4 * 2;
			case ShaderDataType::Float3:   return 4 * 3;
			case ShaderDataType::Float4:   return 4 * 4;
			case ShaderDataType::Mat3:     return 4 * 3 * 3;
			case ShaderDataType::Mat4:     return 4 * 4 * 4;
			case ShaderDataType::Int:      return 4;
			case ShaderDataType::Int2:     return 4 * 2;
			case ShaderDataType::Int3:     return 4 * 3;
			case ShaderDataType::Int4:     return 4 * 4;
			case ShaderDataType::Bool:     return 1;
		}

		GE_CORE_ASSERT(false, "Unknown shader data type!");
		return 0;
	}

	// bgfx vertex attributes are *semantic slots*, not free-form names, so every
	// attribute the engine uses has to be assigned one. Data with no natural
	// semantic (entity IDs, per-vertex texture indices) rides in a spare
	// TexCoordN. This table is the single source of truth - it must stay in sync
	// with the varying.def.sc the shaders are compiled against (Phase 3).
	//
	// Per-instance attributes are deliberately absent: bgfx has no
	// attribute-divisor concept and feeds instance data through
	// setInstanceDataBuffer / i_data0..4 instead.
	inline bgfx::Attrib::Enum AttribFromName(const std::string& name)
	{
		if (name == "a_Position")     return bgfx::Attrib::Position;
		if (name == "a_Normal")       return bgfx::Attrib::Normal;
		if (name == "a_Tangent")      return bgfx::Attrib::Tangent;
		if (name == "a_Color")        return bgfx::Attrib::Color0;
		if (name == "a_TexCoord")     return bgfx::Attrib::TexCoord0;
		if (name == "a_TexIndex")     return bgfx::Attrib::TexCoord1;
		if (name == "a_TilingFactor") return bgfx::Attrib::TexCoord2;
		if (name == "a_EntityID")     return bgfx::Attrib::TexCoord3;

		GE_CORE_ASSERT(false, "Unmapped vertex attribute - add it to AttribFromName, "
			"or route it through an instance data buffer if it is per-instance.");
		return bgfx::Attrib::Count;
	}

	// bgfx offers no 32-bit integer vertex attribute (only Uint8, Uint10, Int16,
	// Half and Float). Integer attributes therefore travel as floats, which is
	// lossless for entity IDs up to 2^24 - far past any realistic entity count.
	//
	// NOTE: the CPU-side vertex data must be written as float for these, not as
	// int32 reinterpreted - the GPU reads the bytes as float. QuadVertex and
	// MeshInstanceData both store float for exactly this reason.
	inline bgfx::AttribType::Enum AttribTypeFromShaderType(ShaderDataType type)
	{
		switch (type)
		{
			case ShaderDataType::Float:
			case ShaderDataType::Float2:
			case ShaderDataType::Float3:
			case ShaderDataType::Float4:
			case ShaderDataType::Mat3:
			case ShaderDataType::Mat4:
			case ShaderDataType::Int:
			case ShaderDataType::Int2:
			case ShaderDataType::Int3:
			case ShaderDataType::Int4:
			case ShaderDataType::Bool:
				return bgfx::AttribType::Float;
		}

		GE_CORE_ASSERT(false, "Unknown shader data type!");
		return bgfx::AttribType::Float;
	}

	struct BufferElement
	{
		ShaderDataType Type;
		std::string Name;
		uint32_t Size;
		uint32_t Offset;
		bool Normalized;
		bool Instanced; // fed through bgfx::setInstanceDataBuffer, not the vertex layout

		BufferElement() {}

		BufferElement(ShaderDataType type, const std::string& name, bool normalized = false, bool instanced = false)
			: Name(name), Type(type), Size(ShaderDataTypeSize(type)), Offset(0), Normalized(normalized), Instanced(instanced)
		{
		}

		uint32_t GetComponentCount() const
		{
			switch (Type)
			{
			case ShaderDataType::Float:    return 1;
			case ShaderDataType::Float2:   return 2;
			case ShaderDataType::Float3:   return 3;
			case ShaderDataType::Float4:   return 4;
			case ShaderDataType::Mat3:     return 3; // 3* float3
			case ShaderDataType::Mat4:     return 4; // 4* float4
			case ShaderDataType::Int:      return 1;
			case ShaderDataType::Int2:     return 2;
			case ShaderDataType::Int3:     return 3;
			case ShaderDataType::Int4:     return 4;
			case ShaderDataType::Bool:     return 1;
			}

			GE_CORE_ASSERT(false, "Unknown shader data type!");
			return 0;
		}
	};

	class BufferLayout
	{
	public:
		BufferLayout() {}
		BufferLayout(const std::initializer_list<BufferElement>& elements)
			: m_Elements(elements)
		{
			CalculateOffsetAndStride();
		}

		inline uint32_t GetStride() const { return m_Stride; }
		inline const std::vector<BufferElement>& GetElements() const { return m_Elements; }

		// Translates into the bgfx description used at buffer creation. Only
		// per-vertex elements take part; instanced ones are skipped.
		bgfx::VertexLayout ToBgfx() const
		{
			bgfx::VertexLayout layout;
			layout.begin();

			for (const BufferElement& element : m_Elements)
			{
				if (element.Instanced)
					continue;

				layout.add(
					AttribFromName(element.Name),
					(uint8_t)element.GetComponentCount(),
					AttribTypeFromShaderType(element.Type),
					element.Normalized);
			}

			layout.end();
			return layout;
		}

		std::vector<BufferElement>::iterator begin() { return m_Elements.begin(); }
		std::vector<BufferElement>::iterator end() { return m_Elements.end(); }
		std::vector<BufferElement>::const_iterator begin() const { return m_Elements.begin(); }
		std::vector<BufferElement>::const_iterator end() const { return m_Elements.end(); }
	private:
		void CalculateOffsetAndStride()
		{
			uint32_t offset = 0;
			m_Stride = 0;
			for (auto& element : m_Elements)
			{
				element.Offset = offset;
				offset += element.Size;
				m_Stride += element.Size;
			}
		}

	private:
		std::vector<BufferElement> m_Elements;
		uint32_t m_Stride = 0;
	};

	// Concrete wrapper over a bgfx vertex buffer. Static when constructed with
	// data, dynamic when constructed with just a size (the SetData path).
	//
	// There is no Bind(): bgfx binds buffers at submit time, so binding is the
	// draw call's job (see RenderCommand).
	class VertexBuffer
	{
	public:
		VertexBuffer(const void* data, uint32_t size, const BufferLayout& layout);
		VertexBuffer(uint32_t size, const BufferLayout& layout);
		~VertexBuffer();

		VertexBuffer(const VertexBuffer&) = delete;
		VertexBuffer& operator=(const VertexBuffer&) = delete;

		// Dynamic buffers only.
		void SetData(const void* data, uint32_t size);

		const BufferLayout& GetLayout() const { return m_Layout; }
		const bgfx::VertexLayout& GetVertexLayout() const { return m_VertexLayout; }

		bool IsDynamic() const { return m_IsDynamic; }
		bool IsValid() const;

		bgfx::VertexBufferHandle GetStaticHandle() const { return m_StaticHandle; }
		bgfx::DynamicVertexBufferHandle GetDynamicHandle() const { return m_DynamicHandle; }

		static Ref<VertexBuffer> Create(uint32_t size, const BufferLayout& layout);
		static Ref<VertexBuffer> Create(const void* data, uint32_t size, const BufferLayout& layout);
	private:
		BufferLayout m_Layout;
		bgfx::VertexLayout m_VertexLayout;

		bool m_IsDynamic = false;
		bgfx::VertexBufferHandle m_StaticHandle = BGFX_INVALID_HANDLE;
		bgfx::DynamicVertexBufferHandle m_DynamicHandle = BGFX_INVALID_HANDLE;
	};

	class IndexBuffer
	{
	public:
		IndexBuffer(const uint32_t* indices, uint32_t count);
		~IndexBuffer();

		IndexBuffer(const IndexBuffer&) = delete;
		IndexBuffer& operator=(const IndexBuffer&) = delete;

		uint32_t GetCount() const { return m_Count; }
		bool IsValid() const;

		bgfx::IndexBufferHandle GetHandle() const { return m_Handle; }

		static Ref<IndexBuffer> Create(const uint32_t* indices, uint32_t count);
	private:
		uint32_t m_Count = 0;
		bgfx::IndexBufferHandle m_Handle = BGFX_INVALID_HANDLE;
	};

	// Replaces VertexArray: bgfx has no VAO concept, so the only thing worth
	// keeping was the vertex+index buffer pairing.
	struct Geometry
	{
		Ref<VertexBuffer> Vertices;
		Ref<IndexBuffer> Indices;

		bool IsValid() const
		{
			return Vertices && Vertices->IsValid() && Indices && Indices->IsValid();
		}
	};
}
