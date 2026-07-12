#include "gepch.h"
#include "PostProcess.h"

#include "Shader.h"
#include "VertexArray.h"
#include "Buffer.h"
#include "RenderCommand.h"
#include "Framebuffer.h"

namespace GanymedE {

	struct PostProcessData
	{
		Ref<Shader> TonemapShader;
		Ref<VertexArray> FullscreenQuad;
	};

	static PostProcessData s_Data;

	void PostProcess::Init()
	{
		s_Data.TonemapShader = Shader::Create("assets/shaders/Tonemap.glsl");

		float quadVertices[] = {
			-1.0f, -1.0f,
			 1.0f, -1.0f,
			 1.0f,  1.0f,
			-1.0f,  1.0f
		};
		uint32_t quadIndices[] = { 0, 1, 2, 2, 3, 0 };

		s_Data.FullscreenQuad = VertexArray::Create();
		Ref<VertexBuffer> quadVB = VertexBuffer::Create(quadVertices, sizeof(quadVertices));
		quadVB->SetLayout({ { ShaderDataType::Float2, "a_Position" } });
		s_Data.FullscreenQuad->AddVertexBuffer(quadVB);
		Ref<IndexBuffer> quadIB = IndexBuffer::Create(quadIndices, 6);
		s_Data.FullscreenQuad->SetIndexBuffer(quadIB);
	}

	void PostProcess::Shutdown()
	{
		s_Data.TonemapShader = nullptr;
		s_Data.FullscreenQuad = nullptr;
	}

	void PostProcess::DrawFullscreenQuad()
	{
		if (!s_Data.FullscreenQuad)
			return;

		RenderCommand::SetDepthTest(false);
		RenderCommand::SetDepthWrite(false);
		RenderCommand::SetCullFace(false);

		RenderCommand::DrawIndexed(s_Data.FullscreenQuad);

		RenderCommand::SetDepthTest(true);
		RenderCommand::SetDepthWrite(true);
		RenderCommand::SetCullFace(true);
	}

	void PostProcess::Tonemap(const Ref<Framebuffer>& source, float exposure,
		const Ref<Framebuffer>& bloom, float bloomIntensity)
	{
		if (!source || !s_Data.TonemapShader || !s_Data.FullscreenQuad)
			return;

		source->BindColorTexture(0, 0);

		s_Data.TonemapShader->Bind();
		s_Data.TonemapShader->SetInt("u_Texture", 0);
		s_Data.TonemapShader->SetFloat("u_Exposure", exposure);

		bool useBloom = bloom != nullptr;
		s_Data.TonemapShader->SetInt("u_UseBloom", useBloom ? 1 : 0);
		if (useBloom)
		{
			bloom->BindColorTexture(0, 1);
			s_Data.TonemapShader->SetInt("u_BloomTexture", 1);
			s_Data.TonemapShader->SetFloat("u_BloomIntensity", bloomIntensity);
		}

		DrawFullscreenQuad();
	}

}
