#include "gepch.h"
#include "Material.h"

#include "GanymedE/Assets/AssetManager.h"

namespace GanymedE {

	Material::Material(const Ref<Shader>& shader)
		: m_Shader(shader)
	{
	}

	Ref<Material> Material::Create(const Ref<Shader>& shader)
	{
		return CreateRef<Material>(shader);
	}

	void Material::Bind() const
	{
		GE_CORE_ASSERT(m_Shader, "Material has no shader!");
		m_Shader->Bind();

		// A map that was still loading when this material was built lands here, on the first
		// frame after its parse completes. The lookup only happens while a map is missing: once
		// it resolves the pointer is cached and this costs one null check per bind. A map that
		// never resolves - a deleted file - settles into the manager's failed set and answers
		// null without work.
		auto resolve = [](const Ref<Texture2D>& map, AssetHandle handle) -> Ref<Texture2D>
		{
			if (map || !IsAssetHandleValid(handle))
				return map;

			return AssetManager::GetAsset<Texture2D>(handle);
		};

		m_AlbedoMap = resolve(m_AlbedoMap, m_AlbedoMapHandle);
		m_NormalMap = resolve(m_NormalMap, m_NormalMapHandle);
		m_MetallicRoughnessMap = resolve(m_MetallicRoughnessMap, m_MetallicRoughnessMapHandle);

		m_Shader->SetFloat4("u_AlbedoColor", m_AlbedoColor);
		m_Shader->SetFloat("u_Metallic", m_Metallic);
		m_Shader->SetFloat("u_Roughness", m_Roughness);

		bool useAlbedoMap = m_AlbedoMap != nullptr;
		m_Shader->SetInt("u_UseAlbedoMap", useAlbedoMap ? 1 : 0);
		if (useAlbedoMap)
		{
			m_Shader->SetTexture("u_AlbedoMap", 0, m_AlbedoMap);
		}

		bool useNormalMap = m_NormalMap != nullptr;
		m_Shader->SetInt("u_UseNormalMap", useNormalMap ? 1 : 0);
		if (useNormalMap)
		{
			m_Shader->SetTexture("u_NormalMap", 1, m_NormalMap);
		}

		bool useMRMap = m_MetallicRoughnessMap != nullptr;
		m_Shader->SetInt("u_UseMetallicRoughnessMap", useMRMap ? 1 : 0);
		if (useMRMap)
		{
			m_Shader->SetTexture("u_MetallicRoughnessMap", 2, m_MetallicRoughnessMap);
		}
	}

}
