#pragma once

#include "GanymedE/Core/Core.h"
#include "GanymedE/Renderer/Shader.h"
#include "GanymedE/Renderer/Texture.h"

#include <glm/glm.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace GanymedE {

	class Material
	{
	public:
		Material() = default;
		Material(const Ref<Shader>& shader);

		void SetShader(const Ref<Shader>& shader) { m_Shader = shader; }
		Ref<Shader> GetShader() const { return m_Shader; }

		void SetAlbedoColor(const glm::vec4& color) { m_AlbedoColor = color; }
		const glm::vec4& GetAlbedoColor() const { return m_AlbedoColor; }

		void SetMetallic(float metallic) { m_Metallic = metallic; }
		float GetMetallic() const { return m_Metallic; }

		void SetRoughness(float roughness) { m_Roughness = roughness; }
		float GetRoughness() const { return m_Roughness; }

		void SetAlbedoMap(const Ref<Texture2D>& texture) { m_AlbedoMap = texture; }
		Ref<Texture2D> GetAlbedoMap() const { return m_AlbedoMap; }
		void SetAlbedoMapPath(const std::string& path) { m_AlbedoMapPath = path; }
		const std::string& GetAlbedoMapPath() const { return m_AlbedoMapPath; }

		void SetNormalMap(const Ref<Texture2D>& texture) { m_NormalMap = texture; }
		Ref<Texture2D> GetNormalMap() const { return m_NormalMap; }
		void SetNormalMapPath(const std::string& path) { m_NormalMapPath = path; }
		const std::string& GetNormalMapPath() const { return m_NormalMapPath; }

		void SetMetallicRoughnessMap(const Ref<Texture2D>& texture) { m_MetallicRoughnessMap = texture; }
		Ref<Texture2D> GetMetallicRoughnessMap() const { return m_MetallicRoughnessMap; }
		void SetMetallicRoughnessMapPath(const std::string& path) { m_MetallicRoughnessMapPath = path; }
		const std::string& GetMetallicRoughnessMapPath() const { return m_MetallicRoughnessMapPath; }

		// Compressed image bytes (PNG/JPEG) for textures embedded in the model file,
		// where the map path is empty; lets MeshCache persist them across sessions
		void SetAlbedoMapEmbeddedData(std::vector<uint8_t> data) { m_AlbedoMapEmbedded = std::move(data); }
		const std::vector<uint8_t>& GetAlbedoMapEmbeddedData() const { return m_AlbedoMapEmbedded; }

		void SetNormalMapEmbeddedData(std::vector<uint8_t> data) { m_NormalMapEmbedded = std::move(data); }
		const std::vector<uint8_t>& GetNormalMapEmbeddedData() const { return m_NormalMapEmbedded; }

		void SetMetallicRoughnessMapEmbeddedData(std::vector<uint8_t> data) { m_MetallicRoughnessMapEmbedded = std::move(data); }
		const std::vector<uint8_t>& GetMetallicRoughnessMapEmbeddedData() const { return m_MetallicRoughnessMapEmbedded; }

		void SetTwoSided(bool twoSided) { m_TwoSided = twoSided; }
		bool IsTwoSided() const { return m_TwoSided; }

		// Transparent materials render in a sorted back-to-front pass after opaque
		void SetTransparent(bool transparent) { m_Transparent = transparent; }
		bool IsTransparent() const { return m_Transparent; }

		void SetName(const std::string& name) { m_Name = name; }
		const std::string& GetName() const { return m_Name; }

		// Bind shader + upload material uniforms/textures (slots 0..2)
		void Bind() const;

		static Ref<Material> Create(const Ref<Shader>& shader);
	private:
		Ref<Shader> m_Shader;
		std::string m_Name = "Material";

		glm::vec4 m_AlbedoColor{ 1.0f };
		float m_Metallic = 0.0f;
		float m_Roughness = 0.5f;

		Ref<Texture2D> m_AlbedoMap;
		Ref<Texture2D> m_NormalMap;
		Ref<Texture2D> m_MetallicRoughnessMap;

		std::string m_AlbedoMapPath;
		std::string m_NormalMapPath;
		std::string m_MetallicRoughnessMapPath;

		std::vector<uint8_t> m_AlbedoMapEmbedded;
		std::vector<uint8_t> m_NormalMapEmbedded;
		std::vector<uint8_t> m_MetallicRoughnessMapEmbedded;

		bool m_TwoSided = false;
		bool m_Transparent = false;
	};

}
