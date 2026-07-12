#include "gepch.h"
#include "Material.h"

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

		m_Shader->SetFloat4("u_AlbedoColor", m_AlbedoColor);
		m_Shader->SetFloat("u_Metallic", m_Metallic);
		m_Shader->SetFloat("u_Roughness", m_Roughness);

		bool useAlbedoMap = m_AlbedoMap != nullptr;
		m_Shader->SetInt("u_UseAlbedoMap", useAlbedoMap ? 1 : 0);
		if (useAlbedoMap)
		{
			m_AlbedoMap->Bind(0);
			m_Shader->SetInt("u_AlbedoMap", 0);
		}

		bool useNormalMap = m_NormalMap != nullptr;
		m_Shader->SetInt("u_UseNormalMap", useNormalMap ? 1 : 0);
		if (useNormalMap)
		{
			m_NormalMap->Bind(1);
			m_Shader->SetInt("u_NormalMap", 1);
		}

		bool useMRMap = m_MetallicRoughnessMap != nullptr;
		m_Shader->SetInt("u_UseMetallicRoughnessMap", useMRMap ? 1 : 0);
		if (useMRMap)
		{
			m_MetallicRoughnessMap->Bind(2);
			m_Shader->SetInt("u_MetallicRoughnessMap", 2);
		}
	}

}
