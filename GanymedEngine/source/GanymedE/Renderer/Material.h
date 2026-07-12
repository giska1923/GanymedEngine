#pragma once

#include "GanymedE/Core/Core.h"
#include "GanymedE/Renderer/Shader.h"
#include "GanymedE/Renderer/Texture.h"

#include <glm/glm.hpp>

#include <string>
#include <unordered_map>

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

		void SetNormalMap(const Ref<Texture2D>& texture) { m_NormalMap = texture; }
		Ref<Texture2D> GetNormalMap() const { return m_NormalMap; }

		void SetMetallicRoughnessMap(const Ref<Texture2D>& texture) { m_MetallicRoughnessMap = texture; }
		Ref<Texture2D> GetMetallicRoughnessMap() const { return m_MetallicRoughnessMap; }

		void SetTwoSided(bool twoSided) { m_TwoSided = twoSided; }
		bool IsTwoSided() const { return m_TwoSided; }

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

		bool m_TwoSided = false;
	};

}
