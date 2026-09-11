#include "gepch.h"
#include "GanymedE/Renderer/MeshShader.h"
#include "MeshImporter.h"

#include "Material.h"
#include "Shader.h"
#include "Texture.h"
#include "GanymedE/Assets/AssetManager.h"
#include "GanymedE/Assets/AssetPaths.h"
#include "GanymedE/Assets/MaterialSerializer.h"
#include "GanymedE/Assets/TextureImporter.h"
#include "GanymedE/Scene/Scene.h"
#include "GanymedE/Scene/Entity.h"
#include "GanymedE/Scene/Components.h"

#include <cgltf.h>

#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace GanymedE {

	namespace {

		// Records where a glTF image lives, without touching the GPU. A URI becomes an
		// asset-root-relative path the material resolves later; an embedded buffer view becomes
		// the compressed bytes themselves, because there is no file to point at.
		void RecordImage(const cgltf_image* image, const std::filesystem::path& basePath,
			std::string& outRelativePath, std::vector<uint8_t>& outEmbeddedData)
		{
			if (!image)
				return;

			if (image->uri)
			{
				std::filesystem::path imagePath = basePath / image->uri;
				std::error_code ec;
				auto relative = std::filesystem::relative(imagePath, GetAssetRoot(), ec);
				outRelativePath = ec ? imagePath.string() : relative.generic_string();
				return;
			}

			if (image->buffer_view)
			{
				const cgltf_buffer_view* view = image->buffer_view;
				const uint8_t* data = (const uint8_t*)view->buffer->data + view->offset;

				// No file identity, so no registry entry and no de-duplication. The bytes ride
				// along in the mesh blob so a cache replay does not need the model file.
				outEmbeddedData.assign(data, data + view->size);
			}
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

		struct NodeEntry
		{
			const cgltf_node* Node;
			glm::mat4 World;
		};

		// Depth-first into a vector, not an unordered_map: submesh order used to be
		// whatever the map iterated, which changed run to run and made cache diffs and
		// joint-to-submesh correlation impossible to reason about.
		void CollectNodes(const cgltf_node* node, const glm::mat4& parentWorld, std::vector<NodeEntry>& out)
		{
			glm::mat4 world = parentWorld * GetNodeLocalTransform(node);
			out.push_back({ node, world });
			for (cgltf_size i = 0; i < node->children_count; i++)
				CollectNodes(node->children[i], world, out);
		}

		template<typename T>
		static T ReadAccessorElement(const cgltf_accessor* accessor, cgltf_size index)
		{
			T value{};
			cgltf_accessor_read_float(accessor, index, (float*)&value, sizeof(T) / sizeof(float));
			return value;
		}

		// glTF requires node matrices to be decomposable to TRS, so no skew or
		// perspective handling. A negative determinant means a mirroring scale; the
		// sign has to land on one axis or the rotation comes out garbage.
		JointPose PoseFromNode(const cgltf_node* node)
		{
			JointPose pose;

			if (!node->has_matrix)
			{
				if (node->has_translation)
					pose.Translation = glm::make_vec3(node->translation);
				if (node->has_rotation)
					pose.Rotation = glm::make_quat(node->rotation);
				if (node->has_scale)
					pose.Scale = glm::make_vec3(node->scale);
				return pose;
			}

			glm::mat4 m = glm::make_mat4(node->matrix);
			pose.Translation = glm::vec3(m[3]);

			glm::vec3 axes[3] = { glm::vec3(m[0]), glm::vec3(m[1]), glm::vec3(m[2]) };
			pose.Scale = { glm::length(axes[0]), glm::length(axes[1]), glm::length(axes[2]) };

			if (glm::determinant(glm::mat3(m)) < 0.0f)
			{
				pose.Scale.x = -pose.Scale.x;
				axes[0] = -axes[0];
			}

			for (int i = 0; i < 3; i++)
			{
				float length = glm::length(axes[i]);
				axes[i] = length > 1e-8f ? axes[i] / length : glm::vec3(i == 0, i == 1, i == 2);
			}

			pose.Rotation = glm::quat_cast(glm::mat3(axes[0], axes[1], axes[2]));
			return pose;
		}

		// Reads skin.joints into parents-before-children order and fills `outRemap`
		// with gltf joint index -> skeleton joint index, which the JOINTS_0 attribute
		// values have to be pushed through.
		bool BuildSkeleton(const cgltf_skin* skin, const std::unordered_map<const cgltf_node*, glm::mat4>& nodeWorld,
			const glm::mat4& meshNodeWorld, Skeleton& outSkeleton, std::vector<uint32_t>& outRemap)
		{
			const cgltf_size jointCount = skin->joints_count;
			if (jointCount == 0)
				return false;

			std::unordered_map<const cgltf_node*, uint32_t> nodeToGltfJoint;
			nodeToGltfJoint.reserve(jointCount);
			for (cgltf_size j = 0; j < jointCount; j++)
				nodeToGltfJoint[skin->joints[j]] = (uint32_t)j;

			// Parent in glTF-joint indices; -1 when the parent node is not itself a joint.
			std::vector<int32_t> gltfParent(jointCount, -1);
			for (cgltf_size j = 0; j < jointCount; j++)
			{
				const cgltf_node* parent = skin->joints[j]->parent;
				if (!parent)
					continue;

				auto it = nodeToGltfJoint.find(parent);
				if (it != nodeToGltfJoint.end())
					gltfParent[j] = (int32_t)it->second;
			}

			// Topological sort. glTF gives no ordering guarantee for skin.joints, but
			// composing globals in a single forward pass needs parents first.
			outRemap.assign(jointCount, UINT32_MAX);
			std::vector<uint32_t> order;
			order.reserve(jointCount);

			bool progressed = true;
			while (order.size() < jointCount && progressed)
			{
				progressed = false;
				for (cgltf_size j = 0; j < jointCount; j++)
				{
					if (outRemap[j] != UINT32_MAX)
						continue;

					int32_t parent = gltfParent[j];
					if (parent >= 0 && outRemap[parent] == UINT32_MAX)
						continue;

					outRemap[j] = (uint32_t)order.size();
					order.push_back((uint32_t)j);
					progressed = true;
				}
			}

			if (order.size() != jointCount)
			{
				GE_CORE_ERROR("Skin '{0}' has a cyclic joint hierarchy - skipping skeleton",
					skin->name ? skin->name : "<unnamed>");
				return false;
			}

			outSkeleton.ParentIndices.resize(jointCount);
			outSkeleton.InverseBind.assign(jointCount, glm::mat4(1.0f));
			outSkeleton.LocalRestPose.resize(jointCount);
			outSkeleton.JointNames.resize(jointCount);

			for (uint32_t sorted = 0; sorted < (uint32_t)jointCount; sorted++)
			{
				uint32_t gltfIndex = order[sorted];
				const cgltf_node* jointNode = skin->joints[gltfIndex];

				int32_t parent = gltfParent[gltfIndex];
				outSkeleton.ParentIndices[sorted] = parent >= 0 ? (int32_t)outRemap[parent] : -1;
				outSkeleton.LocalRestPose[sorted] = PoseFromNode(jointNode);
				outSkeleton.JointNames[sorted] = jointNode->name ? jointNode->name : "Joint";

				// The spec allows the accessor to be absent, meaning identity binds.
				if (skin->inverse_bind_matrices)
				{
					cgltf_accessor_read_float(skin->inverse_bind_matrices, gltfIndex,
						glm::value_ptr(outSkeleton.InverseBind[sorted]), 16);
				}
			}

			// Everything above the root joints. Root joints are almost always a single
			// node ("Armature"), but nothing in the format demands it, so disagreement
			// is reported rather than silently picking one.
			const cgltf_node* rootParent = nullptr;
			bool rootParentSet = false;
			for (uint32_t sorted = 0; sorted < (uint32_t)jointCount; sorted++)
			{
				if (outSkeleton.ParentIndices[sorted] != -1)
					continue;

				const cgltf_node* parent = skin->joints[order[sorted]]->parent;
				if (!rootParentSet)
				{
					rootParent = parent;
					rootParentSet = true;
				}
				else if (parent != rootParent)
				{
					GE_CORE_WARN("Skin '{0}' has root joints under different parents - "
						"using the first one's transform", skin->name ? skin->name : "<unnamed>");
					break;
				}
			}

			// Two corrections fold into one seed matrix.
			//
			// The inverse: glTF says the transform of the node a skinned mesh hangs off MUST be
			// ignored, and that joint matrices carry inverse(meshNodeWorld) instead - skinning
			// output is already in mesh space, so applying the node transform would apply it
			// twice. The multiply: inverse binds are relative to mesh space, so joint globals
			// must include whatever sits above the root joints (a Blender "Armature", a Y-up
			// conversion node).
			//
			// Both are identity in the simple exports, which is why omitting the inverse looked
			// correct until a rest-pose palette was actually composed and failed to come out as
			// identity on CesiumMan.
			glm::mat4 rootParentWorld{ 1.0f };
			if (rootParent)
			{
				auto it = nodeWorld.find(rootParent);
				if (it != nodeWorld.end())
					rootParentWorld = it->second;
			}

			outSkeleton.RootTransform = glm::inverse(meshNodeWorld) * rootParentWorld;

			if (jointCount > Skeleton::MaxBones)
			{
				GE_CORE_WARN("Skin '{0}' has {1} joints, over the {2}-bone palette limit - "
					"joints past the limit will not animate",
					skin->name ? skin->name : "<unnamed>", (uint32_t)jointCount, Skeleton::MaxBones);
			}

			return true;
		}

		using ChannelPath = AnimationClip::Channel::Path;

		ChannelPath PathFromGltf(cgltf_animation_path_type path, bool& outSupported)
		{
			outSupported = true;
			switch (path)
			{
				case cgltf_animation_path_type_translation: return ChannelPath::Translation;
				case cgltf_animation_path_type_rotation:    return ChannelPath::Rotation;
				case cgltf_animation_path_type_scale:       return ChannelPath::Scale;
				default: break;
			}

			// Morph-target weights are explicitly out of scope for v1.
			outSupported = false;
			return ChannelPath::Translation;
		}

		std::vector<AnimationClip> BuildClips(const cgltf_data* data,
			const std::unordered_map<const cgltf_node*, uint32_t>& nodeToJoint)
		{
			std::vector<AnimationClip> clips;
			clips.reserve(data->animations_count);

			std::vector<float> scratch;

			for (cgltf_size a = 0; a < data->animations_count; a++)
			{
				const cgltf_animation& src = data->animations[a];

				AnimationClip clip;
				clip.Name = src.name && src.name[0] ? src.name : ("Clip " + std::to_string(a));
				bool warnedCubic = false;

				for (cgltf_size c = 0; c < src.channels_count; c++)
				{
					const cgltf_animation_channel& channel = src.channels[c];
					if (!channel.target_node || !channel.sampler)
						continue;

					auto joint = nodeToJoint.find(channel.target_node);
					if (joint == nodeToJoint.end())
						continue;

					bool supported = false;
					ChannelPath target = PathFromGltf(channel.target_path, supported);
					if (!supported)
						continue;

					const cgltf_animation_sampler* sampler = channel.sampler;
					if (!sampler->input || !sampler->output || sampler->input->count == 0)
						continue;

					AnimationClip::Channel out;
					out.Joint = joint->second;
					out.Target = target;
					out.Mode = sampler->interpolation == cgltf_interpolation_type_step
						? AnimationClip::Channel::Interp::Step
						: AnimationClip::Channel::Interp::Linear;

					const bool cubic = sampler->interpolation == cgltf_interpolation_type_cubic_spline;
					if (cubic && !warnedCubic)
					{
						GE_CORE_WARN("Clip '{0}' uses CUBICSPLINE interpolation - degraded to linear", clip.Name);
						warnedCubic = true;
					}

					const cgltf_size keyCount = sampler->input->count;
					out.Times.resize(keyCount);
					cgltf_accessor_unpack_floats(sampler->input, out.Times.data(), keyCount);

					// CUBICSPLINE stores (inTangent, value, outTangent) per key; the
					// middle element is the one linear sampling wants.
					const cgltf_size components = cgltf_num_components(sampler->output->type);
					const cgltf_size stride = cubic ? components * 3 : components;
					const cgltf_size valueOffset = cubic ? components : 0;

					scratch.resize(cgltf_accessor_unpack_floats(sampler->output, nullptr, 0));
					cgltf_accessor_unpack_floats(sampler->output, scratch.data(), scratch.size());

					if (scratch.size() < keyCount * stride)
						continue;

					out.Values.resize(keyCount, glm::vec4(0.0f));
					for (cgltf_size k = 0; k < keyCount; k++)
					{
						const float* value = scratch.data() + k * stride + valueOffset;
						for (cgltf_size comp = 0; comp < components && comp < 4; comp++)
							out.Values[k][(int)comp] = value[comp];
					}

					clip.Duration = std::max(clip.Duration, out.Times.back());
					clip.Channels.push_back(std::move(out));
				}

				if (clip.Channels.empty())
					continue;

				GE_CORE_INFO("  clip '{0}': {1} channels, {2:.3f}s",
					clip.Name, clip.Channels.size(), clip.Duration);
				clips.push_back(std::move(clip));
			}

			return clips;
		}

	}

	bool MeshImporter::Import(const std::filesystem::path& path, MeshSource& out,
		std::vector<std::string>* outDependencies)
	{
		cgltf_options options = {};
		cgltf_data* data = nullptr;
		cgltf_result result = cgltf_parse_file(&options, path.string().c_str(), &data);
		if (result != cgltf_result_success)
		{
			GE_CORE_ERROR("Failed to parse glTF '{0}' (code {1})", path.string(), (int)result);
			return false;
		}

		result = cgltf_load_buffers(&options, data, path.string().c_str());
		if (result != cgltf_result_success)
		{
			GE_CORE_ERROR("Failed to load glTF buffers for '{0}'", path.string());
			cgltf_free(data);
			return false;
		}

		std::filesystem::path basePath = path.parent_path();

		// Every external file cgltf just resolved. A URI is external iff it is not a data: blob
		// and not the `.glb`'s own binary chunk, and only the ones that land inside assets/ are
		// worth recording - a path escaping the asset root has no identity for the epoch record
		// to hash by relative path.
		if (outDependencies)
		{
			auto record = [&](const char* uri)
			{
				// A data: URI is the payload itself - there is no file to depend on.
				const std::string text = uri ? uri : "";
				if (text.empty() || text.rfind("data:", 0) == 0)
					return;

				std::error_code ec;
				auto relative = std::filesystem::relative(basePath / text, GetAssetRoot(), ec);
				if (ec)
					return;

				std::string recorded = relative.generic_string();
				if (recorded.rfind("..", 0) == 0)
					return;

				if (std::find(outDependencies->begin(), outDependencies->end(), recorded)
					== outDependencies->end())
				{
					outDependencies->push_back(std::move(recorded));
				}
			};

			for (cgltf_size i = 0; i < data->buffers_count; i++)
				record(data->buffers[i].uri);
			for (cgltf_size i = 0; i < data->images_count; i++)
				record(data->images[i].uri);
		}

		// Materials, as descriptions. No Material object and no texture is created here - that
		// is BuildMesh's job, on the main thread.
		std::vector<MeshMaterialSource> materials;
		materials.reserve(data->materials_count);
		for (cgltf_size i = 0; i < data->materials_count; i++)
		{
			const cgltf_material& src = data->materials[i];

			MeshMaterialSource material;
			if (src.name)
				material.Name = src.name;

			material.TwoSided = src.double_sided;
			material.Transparent = src.alpha_mode == cgltf_alpha_mode_blend;

			if (src.has_pbr_metallic_roughness)
			{
				const auto& pbr = src.pbr_metallic_roughness;
				material.Albedo = glm::make_vec4(pbr.base_color_factor);
				material.Metallic = pbr.metallic_factor;
				material.Roughness = pbr.roughness_factor;

				if (pbr.base_color_texture.texture)
				{
					RecordImage(pbr.base_color_texture.texture->image, basePath,
						material.AlbedoMapPath, material.AlbedoEmbedded);
				}
				if (pbr.metallic_roughness_texture.texture)
				{
					RecordImage(pbr.metallic_roughness_texture.texture->image, basePath,
						material.MetallicRoughnessMapPath, material.MetallicRoughnessEmbedded);
				}
			}

			if (src.normal_texture.texture)
			{
				RecordImage(src.normal_texture.texture->image, basePath,
					material.NormalMapPath, material.NormalEmbedded);
			}

			materials.push_back(std::move(material));
		}

		// Node world transforms, in a deterministic depth-first order
		std::vector<NodeEntry> sceneNodes;
		if (data->scenes_count > 0)
		{
			const cgltf_scene& scene = data->scene ? *data->scene : data->scenes[0];
			for (cgltf_size i = 0; i < scene.nodes_count; i++)
				CollectNodes(scene.nodes[i], glm::mat4(1.0f), sceneNodes);
		}
		else
		{
			for (cgltf_size i = 0; i < data->nodes_count; i++)
			{
				if (!data->nodes[i].parent)
					CollectNodes(&data->nodes[i], glm::mat4(1.0f), sceneNodes);
			}
		}

		std::unordered_map<const cgltf_node*, glm::mat4> nodeWorld;
		nodeWorld.reserve(sceneNodes.size());
		for (const NodeEntry& entry : sceneNodes)
			nodeWorld[entry.Node] = entry.World;

		// Skin: the first one only. Multi-skin files are a v1 gap, not a silent one.
		const cgltf_skin* skin = nullptr;
		Skeleton skeleton;
		std::vector<uint32_t> jointRemap;
		std::unordered_map<const cgltf_node*, uint32_t> nodeToJoint;

		if (data->skins_count > 0)
		{
			if (data->skins_count > 1)
			{
				GE_CORE_WARN("glTF '{0}' has {1} skins - only the first is imported",
					path.filename().string(), (uint32_t)data->skins_count);
			}

			// The node the skinned mesh hangs off, whose transform the joint matrices cancel.
			glm::mat4 skinnedMeshWorld{ 1.0f };
			for (const NodeEntry& entry : sceneNodes)
			{
				if (entry.Node->mesh && entry.Node->skin == &data->skins[0])
				{
					skinnedMeshWorld = entry.World;
					break;
				}
			}

			if (BuildSkeleton(&data->skins[0], nodeWorld, skinnedMeshWorld, skeleton, jointRemap))
			{
				skin = &data->skins[0];
				for (cgltf_size j = 0; j < skin->joints_count; j++)
					nodeToJoint[skin->joints[j]] = jointRemap[j];
			}
		}

		std::vector<MeshVertex> vertices;
		std::vector<SkinVertex> skinVertices;
		std::vector<uint32_t> indices;
		std::vector<Submesh> submeshes;
		std::vector<float> weightScratch;

		auto appendPrimitive = [&](const cgltf_primitive& primitive, const glm::mat4& transform,
			const char* name, bool nodeIsSkinned)
		{
			if (primitive.type != cgltf_primitive_type_triangles)
				return;

			const cgltf_accessor* positionAccessor = nullptr;
			const cgltf_accessor* normalAccessor = nullptr;
			const cgltf_accessor* tangentAccessor = nullptr;
			const cgltf_accessor* texcoordAccessor = nullptr;
			const cgltf_accessor* jointsAccessor = nullptr;
			const cgltf_accessor* weightsAccessor = nullptr;

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
					// JOINTS_1/WEIGHTS_1 exist for >4 influences per vertex; the four
					// strongest is the standard budget and all the palette shader takes.
					case cgltf_attribute_type_joints:
						if (attr.index == 0)
							jointsAccessor = attr.data;
						break;
					case cgltf_attribute_type_weights:
						if (attr.index == 0)
							weightsAccessor = attr.data;
						break;
					default: break;
				}
			}

			if (!positionAccessor)
				return;

			// glTF allows a primitive with no index accessor - the positions are
			// triangle soup in draw order (Khronos' Fox is one). We always draw
			// indexed, so the trivial 0..n-1 list gets synthesized below.
			const cgltf_size indexCount = primitive.indices ? primitive.indices->count : positionAccessor->count;

			const bool skinned = nodeIsSkinned && jointsAccessor && weightsAccessor;

			Submesh submesh;
			submesh.BaseVertex = (uint32_t)vertices.size();
			submesh.BaseIndex = (uint32_t)indices.size();
			submesh.IndexCount = (uint32_t)indexCount;
			submesh.LocalTransform = transform;
			submesh.Name = name ? name : "Submesh";
			submesh.IsSkinned = skinned;

			int materialIndex = 0;
			if (primitive.material)
				materialIndex = (int)(primitive.material - data->materials);
			if (materialIndex < 0 || materialIndex >= (int)materials.size())
				materialIndex = 0;
			submesh.MaterialIndex = (uint32_t)materialIndex;

			// Skinned vertices stay in skin space: the joint matrices already carry the
			// world placement, and the spec says a skinned mesh node's own transform is
			// ignored. Baking it in would apply the node transform twice.
			const glm::mat4 bake = skinned ? glm::mat4(1.0f) : transform;
			const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(bake)));

			for (cgltf_size v = 0; v < positionAccessor->count; v++)
			{
				MeshVertex vertex{};
				glm::vec3 pos = ReadAccessorElement<glm::vec3>(positionAccessor, v);
				vertex.Position = glm::vec3(bake * glm::vec4(pos, 1.0f));

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

			for (cgltf_size i = 0; i < indexCount; i++)
			{
				indices.push_back(primitive.indices
					? (uint32_t)cgltf_accessor_read_index(primitive.indices, i)
					: (uint32_t)i);
			}

			// Indices are relative to this primitive's vertices; BaseVertex handles the offset in the draw call.
			// Our IndexBuffer stores raw primitive indices (0-based per primitive), so BaseVertex is required.
			submeshes.push_back(submesh);

			// Once the file has a skeleton every vertex needs a skin entry, whether its
			// primitive is skinned or not: bgfx binds both streams with one startVertex,
			// so a short stream would read past its end for later submeshes.
			if (skeleton.IsEmpty())
				return;

			const size_t skinBase = skinVertices.size();
			skinVertices.resize(skinBase + positionAccessor->count);
			if (!skinned)
				return;

			// Weights are commonly stored normalized ubyte/ushort; unpack_floats applies
			// the accessor's normalization and, unlike read_float, handles sparse data.
			const cgltf_size weightComponents = cgltf_num_components(weightsAccessor->type);
			weightScratch.resize(cgltf_accessor_unpack_floats(weightsAccessor, nullptr, 0));
			cgltf_accessor_unpack_floats(weightsAccessor, weightScratch.data(), weightScratch.size());

			const uint32_t jointCount = skeleton.JointCount();

			for (cgltf_size v = 0; v < positionAccessor->count; v++)
			{
				SkinVertex& skinVertex = skinVertices[skinBase + v];

				// read_uint, not read_float: JOINTS_0 is an unnormalized integer
				// accessor, and read_float would hand back raw bytes for the ubyte case.
				cgltf_uint joints[4] = { 0, 0, 0, 0 };
				cgltf_accessor_read_uint(jointsAccessor, v, joints, 4);

				glm::vec4 weights(0.0f);
				if ((v + 1) * weightComponents <= weightScratch.size())
				{
					for (cgltf_size comp = 0; comp < weightComponents && comp < 4; comp++)
						weights[(int)comp] = weightScratch[v * weightComponents + comp];
				}

				// A joint index past the palette would sample whatever follows it.
				for (int i = 0; i < 4; i++)
				{
					uint32_t index = joints[i] < jointCount ? joints[i] : 0;
					skinVertex.JointIndices[i] = (float)index;
				}

				// Exporters quantize weights and they stop summing to 1; unnormalized
				// weights show up as limbs that shrink and swell as the rig moves.
				float sum = weights.x + weights.y + weights.z + weights.w;
				skinVertex.JointWeights = sum > 1e-6f ? weights / sum : glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
			}
		};

		for (const NodeEntry& entry : sceneNodes)
		{
			const cgltf_node* node = entry.Node;
			if (!node->mesh)
				continue;

			for (cgltf_size p = 0; p < node->mesh->primitives_count; p++)
			{
				appendPrimitive(node->mesh->primitives[p], entry.World,
					node->name ? node->name : node->mesh->name, skin && node->skin == skin);
			}
		}

		// Fallback: no scene nodes referenced meshes — load all meshes at identity
		if (submeshes.empty())
		{
			for (cgltf_size m = 0; m < data->meshes_count; m++)
			{
				const cgltf_mesh& mesh = data->meshes[m];
				for (cgltf_size p = 0; p < mesh.primitives_count; p++)
					appendPrimitive(mesh.primitives[p], glm::mat4(1.0f), mesh.name, false);
			}
		}

		std::vector<AnimationClip> clips;
		if (!nodeToJoint.empty())
			clips = BuildClips(data, nodeToJoint);

		cgltf_free(data);

		if (vertices.empty() || indices.empty())
		{
			GE_CORE_ERROR("glTF '{0}' contained no triangle geometry", path.string());
			return nullptr;
		}

		// A file can declare a skin that no primitive actually uses. Carrying a
		// skeleton with no skinned geometry would just cost a palette upload.
		bool anySkinned = false;
		for (const Submesh& submesh : submeshes)
			anySkinned |= submesh.IsSkinned;

		if (!anySkinned)
		{
			skeleton = {};
			skinVertices.clear();
			clips.clear();
		}

		// Static vertices were baked to world space above, so their transform is spent.
		// A skinned submesh could not be baked - the palette has to operate on the
		// original bind-space positions - so it KEEPS the node transform. That looks
		// like a spec violation (glTF says a skinned mesh node's transform is ignored)
		// and is not one: Skeleton::RootTransform carries the inverse of the same
		// matrix, so re-applying it here cancels rather than double-applies. Dropping
		// it instead leaves the mesh in raw bind space, which for any file with a
		// Y-up correction node means the character renders lying on its side.
		for (auto& submesh : submeshes)
		{
			if (!submesh.IsSkinned)
				submesh.LocalTransform = glm::mat4(1.0f);
		}

		out.Vertices = std::move(vertices);
		out.Indices = std::move(indices);
		out.Submeshes = std::move(submeshes);
		out.Materials = std::move(materials);
		out.SkinVertices = std::move(skinVertices);
		out.Skeleton = std::move(skeleton);
		out.Clips = std::move(clips);
		out.RelativePath = MakeAssetRelative(path).generic_string();

		GE_CORE_INFO("Imported mesh '{0}' ({1} verts, {2} indices, {3} submeshes, {4} joints, {5} clips)",
			path.filename().string(), out.Vertices.size(), out.Indices.size(), out.Submeshes.size(),
			out.Skeleton.JointCount(), out.Clips.size());
		return out.IsValid();
	}

	Entity MeshImporter::Instantiate(Scene* scene, const std::filesystem::path& path)
	{
		std::filesystem::path relativePath = MakeAssetRelative(path);
		AssetHandle handle = AssetManager::ImportAsset(relativePath);
		if (!IsAssetHandleValid(handle))
			return {};

		// Loaded through a reference rather than a bare GetAsset, so the object has an owner
		// before this scope ends: the manager cache is weak, and a mesh nobody holds is
		// collected the moment the local goes out of scope.
		AssetRef<Mesh> meshRef(handle);
		meshRef.Get();

		// The one blocking wait in the engine. Instantiate has to read the mesh's material list
		// to size and fill the new entity's override slots, and "not loaded yet" is not an answer
		// it can act on - the entity would end up with no slots at all. Everything else on a
		// frame path tolerates a null asset for a frame or two instead.
		AssetManager::WaitFor(handle);

		const Ref<Mesh>& mesh = meshRef.Get();
		if (!mesh)
			return {};

		Entity entity = scene->CreateEntity(path.stem().string());
		auto& smc = entity.AddComponent<StaticMeshComponent>();
		smc.Mesh = meshRef;

		// Point the new entity at the sidecars the load above just generated, so a
		// drag-dropped mesh authors against .gmat from birth rather than needing a manual
		// assignment per slot. The path rule is MaterialSerializer::SidecarPath in both
		// places - the generator and this - so they cannot disagree about a name.
		//
		// ImportAsset is idempotent and returns the invalid handle for a file that is not
		// there, which is the correct outcome for a read-only install: the slot stays unset
		// and the mesh's own material renders.
		const auto& materials = mesh->GetMaterials();
		smc.MaterialOverrides.reserve(materials.size());
		for (uint32_t i = 0; i < (uint32_t)materials.size(); i++)
		{
			const std::string name = materials[i] ? materials[i]->GetName() : std::string();
			const std::filesystem::path sidecar = MaterialSerializer::SidecarPath(relativePath, i, name);

			smc.MaterialOverrides.emplace_back(std::filesystem::exists(GetAssetRoot() / sidecar)
				? AssetManager::ImportAsset(sidecar) : InvalidAssetHandle);
		}

		return entity;
	}

}
