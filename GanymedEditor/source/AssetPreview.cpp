#include "GanymedE/Core/Log.h"
#include "AssetPreview.h"

#include "GanymedE/Assets/AssetManager.h"
#include "GanymedE/Assets/AssetPaths.h"
#include "GanymedE/Assets/CompiledCache.h"
#include "GanymedE/Renderer/EditorCamera.h"
#include "GanymedE/Renderer/Environment.h"
#include "GanymedE/Renderer/Framebuffer.h"
#include "GanymedE/Renderer/Material.h"
#include "GanymedE/Renderer/Mesh.h"
#include "GanymedE/Renderer/MeshImporter.h"
#include "GanymedE/Renderer/RenderPassIDs.h"
#include "GanymedE/Renderer/Renderer.h"
#include "GanymedE/Renderer/Renderer3D.h"
#include "GanymedE/Renderer/SceneRenderer.h"
#include "GanymedE/Renderer/Texture.h"

#include <bgfx/bgfx.h>
#include <imgui/imgui.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace GanymedE {

	namespace {

		constexpr uint32_t kPreviewSize = 384;
		constexpr uint32_t kThumbSize = 128;
		constexpr uint32_t kRendersPerFrame = 1;
		constexpr uint32_t kDiskLoadsPerFrame = 8;
		constexpr std::size_t kMaxGpuThumbs = 256;
		constexpr uint8_t kPreviewPaletteBase = 2;
		constexpr uint8_t kThumbPaletteBase = 4;
		constexpr uint32_t kThumbMagic = 0x42485447; // 'GTHB' little-endian
		constexpr uint32_t kThumbVersion = 2;
		constexpr const char* kStudioEnvironment = "environments/studio_small_08_1k.hdr";

		struct OrbitState
		{
			float Pitch = 0.40f;
			float Yaw = 0.70f;
			float Distance = 10.0f;
			glm::vec3 Focal{ 0.0f };
		};

		struct PendingReadback
		{
			AssetHandle Handle = InvalidAssetHandle;
			uint64_t ConfigHash = 0;
			uint32_t ReadyFrame = 0;
			std::vector<uint8_t> Pixels;
			bool Active = false;
		};

		struct State
		{
			AssetHandle Handle = InvalidAssetHandle;
			AssetType Type = AssetType::None;
			bool Dirty = false;
			bool Framed = false;

			Ref<SceneRenderer> Renderer;
			Ref<SceneRenderer> ThumbRenderer;
			EditorCamera Camera{ 45.0f, 1.0f, 0.05f, 10000.0f };
			EditorCamera ThumbCamera{ 45.0f, 1.0f, 0.05f, 10000.0f };
			std::unordered_map<AssetHandle, OrbitState> Cameras;

			AssetHandle StudioHandle = InvalidAssetHandle;
			Ref<Environment> Studio;
			bool StudioTried = false;

			uint32_t RenderCount = 0;
			uint32_t ThumbRenderCount = 0;
			uint32_t BudgetUsed = 0;

			std::unordered_set<AssetHandle> Visible;
			bool GridIdle = true;
			bool SkipDisk = false;

			std::unordered_map<AssetHandle, Ref<Texture2D>> Gpu;
			std::deque<AssetHandle> GpuOrder;
			PendingReadback Pending;
			uint64_t DiskBytes = 0;
			bool DiskBytesKnown = false;
		};

		State& Get()
		{
			static State s;
			return s;
		}

		std::filesystem::path ThumbPath(const std::string& relativeSourcePath)
		{
			std::filesystem::path path = CompiledCache::OutputPath(relativeSourcePath);
			path.replace_extension(".thumb");
			return path;
		}

		void FlipY(uint8_t* pixels, uint32_t width, uint32_t height)
		{
			if (!pixels || width == 0 || height < 2)
				return;

			const uint32_t stride = width * 4;
			std::vector<uint8_t> row(stride);
			for (uint32_t y = 0; y < height / 2; y++)
			{
				uint8_t* a = pixels + (std::size_t)y * stride;
				uint8_t* b = pixels + (std::size_t)(height - 1 - y) * stride;
				std::memcpy(row.data(), a, stride);
				std::memcpy(a, b, stride);
				std::memcpy(b, row.data(), stride);
			}
		}

		uint64_t ScanThumbBytes()
		{
			uint64_t total = 0;
			const std::filesystem::path root = GetAssetRoot() / ".compiled";
			std::error_code ec;
			if (!std::filesystem::exists(root, ec))
				return 0;

			for (const auto& entry : std::filesystem::recursive_directory_iterator(root, ec))
			{
				if (ec)
					break;
				if (!entry.is_regular_file(ec))
					continue;
				if (entry.path().extension() != ".thumb")
					continue;
				total += (uint64_t)entry.file_size(ec);
			}
			return total;
		}

		void RememberGpu(State& s, AssetHandle handle, const Ref<Texture2D>& texture)
		{
			if (!texture)
				return;

			auto existing = s.Gpu.find(handle);
			if (existing != s.Gpu.end())
			{
				existing->second = texture;
				return;
			}

			while (s.Gpu.size() >= kMaxGpuThumbs && !s.GpuOrder.empty())
			{
				AssetHandle oldest = s.GpuOrder.front();
				s.GpuOrder.pop_front();
				s.Gpu.erase(oldest);
			}

			s.Gpu[handle] = texture;
			s.GpuOrder.push_back(handle);
		}

		void EvictGpu(State& s, AssetHandle handle)
		{
			s.Gpu.erase(handle);
			if (s.Pending.Active && s.Pending.Handle == handle)
				s.Pending.Active = false;
		}

		void DeleteThumbFile(State& s, const AssetMetadata& metadata)
		{
			const std::filesystem::path path = ThumbPath(metadata.FilePath);
			std::error_code ec;
			if (!std::filesystem::exists(path, ec))
				return;

			const uint64_t size = (uint64_t)std::filesystem::file_size(path, ec);
			if (std::filesystem::remove(path, ec) && s.DiskBytesKnown && size > 0)
			{
				if (s.DiskBytes >= size)
					s.DiskBytes -= size;
				else
					s.DiskBytes = 0;
			}
		}

		bool DiskThumbCurrent(const AssetMetadata& metadata)
		{
			if (CompiledCache::QueryOutput(metadata) != CompiledCache::OutputStatus::Current)
				return false;

			const std::filesystem::path path = ThumbPath(metadata.FilePath);
			std::error_code ec;
			if (!std::filesystem::exists(path, ec))
				return false;

			std::ifstream in(path, std::ios::binary);
			if (!in)
				return false;

			uint32_t magic = 0, version = 0, width = 0, height = 0;
			uint64_t configHash = 0;
			in.read(reinterpret_cast<char*>(&magic), sizeof(magic));
			in.read(reinterpret_cast<char*>(&version), sizeof(version));
			in.read(reinterpret_cast<char*>(&configHash), sizeof(configHash));
			in.read(reinterpret_cast<char*>(&width), sizeof(width));
			in.read(reinterpret_cast<char*>(&height), sizeof(height));
			if (!in || magic != kThumbMagic || version != kThumbVersion)
				return false;
			if (width != kThumbSize || height != kThumbSize)
				return false;
			return configHash == CompiledCache::HashConfig(metadata.Config);
		}

		Ref<Texture2D> LoadThumbFromDisk(State& s, const AssetMetadata& metadata)
		{
			if (s.SkipDisk || !DiskThumbCurrent(metadata))
				return nullptr;

			const std::filesystem::path path = ThumbPath(metadata.FilePath);
			std::ifstream in(path, std::ios::binary);
			if (!in)
				return nullptr;

			uint32_t magic = 0, version = 0, width = 0, height = 0;
			uint64_t configHash = 0;
			in.read(reinterpret_cast<char*>(&magic), sizeof(magic));
			in.read(reinterpret_cast<char*>(&version), sizeof(version));
			in.read(reinterpret_cast<char*>(&configHash), sizeof(configHash));
			in.read(reinterpret_cast<char*>(&width), sizeof(width));
			in.read(reinterpret_cast<char*>(&height), sizeof(height));
			if (!in)
				return nullptr;

			std::vector<uint8_t> pixels((std::size_t)width * height * 4);
			in.read(reinterpret_cast<char*>(pixels.data()), (std::streamsize)pixels.size());
			if (!in)
				return nullptr;

			return Texture2D::Create(pixels.data(), width, height);
		}

		void WriteThumbToDisk(State& s, const AssetMetadata& metadata, uint64_t configHash,
			const uint8_t* pixels, uint32_t width, uint32_t height)
		{
			const std::filesystem::path path = ThumbPath(metadata.FilePath);
			std::error_code ec;
			std::filesystem::create_directories(path.parent_path(), ec);

			uint64_t previous = 0;
			if (std::filesystem::exists(path, ec))
				previous = (uint64_t)std::filesystem::file_size(path, ec);

			std::ofstream out(path, std::ios::binary | std::ios::trunc);
			if (!out)
				return;

			const uint32_t magic = kThumbMagic;
			const uint32_t version = kThumbVersion;
			out.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
			out.write(reinterpret_cast<const char*>(&version), sizeof(version));
			out.write(reinterpret_cast<const char*>(&configHash), sizeof(configHash));
			out.write(reinterpret_cast<const char*>(&width), sizeof(width));
			out.write(reinterpret_cast<const char*>(&height), sizeof(height));
			out.write(reinterpret_cast<const char*>(pixels),
				(std::streamsize)width * height * 4);
			out.close();

			if (!s.DiskBytesKnown)
				return;

			const uint64_t next = (uint64_t)std::filesystem::file_size(path, ec);
			if (s.DiskBytes >= previous)
				s.DiskBytes -= previous;
			else
				s.DiskBytes = 0;
			s.DiskBytes += next;
		}

		void SaveCamera(State& s)
		{
			if (!IsAssetHandleValid(s.Handle))
				return;

			OrbitState orbit;
			s.Camera.GetOrbitState(orbit.Pitch, orbit.Yaw, orbit.Distance, orbit.Focal);
			s.Cameras[s.Handle] = orbit;
		}

		void FrameMesh(EditorCamera& camera, uint32_t size, const Mesh& mesh)
		{
			const AABB& bounds = mesh.GetBounds();
			const glm::vec3 center = (bounds.Min + bounds.Max) * 0.5f;
			const float radius = glm::max(glm::length(bounds.Max - bounds.Min) * 0.5f, 0.05f);

			camera.SetViewportSize((float)size, (float)size);
			camera.SetOrbitState(0.40f, 0.70f, 10.0f, center);
			camera.Frame(center, radius);
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

		void SubmitPreviewLighting(State& s)
		{
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
		}

		void SubmitPreviewMesh(const Ref<Mesh>& mesh)
		{
			// Rest palette, not SubmitMesh: LocalTransform without InverseBind is the
			// centimetre-character that made Meshy thumbnails empty.
			if (mesh->HasSkeleton())
				Renderer3D::SubmitSkinnedMesh(mesh, glm::mat4(1.0f), nullptr, 0);
			else
				Renderer3D::SubmitMesh(mesh, glm::mat4(1.0f));
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
			{
				FrameMesh(s.Camera, kPreviewSize, *mesh);
				s.Framed = true;
			}

			s.Renderer->BeginFrame();
			Renderer3D::BeginScene(s.Camera);
			SubmitPreviewLighting(s);
			SubmitPreviewMesh(mesh);

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

		bool RenderThumbnail(State& s, AssetHandle handle, const Ref<Mesh>& mesh,
			const AssetMetadata& metadata)
		{
			if (s.Pending.Active)
				return false;

			if (!s.ThumbRenderer)
			{
				s.ThumbRenderer = CreateRef<SceneRenderer>(kThumbSize, kThumbSize,
					RenderPass::ThumbnailViewBase, kThumbPaletteBase);
				s.ThumbRenderer->GetSettings().ClearColor = { 0.12f, 0.12f, 0.14f, 1.0f };
			}

			FrameMesh(s.ThumbCamera, kThumbSize, *mesh);

			s.ThumbRenderer->BeginFrame();
			Renderer3D::BeginScene(s.ThumbCamera);
			SubmitPreviewLighting(s);
			SubmitPreviewMesh(mesh);
			Renderer3D::EndScene();
			s.ThumbRenderer->EndFrame();

			s.Pending.Handle = handle;
			s.Pending.ConfigHash = CompiledCache::HashConfig(metadata.Config);
			s.Pending.Pixels.assign((std::size_t)kThumbSize * kThumbSize * 4, 0);

			const uint16_t blitView = (uint16_t)(RenderPass::ThumbnailViewBase
				+ RenderPass::SceneViewCount);
			bgfx::setViewRect(blitView, 0, 0, (uint16_t)kThumbSize, (uint16_t)kThumbSize);
			s.Pending.ReadyFrame = s.ThumbRenderer->GetCompositeFramebuffer()->RequestImageRead(
				blitView, 0, s.Pending.Pixels.data());
			if (s.Pending.ReadyFrame == 0)
			{
				s.Pending.Active = false;
				return false;
			}

			s.Pending.Active = true;
			s.BudgetUsed++;
			s.ThumbRenderCount++;
			return true;
		}

		void PollReadback(State& s)
		{
			if (!s.Pending.Active)
				return;
			if (Renderer::GetFrameNumber() < s.Pending.ReadyFrame)
				return;

			s.Pending.Active = false;

			const AssetMetadata* metadata = AssetManager::GetMetadata(s.Pending.Handle);
			if (!metadata || metadata->Type != AssetType::StaticMesh)
				return;

			if (bgfx::getCaps()->originBottomLeft)
				FlipY(s.Pending.Pixels.data(), kThumbSize, kThumbSize);

			Ref<Texture2D> texture = Texture2D::Create(
				s.Pending.Pixels.data(), kThumbSize, kThumbSize);
			RememberGpu(s, s.Pending.Handle, texture);
			WriteThumbToDisk(s, *metadata, s.Pending.ConfigHash,
				s.Pending.Pixels.data(), kThumbSize, kThumbSize);
		}

	}

	void AssetPreview::Init()
	{
		State& s = Get();
		s.Camera.SetViewportSize((float)kPreviewSize, (float)kPreviewSize);
		s.ThumbCamera.SetViewportSize((float)kThumbSize, (float)kThumbSize);

		AssetManager::AddAssetChangedListener([](AssetHandle handle, AssetType type)
		{
			State& state = Get();

			if (type == AssetType::StaticMesh)
			{
				EvictGpu(state, handle);
				if (const AssetMetadata* metadata = AssetManager::GetMetadata(handle))
					DeleteThumbFile(state, *metadata);
			}
			else if (type == AssetType::Material || type == AssetType::Texture
				|| type == AssetType::Environment)
			{
				state.Gpu.clear();
				state.GpuOrder.clear();
				state.SkipDisk = true;
				if (state.Pending.Active)
					state.Pending.Active = false;
			}

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
		s.ThumbRenderer.reset();
		s.Studio.reset();
		s.Gpu.clear();
		s.GpuOrder.clear();
		s.Pending.Active = false;
		s.Pending.Pixels.clear();
		s.Visible.clear();
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

		PollReadback(s);

		std::unordered_set<AssetHandle> wanted = std::move(s.Visible);
		s.Visible.clear();

		uint32_t diskLoads = 0;
		for (AssetHandle handle : wanted)
		{
			if (s.Gpu.find(handle) != s.Gpu.end())
				continue;

			const AssetMetadata* metadata = AssetManager::GetMetadata(handle);
			if (!metadata || metadata->Type != AssetType::StaticMesh)
				continue;

			if (diskLoads >= kDiskLoadsPerFrame)
				break;

			Ref<Texture2D> loaded = LoadThumbFromDisk(s, *metadata);
			if (loaded)
			{
				RememberGpu(s, handle, loaded);
				diskLoads++;
			}
		}

		if (s.Dirty)
		{
			if (s.Type == AssetType::StaticMesh && IsAssetHandleValid(s.Handle)
				&& s.BudgetUsed < kRendersPerFrame)
			{
				Ref<Mesh> mesh = AssetManager::GetAsset<Mesh>(s.Handle);
				if (mesh)
					RenderMesh(s, mesh);
			}
			else if (s.Type != AssetType::StaticMesh || !IsAssetHandleValid(s.Handle))
			{
				s.Dirty = false;
				EvictRenderer(s);
			}
		}

		// Inspector pending on a load still owns the budget: a folder of meshes
		// must not starve the selection the author is looking at.
		if (!s.GridIdle || s.BudgetUsed >= kRendersPerFrame || s.Pending.Active || s.Dirty)
			return;

		for (AssetHandle handle : wanted)
		{
			if (s.Gpu.find(handle) != s.Gpu.end())
				continue;
			if (s.Pending.Active && s.Pending.Handle == handle)
				continue;

			const AssetMetadata* metadata = AssetManager::GetMetadata(handle);
			if (!metadata || metadata->Type != AssetType::StaticMesh)
				continue;
			if (!s.SkipDisk && DiskThumbCurrent(*metadata))
				continue;

			Ref<Mesh> mesh = AssetManager::GetAsset<Mesh>(handle);
			if (!mesh)
				return;

			RenderThumbnail(s, handle, mesh, *metadata);
			return;
		}
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

	void AssetPreview::RequestVisible(AssetHandle handle)
	{
		if (IsAssetHandleValid(handle))
			Get().Visible.insert(handle);
	}

	void AssetPreview::SetGridIdle(bool idle)
	{
		Get().GridIdle = idle;
	}

	Ref<Texture2D> AssetPreview::GetThumbnail(AssetHandle handle)
	{
		State& s = Get();
		auto it = s.Gpu.find(handle);
		if (it == s.Gpu.end())
			return nullptr;
		return it->second;
	}

	uint32_t AssetPreview::ThumbnailRenderCount()
	{
		return Get().ThumbRenderCount;
	}

	std::size_t AssetPreview::ThumbnailResident()
	{
		return Get().Gpu.size();
	}

	uint64_t AssetPreview::ThumbnailDiskBytes()
	{
		State& s = Get();
		if (!s.DiskBytesKnown)
		{
			s.DiskBytes = ScanThumbBytes();
			s.DiskBytesKnown = true;
		}
		return s.DiskBytes;
	}

}
