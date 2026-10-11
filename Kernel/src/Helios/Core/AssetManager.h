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

	/* 纹理: 带加载配置 */
	template<>
	struct AssetFactory<DeviceTexture>
	{
		using LoadConfig = TextureLoadConfig;
		static SharedPtr<DeviceTexture> Create(const std::string& path, const TextureLoadConfig& load_config);
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

	/* 资源缓存(每种资源类型一个单例实例)
	 * AssetType : 资源类型
	 * LoadConfig: 加载配置类型, 无配置时使用 NoConfig
	 * 线程安全: 所有对资源映射的读写均受内部互斥锁保护。
	 * 失败(返回空指针)的创建不入缓存, 下次调用会重试。 */
	template<typename AssetType, typename LoadConfig = NoConfig>
	class AssetStore
	{
	public:
		/* 单例 */
		static AssetStore& Instance();

		/* 获取或创建资源(使用默认加载配置) */
		SharedPtr<AssetType> GetOrLoad(const std::string& path);

		/* 获取或创建资源(指定加载配置); 无配置类型时此重载被约束禁用 */
		SharedPtr<AssetType> GetOrLoad(const std::string& path, const LoadConfig& load_config) requires (!std::is_same_v<LoadConfig, NoConfig>);

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

		/* 资源条目: 记录资源及其创建时使用的加载配置 */
		struct Entry
		{
			SharedPtr<AssetType> Asset;
			LoadConfig Config;
		};

		/* 资源路径Hash到资源条目的映射<NameHash, Entry> */
		std::unordered_map<uint32_t, Entry> m_AssetMap;
		/* 保护资源映射的互斥锁 (mutable: const 查询接口也需加锁) */
		mutable std::mutex m_Mutex;
	};

	/* AssetStore显式实例化声明 */
	extern template class AssetStore<DeviceShader, NoConfig>;
	extern template class AssetStore<DeviceTexture, TextureLoadConfig>;
	extern template class AssetStore<Model, NoConfig>;
	extern template class AssetStore<Material, NoConfig>;

	/* 统一管理所有资源, 通过模板参数指定资源类型:
	 * - auto shader = AssetManager::Instance().GetOrLoad<DeviceShader>("Shaders/a.glsl");
	 * - auto texture = AssetManager::Instance().GetOrLoad<DeviceTexture>("Textures/a.png", TextureLoadConfig{});
	 * - auto model = AssetManager::Instance().GetOrLoad<Model>("Models/a.mesh");
	 * - auto material = AssetManager::Instance().GetOrLoad<Material>("Materials/a.mtl");
	 * - AssetManager::Instance().Refresh<Material>(path, edited);  (材质编辑保存后同步缓存)
	 */
	class AssetManager
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

	private:
		AssetManager() = default;
	};
}
