#include "gepch.h"
#include "Platform/Bgfx/ImGuiRendererBgfx.h"

#include "GanymedE/Renderer/RenderPassIDs.h"
#include "GanymedE/Renderer/Shader.h"

#include "imgui.h"

#include <bgfx/bgfx.h>

namespace GanymedE {

	namespace ImGuiRendererBgfx {

		namespace {

			Ref<Shader> s_Shader;
			bgfx::VertexLayout s_Layout;
			bgfx::TextureHandle s_FontTexture = BGFX_INVALID_HANDLE;
			bgfx::UniformHandle s_TextureUniform = BGFX_INVALID_HANDLE;

			void CreateFontAtlas()
			{
				ImGuiIO& io = ImGui::GetIO();

				uint8_t* pixels = nullptr;
				int width = 0, height = 0;
				io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

				if (bgfx::isValid(s_FontTexture))
					bgfx::destroy(s_FontTexture);

				s_FontTexture = bgfx::createTexture2D(
					(uint16_t)width, (uint16_t)height, false, 1,
					bgfx::TextureFormat::RGBA8, 0,
					bgfx::copy(pixels, width * height * 4));

				io.Fonts->SetTexID(ToImTextureID(s_FontTexture.idx));

				// The atlas pixels are ImGui's to free once uploaded.
				io.Fonts->ClearTexData();
			}

		}

		uint64_t ToImTextureID(uint16_t bgfxTextureIdx)
		{
			return (uint64_t)bgfxTextureIdx;
		}

		bool Init()
		{
			s_Shader = Shader::Create("ImGui");
			if (!s_Shader || !s_Shader->IsValid())
			{
				GE_CORE_ERROR("ImGui bgfx backend: failed to load the ImGui shader "
					"- run scripts/compile_shaders.bat");
				return false;
			}

			// Matches ImDrawVert exactly: vec2 pos, vec2 uv, packed RGBA8 colour.
			s_Layout.begin()
				.add(bgfx::Attrib::Position,  2, bgfx::AttribType::Float)
				.add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
				.add(bgfx::Attrib::Color0,    4, bgfx::AttribType::Uint8, true)
				.end();

			s_TextureUniform = bgfx::createUniform("s_Texture", bgfx::UniformType::Sampler);

			CreateFontAtlas();
			return true;
		}

		void Shutdown()
		{
			if (bgfx::isValid(s_FontTexture))
				bgfx::destroy(s_FontTexture);
			s_FontTexture = BGFX_INVALID_HANDLE;

			if (bgfx::isValid(s_TextureUniform))
				bgfx::destroy(s_TextureUniform);
			s_TextureUniform = BGFX_INVALID_HANDLE;

			s_Shader = nullptr;
		}

		void NewFrame()
		{
			if (!ImGui::GetIO().Fonts->IsBuilt())
				CreateFontAtlas();
		}

		void RenderDrawData(ImDrawData* drawData)
		{
			if (!drawData || drawData->CmdListsCount == 0 || !s_Shader || !s_Shader->IsValid())
				return;

			// Framebuffer-space size; differs from DisplaySize under DPI scaling.
			const int fbWidth = (int)(drawData->DisplaySize.x * drawData->FramebufferScale.x);
			const int fbHeight = (int)(drawData->DisplaySize.y * drawData->FramebufferScale.y);
			if (fbWidth <= 0 || fbHeight <= 0)
				return;

			const uint16_t view = RenderPass::ImGui;

			bgfx::setViewName(view, "ImGui");
			bgfx::setViewMode(view, bgfx::ViewMode::Sequential);
			bgfx::setViewRect(view, 0, 0, (uint16_t)fbWidth, (uint16_t)fbHeight);

			// ImGui vertices arrive in screen pixels, so the view needs an ortho
			// matrix over the display rect. bx::mtxOrtho is used rather than glm's
			// because it takes homogeneousDepth from caps directly, which keeps
			// this correct on backends that disagree about clip depth.
			{
				const float x = drawData->DisplayPos.x;
				const float y = drawData->DisplayPos.y;
				const float width = drawData->DisplaySize.x;
				const float height = drawData->DisplaySize.y;
				const bool homogeneousDepth = bgfx::getCaps()->homogeneousDepth;

				float ortho[16];
				// Column-major, top-left origin: maps [x, x+w] x [y, y+h] to clip.
				const float L = x, R = x + width, T = y, B = y + height;
				const float zn = homogeneousDepth ? -1.0f : 0.0f;
				const float zf = 1.0f;

				memset(ortho, 0, sizeof(ortho));
				ortho[0]  = 2.0f / (R - L);
				ortho[5]  = 2.0f / (T - B);
				ortho[10] = 1.0f / (zf - zn);
				ortho[12] = (R + L) / (L - R);
				ortho[13] = (T + B) / (B - T);
				ortho[14] = zn == 0.0f ? 0.0f : zn / (zn - zf);
				ortho[15] = 1.0f;

				bgfx::setViewTransform(view, nullptr, ortho);
			}

			const ImVec2 clipOffset = drawData->DisplayPos;
			const ImVec2 clipScale = drawData->FramebufferScale;

			for (int n = 0; n < drawData->CmdListsCount; n++)
			{
				const ImDrawList* cmdList = drawData->CmdLists[n];

				const uint32_t numVertices = (uint32_t)cmdList->VtxBuffer.Size;
				const uint32_t numIndices = (uint32_t)cmdList->IdxBuffer.Size;

				// Transient buffers are the right fit: this geometry is rebuilt
				// from scratch every frame and never reused.
				if (bgfx::getAvailTransientVertexBuffer(numVertices, s_Layout) < numVertices
					|| bgfx::getAvailTransientIndexBuffer(numIndices) < numIndices)
				{
					GE_CORE_WARN("ImGui bgfx backend: out of transient buffer space, dropping a draw list");
					continue;
				}

				bgfx::TransientVertexBuffer tvb;
				bgfx::TransientIndexBuffer tib;
				bgfx::allocTransientVertexBuffer(&tvb, numVertices, s_Layout);
				bgfx::allocTransientIndexBuffer(&tib, numIndices);

				memcpy(tvb.data, cmdList->VtxBuffer.Data, numVertices * sizeof(ImDrawVert));
				memcpy(tib.data, cmdList->IdxBuffer.Data, numIndices * sizeof(ImDrawIdx));

				for (int i = 0; i < cmdList->CmdBuffer.Size; i++)
				{
					const ImDrawCmd& cmd = cmdList->CmdBuffer[i];

					if (cmd.UserCallback)
					{
						cmd.UserCallback(cmdList, &cmd);
						continue;
					}

					if (cmd.ElemCount == 0)
						continue;

					// Clip rect is in display space; convert to framebuffer pixels.
					const float x0 = (cmd.ClipRect.x - clipOffset.x) * clipScale.x;
					const float y0 = (cmd.ClipRect.y - clipOffset.y) * clipScale.y;
					const float x1 = (cmd.ClipRect.z - clipOffset.x) * clipScale.x;
					const float y1 = (cmd.ClipRect.w - clipOffset.y) * clipScale.y;

					if (x1 <= x0 || y1 <= y0)
						continue;

					bgfx::setScissor(
						(uint16_t)std::max(x0, 0.0f),
						(uint16_t)std::max(y0, 0.0f),
						(uint16_t)(std::min(x1, (float)fbWidth) - std::max(x0, 0.0f)),
						(uint16_t)(std::min(y1, (float)fbHeight) - std::max(y0, 0.0f)));

					bgfx::TextureHandle texture = { (uint16_t)cmd.GetTexID() };
					bgfx::setTexture(0, s_TextureUniform, texture);

					bgfx::setState(BGFX_STATE_WRITE_RGB
						| BGFX_STATE_WRITE_A
						| BGFX_STATE_MSAA
						| BGFX_STATE_BLEND_FUNC_SEPARATE(
							BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA,
							BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA));

					bgfx::setVertexBuffer(0, &tvb, 0, numVertices);
					bgfx::setIndexBuffer(&tib, cmd.IdxOffset, cmd.ElemCount);

					bgfx::submit(view, s_Shader->GetProgram());
				}
			}
		}

	}

}
