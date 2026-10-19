#pragma once
#include <string>
#include "HeliosConfig.h"

namespace Helios
{
	/* 脚本模块：编译 Assets/Scripts 下的源码，加载产物到进程里。
	 * 旧模块不卸载（macOS 上 dlclose 会留下悬挂引用），重载只是再加载一个新文件；
	 * 注册表在加载前清空，所以旧库里的类不会再被调用。 */
	class ScriptModule
	{
	public:
		ScriptModule() = default;
		~ScriptModule();

		/* 启动时加载已有产物；没有产物就只记一条状态 */
		bool LoadLatest();
		/* 编译 + 加载：返回脚本类是否可用 */
		bool Recompile();

		/* 产物目录下最新的 .hsmod。多配置生成器（Xcode / Visual Studio）会把产物放在
		 * <目录>/<配置>/ 下，所以是递归找；Loaded 里是运行期副本，跳过。
		 * 参数留出来是给检查用的。 */
		static std::string FindLatestModule(const std::string& output_directory = HELIOS_SCRIPT_OUTPUT_DIR);

		[[nodiscard]] const std::string& GetStatus() const { return m_Status; }
		[[nodiscard]] int GetClassCount() const;

	private:
		bool Compile();
		bool LoadFile(const std::string& path);

		void*       m_hModule{ nullptr };
		std::string m_Status{ "Scripts not loaded" };
	};
}
