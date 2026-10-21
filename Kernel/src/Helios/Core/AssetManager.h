#pragma once
#include <string>
#include <unordered_map>
#include <mutex>
#include <type_traits>
#include <utility>
#include <vector>
#include "Helios/Common/Common.h"
#include "Helios/VirtualDevice/DeviceTexture.h"

namespace Helios
{
	class DeviceShader;
	class Model;
	class Material;
	class AnimationClip;

	/* 无加载配置的占位类型: 用于无需加载配置的资源 */
	struct NoConfig {};

	/* 资源工厂: 定义资源的加载配置类型与创建方式
	 * 特化须提供:
	 *   - using LoadConfig = ...;             (NoConfig 表示无加载配置)
	 *   - static SharedPtr<AssetType> Create(参数...);  (定义位于 AssetManager.cpp)
	 * 可选提供(热刷新, 未提供则 Refresh 对该类型为空操作):
	 *   - static void Refresh(const SharedPtr<AssetType>& cached, const SharedPtr<AssetType>& incoming); */
	template<typename AssetType>
	struct AssetFactory;

	/* Shader: 无加载配置 */
	template<>
	struct AssetFactory<DeviceShader>
	{
		using LoadConfig = NoConfig;
		static SharedPtr<DeviceShader> Create(const std::string& path);
	};

	/* 纹理: 带加载配置（Create 内部会按贴图资产设置解析） */
	template<>
	struct AssetFactory<DeviceTexture>
	{
		using LoadConfig = TextureLoadConfig;
		static SharedPtr<DeviceTexture> Create(const std::string& path, const TextureLoadConfig& load_config);
		/* 缓存键用解析后的有效配置，同一张图被不同调用点请求时仍只有一份 */
		static TextureLoadConfig ResolveConfig(const std::string& path, const TextureLoadConfig& load_config);
	};

	/* 模型: 无加载配置 */
	template<>
	struct AssetFactory<Model>
	{
		using LoadConfig = NoConfig;
		static SharedPtr<Model> Create(const std::string& path);
	};

	/* 材质: 无加载配置, 从 .mtl 文件加载独立材质资产(单条目、内嵌定义);
	 * 提供 Refresh: 材质资产被编辑保存后把新定义就地写进缓存中的共享材质 */
	template<>
	struct AssetFactory<Material>
	{
		using LoadConfig = NoConfig;
		static SharedPtr<Material> Create(const std::string& path);
		static void Refresh(const SharedPtr<Material>& cached, const SharedPtr<Material>& incoming);
	};

	/* 动画片段: 无加载配置, 从 .anim 文件加载 */
	template<>
	struct AssetFactory<AnimationClip>
	{
		using LoadConfig = NoConfig;
		static SharedPtr<AnimationClip> Create(const std::string& path);
	};

	/* 资源缓存(每种资源类型一个单例实例)
	 * AssetType : 资源类型
	 * LoadConfig: 加载配置类型, 无配置时使用 NoConfig
	 * 线程安全: 所有对资源映射的读写均受内部互斥锁保护。
	 * 失败(返回空指针)的创建不入缓存, 下次调用会重试。 */
	template<typename AssetType, typename LoadConfig = NoConfig>
	class HELIOS_API AssetStore
	{
	public:
		/* 单例 */
		static AssetStore& Instance();

		/* 获取或创建资源(使用默认加载配置) */
		SharedPtr<AssetType> GetOrLoad(const std::string& path);

		/* 获取或创建资源(指定加载配置); 无配置类型时此重载被约束禁用 */
		SharedPtr<AssetType> GetOrLoad(const std::string& path, const LoadConfig& load_config) requires (!std::is_same_v<LoadConfig, NoConfig>);

		/* 按路径重载（只对提供 Reload 的资源类型生效）。加载配置可能因资产设置变化，
		 * 重载后按新的有效配置重新分键。返回是否至少重载成功一个。 */
		bool ReloadByPath(const std::string& path);

		/* 刷新已缓存资源的内容(热重载); 仅对提供了 AssetFactory<AssetType>::Refresh 的类型生效,
		 * 未命中缓存时什么也不做。 */
		void Refresh(const std::string& path, const SharedPtr<AssetType>& incoming);

		/* 清空所有资源 */
		void Clear();

		/* 获取所有已缓存的资源 */
		std::vector<SharedPtr<AssetType>> GetAllAssets() const;
		/* 已缓存资源的数量 */
		size_t GetCount() const;
		/* 指定路径是否已缓存 */
		bool Contains(const std::string& path) const;

	private:
		AssetStore() = default;

		/* 无锁内部实现: 调用方须已持有 m_Mutex */
		SharedPtr<AssetType> GetOrLoadInternal(const std::string& path, const LoadConfig& load_config);

		/* 资源条目: 记录资源、创建时的有效配置、路径与调用点原始配置 */
		struct Entry
		{
			SharedPtr<AssetType> Asset;
			LoadConfig Config;
			std::string Path;
			LoadConfig Requested;
		};

		/* 缓存键：无配置类型用路径；有配置时拼上有效配置的指纹 */
		[[nodiscard]] static std::string MakeKey(const std::string& path, const LoadConfig& config)
		{
			if constexpr (std::is_same_v<LoadConfig, NoConfig>)
				return path;
			else
				return path + "#" + std::to_string(config.Fingerprint());
		}

		/* 资源路径 + 配置指纹 -> 资源条目的映射 */
		std::unordered_map<std::string, Entry> m_AssetMap;
		/* 保护资源映射的互斥锁 (mutable: const 查询接口也需加锁) */
		mutable std::mutex m_Mutex;
	};

	/* AssetStore显式实例化声明 */
	extern template class AssetStore<DeviceShader, NoConfig>;
	extern template class AssetStore<DeviceTexture, TextureLoadConfig>;
	extern template class AssetStore<Model, NoConfig>;
	extern template class AssetStore<Material, NoConfig>;
	extern template class AssetStore<AnimationClip, NoConfig>;

	/* 统一管理所有资源, 通过模板参数指定资源类型:
	 * - auto shader = AssetManager::Instance().GetOrLoad<DeviceShader>("Shaders/a.glsl");
	 * - auto texture = AssetManager::Instance().GetOrLoad<DeviceTexture>("Textures/a.png", TextureLoadConfig{});
	 * - auto model = AssetManager::Instance().GetOrLoad<Model>("Models/a.mesh");
	 * - auto material = AssetManager::Instance().GetOrLoad<Material>("Materials/a.mtl");
	 * - AssetManager::Instance().Refresh<Material>(path, edited);  (材质编辑保存后同步缓存)
	 */
	class HELIOS_API AssetManager
	{
	public:
		/* 单例 */
		static AssetManager& Instance();

		/* 获取或创建资源(使用默认/无加载配置) */
		template<typename AssetType>
		SharedPtr<AssetType> GetOrLoad(const std::string& path)
		{
			using LoadConfig = typename AssetFactory<AssetType>::LoadConfig;
			return AssetStore<AssetType, LoadConfig>::Instance().GetOrLoad(path);
		}

		/* 获取或创建资源(指定加载配置); 配置类型自动由资源类型推导 */
		template<typename AssetType, typename LoadConfig = typename AssetFactory<AssetType>::LoadConfig>
		SharedPtr<AssetType> GetOrLoad(const std::string& path, const LoadConfig& load_config)
		{
			return AssetStore<AssetType, LoadConfig>::Instance().GetOrLoad(path, load_config);
		}

		/* 刷新某类已缓存资源的内容(热重载) */
		template<typename AssetType>
		void Refresh(const std::string& path, const SharedPtr<AssetType>& incoming)
		{
			using LoadConfig = typename AssetFactory<AssetType>::LoadConfig;
			AssetStore<AssetType, LoadConfig>::Instance().Refresh(path, incoming);
		}

		/* 清空某类资源 */
		template<typename AssetType>
		void Clear()
		{
			using LoadConfig = typename AssetFactory<AssetType>::LoadConfig;
			AssetStore<AssetType, LoadConfig>::Instance().Clear();
		}

		/* 获取某类资源所有已缓存的资源 */
		template<typename AssetType>
		std::vector<SharedPtr<AssetType>> GetAll()
		{
			using LoadConfig = typename AssetFactory<AssetType>::LoadConfig;
			return AssetStore<AssetType, LoadConfig>::Instance().GetAllAssets();
		}

		/* 某类资源已缓存的数量 */
		template<typename AssetType>
		size_t GetCount()
		{
			using LoadConfig = typename AssetFactory<AssetType>::LoadConfig;
			return AssetStore<AssetType, LoadConfig>::Instance().GetCount();
		}

		/* 某类资源是否已缓存指定路径 */
		template<typename AssetType>
		bool Contains(const std::string& path)
		{
			using LoadConfig = typename AssetFactory<AssetType>::LoadConfig;
			return AssetStore<AssetType, LoadConfig>::Instance().Contains(path);
		}

		/* 贴图设置改完后按路径原地重载（材质持有的指针不变） */
		HELIOS_API bool ReloadTexture(const std::string& path);

	private:
		AssetManager() = default;
	};
}
