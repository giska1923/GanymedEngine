#include "gepch.h"
#include "MeshImporter.h"

#include "Material.h"
#include "Shader.h"
#include "Texture.h"
#include "GanymedE/Assets/AssetManager.h"
#include "GanymedE/Assets/AssetPaths.h"
#include "GanymedE/Scene/Scene.h"
#include "GanymedE/Scene/Entity.h"
#include "GanymedE/Scene/Components.h"

#include <cgltf.h>
#include <stb_image.h>

#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace GanymedE {

	namespace {

		Ref<Shader> GetMeshShader()
		{
			static Ref<Shader> s_Shader = Shader::Create("assets/shaders/Phong.glsl");
			return s_Shader;
		}

		Ref<Texture2D> CreateTextureFromImage(const cgltf_image* image, const std::filesystem::path& basePath,
			std::string* outRelativePath = nullptr, std::vector<uint8_t>* outEmbeddedData = nullptr)
		{
			if (!image)
				return nullptr;

			if (image->uri)
			{
				std::filesystem::path imagePath = basePath / image->uri;
				if (outRelativePath)
				{
					std::error_code ec;
					auto relative = std::filesystem::relative(imagePath, GetAssetRoot(), ec);
					*outRelativePath = ec ? imagePath.string() : relative.generic_string();
				}
				return Texture2D::Create(imagePath.string());
			}

			if (image->buffer_view)
			{
				const cgltf_buffer_view* view = image->buffer_view;
				const uint8_t* data = (const uint8_t*)view->buffer->data + view->offset;

				stbi_set_flip_vertically_on_load(1);
				int width, height, channels;
				unsigned char* pixels = stbi_load_from_memory(data, (int)view->size, &width, &height, &channels, 4);
				if (!pixels)
					return nullptr;

				Ref<Texture2D> texture = Texture2D::Create((uint32_t)width, (uint32_t)height);
				texture->SetData(pixels, width * height * 4);
				stbi_image_free(pixels);

				// No file on disk to reload from — keep the compressed bytes so MeshCache can persist them
				if (outEmbeddedData)
					outEmbeddedData->assign(data, data + view->size);

				return texture;
			}

			return nullptr;
		}

		glm::mat4 GetNodeLocalTransform(const cgltf_node* node)
		{
			glm::mat4 transform(1.0f);
			if (node->has_matrix)
			{
				transform = glm::make_mat4(node->matrix);
			}
			else
			{
				glm::vec3 translation(0.0f);
				if (node->has_translation)
					translation = glm::make_vec3(node->translation);

				glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
				if (node->has_rotation)
					rotation = glm::make_quat(node->rotation);

				glm::vec3 scale(1.0f);
				if (node->has_scale)
					scale = glm::make_vec3(node->scale);

				transform = glm::translate(glm::mat4(1.0f), translation)
					* glm::mat4_cast(rotation)
					* glm::scale(glm::mat4(1.0f), scale);
			}
			return transform;
		}

		void ComputeNodeWorldTransforms(const cgltf_node* node, const glm::mat4& parentWorld,
			std::unordered_map<const cgltf_node*, glm::mat4>& outWorld)
		{
			glm::mat4 world = parentWorld * GetNodeLocalTransform(node);
			outWorld[node] = world;
			for (cgltf_size i = 0; i < node->children_count; i++)
				ComputeNodeWorldTransforms(node->children[i], world, outWorld);
		}

		template<typename T>
		static T ReadAccessorElement(const cgltf_accessor* accessor, cgltf_size index)
		{
			T value{};
			cgltf_accessor_read_float(accessor, index, (float*)&value, sizeof(T) / sizeof(float));
			return value;
		}

	}

	Ref<Mesh> MeshImporter::Load(const std::filesystem::path& path)
	{
		cgltf_options options = {};
		cgltf_data* data = nullptr;
		cgltf_result result = cgltf_parse_file(&options, path.string().c_str(), &data);
		if (result != cgltf_result_success)
		{
			GE_CORE_ERROR("Failed to parse glTF '{0}' (code {1})", path.string(), (int)result);
			return nullptr;
		}

		result = cgltf_load_buffers(&options, data, path.string().c_str());
		if (result != cgltf_result_success)
		{
			GE_CORE_ERROR("Failed to load glTF buffers for '{0}'", path.string());
			cgltf_free(data);
			return nullptr;
		}

		std::filesystem::path basePath = path.parent_path();
		Ref<Shader> shader = GetMeshShader();

		// Materials
		std::vector<Ref<Material>> materials;
		materials.reserve(data->materials_count);
		for (cgltf_size i = 0; i < data->materials_count; i++)
		{
			const cgltf_material& src = data->materials[i];
			Ref<Material> material = Material::Create(shader);
			if (src.name)
				material->SetName(src.name);

			material->SetTwoSided(src.double_sided);
			material->SetTransparent(src.alpha_mode == cgltf_alpha_mode_blend);

			if (src.has_pbr_metallic_roughness)
			{
				const auto& pbr = src.pbr_metallic_roughness;
				material->SetAlbedoColor(glm::make_vec4(pbr.base_color_factor));
				material->SetMetallic(pbr.metallic_factor);
				material->SetRoughness(pbr.roughness_factor);

				if (pbr.base_color_texture.texture)
				{
					std::string texPath;
					std::vector<uint8_t> embedded;
					material->SetAlbedoMap(CreateTextureFromImage(pbr.base_color_texture.texture->image, basePath, &texPath, &embedded));
					material->SetAlbedoMapPath(texPath);
					material->SetAlbedoMapEmbeddedData(std::move(embedded));
				}
				if (pbr.metallic_roughness_texture.texture)
				{
					std::string texPath;
					std::vector<uint8_t> embedded;
					material->SetMetallicRoughnessMap(CreateTextureFromImage(pbr.metallic_roughness_texture.texture->image, basePath, &texPath, &embedded));
					material->SetMetallicRoughnessMapPath(texPath);
					material->SetMetallicRoughnessMapEmbeddedData(std::move(embedded));
				}
			}

			if (src.normal_texture.texture)
			{
				std::string texPath;
				std::vector<uint8_t> embedded;
				material->SetNormalMap(CreateTextureFromImage(src.normal_texture.texture->image, basePath, &texPath, &embedded));
				material->SetNormalMapPath(texPath);
				material->SetNormalMapEmbeddedData(std::move(embedded));
			}

			materials.push_back(material);
		}

		if (materials.empty())
			materials.push_back(Material::Create(shader));

		// Node world transforms
		std::unordered_map<const cgltf_node*, glm::mat4> nodeWorld;
		if (data->scenes_count > 0)
		{
			const cgltf_scene& scene = data->scene ? *data->scene : data->scenes[0];
			for (cgltf_size i = 0; i < scene.nodes_count; i++)
				ComputeNodeWorldTransforms(scene.nodes[i], glm::mat4(1.0f), nodeWorld);
		}
		else
		{
			for (cgltf_size i = 0; i < data->nodes_count; i++)
			{
				if (!data->nodes[i].parent)
					ComputeNodeWorldTransforms(&data->nodes[i], glm::mat4(1.0f), nodeWorld);
			}
		}

		std::vector<MeshVertex> vertices;
		std::vector<uint32_t> indices;
		std::vector<Submesh> submeshes;

		auto appendPrimitive = [&](const cgltf_primitive& primitive, const glm::mat4& transform, const char* name)
		{
			if (primitive.type != cgltf_primitive_type_triangles)
				return;

			const cgltf_accessor* positionAccessor = nullptr;
			const cgltf_accessor* normalAccessor = nullptr;
			const cgltf_accessor* tangentAccessor = nullptr;
			const cgltf_accessor* texcoordAccessor = nullptr;

			for (cgltf_size a = 0; a < primitive.attributes_count; a++)
			{
				const cgltf_attribute& attr = primitive.attributes[a];
				switch (attr.type)
				{
					case cgltf_attribute_type_position: positionAccessor = attr.data; break;
					case cgltf_attribute_type_normal:   normalAccessor = attr.data; break;
					case cgltf_attribute_type_tangent:  tangentAccessor = attr.data; break;
					case cgltf_attribute_type_texcoord:
						if (attr.index == 0)
							texcoordAccessor = attr.data;
						break;
					default: break;
				}
			}

			if (!positionAccessor || !primitive.indices)
				return;

			Submesh submesh;
			submesh.BaseVertex = (uint32_t)vertices.size();
			submesh.BaseIndex = (uint32_t)indices.size();
			submesh.IndexCount = (uint32_t)primitive.indices->count;
			submesh.LocalTransform = transform;
			submesh.Name = name ? name : "Submesh";

			int materialIndex = 0;
			if (primitive.material)
				materialIndex = (int)(primitive.material - data->materials);
			if (materialIndex < 0 || materialIndex >= (int)materials.size())
				materialIndex = 0;
			submesh.MaterialIndex = (uint32_t)materialIndex;

			glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(transform)));

			for (cgltf_size v = 0; v < positionAccessor->count; v++)
			{
				MeshVertex vertex{};
				glm::vec3 pos = ReadAccessorElement<glm::vec3>(positionAccessor, v);
				vertex.Position = glm::vec3(transform * glm::vec4(pos, 1.0f));

				if (normalAccessor)
				{
					glm::vec3 n = ReadAccessorElement<glm::vec3>(normalAccessor, v);
					vertex.Normal = glm::normalize(normalMatrix * n);
				}
				else
				{
					vertex.Normal = { 0.0f, 1.0f, 0.0f };
				}

				if (tangentAccessor)
				{
					glm::vec4 t = ReadAccessorElement<glm::vec4>(tangentAccessor, v);
					vertex.Tangent = glm::normalize(normalMatrix * glm::vec3(t));
				}
				else
				{
					vertex.Tangent = { 1.0f, 0.0f, 0.0f };
				}

				if (texcoordAccessor)
					vertex.TexCoord = ReadAccessorElement<glm::vec2>(texcoordAccessor, v);
				else
					vertex.TexCoord = { 0.0f, 0.0f };

				vertices.push_back(vertex);
			}

			for (cgltf_size i = 0; i < primitive.indices->count; i++)
				indices.push_back((uint32_t)cgltf_accessor_read_index(primitive.indices, i));

			// Indices are relative to this primitive's vertices; BaseVertex handles the offset in the draw call.
			// Our IndexBuffer stores raw primitive indices (0-based per primitive), so BaseVertex is required.
			submeshes.push_back(submesh);
		};

		for (auto& [node, world] : nodeWorld)
		{
			if (!node->mesh)
				continue;

			for (cgltf_size p = 0; p < node->mesh->primitives_count; p++)
				appendPrimitive(node->mesh->primitives[p], world, node->name ? node->name : node->mesh->name);
		}

		// Fallback: no scene nodes referenced meshes — load all meshes at identity
		if (submeshes.empty())
		{
			for (cgltf_size m = 0; m < data->meshes_count; m++)
			{
				const cgltf_mesh& mesh = data->meshes[m];
				for (cgltf_size p = 0; p < mesh.primitives_count; p++)
					appendPrimitive(mesh.primitives[p], glm::mat4(1.0f), mesh.name);
			}
		}

		cgltf_free(data);

		if (vertices.empty() || indices.empty())
		{
			GE_CORE_ERROR("glTF '{0}' contained no triangle geometry", path.string());
			return nullptr;
		}

		// Vertices already transformed; clear LocalTransform so Renderer doesn't double-apply
		for (auto& submesh : submeshes)
			submesh.LocalTransform = glm::mat4(1.0f);

		Ref<Mesh> mesh = Mesh::Create(vertices, indices, submeshes, materials);
		mesh->SetPath(MakeAssetRelative(path).generic_string());
		GE_CORE_INFO("Loaded mesh '{0}' ({1} verts, {2} indices, {3} submeshes)",
			path.filename().string(), vertices.size(), indices.size(), submeshes.size());
		return mesh;
	}

	Entity MeshImporter::Instantiate(Scene* scene, const std::filesystem::path& path)
	{
		std::filesystem::path relativePath = MakeAssetRelative(path);
		AssetHandle handle = AssetManager::ImportAsset(relativePath);
		if (!IsAssetHandleValid(handle))
			return {};

		Ref<Mesh> mesh = AssetManager::GetAsset<Mesh>(handle);
		if (!mesh)
			return {};

		Entity entity = scene->CreateEntity(path.stem().string());
		auto& smc = entity.AddComponent<StaticMeshComponent>();
		smc.Mesh = handle;
		return entity;
	}

}
