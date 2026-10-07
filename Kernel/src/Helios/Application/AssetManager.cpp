#include "Pch.h"
#include "AssetManager.h"
#include "Helios/Scene/Material.h"
#include "Helios/Scene/SceneCommon.h"
#include "Helios/VirtualDevice/DeviceShader.h"
#include "Helios/VirtualDevice/DeviceTexture.h"

namespace Helios
{
	/* 单例 */
	ShaderAssetManager& ShaderAssetManager::Instance()
	{
		static ShaderAssetManager instance;
		return instance;
	}

	/* 从文件中加载Shader */
	SharedPtr<DeviceShader> ShaderAssetManager::GetOrLoad(const std::string& path)
	{
		PROFILE_FUNCTION();

		/* 先在map中查找 */
		const auto key = ToID(path);
		const auto iter = m_ShaderAssetMap.find(key);
		if (iter != m_ShaderAssetMap.end())
			return iter->second;

		/* 找不到，则创建 */
		auto shader = DeviceShader::Create(path);
		m_ShaderAssetMap[key] = shader;
		return shader;
	}

	/* 清空所有Shader */
	void ShaderAssetManager::Clear()
	{
		m_ShaderAssetMap.clear();
	}

	/* 单例 */
	TextureAssetManager& TextureAssetManager::Instance()
	{
		static TextureAssetManager instance;
		return instance;
	}

	SharedPtr<DeviceTexture> TextureAssetManager::GetOrCreateTexture(const std::string& path)
	{
		constexpr TextureLoadConfig load_config;
		return GetOrCreateTexture(path, load_config);
	}

	SharedPtr<DeviceTexture> TextureAssetManager::GetOrCreateTexture(const std::string& path, const TextureLoadConfig& load_config)
	{
		PROFILE_FUNCTION();

		/* 先在map中查找 */
		const auto key = ToID(path);
		const auto iter = m_TextureAssetMap.find(key);
		if (iter != m_TextureAssetMap.end())
		{
			/* 判断LoadConfig是否被修改 */
			if (load_config == iter->second->GetTextureLoadConfig())
				return iter->second;
		}

		/* 找不到，则创建 */
		auto texture = DeviceTexture::Create(path, load_config);
		m_TextureAssetMap[key] = texture;
		return texture;
	}

	/* 清空所有Texture */
	void TextureAssetManager::Clear()
	{
		m_TextureAssetMap.clear();
	}

	/* 单例 */
	MaterialAssetManager& MaterialAssetManager::Instance()
	{
		static MaterialAssetManager instance;
		return instance;
	}

	/* 从 .mtl 文件加载单条材质（独立材质资产） */
	SharedPtr<Material> MaterialAssetManager::GetOrLoad(const std::string& path)
	{
		PROFILE_FUNCTION();

		/* 先在map中查找 */
		const auto key = ToID(path);
		const auto iter = m_MaterialAssetMap.find(key);
		if (iter != m_MaterialAssetMap.end())
			return iter->second;

		/* 独立材质资产约定：文件内单条目、内嵌定义（引用形态留给模型槽表使用） */
		auto group = CreateSharedPtr<MaterialGroup>();
		if (!group->Deserializer(path))
			return nullptr;

		const auto* entry = group->GetEntryByIndex(0);
		if (entry == nullptr || entry->pMaterial == nullptr)
		{
			CORE_LOG_ERROR("Material asset must contain one embedded material entry: {}.", path);
			return nullptr;
		}

		auto material = entry->pMaterial;
		material->SetPath(ABSOLUTE_PATH(path));
		m_MaterialAssetMap[key] = material;
		return material;
	}

	/* 材质资产被编辑保存后刷新缓存 */
	void MaterialAssetManager::Refresh(const std::string& path, const SharedPtr<Material>& material)
	{
		if (material == nullptr)
			return;

		const auto key = ToID(path);
		const auto iter = m_MaterialAssetMap.find(key);
		if (iter == m_MaterialAssetMap.end() || iter->second == nullptr)
			return;

		iter->second->CopyDefinitionFrom(*material);
	}

	/* 清空所有材质 */
	void MaterialAssetManager::Clear()
	{
		m_MaterialAssetMap.clear();
	}
}
