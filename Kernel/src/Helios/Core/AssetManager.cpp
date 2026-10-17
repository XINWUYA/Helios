#include "Pch.h"
#include "AssetManager.h"
#include "Profiler.h"
#include <Helios/VirtualDevice/DeviceShader.h>
#include <Helios/Scene/Model.h>
#include <Helios/Scene/Material.h>
#include <Helios/Scene/AnimationClip.h>
#include <Helios/Scene/SceneCommon.h>
#include <Helios/Common/Utils.h>

namespace Helios
{
	/* Shader: 从路径加载 */
	SharedPtr<DeviceShader> AssetFactory<DeviceShader>::Create(const std::string& path)
	{
		return DeviceShader::Create(path);
	}

	/* 纹理: 从路径加载, 附带加载配置 */
	SharedPtr<DeviceTexture> AssetFactory<DeviceTexture>::Create(const std::string& path, const TextureLoadConfig& load_config)
	{
		return DeviceTexture::Create(path, load_config);
	}

	/* 模型: 从路径加载 */
	SharedPtr<Model> AssetFactory<Model>::Create(const std::string& path)
	{
		return Model::Create(path);
	}

	/* 材质: 从 .mtl 文件加载独立材质资产(单条目、内嵌定义; 引用形态留给模型槽表使用) */
	SharedPtr<Material> AssetFactory<Material>::Create(const std::string& path)
	{
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
		return material;
	}

	/* 材质热刷新: 把新定义就地写进缓存中的共享材质, 已持有它的槽位随之生效 */
	void AssetFactory<Material>::Refresh(const SharedPtr<Material>& cached, const SharedPtr<Material>& incoming)
	{
		cached->CopyDefinitionFrom(*incoming);
	}

	/* 动画片段: 从 .anim 文件加载 */
	SharedPtr<AnimationClip> AssetFactory<AnimationClip>::Create(const std::string& path)
	{
		return AnimationClip::Create(path);
	}


	template<typename AssetType, typename LoadConfig>
	AssetStore<AssetType, LoadConfig>& AssetStore<AssetType, LoadConfig>::Instance()
	{
		static AssetStore instance;
		return instance;
	}

	template<typename AssetType, typename LoadConfig>
	SharedPtr<AssetType> AssetStore<AssetType, LoadConfig>::GetOrLoadInternal(const std::string& path, const LoadConfig& load_config)
	{
		PROFILE_FUNCTION();

		const auto key = ToID(path);
		const auto iter = m_AssetMap.find(key);
		if (iter != m_AssetMap.end())
		{
			if constexpr (std::is_same_v<LoadConfig, NoConfig>)
			{
				return iter->second.Asset;
			}
			else if (iter->second.Config == load_config)
			{
				return iter->second.Asset;
			}
		}

		if constexpr (std::is_same_v<LoadConfig, NoConfig>)
		{
			/* 找不到, 则创建; 创建失败不入缓存, 下次调用重试 */
			auto asset = AssetFactory<AssetType>::Create(path);
			if (asset != nullptr)
				m_AssetMap[key] = { asset, {} };
			return asset;
		}
		else
		{
			/* 找不到或配置不匹配, 则创建 */
			auto asset = AssetFactory<AssetType>::Create(path, load_config);
			if (asset != nullptr)
				m_AssetMap[key] = { asset, load_config };
			return asset;
		}
	}

	template<typename AssetType, typename LoadConfig>
	SharedPtr<AssetType> AssetStore<AssetType, LoadConfig>::GetOrLoad(const std::string& path)
	{
		std::lock_guard lock(m_Mutex);
		const LoadConfig default_config;
		return GetOrLoadInternal(path, default_config);
	}

	template<typename AssetType, typename LoadConfig>
	SharedPtr<AssetType> AssetStore<AssetType, LoadConfig>::GetOrLoad(const std::string& path, const LoadConfig& load_config) requires (!std::is_same_v<LoadConfig, NoConfig>)
	{
		std::lock_guard lock(m_Mutex);
		return GetOrLoadInternal(path, load_config);
	}

	template<typename AssetType, typename LoadConfig>
	void AssetStore<AssetType, LoadConfig>::Refresh(const std::string& path, const SharedPtr<AssetType>& incoming)
	{
		if constexpr (requires { AssetFactory<AssetType>::Refresh(std::declval<const SharedPtr<AssetType>&>(), std::declval<const SharedPtr<AssetType>&>()); })
		{
			if (incoming == nullptr)
				return;

			std::lock_guard lock(m_Mutex);
			const auto iter = m_AssetMap.find(ToID(path));
			if (iter == m_AssetMap.end() || iter->second.Asset == nullptr)
				return;

			AssetFactory<AssetType>::Refresh(iter->second.Asset, incoming);
		}
		else
		{
			/* 该类型未提供热刷新能力, 空操作 */
			(void)path;
			(void)incoming;
		}
	}

	template<typename AssetType, typename LoadConfig>
	std::vector<SharedPtr<AssetType>> AssetStore<AssetType, LoadConfig>::GetAllAssets() const
	{
		std::lock_guard lock(m_Mutex);
		std::vector<SharedPtr<AssetType>> assets;
		assets.reserve(m_AssetMap.size());
		for (const auto& [key, entry] : m_AssetMap)
			assets.push_back(entry.Asset);
		return assets;
	}

	template<typename AssetType, typename LoadConfig>
	size_t AssetStore<AssetType, LoadConfig>::GetCount() const
	{
		std::lock_guard lock(m_Mutex);
		return m_AssetMap.size();
	}

	template<typename AssetType, typename LoadConfig>
	bool AssetStore<AssetType, LoadConfig>::Contains(const std::string& path) const
	{
		std::lock_guard lock(m_Mutex);
		return m_AssetMap.find(ToID(path)) != m_AssetMap.end();
	}

	template<typename AssetType, typename LoadConfig>
	void AssetStore<AssetType, LoadConfig>::Clear()
	{
		std::lock_guard lock(m_Mutex);
		m_AssetMap.clear();
	}

	/* 显式实例化资源缓存, 供外部链接使用 */
	template class AssetStore<DeviceShader, NoConfig>;
	template class AssetStore<DeviceTexture, TextureLoadConfig>;
	template class AssetStore<Model, NoConfig>;
	template class AssetStore<Material, NoConfig>;
	template class AssetStore<AnimationClip, NoConfig>;


	AssetManager& AssetManager::Instance()
	{
		static AssetManager instance;
		return instance;
	}
}
