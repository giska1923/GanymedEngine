#include "GanymedE/Core/Log.h"
#include "AssetInspectorPanel.h"

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

#include <cctype>
#include <cinttypes>
#include <cmath>
#include <cstdio>

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
			return;

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
		m_Mesh.Clips.reserve(mesh->GetClips().size());
		for (const AnimationClip& clip : mesh->GetClips())
			m_Mesh.Clips.push_back({ clip.Name, clip.Duration });

		m_Mesh.MirroredUvShells = MeshHasMirroredUvShells(*mesh);
		m_Mesh.Ready = true;
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
			ImGui::Text("Joints: %u", m_Mesh.JointCount);
			if (m_Mesh.Clips.empty())
			{
				ImGui::TextDisabled("No animation clips");
			}
			else if (ImGui::BeginTable("##clips", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
				| ImGuiTableFlags_SizingStretchProp))
			{
				ImGui::TableSetupColumn("Clip");
				ImGui::TableSetupColumn("Duration", ImGuiTableColumnFlags_WidthFixed, 80.0f);
				ImGui::TableHeadersRow();
				for (const ClipRow& clip : m_Mesh.Clips)
				{
					ImGui::TableNextRow();
					ImGui::TableNextColumn(); ImGui::TextUnformatted(clip.Name.c_str());
					ImGui::TableNextColumn(); ImGui::Text("%.3f s", clip.Duration);
				}
				ImGui::EndTable();
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
