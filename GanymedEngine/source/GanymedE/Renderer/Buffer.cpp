#include "gepch.h"
#include "Buffer.h"
#include "Renderer.h"

namespace GanymedE {

	// ---------------------------------------------------------------- VertexBuffer

	VertexBuffer::VertexBuffer(const void* data, uint32_t size, const BufferLayout& layout)
		: m_Layout(layout), m_IsDynamic(false)
	{
		m_VertexLayout = layout.ToBgfx();

		// bgfx::copy takes its own copy, so the caller's data can die
		// immediately - matching the old glBufferData semantics.
		m_StaticHandle = bgfx::createVertexBuffer(bgfx::copy(data, size), m_VertexLayout);
	}

	VertexBuffer::VertexBuffer(uint32_t size, const BufferLayout& layout)
		: m_Layout(layout), m_IsDynamic(true)
	{
		m_VertexLayout = layout.ToBgfx();

		// Dynamic buffers are sized in vertices, not bytes.
		const uint16_t stride = m_VertexLayout.getStride();
		const uint32_t vertexCount = stride > 0 ? size / stride : 0;

		m_DynamicHandle = bgfx::createDynamicVertexBuffer(vertexCount, m_VertexLayout);
	}

	VertexBuffer::~VertexBuffer()
	{
		// See Renderer::IsGpuAlive - this object may outlive bgfx.
		if (!Renderer::IsGpuAlive())
			return;

		if (m_IsDynamic)
		{
			if (bgfx::isValid(m_DynamicHandle))
				bgfx::destroy(m_DynamicHandle);
		}
		else if (bgfx::isValid(m_StaticHandle))
		{
			bgfx::destroy(m_StaticHandle);
		}
	}

	void VertexBuffer::SetData(const void* data, uint32_t size)
	{
		GE_CORE_ASSERT(m_IsDynamic, "SetData called on a static vertex buffer!");

		if (!bgfx::isValid(m_DynamicHandle))
			return;

		bgfx::update(m_DynamicHandle, 0, bgfx::copy(data, size));
	}

	bool VertexBuffer::IsValid() const
	{
		return m_IsDynamic ? bgfx::isValid(m_DynamicHandle) : bgfx::isValid(m_StaticHandle);
	}

	Ref<VertexBuffer> VertexBuffer::Create(uint32_t size, const BufferLayout& layout)
	{
		return CreateRef<VertexBuffer>(size, layout);
	}

	Ref<VertexBuffer> VertexBuffer::Create(const void* data, uint32_t size, const BufferLayout& layout)
	{
		return CreateRef<VertexBuffer>(data, size, layout);
	}

	// ----------------------------------------------------------------- IndexBuffer

	IndexBuffer::IndexBuffer(const uint32_t* indices, uint32_t count)
		: m_Count(count)
	{
		// The engine indexes with uint32 throughout; bgfx defaults to 16-bit
		// indices and needs to be told otherwise.
		m_Handle = bgfx::createIndexBuffer(
			bgfx::copy(indices, count * sizeof(uint32_t)),
			BGFX_BUFFER_INDEX32);
	}

	IndexBuffer::~IndexBuffer()
	{
		// See Renderer::IsGpuAlive - this object may outlive bgfx.
		if (!Renderer::IsGpuAlive())
			return;

		if (bgfx::isValid(m_Handle))
			bgfx::destroy(m_Handle);
	}

	bool IndexBuffer::IsValid() const
	{
		return bgfx::isValid(m_Handle);
	}

	Ref<IndexBuffer> IndexBuffer::Create(const uint32_t* indices, uint32_t count)
	{
		return CreateRef<IndexBuffer>(indices, count);
	}

}
