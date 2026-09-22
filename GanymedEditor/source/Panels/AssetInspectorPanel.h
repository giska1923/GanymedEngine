#pragma once

#include "GanymedE/Assets/AssetCompiler.h"
#include "GanymedE/Assets/AssetTypes.h"
#include "GanymedE/Math/BoundingVolumes.h"
#include "GanymedE/Renderer/Animation.h"

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

		struct ClipScaleHit
		{
			std::string Joint;
			glm::vec3 Scale{ 1.0f };
			bool Uniform = true;
		};

		struct ClipRow
		{
			std::string Name;
			float Duration = 0.0f;
			uint32_t ChannelCount = 0;
			uint32_t JointsAnimated = 0;
			bool HasTranslation = false;
			bool HasRotation = false;
			bool HasScale = false;
			std::vector<ClipScaleHit> ConstantScale;
			glm::vec3 RootNet{ 0.0f };
			glm::vec3 RootResidualMax{ 0.0f };
			float HeadY = 0.0f;
			float HipsY = 0.0f;
			float HipsZ = 0.0f;
			bool HasHead = false;
			bool HasHips = false;
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
			bool OverJointLimit = false;
			std::string RootJointName;
			std::string HipsJointName;
			std::string HeadJointName;
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
		void DrawImportSettings();
		void DrawTextureImportSettings();
		void DrawMeshImportSettings();
		void CommitConfig(const AssetConfig& keys);
		void Reimport();
		void EnsureMeshCache();
		void FillClipRow(const Skeleton& skeleton, const glm::mat4& skinTransform,
			const AnimationClip& clip, int32_t rootJoint, int32_t hipsJoint, int32_t headJoint,
			std::vector<JointPose>& locals, std::vector<glm::mat4>& globals, ClipRow& row);

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
