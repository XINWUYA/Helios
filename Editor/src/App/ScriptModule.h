#pragma once
#include <filesystem>
#include <memory>
#include <string>
#include "Helios/Core/Jobs.h"
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

		/* 启动时加载已有产物；还没编过就丢一个后台编译（新项目第一次打开走这条） */
		bool LoadLatest();
		/* 丢一个后台编译：立刻返回是否已开始。编完在 Tick 里交给调用方换上 */
		bool Recompile();
		/* 后台正在编：菜单可以据此置灰，状态行也据此显示 */
		[[nodiscard]] bool IsCompiling() const { return m_CompileActive; }

		/* 每帧调一次：收后台编译结果，外部构建编出新产物时也在这里重载。
		 * 返回 true 表示产物可以换了，调用方要走 ReloadScripts 让实例迁移。
		 * 编译期间不扫产物，链接到一半的 .hsmod 读进来会失败，时间戳还会记成最新。 */
		bool Tick(float delta_time);

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
		/* 后台可跑的部分：只读入参、只写磁盘与出参，不碰成员与全局 */
		static bool CompileProject(const std::filesystem::path& project_dir, std::string& status);
		/* 提交后台编译：路径与结果槽先备好，主线程只改状态 */
		bool RequestCompile();
		/* 外部产物监听（原 TickAutoReload 的内容） */
		void PollExternalModule(float delta_time);
		bool LoadFile(const std::string& path);

		struct BuildOutcome
		{
			bool Success{ false };
			std::string Status;
		};

		void*       m_hModule{ nullptr };
		std::string m_Status{ "Scripts not loaded" };

		/* 后台编译：任务终态 + 结果槽，Tick 里收 */
		Jobs::Task m_CompileTask{};
		std::shared_ptr<BuildOutcome> m_BuildOutcome{};
		bool m_CompileActive{ false };

		/* 外部产物监听：一秒查一次，只认修改时间往前走的那一份 */
		bool                            m_AutoReloadEnabled{ true };
		bool                            m_AutoCompileEnabled{ true };
		float                           m_AutoReloadTimer{ 0.0f };
		std::filesystem::file_time_type m_LastModuleTime{};
		bool                            m_AutoReloadPrimed{ false };
	};
}
