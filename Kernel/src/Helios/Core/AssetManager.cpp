#include "Pch.h"
#include "AssetManager.h"
#include "TextureSettings.h"
#include "Profiler.h"
#include <Helios/VirtualDevice/DeviceShader.h>
#include <Helios/Scene/Model.h>
#include <Helios/Scene/Material.h>
#include <Helios/Scene/AnimationClip.h>
#include <Helios/Scene/SceneCommon.h>
#include <Helios/Common/Utils.h>
#if !defined(PLATFORM_WINDOWS)
/* 模板的实例化符号继承模板定义处的可见性，只能整段放开 */
#pragma GCC visibility push(default)
#endif

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

	/* 缓存键用解析后的有效配置：同一张图被材质与预览请求时不会再互顶 */
	TextureLoadConfig AssetFactory<DeviceTexture>::ResolveConfig(const std::string& path, const TextureLoadConfig& load_config)
	{
		return TextureSettingsStore::Instance().Resolve(path, load_config);
	}

	/* 模型: 从路径加载 */
	SharedPtr<Model> AssetFactory<Model>::Create(const std::string& path)
	{
		return Model::Create(path);
	}

	/* 材质: 从 .mtl 文件加载独立材质资产(单条目、内嵌定义; 引用形态留给模型槽表使用) */
	SharedPtr<Material> AssetFactory<Material>::Create(const std::string& path)
	{
		auto material = CreateSharedPtr<Material>();
		if (!MaterialIO::ReadFile(path, *material))
			return nullptr;

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

		LoadConfig effective = load_config;
		if constexpr (requires { AssetFactory<AssetType>::ResolveConfig(path, load_config); })
			effective = AssetFactory<AssetType>::ResolveConfig(path, load_config);

		const std::string key = MakeKey(path, effective);
		const auto iter = m_AssetMap.find(key);
		if (iter != m_AssetMap.end())
			return iter->second.Asset;

		/* 找不到, 则创建; 创建失败不入缓存, 下次调用重试 */
		SharedPtr<AssetType> asset;
		if constexpr (std::is_same_v<LoadConfig, NoConfig>)
			asset = AssetFactory<AssetType>::Create(path);
		else
			asset = AssetFactory<AssetType>::Create(path, effective);

		if (asset != nullptr)
			m_AssetMap[key] = { asset, effective, path, load_config };
		return asset;
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
			for (auto& [key, entry] : m_AssetMap)
			{
				if (entry.Path == path && entry.Asset != nullptr)
				{
					AssetFactory<AssetType>::Refresh(entry.Asset, incoming);
					return;
				}
			}
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
		for (const auto& [key, entry] : m_AssetMap)
		{
			if (entry.Path == path)
				return true;
		}
		return false;
	}

	template<typename AssetType, typename LoadConfig>
	bool AssetStore<AssetType, LoadConfig>::ReloadByPath(const std::string& path)
	{
		if constexpr (!requires { std::declval<AssetType&>().Reload(); })
		{
			(void)path;
			return false;
		}
		else
		{
			std::lock_guard lock(m_Mutex);

			std::vector<Entry> refreshed;
			for (auto iter = m_AssetMap.begin(); iter != m_AssetMap.end();)
			{
				if (iter->second.Path != path)
				{
					++iter;
					continue;
				}

				Entry entry = iter->second;
				if (entry.Asset == nullptr || !entry.Asset->Reload())
				{
					++iter;
					continue;
				}

				iter = m_AssetMap.erase(iter);

				if constexpr (requires { AssetFactory<AssetType>::ResolveConfig(path, entry.Requested); })
					entry.Config = AssetFactory<AssetType>::ResolveConfig(path, entry.Requested);

				refreshed.push_back(std::move(entry));
			}

			for (Entry& entry : refreshed)
				m_AssetMap[MakeKey(path, entry.Config)] = std::move(entry);

			return !refreshed.empty();
		}
	}

	template<typename AssetType, typename LoadConfig>
	void AssetStore<AssetType, LoadConfig>::Clear()
	{
		std::lock_guard lock(m_Mutex);
		m_AssetMap.clear();
	}

	/* 显式实例化资源缓存, 供外部链接使用 */
	template class HELIOS_API AssetStore<DeviceShader, NoConfig>;
	template class HELIOS_API AssetStore<DeviceTexture, TextureLoadConfig>;
	template class HELIOS_API AssetStore<Model, NoConfig>;
	template class HELIOS_API AssetStore<Material, NoConfig>;
	template class HELIOS_API AssetStore<AnimationClip, NoConfig>;


	AssetManager& AssetManager::Instance()
	{
		static AssetManager instance;
		return instance;
	}

	bool AssetManager::ReloadTexture(const std::string& path)
	{
		return AssetStore<DeviceTexture, TextureLoadConfig>::Instance().ReloadByPath(path);
	}
}
#if !defined(PLATFORM_WINDOWS)
#pragma GCC visibility pop
#endif
