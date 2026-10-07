#pragma once
#include <string>
#include <unordered_map>

namespace Helios
{
	class DeviceShader;
	class DeviceTexture;
	class Material;
	struct TextureLoadConfig;

	/* 统一管理Shader, 避免重复创建 */
	class ShaderAssetManager
	{
	public:
		/* 单例 */
		static ShaderAssetManager& Instance();

		/* 从文件中加载Shader */
		SharedPtr<DeviceShader> GetOrLoad(const std::string& path);
		/* 清空所有Shader */
		void Clear();

	private:
		/* Shader名Hash到Shader的映射<NameHash, SharedPtr<DeviceShader>> */
		std::unordered_map<uint32_t, SharedPtr<DeviceShader>> m_ShaderAssetMap;
	};


	/* 统一管理纹理资产, 避免重复创建 */
	class TextureAssetManager
	{
	public:
		/* 单例 */
		static TextureAssetManager& Instance();

		/* 获取Texture */
		SharedPtr<DeviceTexture> GetOrCreateTexture(const std::string& path);
		SharedPtr<DeviceTexture> GetOrCreateTexture(const std::string& path, const TextureLoadConfig& load_config);
		/* 清空所有Texture */
		void Clear();

	private:
		TextureAssetManager() = default;

		/* 相对路径的Hash值作为Key */
		std::unordered_map<uint32_t, SharedPtr<DeviceTexture>> m_TextureAssetMap;
	};

	/* 统一管理材质资产（.mtl），同路径同对象 */
	class MaterialAssetManager
	{
	public:
		/* 单例 */
		static MaterialAssetManager& Instance();

		/* 从 .mtl 文件加载（约定单条目 = 独立材质资产），按路径缓存；
		 * 失败返回 nullptr（调用方兜底白模），文件需含一个内嵌定义条目。 */
		SharedPtr<Material> GetOrLoad(const std::string& path);
		/* 材质资产被编辑保存后刷新缓存：命中缓存（有槽位引用了这份资产）时把新定义
		 * 就地写进那份共享材质 —— 已持有它的槽位随之生效、无需重载场景；
		 * 未命中说明当前没人引用它，什么也不做（下次 GetOrLoad 自会读到新文件）。 */
		void Refresh(const std::string& path, const SharedPtr<Material>& material);
		/* 清空所有材质 */
		void Clear();

	private:
		MaterialAssetManager() = default;

		/* 资产路径Hash值作为Key */
		std::unordered_map<uint32_t, SharedPtr<Material>> m_MaterialAssetMap;
	};
}
