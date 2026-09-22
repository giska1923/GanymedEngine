#include "GanymedE/Core/Log.h"
#include "AssetInspectorPanel.h"

#include "../AssetPreview.h"
#include "../EditorInspector.h"
#include "../EditorTheme.h"
#include "../EditorWidgets.h"

#include "GanymedE/Assets/AssetManager.h"
#include "GanymedE/Assets/AssetMeta.h"
#include "GanymedE/Assets/AssetPaths.h"
#include "GanymedE/Assets/CompiledCache.h"
#include "GanymedE/Assets/MaterialSerializer.h"
#include "GanymedE/Assets/TextureCompiler.h"
#include "GanymedE/Renderer/Material.h"
#include "GanymedE/Renderer/Mesh.h"
#include "GanymedE/Renderer/MeshImporter.h"
#include "GanymedE/Renderer/Texture.h"

#include <bgfx/bgfx.h>
#include <imgui/imgui.h>

#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_set>

namespace GanymedE {

	namespace {

		const char* SidecarLabel(int state)
		{
			switch (state)
			{
				case 1: return "present";
				case 2: return "quarantined";
				default: return "missing";
			}
		}

		const char* CompiledLabel(CompiledCache::OutputStatus status)
		{
			switch (status)
			{
				case CompiledCache::OutputStatus::None:    return "n/a (source is the artifact)";
				case CompiledCache::OutputStatus::Missing: return "missing";
				case CompiledCache::OutputStatus::Stale:   return "stale";
				case CompiledCache::OutputStatus::Current: return "current epoch";
			}
			return "missing";
		}

		std::string FormatBytes(uint64_t bytes)
		{
			if (bytes < 1024)
				return std::to_string(bytes) + " B";

			const char* units[] = { "KB", "MB", "GB" };
			double value = (double)bytes / 1024.0;
			int unit = 0;
			while (value >= 1024.0 && unit < 2)
			{
				value /= 1024.0;
				++unit;
			}

			char buf[32];
			std::snprintf(buf, sizeof(buf), value >= 10.0 ? "%.0f %s" : "%.1f %s", value, units[unit]);
			return buf;
		}

		const char* GpuFormatName(uint32_t format)
		{
			switch ((bgfx::TextureFormat::Enum)format)
			{
				case bgfx::TextureFormat::BC1:   return "BC1";
				case bgfx::TextureFormat::BC3:   return "BC3";
				case bgfx::TextureFormat::BC4:   return "BC4";
				case bgfx::TextureFormat::BC5:   return "BC5";
				case bgfx::TextureFormat::BC7:   return "BC7";
				case bgfx::TextureFormat::RGBA8: return "RGBA8";
				case bgfx::TextureFormat::BGRA8: return "BGRA8";
				default:                         return "unknown";
			}
		}

		std::string SourceFormatFromExtension(const std::filesystem::path& path)
		{
			std::string ext = path.extension().string();
			if (ext.empty())
				return "unknown";
			if (ext[0] == '.')
				ext.erase(0, 1);
			for (char& c : ext)
				c = (char)std::toupper((unsigned char)c);
			return ext;
		}

		bool TriangleIsMirrored(const MeshVertex& v0, const MeshVertex& v1, const MeshVertex& v2)
		{
			const glm::vec3 edge1 = v1.Position - v0.Position;
			const glm::vec3 edge2 = v2.Position - v0.Position;
			const glm::vec2 duv1 = v1.TexCoord - v0.TexCoord;
			const glm::vec2 duv2 = v2.TexCoord - v0.TexCoord;

			const float uvDet = duv1.x * duv2.y - duv2.x * duv1.y;
			const float geoDet = glm::dot(glm::cross(edge1, edge2), v0.Normal);
			if (std::abs(uvDet) < 1e-12f || std::abs(geoDet) < 1e-12f)
				return false;
			return uvDet * geoDet < 0.0f;
		}

		bool MeshHasMirroredUvShells(const Mesh& mesh)
		{
			const auto& vertices = mesh.GetVertices();
			const auto& indices = mesh.GetIndices();
			for (const Submesh& submesh : mesh.GetSubmeshes())
			{
				for (uint32_t i = 0; i + 2 < submesh.IndexCount; i += 3)
				{
					const uint32_t i0 = submesh.BaseVertex + indices[submesh.BaseIndex + i + 0];
					const uint32_t i1 = submesh.BaseVertex + indices[submesh.BaseIndex + i + 1];
					const uint32_t i2 = submesh.BaseVertex + indices[submesh.BaseIndex + i + 2];
					if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size())
						continue;
					if (TriangleIsMirrored(vertices[i0], vertices[i1], vertices[i2]))
						return true;
				}
			}
			return false;
		}

		void WarningLine(const char* text)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, EditorUI::Color(EditorUI::Theme().Warning));
			ImGui::TextWrapped("%s", text);
			ImGui::PopStyleColor();
		}

		bool IEquals(const std::string& a, const char* b)
		{
			size_t i = 0;
			for (; b[i]; i++)
			{
				if (i >= a.size())
					return false;
				if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
					return false;
			}
			return i == a.size();
		}

		bool IEndsWithToken(const std::string& a, const char* token)
		{
			const size_t n = std::strlen(token);
			if (a.size() < n)
				return false;
			const size_t start = a.size() - n;
			for (size_t i = 0; i < n; i++)
			{
				if (std::tolower((unsigned char)a[start + i]) != std::tolower((unsigned char)token[i]))
					return false;
			}
			if (start == 0)
				return true;
			return !std::isalnum((unsigned char)a[start - 1]);
		}

		bool IContains(const std::string& a, const char* token)
		{
			const size_t n = std::strlen(token);
			if (n == 0 || a.size() < n)
				return false;
			for (size_t start = 0; start + n <= a.size(); start++)
			{
				bool match = true;
				for (size_t i = 0; i < n; i++)
				{
					if (std::tolower((unsigned char)a[start + i]) != std::tolower((unsigned char)token[i]))
					{
						match = false;
						break;
					}
				}
				if (match)
					return true;
			}
			return false;
		}

		int32_t FindJointNamed(const Skeleton& skeleton, const char* token, bool skipHeadAffixes)
		{
			int32_t suffix = -1;
			for (uint32_t i = 0; i < skeleton.JointCount(); i++)
			{
				if (i >= skeleton.JointNames.size())
					break;
				const std::string& name = skeleton.JointNames[i];
				if (skipHeadAffixes && (IContains(name, "Top") || IContains(name, "Hint")
					|| IEndsWithToken(name, "End")))
				{
					continue;
				}
				if (IEquals(name, token))
					return (int32_t)i;
				if (suffix < 0 && IEndsWithToken(name, token))
					suffix = (int32_t)i;
			}
			return suffix;
		}

		int32_t FindRootJoint(const Skeleton& skeleton)
		{
			for (uint32_t i = 0; i < skeleton.JointCount(); i++)
			{
				if (i >= skeleton.ParentIndices.size())
					break;
				if (skeleton.ParentIndices[i] < 0)
					return (int32_t)i;
			}
			return skeleton.JointCount() > 0 ? 0 : -1;
		}

		int32_t FindHipsJoint(const Skeleton& skeleton)
		{
			int32_t hips = FindJointNamed(skeleton, "Hips", false);
			if (hips < 0)
				hips = FindJointNamed(skeleton, "Pelvis", false);
			return hips >= 0 ? hips : FindRootJoint(skeleton);
		}

		int32_t FindHeadJoint(const Skeleton& skeleton)
		{
			int32_t head = FindJointNamed(skeleton, "Head", true);
			if (head < 0)
				head = FindJointNamed(skeleton, "Head", false);
			return head;
		}

		glm::vec3 Vec3Nearly(const glm::vec4& v)
		{
			return glm::vec3(v);
		}

		bool Vec3Equal(const glm::vec3& a, const glm::vec3& b, float eps = 1.0e-4f)
		{
			return glm::all(glm::lessThanEqual(glm::abs(a - b), glm::vec3(eps)));
		}

		const char* JointName(const Skeleton& skeleton, int32_t joint)
		{
			if (joint < 0 || (uint32_t)joint >= skeleton.JointNames.size())
				return "(unknown)";
			return skeleton.JointNames[(uint32_t)joint].c_str();
		}

	}

	void AssetInspectorPanel::SetSelectedPath(const std::filesystem::path& absolutePath)
	{
		if (m_Absolute == absolutePath)
			return;

		m_Absolute = absolutePath;
		m_Mesh = {};
		ResolveSelection();
	}

	void AssetInspectorPanel::ResolveSelection()
	{
		m_Relative.clear();
		m_Handle = InvalidAssetHandle;
		m_Type = AssetType::None;
		m_IsDirectory = false;
		m_Sidecar = SidecarState::Missing;
		m_FileSize = 0;

		if (m_Absolute.empty())
		{
			AssetPreview::SetSelection(InvalidAssetHandle, AssetType::None);
			return;
		}

		std::error_code ec;
		m_IsDirectory = std::filesystem::is_directory(m_Absolute, ec);
		m_Relative = MakeAssetRelative(m_Absolute);

		if (!m_IsDirectory)
		{
			m_FileSize = 0;
			if (std::filesystem::is_regular_file(m_Absolute, ec))
				m_FileSize = (uint64_t)std::filesystem::file_size(m_Absolute, ec);

			m_Handle = AssetManager::GetHandle(m_Relative.generic_string());
			if (const AssetMetadata* metadata = AssetManager::GetMetadata(m_Handle))
				m_Type = metadata->Type;
			else
				m_Type = AssetTypeFromExtension(m_Absolute.extension().string());
		}

		// Disk, not the in-memory index: a deleted sidecar must read as missing rather than
		// being silently re-minted by ImportAsset or ScanAssets.
		const std::filesystem::path sidecar = AssetMetaSerializer::SidecarPath(m_Absolute);
		std::filesystem::path quarantined = sidecar;
		quarantined += ".bad";
		if (std::filesystem::exists(sidecar, ec))
			m_Sidecar = SidecarState::Present;
		else if (std::filesystem::exists(quarantined, ec))
			m_Sidecar = SidecarState::Quarantined;
		else
			m_Sidecar = SidecarState::Missing;

		AssetPreview::SetSelection(m_Handle, m_Type);
	}

	void AssetInspectorPanel::OnImGuiRender()
	{
		using EditorUI::BeginPanel;
		using EditorUI::EndPanel;
		using EditorUI::BeginPanelBody;
		using EditorUI::EndPanelBody;

		if (BeginPanel("Asset Inspector"))
		{
			BeginPanelBody();
			if (m_Absolute.empty())
			{
				ImGui::TextDisabled("Select an asset in the Content Browser");
			}
			else
			{
				DrawHeader();
				if (!m_IsDirectory)
				{
					ImGui::Separator();
					switch (m_Type)
					{
						case AssetType::StaticMesh: DrawMeshBody();     break;
						case AssetType::Texture:    DrawTextureBody();  break;
						case AssetType::Material:   DrawMaterialBody(); break;
						default:
							ImGui::TextDisabled("No inspector for this type");
							break;
					}
				}
			}
			EndPanelBody();
		}
		EndPanel();
	}

	void AssetInspectorPanel::DrawHeader()
	{
		ImGui::TextUnformatted(m_Relative.empty()
			? m_Absolute.generic_string().c_str()
			: m_Relative.generic_string().c_str());

		if (m_IsDirectory)
		{
			ImGui::TextDisabled("Folder");
			return;
		}

		ImGui::Text("Type: %s", AssetTypeToString(m_Type));

		if (IsAssetHandleValid(m_Handle))
			ImGui::Text("Handle: %" PRIu64, static_cast<uint64_t>(m_Handle));
		else
			ImGui::TextDisabled("Handle: not indexed");

		ImGui::Text("File size: %s", FormatBytes(m_FileSize).c_str());

		const char* sidecar = SidecarLabel((int)m_Sidecar);
		if (m_Sidecar == SidecarState::Present)
			ImGui::Text("Sidecar: %s", sidecar);
		else
			WarningLine((std::string("Sidecar: ") + sidecar).c_str());

		if (const AssetMetadata* metadata = AssetManager::GetMetadata(m_Handle))
			ImGui::Text("Compiled: %s", CompiledLabel(CompiledCache::QueryOutput(*metadata)));
		else
			ImGui::TextDisabled("Compiled: not indexed");

		if (m_Type == AssetType::StaticMesh || m_Type == AssetType::Texture)
			DrawImportSettings();
	}

	void AssetInspectorPanel::CommitConfig(const AssetConfig& keys)
	{
		if (!AssetManager::SetAssetConfig(m_Handle, keys))
			return;
		AssetPreview::MarkDirty();

		// Collision is authoring metadata: SetAssetConfig does not Reload, so the
		// resident mesh is still valid. Import settings do Reload, and the cache
		// has to drop so the next frame picks up the new bounds.
		for (const auto& [key, value] : keys)
		{
			if (ConfigAffectsCompile(key))
			{
				m_Mesh.Ready = false;
				break;
			}
		}
		ResolveSelection();
	}

	void AssetInspectorPanel::Reimport()
	{
		if (!IsAssetHandleValid(m_Handle))
			return;
		AssetManager::Reload(m_Handle);
		m_Mesh.Ready = false;
		ResolveSelection();
		AssetPreview::MarkDirty();
	}

	void AssetInspectorPanel::DrawImportSettings()
	{
		if (!IsAssetHandleValid(m_Handle))
			return;

		if (!ImGui::CollapsingHeader("Import Settings", ImGuiTreeNodeFlags_DefaultOpen))
			return;

		ImGui::TextDisabled("Edits apply to this asset everywhere it is used, and are not undoable");

		const bool writable = AssetManager::IsAssetsWritable();
		if (!writable)
			ImGui::BeginDisabled();

		if (m_Type == AssetType::Texture)
			DrawTextureImportSettings();
		else
			DrawMeshImportSettings();

		if (ImGui::Button("Reimport"))
			Reimport();
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Force a recompile even if nothing changed.\nUse when a source edit did not trip the watcher.");

		if (!writable)
			ImGui::EndDisabled();
	}

	void AssetInspectorPanel::DrawTextureImportSettings()
	{
		const AssetMetadata* metadata = AssetManager::GetMetadata(m_Handle);
		const AssetConfig empty;
		const AssetConfig& config = metadata ? metadata->Config : empty;

		const char* formats[] = { "auto", "BC1", "BC3", "BC5", "BC7", "RGBA8" };
		const char* formatLabels[] = { "auto", "BC1", "BC3", "BC5", "BC7", "raw (RGBA8)" };
		std::string format = ConfigString(config, "Format", TextureImportSettings::Format);
		int formatIndex = 0;
		for (int i = 0; i < 6; i++)
		{
			if (format == formats[i])
			{
				formatIndex = i;
				break;
			}
		}

		if (ImGui::BeginCombo("Format", formatLabels[formatIndex]))
		{
			for (int i = 0; i < 6; i++)
			{
				if (ImGui::Selectable(formatLabels[i], formatIndex == i))
					CommitConfig({ { "Format", formats[i] } });
			}
			ImGui::EndCombo();
		}

		bool normalMap = ConfigBool(config, "NormalMap", TextureImportSettings::NormalMap);
		if (ImGui::Checkbox("Normal Map", &normalMap))
			CommitConfig({ { "NormalMap", normalMap ? "true" : "false" } });

		bool generateMips = ConfigBool(config, "GenerateMips", TextureImportSettings::GenerateMips);
		if (ImGui::Checkbox("Generate Mips", &generateMips))
			CommitConfig({ { "GenerateMips", generateMips ? "true" : "false" } });

		int maxSize = ConfigInt(config, "MaxSize", TextureImportSettings::MaxSize);
		ImGui::DragInt("Max Size", &maxSize, 1.0f, 0, 16384);
		if (ImGui::IsItemDeactivatedAfterEdit())
		{
			if (maxSize < 0)
				maxSize = 0;
			CommitConfig({ { "MaxSize", std::to_string(maxSize) } });
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Longest edge in pixels. 0 = no limit.\nApplied by dropping top mips, not resampling.");
	}

	void AssetInspectorPanel::DrawMeshImportSettings()
	{
		const AssetMetadata* metadata = AssetManager::GetMetadata(m_Handle);
		const AssetConfig empty;
		const AssetConfig& config = metadata ? metadata->Config : empty;

		float scale = ConfigFloat(config, "ImportScale", MeshImportSettings::ImportScale);
		ImGui::DragFloat("Import Scale", &scale, 0.01f, 0.0001f, 1000.0f, "%.4f");
		if (ImGui::IsItemDeactivatedAfterEdit())
		{
			if (scale <= 0.0f)
				scale = MeshImportSettings::ImportScale;
			char buf[32];
			std::snprintf(buf, sizeof(buf), "%.6g", scale);
			CommitConfig({ { "ImportScale", buf } });
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Uniform scale baked at import.\nRescales every placement of this mesh and is not undoable.");

		const char* policies[] = { "WhenMissing", "Always", "Never" };
		std::string policy = ConfigString(config, "TangentPolicy", MeshImportSettings::TangentPolicy);
		int policyIndex = 0;
		for (int i = 0; i < 3; i++)
		{
			if (policy == policies[i])
			{
				policyIndex = i;
				break;
			}
		}
		if (ImGui::BeginCombo("Tangent Policy", policies[policyIndex]))
		{
			for (int i = 0; i < 3; i++)
			{
				if (ImGui::Selectable(policies[i], policyIndex == i))
					CommitConfig({ { "TangentPolicy", policies[i] } });
			}
			ImGui::EndCombo();
		}

		const char* axes[] = { "Y", "Z" };
		std::string axis = ConfigString(config, "UpAxis", MeshImportSettings::UpAxis);
		int axisIndex = (axis == "Z") ? 1 : 0;
		if (ImGui::BeginCombo("Up Axis", axes[axisIndex]))
		{
			for (int i = 0; i < 2; i++)
			{
				if (ImGui::Selectable(axes[i], axisIndex == i))
					CommitConfig({ { "UpAxis", axes[i] } });
			}
			ImGui::EndCombo();
		}

		ImGui::Separator();
		const char* collisions[] = { "None", "Box" };
		std::string collision = ConfigString(config, "Collision", MeshImportSettings::Collision);
		int collisionIndex = (collision == "Box") ? 1 : 0;
		if (ImGui::BeginCombo("Collision", collisions[collisionIndex]))
		{
			for (int i = 0; i < 2; i++)
			{
				if (ImGui::Selectable(collisions[i], collisionIndex == i))
					CommitConfig({ { "Collision", collisions[i] } });
			}
			ImGui::EndCombo();
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip(
				"Seed for new placements, not an override.\n"
				"Box: drag-drop / map place / scatter add a fitted BoxCollider.\n"
				"Existing entities keep the component they already have.");

		EnsureMeshCache();
		if (m_Mesh.Ready)
		{
			glm::vec3 halfExtents, offset;
			MeshCollision::FitFromAABB(m_Mesh.Bounds, halfExtents, offset);
			ImGui::Text("Fitted box: half (%.3f, %.3f, %.3f)  offset (%.3f, %.3f, %.3f)",
				halfExtents.x, halfExtents.y, halfExtents.z,
				offset.x, offset.y, offset.z);
			ImGui::TextDisabled("From Mesh::GetBounds(), computed at import. No 3D overlay until P5.");
		}
	}

	void AssetInspectorPanel::EnsureMeshCache()
	{
		if (m_Mesh.Handle != m_Handle)
		{
			m_Mesh = {};
			m_Mesh.Handle = m_Handle;
		}

		if (m_Mesh.Ready)
			return;

		Ref<Mesh> mesh = AssetManager::GetAsset<Mesh>(m_Handle);
		if (!mesh)
			return;

		m_Mesh.VertexCount = (uint32_t)mesh->GetVertices().size();
		m_Mesh.IndexCount = (uint32_t)mesh->GetIndices().size();
		m_Mesh.Bounds = mesh->GetBounds();
		m_Mesh.HasSkin = mesh->HasSkeleton() && !mesh->GetSkinVertices().empty();
		m_Mesh.JointCount = mesh->GetSkeleton().JointCount();
		m_Mesh.OverJointLimit = m_Mesh.JointCount > Skeleton::MaxBones;
		m_Mesh.RootJointName.clear();
		m_Mesh.HipsJointName.clear();
		m_Mesh.HeadJointName.clear();

		m_Mesh.TriangleCount = 0;
		m_Mesh.Submeshes.clear();
		m_Mesh.Submeshes.reserve(mesh->GetSubmeshes().size());
		for (uint32_t i = 0; i < (uint32_t)mesh->GetSubmeshes().size(); i++)
		{
			const Submesh& submesh = mesh->GetSubmeshes()[i];
			SubmeshRow row;
			row.Index = i;
			row.Name = submesh.Name;
			row.MaterialIndex = submesh.MaterialIndex;
			row.TriangleCount = submesh.IndexCount / 3;
			row.Bounds = submesh.Bounds;
			m_Mesh.TriangleCount += row.TriangleCount;
			m_Mesh.Submeshes.push_back(std::move(row));
		}

		m_Mesh.Clips.clear();
		if (mesh->HasSkeleton())
		{
			const Skeleton& skeleton = mesh->GetSkeleton();
			const int32_t rootJoint = FindRootJoint(skeleton);
			const int32_t hipsJoint = FindHipsJoint(skeleton);
			const int32_t headJoint = FindHeadJoint(skeleton);
			if (rootJoint >= 0)
				m_Mesh.RootJointName = JointName(skeleton, rootJoint);
			if (hipsJoint >= 0)
				m_Mesh.HipsJointName = JointName(skeleton, hipsJoint);
			if (headJoint >= 0)
				m_Mesh.HeadJointName = JointName(skeleton, headJoint);

			std::vector<JointPose> locals;
			std::vector<glm::mat4> globals;
			m_Mesh.Clips.reserve(mesh->GetClips().size());
			for (const AnimationClip& clip : mesh->GetClips())
			{
				ClipRow row;
				FillClipRow(skeleton, clip, rootJoint, hipsJoint, headJoint, locals, globals, row);
				m_Mesh.Clips.push_back(std::move(row));
			}
		}

		m_Mesh.MirroredUvShells = MeshHasMirroredUvShells(*mesh);
		m_Mesh.Ready = true;
	}

	void AssetInspectorPanel::FillClipRow(const Skeleton& skeleton, const AnimationClip& clip,
		int32_t rootJoint, int32_t hipsJoint, int32_t headJoint,
		std::vector<JointPose>& locals, std::vector<glm::mat4>& globals, ClipRow& row)
	{
		row = {};
		row.Name = clip.Name;
		row.Duration = clip.Duration;
		row.ChannelCount = (uint32_t)clip.Channels.size();

		std::unordered_set<uint32_t> joints;
		for (const AnimationClip::Channel& channel : clip.Channels)
		{
			joints.insert(channel.Joint);
			switch (channel.Target)
			{
				case AnimationClip::Channel::Path::Translation: row.HasTranslation = true; break;
				case AnimationClip::Channel::Path::Rotation:    row.HasRotation = true; break;
				case AnimationClip::Channel::Path::Scale:       row.HasScale = true; break;
			}

			if (channel.Target != AnimationClip::Channel::Path::Scale || channel.Values.empty())
				continue;

			const glm::vec3 first = Vec3Nearly(channel.Values.front());
			bool constant = true;
			for (const glm::vec4& value : channel.Values)
			{
				if (!Vec3Equal(Vec3Nearly(value), first))
				{
					constant = false;
					break;
				}
			}
			if (!constant || Vec3Equal(first, glm::vec3(1.0f)))
				continue;

			ClipScaleHit hit;
			hit.Joint = JointName(skeleton, (int32_t)channel.Joint);
			hit.Scale = first;
			hit.Uniform = std::abs(first.x - first.y) < 1.0e-4f
				&& std::abs(first.y - first.z) < 1.0e-4f;
			row.ConstantScale.push_back(std::move(hit));
		}
		row.JointsAnimated = (uint32_t)joints.size();

		if (rootJoint < 0 || !SampleClipGlobals(skeleton, &clip, 0.0f, locals, globals))
			return;

		auto origin = [](const glm::mat4& m) { return glm::vec3(m[3]); };

		const glm::vec3 start = origin(globals[(uint32_t)rootJoint]);
		if (!SampleClipGlobals(skeleton, &clip, clip.Duration, locals, globals))
			return;
		const glm::vec3 end = origin(globals[(uint32_t)rootJoint]);
		row.RootNet = end - start;

		std::vector<float> samples;
		samples.push_back(0.0f);
		samples.push_back(clip.Duration);
		for (const AnimationClip::Channel& channel : clip.Channels)
		{
			if (channel.Target != AnimationClip::Channel::Path::Translation
				|| (int32_t)channel.Joint != rootJoint)
			{
				continue;
			}
			samples.insert(samples.end(), channel.Times.begin(), channel.Times.end());
		}
		std::sort(samples.begin(), samples.end());
		samples.erase(std::unique(samples.begin(), samples.end()), samples.end());

		const size_t maxSamples = 32;
		const size_t stride = samples.size() > maxSamples
			? (samples.size() + maxSamples - 1) / maxSamples : 1;

		glm::vec3 residualMax{ 0.0f };
		const float duration = clip.Duration > 1.0e-6f ? clip.Duration : 1.0f;
		for (size_t i = 0; i < samples.size(); i += stride)
		{
			if (!SampleClipGlobals(skeleton, &clip, samples[i], locals, globals))
				break;
			const float u = samples[i] / duration;
			const glm::vec3 expected = glm::mix(start, end, u);
			const glm::vec3 residual = glm::abs(origin(globals[(uint32_t)rootJoint]) - expected);
			residualMax = glm::max(residualMax, residual);
		}
		row.RootResidualMax = residualMax;

		if (!SampleClipGlobals(skeleton, &clip, 0.0f, locals, globals))
			return;

		if (headJoint >= 0)
		{
			row.HeadY = origin(globals[(uint32_t)headJoint]).y;
			row.HasHead = true;
		}
		if (hipsJoint >= 0)
		{
			const glm::vec3 hips = origin(globals[(uint32_t)hipsJoint]);
			row.HipsY = hips.y;
			row.HipsZ = hips.z;
			row.HasHips = true;
		}
	}

	void AssetInspectorPanel::DrawMeshBody()
	{
		if (!IsAssetHandleValid(m_Handle))
		{
			ImGui::TextDisabled("Mesh is not in the asset index");
			return;
		}

		EnsureMeshCache();
		if (!m_Mesh.Ready)
		{
			ImGui::TextDisabled("Loading...");
			return;
		}

		Ref<Mesh> mesh = AssetManager::GetAsset<Mesh>(m_Handle);
		if (!mesh)
		{
			ImGui::TextDisabled("Loading...");
			return;
		}

		if (!m_Mesh.Inspected)
		{
			MeshSourceInspect inspect;
			m_Mesh.Inspected = MeshImporter::InspectSource(m_Absolute, inspect);
			if (m_Mesh.Inspected)
			{
				m_Mesh.SourceSkinCount = inspect.SkinCount;
				m_Mesh.SourceMissingTangent = inspect.AnyPrimitiveMissingTangent;
				m_Mesh.SourceHasNormalMap = inspect.AnyNormalMap;
			}
		}

		AssetPreview::DrawInspector(ImGui::GetContentRegionAvail().x);
		ImGui::Text("Vertices: %u", m_Mesh.VertexCount);
		ImGui::Text("Indices: %u", m_Mesh.IndexCount);
		ImGui::Text("Triangles: %u", m_Mesh.TriangleCount);
		ImGui::Text("Bounds: (%.3f, %.3f, %.3f) .. (%.3f, %.3f, %.3f)",
			m_Mesh.Bounds.Min.x, m_Mesh.Bounds.Min.y, m_Mesh.Bounds.Min.z,
			m_Mesh.Bounds.Max.x, m_Mesh.Bounds.Max.y, m_Mesh.Bounds.Max.z);
		ImGui::Text("Skin data: %s", m_Mesh.HasSkin ? "present" : "none");

		if (m_Mesh.Inspected && m_Mesh.SourceSkinCount > 1)
			WarningLine("This file has more than one skin — only the first is imported.");
		if (m_Mesh.Inspected && m_Mesh.SourceHasNormalMap && m_Mesh.SourceMissingTangent)
			WarningLine("A normal map is present and the file has no TANGENT attribute — tangents were generated.");
		if (m_Mesh.MirroredUvShells)
			WarningLine("Mirrored UV shells: MeshVertex::Tangent is a vec3, so glTF's tangent w is dropped and a mirrored shell lights as though it were not.");

		if (ImGui::CollapsingHeader("Submeshes", ImGuiTreeNodeFlags_DefaultOpen))
		{
			if (ImGui::BeginTable("##submeshes", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
				| ImGuiTableFlags_SizingStretchProp))
			{
				ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 28.0f);
				ImGui::TableSetupColumn("Name");
				ImGui::TableSetupColumn("Mat", ImGuiTableColumnFlags_WidthFixed, 36.0f);
				ImGui::TableSetupColumn("Tris", ImGuiTableColumnFlags_WidthFixed, 56.0f);
				ImGui::TableSetupColumn("Bounds");
				ImGui::TableHeadersRow();

				for (const SubmeshRow& row : m_Mesh.Submeshes)
				{
					ImGui::TableNextRow();
					ImGui::TableNextColumn(); ImGui::Text("%u", row.Index);
					ImGui::TableNextColumn(); ImGui::TextUnformatted(row.Name.c_str());
					ImGui::TableNextColumn(); ImGui::Text("%u", row.MaterialIndex);
					ImGui::TableNextColumn(); ImGui::Text("%u", row.TriangleCount);
					ImGui::TableNextColumn();
					ImGui::Text("(%.2f, %.2f, %.2f)",
						row.Bounds.Max.x - row.Bounds.Min.x,
						row.Bounds.Max.y - row.Bounds.Min.y,
						row.Bounds.Max.z - row.Bounds.Min.z);
				}
				ImGui::EndTable();
			}
		}

		const auto& materials = mesh->GetMaterials();
		if (ImGui::CollapsingHeader("Materials", ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::Text("Slots: %u", (uint32_t)materials.size());
			ImGui::TextDisabled("These are the mesh's imported defaults, not per-entity overrides");

			for (uint32_t i = 0; i < (uint32_t)materials.size(); i++)
			{
				ImGui::PushID((int)i);
				ImGui::Separator();

				const std::string imported = materials[i] ? materials[i]->GetName() : std::string("(none)");
				const std::filesystem::path sidecar = MaterialSerializer::SidecarPath(
					m_Relative, i, materials[i] ? materials[i]->GetName() : std::string());
				const AssetHandle slot = AssetManager::GetHandle(sidecar.generic_string());

				if (IsAssetHandleValid(slot))
				{
					ImGui::Text("Slot %u: %s", i, sidecar.generic_string().c_str());
					EditorUI::DrawMaterialAssetEditor(slot);
				}
				else
				{
					ImGui::Text("Slot %u: (imported default: %s)", i, imported.c_str());
					ImGui::TextDisabled("No .gmat sidecar");
				}

				ImGui::PopID();
			}

			if (ImGui::Button("Generate material sidecars"))
			{
				MaterialSerializer::GenerateSidecars(mesh, m_Relative);
				m_Mesh.Ready = false;
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Writes .gmat files next to the mesh and extracts embedded textures.\nNever overwrites an existing sidecar.");
		}

		if (mesh->HasSkeleton() && ImGui::CollapsingHeader("Skeleton", ImGuiTreeNodeFlags_DefaultOpen))
		{
			if (m_Mesh.OverJointLimit)
			{
				char buf[160];
				std::snprintf(buf, sizeof(buf),
					"Joints: %u - over the %u-bone palette limit; joints past the limit will not animate.",
					m_Mesh.JointCount, Skeleton::MaxBones);
				WarningLine(buf);
			}
			else
			{
				ImGui::Text("Joints: %u", m_Mesh.JointCount);
			}

			ImGui::TextDisabled("Root %s · Hips %s · Head %s",
				m_Mesh.RootJointName.empty() ? "(none)" : m_Mesh.RootJointName.c_str(),
				m_Mesh.HipsJointName.empty() ? "(not found)" : m_Mesh.HipsJointName.c_str(),
				m_Mesh.HeadJointName.empty() ? "(not found)" : m_Mesh.HeadJointName.c_str());

			if (m_Mesh.Clips.empty())
			{
				ImGui::TextDisabled("No animation clips");
			}
			else
			{
				ImGui::TextDisabled("Read-only. Numbers are measurements, not a verdict.");

				if (ImGui::BeginTable("##clips", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
					| ImGuiTableFlags_SizingStretchProp))
				{
					ImGui::TableSetupColumn("Clip");
					ImGui::TableSetupColumn("Duration", ImGuiTableColumnFlags_WidthFixed, 72.0f);
					ImGui::TableSetupColumn("Ch", ImGuiTableColumnFlags_WidthFixed, 36.0f);
					ImGui::TableSetupColumn("Joints", ImGuiTableColumnFlags_WidthFixed, 48.0f);
					ImGui::TableSetupColumn("Paths", ImGuiTableColumnFlags_WidthFixed, 48.0f);
					ImGui::TableHeadersRow();
					for (const ClipRow& clip : m_Mesh.Clips)
					{
						ImGui::TableNextRow();
						ImGui::TableNextColumn(); ImGui::TextUnformatted(clip.Name.c_str());
						ImGui::TableNextColumn(); ImGui::Text("%.3f s", clip.Duration);
						ImGui::TableNextColumn(); ImGui::Text("%u", clip.ChannelCount);
						ImGui::TableNextColumn(); ImGui::Text("%u", clip.JointsAnimated);
						ImGui::TableNextColumn();
						ImGui::Text("%s %s %s",
							clip.HasTranslation ? "T" : "-",
							clip.HasRotation ? "R" : "-",
							clip.HasScale ? "S" : "-");
					}
					ImGui::EndTable();
				}

				bool anyConstantScale = false;
				for (const ClipRow& clip : m_Mesh.Clips)
					anyConstantScale = anyConstantScale || !clip.ConstantScale.empty();
				if (anyConstantScale)
				{
					ImGui::Spacing();
					ImGui::TextUnformatted("Constant scale");
					ImGui::TextDisabled("A Scale channel whose keys are all equal and not 1. A clip may scale a joint on purpose.");
					for (const ClipRow& clip : m_Mesh.Clips)
					{
						for (const ClipScaleHit& hit : clip.ConstantScale)
						{
							char buf[192];
							if (hit.Uniform)
							{
								std::snprintf(buf, sizeof(buf), "%s / %s  scale is constant at %.4f (not 1)",
									clip.Name.c_str(), hit.Joint.c_str(), hit.Scale.x);
							}
							else
							{
								std::snprintf(buf, sizeof(buf),
									"%s / %s  scale is constant at (%.4f, %.4f, %.4f) (not 1)",
									clip.Name.c_str(), hit.Joint.c_str(),
									hit.Scale.x, hit.Scale.y, hit.Scale.z);
							}
							WarningLine(buf);
						}
					}
				}

				ImGui::Spacing();
				ImGui::TextUnformatted("Root motion");
				ImGui::TextDisabled("Net = root translation at Duration minus t=0, mesh space. Residual is max |sample − lerp(start,end)| on any axis.");
				if (ImGui::BeginTable("##rootmotion", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
					| ImGuiTableFlags_SizingStretchProp))
				{
					ImGui::TableSetupColumn("Clip");
					ImGui::TableSetupColumn("Δx", ImGuiTableColumnFlags_WidthFixed, 64.0f);
					ImGui::TableSetupColumn("Δy", ImGuiTableColumnFlags_WidthFixed, 64.0f);
					ImGui::TableSetupColumn("Δz", ImGuiTableColumnFlags_WidthFixed, 64.0f);
					ImGui::TableSetupColumn("max |r|", ImGuiTableColumnFlags_WidthFixed, 64.0f);
					ImGui::TableHeadersRow();
					for (const ClipRow& clip : m_Mesh.Clips)
					{
						ImGui::TableNextRow();
						ImGui::TableNextColumn(); ImGui::TextUnformatted(clip.Name.c_str());
						ImGui::TableNextColumn(); ImGui::Text("%+.3f", clip.RootNet.x);
						ImGui::TableNextColumn(); ImGui::Text("%+.3f", clip.RootNet.y);
						ImGui::TableNextColumn(); ImGui::Text("%+.3f", clip.RootNet.z);
						ImGui::TableNextColumn();
						ImGui::Text("%.3f", std::max({ clip.RootResidualMax.x,
							clip.RootResidualMax.y, clip.RootResidualMax.z }));
					}
					ImGui::EndTable();
				}

				ImGui::Spacing();
				ImGui::TextUnformatted("Pose at t = 0");
				ImGui::TextDisabled("Head Y, Hips Y, Hips Z in mesh metres at the first frame.");
				if (ImGui::BeginTable("##pose0", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
					| ImGuiTableFlags_SizingStretchProp))
				{
					ImGui::TableSetupColumn("Clip");
					ImGui::TableSetupColumn("Head Y", ImGuiTableColumnFlags_WidthFixed, 72.0f);
					ImGui::TableSetupColumn("Hips Y", ImGuiTableColumnFlags_WidthFixed, 72.0f);
					ImGui::TableSetupColumn("Hips Z", ImGuiTableColumnFlags_WidthFixed, 72.0f);
					ImGui::TableHeadersRow();

					float headMin = 0.0f, headMax = 0.0f, hipsMin = 0.0f, hipsMax = 0.0f, zMin = 0.0f, zMax = 0.0f;
					bool anyHead = false, anyHips = false;
					for (const ClipRow& clip : m_Mesh.Clips)
					{
						ImGui::TableNextRow();
						ImGui::TableNextColumn(); ImGui::TextUnformatted(clip.Name.c_str());
						ImGui::TableNextColumn();
						if (clip.HasHead)
							ImGui::Text("%.3f", clip.HeadY);
						else
							ImGui::TextDisabled("—");
						ImGui::TableNextColumn();
						if (clip.HasHips)
						{
							ImGui::Text("%.3f", clip.HipsY);
							ImGui::TableNextColumn();
							ImGui::Text("%.3f", clip.HipsZ);
						}
						else
						{
							ImGui::TextDisabled("—");
							ImGui::TableNextColumn();
							ImGui::TextDisabled("—");
						}

						if (clip.HasHead)
						{
							if (!anyHead)
							{
								headMin = headMax = clip.HeadY;
								anyHead = true;
							}
							else
							{
								headMin = std::min(headMin, clip.HeadY);
								headMax = std::max(headMax, clip.HeadY);
							}
						}
						if (clip.HasHips)
						{
							if (!anyHips)
							{
								hipsMin = hipsMax = clip.HipsY;
								zMin = zMax = clip.HipsZ;
								anyHips = true;
							}
							else
							{
								hipsMin = std::min(hipsMin, clip.HipsY);
								hipsMax = std::max(hipsMax, clip.HipsY);
								zMin = std::min(zMin, clip.HipsZ);
								zMax = std::max(zMax, clip.HipsZ);
							}
						}
					}
					ImGui::EndTable();

					if (anyHead || anyHips)
					{
						char span[192];
						if (anyHead && anyHips)
						{
							std::snprintf(span, sizeof(span),
								"Span  Head Y %.1f cm   Hips Y %.1f cm   Hips Z %.1f cm",
								(headMax - headMin) * 100.0f,
								(hipsMax - hipsMin) * 100.0f,
								(zMax - zMin) * 100.0f);
						}
						else if (anyHead)
						{
							std::snprintf(span, sizeof(span), "Span  Head Y %.1f cm",
								(headMax - headMin) * 100.0f);
						}
						else
						{
							std::snprintf(span, sizeof(span), "Span  Hips Y %.1f cm   Hips Z %.1f cm",
								(hipsMax - hipsMin) * 100.0f, (zMax - zMin) * 100.0f);
						}
						ImGui::TextDisabled("%s", span);
					}
				}
			}
		}
	}

	void AssetInspectorPanel::DrawTextureBody()
	{
		if (!IsAssetHandleValid(m_Handle))
		{
			ImGui::TextDisabled("Texture is not in the asset index");
			return;
		}

		Ref<Texture2D> texture = AssetManager::GetAsset<Texture2D>(m_Handle);
		if (!texture)
		{
			ImGui::TextDisabled("Loading...");
			return;
		}

		ImGui::Text("Dimensions: %u x %u", texture->GetWidth(), texture->GetHeight());
		ImGui::Text("Source format: %s", SourceFormatFromExtension(m_Absolute).c_str());
		ImGui::Text("Mips: %u", (uint32_t)texture->GetMipCount());
		ImGui::Text("Compiled format: %s", GpuFormatName(texture->GetGpuFormat()));
		ImGui::Text("Compiled size: %s", FormatBytes(texture->GetGpuBytes()).c_str());
	}

	void AssetInspectorPanel::DrawMaterialBody()
	{
		if (!IsAssetHandleValid(m_Handle))
		{
			ImGui::TextDisabled("Material is not in the asset index");
			return;
		}

		Ref<Material> material = AssetManager::GetAsset<Material>(m_Handle);
		if (!material)
		{
			ImGui::TextDisabled("Loading...");
			return;
		}

		EditorUI::DrawMaterialAssetEditor(m_Handle);
	}

}
