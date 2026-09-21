#pragma once

#include "GanymedE/Assets/AssetTypes.h"
#include "GanymedE/Math/BoundingVolumes.h"

#include <filesystem>
#include <string>
#include <vector>

namespace GanymedE {

	class AssetInspectorPanel
	{
	public:
		void SetSelectedPath(const std::filesystem::path& absolutePath);
		void OnImGuiRender();
	private:
		enum class SidecarState { Missing, Present, Quarantined };

		struct SubmeshRow
		{
			uint32_t Index = 0;
			std::string Name;
			uint32_t MaterialIndex = 0;
			uint32_t TriangleCount = 0;
			AABB Bounds;
		};

		struct ClipRow
		{
			std::string Name;
			float Duration = 0.0f;
		};

		struct MeshCache
		{
			AssetHandle Handle = InvalidAssetHandle;
			bool Ready = false;
			uint32_t VertexCount = 0;
			uint32_t IndexCount = 0;
			uint32_t TriangleCount = 0;
			AABB Bounds;
			bool HasSkin = false;
			uint32_t JointCount = 0;
			std::vector<SubmeshRow> Submeshes;
			std::vector<ClipRow> Clips;
			bool MirroredUvShells = false;
			bool Inspected = false;
			uint32_t SourceSkinCount = 0;
			bool SourceMissingTangent = false;
			bool SourceHasNormalMap = false;
		};

		void ResolveSelection();
		void DrawHeader();
		void DrawMeshBody();
		void DrawTextureBody();
		void DrawMaterialBody();
		void EnsureMeshCache();

		std::filesystem::path m_Absolute;
		std::filesystem::path m_Relative;
		AssetHandle m_Handle = InvalidAssetHandle;
		AssetType m_Type = AssetType::None;
		bool m_IsDirectory = false;
		SidecarState m_Sidecar = SidecarState::Missing;
		uint64_t m_FileSize = 0;

		MeshCache m_Mesh;
	};

}
