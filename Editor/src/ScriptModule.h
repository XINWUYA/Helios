#pragma once
#include <filesystem>
#include <string>
#include "HeliosConfig.h"

namespace Helios
{
	/* 脚本模块：编译当前项目 Scripts 下的源码并加载产物。
	 * 源文件、中间文件和产物都在项目自己的 Cache 下，引擎只提供编译模板；
	 * 旧模块不卸载（macOS 上 dlclose 会留悬挂引用），重载时清空注册表再加载。 */
	class ScriptModule
	{
	public:
		ScriptModule() = default;
		~ScriptModule();

		/* 启动时加载已有产物；还没编过就现编一次（新项目第一次打开走这条） */
		bool LoadLatest();
		/* 编译 + 加载：返回脚本类是否可用 */
		bool Recompile();

		/* 每帧调一次：外部构建（IDE）编出新产物时自动重载。
		 * 只盯 .hsmod 的修改时间 —— 编译失败不会产出新文件，所以天然是"成功才重载"。 */
		void TickAutoReload(float delta_time);

		/* 自动化的两个开关（跟项目本地存，见 EditorSettings）：
		 *   AutoReload —— 产物一变就重载；调试时关掉，免得编一次换一个 image
		 *   AutoCompile —— 打开项目时没产物就编一次；关掉则只报状态，等手动 Recompile */
		void SetAutoReloadEnabled(bool enabled) { m_AutoReloadEnabled = enabled; }
		[[nodiscard]] bool IsAutoReloadEnabled() const { return m_AutoReloadEnabled; }
		void SetAutoCompileEnabled(bool enabled) { m_AutoCompileEnabled = enabled; }
		[[nodiscard]] bool IsAutoCompileEnabled() const { return m_AutoCompileEnabled; }

		/* 项目脚本的构建目录：<项目>/Cache/ScriptsBuild（工程文件 + build/） */
		static std::filesystem::path ModuleDirectory();

		/* 目录下最新的 .hsmod。多配置生成器（Xcode / Visual Studio）会把产物放在
		 * <目录>/<配置>/ 下，所以是递归找；Loaded 里是运行期副本，跳过。 */
		static std::string FindLatestModule(const std::string& output_directory);

		[[nodiscard]] const std::string& GetStatus() const { return m_Status; }
		[[nodiscard]] int GetClassCount() const;

	private:
		bool Compile();
		bool LoadFile(const std::string& path);

		void*       m_hModule{ nullptr };
		std::string m_Status{ "Scripts not loaded" };

		/* 外部产物监听：一秒查一次，只认修改时间往前走的那一份 */
		bool                            m_AutoReloadEnabled{ true };
		bool                            m_AutoCompileEnabled{ true };
		float                           m_AutoReloadTimer{ 0.0f };
		std::filesystem::file_time_type m_LastModuleTime{};
		bool                            m_AutoReloadPrimed{ false };
	};
}
