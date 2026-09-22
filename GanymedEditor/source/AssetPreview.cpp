#include "GanymedE/Core/Log.h"
#include "AssetPreview.h"

#include "GanymedE/Assets/AssetManager.h"
#include "GanymedE/Assets/AssetPaths.h"
#include "GanymedE/Renderer/EditorCamera.h"
#include "GanymedE/Renderer/Environment.h"
#include "GanymedE/Renderer/Material.h"
#include "GanymedE/Renderer/Mesh.h"
#include "GanymedE/Renderer/MeshImporter.h"
#include "GanymedE/Renderer/RenderPassIDs.h"
#include "GanymedE/Renderer/Renderer.h"
#include "GanymedE/Renderer/Renderer3D.h"
#include "GanymedE/Renderer/SceneRenderer.h"

#include <bgfx/bgfx.h>
#include <imgui/imgui.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <filesystem>
#include <unordered_map>

namespace GanymedE {

	namespace {

		constexpr uint32_t kPreviewSize = 384;
		constexpr uint32_t kRendersPerFrame = 1;
		constexpr uint8_t kPreviewPaletteBase = 2;
		constexpr const char* kStudioEnvironment = "environments/studio_small_08_1k.hdr";

		struct OrbitState
		{
			float Pitch = 0.40f;
			float Yaw = 0.70f;
			float Distance = 10.0f;
			glm::vec3 Focal{ 0.0f };
		};

		struct State
		{
			AssetHandle Handle = InvalidAssetHandle;
			AssetType Type = AssetType::None;
			bool Dirty = false;
			bool Framed = false;

			Ref<SceneRenderer> Renderer;
			EditorCamera Camera{ 45.0f, 1.0f, 0.05f, 10000.0f };
			std::unordered_map<AssetHandle, OrbitState> Cameras;

			AssetHandle StudioHandle = InvalidAssetHandle;
			Ref<Environment> Studio;
			bool StudioTried = false;

			uint32_t RenderCount = 0;
			uint32_t BudgetUsed = 0;
		};

		State& Get()
		{
			static State s;
			return s;
		}

		void SaveCamera(State& s)
		{
			if (!IsAssetHandleValid(s.Handle))
				return;

			OrbitState orbit;
			s.Camera.GetOrbitState(orbit.Pitch, orbit.Yaw, orbit.Distance, orbit.Focal);
			s.Cameras[s.Handle] = orbit;
		}

		void FrameMesh(State& s, const Mesh& mesh)
		{
			const AABB& bounds = mesh.GetBounds();
			const glm::vec3 center = (bounds.Min + bounds.Max) * 0.5f;
			const float radius = glm::max(glm::length(bounds.Max - bounds.Min) * 0.5f, 0.05f);

			s.Camera.SetViewportSize((float)kPreviewSize, (float)kPreviewSize);
			s.Camera.SetOrbitState(0.40f, 0.70f, 10.0f, center);
			s.Camera.Frame(center, radius);
			s.Framed = true;
		}

		void EvictRenderer(State& s)
		{
			s.Renderer.reset();
		}

		void EnsureStudio(State& s)
		{
			if (s.StudioTried)
				return;
			s.StudioTried = true;

			std::error_code ec;
			if (!std::filesystem::exists(GetAssetRoot() / kStudioEnvironment, ec))
				return;

			s.StudioHandle = AssetManager::ImportAsset(kStudioEnvironment);
		}

		bool MeshUsesHandle(const Mesh& mesh, AssetHandle handle)
		{
			for (const Ref<Material>& material : mesh.GetMaterials())
			{
				if (!material)
					continue;
				if (material->GetAlbedoMapHandle() == handle
					|| material->GetNormalMapHandle() == handle
					|| material->GetMetallicRoughnessMapHandle() == handle)
					return true;
			}
			return false;
		}

		void RenderMesh(State& s, const Ref<Mesh>& mesh)
		{
			if (!s.Renderer)
			{
				s.Renderer = CreateRef<SceneRenderer>(kPreviewSize, kPreviewSize,
					RenderPass::PreviewViewBase, kPreviewPaletteBase);
				s.Renderer->GetSettings().ClearColor = { 0.12f, 0.12f, 0.14f, 1.0f };
			}

			if (!s.Framed)
				FrameMesh(s, *mesh);

			s.Renderer->BeginFrame();
			Renderer3D::BeginScene(s.Camera);

			Renderer3D::SubmitDirectionalLight(
				glm::vec3(-0.45f, -1.0f, -0.35f),
				glm::vec3(1.0f, 0.98f, 0.94f),
				2.5f, false);

			if (s.Studio && s.Studio->IsValid())
				Renderer3D::SubmitEnvironment(s.Studio, 1.0f, true);
			else
				Renderer3D::SubmitSkyLight(
					glm::vec3(0.42f, 0.50f, 0.68f),
					glm::vec3(0.16f, 0.14f, 0.13f),
					1.0f, true);

			Renderer3D::SubmitMesh(mesh, glm::mat4(1.0f));

			const AssetMetadata* meta = AssetManager::GetMetadata(s.Handle);
			if (meta && MeshCollision::WantsBoxCollider(meta->Config))
			{
				glm::vec3 halfExtents, offset;
				MeshCollision::FitFromAABB(mesh->GetBounds(), halfExtents, offset);
				glm::mat4 box = glm::translate(glm::mat4(1.0f), offset)
					* glm::scale(glm::mat4(1.0f), halfExtents * 2.0f);
				Renderer3D::DrawWireBox(box, glm::vec4(0.2f, 0.9f, 0.35f, 1.0f));
			}

			Renderer3D::EndScene();
			s.Renderer->EndFrame();

			s.Dirty = false;
			s.BudgetUsed++;
			s.RenderCount++;
		}

	}

	void AssetPreview::Init()
	{
		State& s = Get();
		s.Camera.SetViewportSize((float)kPreviewSize, (float)kPreviewSize);

		AssetManager::AddAssetChangedListener([](AssetHandle handle, AssetType type)
		{
			State& state = Get();
			if (!IsAssetHandleValid(state.Handle) || state.Type != AssetType::StaticMesh)
				return false;

			if (handle == state.Handle
				|| type == AssetType::Material
				|| type == AssetType::Texture
				|| type == AssetType::Environment)
			{
				state.Dirty = true;
				return true;
			}

			if (type == AssetType::StaticMesh)
				return false;

			Ref<Mesh> mesh = AssetManager::GetAsset<Mesh>(state.Handle);
			if (mesh && MeshUsesHandle(*mesh, handle))
			{
				state.Dirty = true;
				return true;
			}
			return false;
		});
	}

	void AssetPreview::Shutdown()
	{
		State& s = Get();
		SaveCamera(s);
		s.Renderer.reset();
		s.Studio.reset();
		s.StudioHandle = InvalidAssetHandle;
		s.Handle = InvalidAssetHandle;
		s.Type = AssetType::None;
	}

	void AssetPreview::SetSelection(AssetHandle handle, AssetType type)
	{
		State& s = Get();
		if (s.Handle == handle && s.Type == type)
			return;

		SaveCamera(s);
		s.Handle = handle;
		s.Type = type;
		s.Framed = false;
		s.Dirty = (type == AssetType::StaticMesh && IsAssetHandleValid(handle));

		if (s.Dirty)
		{
			auto it = s.Cameras.find(handle);
			if (it != s.Cameras.end())
			{
				s.Camera.SetOrbitState(it->second.Pitch, it->second.Yaw,
					it->second.Distance, it->second.Focal);
				s.Framed = true;
			}
		}
		else
		{
			EvictRenderer(s);
		}
	}

	void AssetPreview::MarkDirty()
	{
		Get().Dirty = true;
	}

	void AssetPreview::Tick()
	{
		State& s = Get();
		s.BudgetUsed = 0;

		EnsureStudio(s);
		if (IsAssetHandleValid(s.StudioHandle) && !s.Studio)
		{
			s.Studio = AssetManager::GetAsset<Environment>(s.StudioHandle);
			if (s.Studio)
				s.Dirty = true;
		}

		if (!s.Dirty)
			return;
		if (s.Type != AssetType::StaticMesh || !IsAssetHandleValid(s.Handle))
		{
			s.Dirty = false;
			EvictRenderer(s);
			return;
		}
		if (s.BudgetUsed >= kRendersPerFrame)
			return;

		Ref<Mesh> mesh = AssetManager::GetAsset<Mesh>(s.Handle);
		if (!mesh)
			return;

		RenderMesh(s, mesh);
	}

	void AssetPreview::DrawInspector(float width)
	{
		State& s = Get();
		if (s.Type != AssetType::StaticMesh || !s.Renderer)
		{
			if (s.Type == AssetType::StaticMesh)
				ImGui::TextDisabled("Preview pending...");
			return;
		}

		const float size = glm::max(width, 64.0f);
		const bool flipV = bgfx::getCaps()->originBottomLeft;
		const ImVec2 uv0 = flipV ? ImVec2{ 0.0f, 1.0f } : ImVec2{ 0.0f, 0.0f };
		const ImVec2 uv1 = flipV ? ImVec2{ 1.0f, 0.0f } : ImVec2{ 1.0f, 1.0f };

		ImGui::InvisibleButton("##assetpreview", ImVec2(size, size));
		const ImVec2 min = ImGui::GetItemRectMin();
		const ImVec2 max = ImGui::GetItemRectMax();
		const uint32_t textureID = s.Renderer->GetFinalImageRendererID();
		ImGui::GetWindowDrawList()->AddImage(
			static_cast<ImTextureID>(static_cast<uintptr_t>(textureID)),
			min, max, uv0, uv1);

		if (ImGui::IsItemHovered())
		{
			ImGuiIO& io = ImGui::GetIO();
			if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f))
			{
				s.Camera.Orbit({ io.MouseDelta.x, io.MouseDelta.y });
				s.Dirty = true;
			}
			if (io.MouseWheel != 0.0f)
			{
				s.Camera.Zoom(io.MouseWheel * 0.1f);
				s.Dirty = true;
			}

			ImGui::SetTooltip("LMB orbit, wheel zoom\nPreview renders: %u", s.RenderCount);
		}

		Ref<Mesh> mesh = AssetManager::GetAsset<Mesh>(s.Handle);
		if (mesh && mesh->HasSkeleton())
			ImGui::TextDisabled("Bind pose — the CPU vertices are the rest pose, not a clip.");
	}

	uint32_t AssetPreview::RenderCount()
	{
		return Get().RenderCount;
	}

}
