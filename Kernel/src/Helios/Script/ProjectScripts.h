#pragma once
#include <string>

#include "Helios/Common/Export.h"

namespace Helios
{
	/* 项目脚本：把 <项目>/Cache/ScriptsBuild 下最新的 .hsmod 挂进进程。
	 * 脚本类由库的静态初始化登记进 ScriptRegistry，挂上就能给场景建实例；
	 * 产物由编辑器编，这里只管加载。iOS 不允许运行期加载代码，那边要把脚本编进主二进制。 */
	class HELIOS_API ProjectScripts
	{
	public:
		ProjectScripts() = default;
		~ProjectScripts();
		ProjectScripts(const ProjectScripts&) = delete;
		ProjectScripts& operator=(const ProjectScripts&) = delete;

		/* 挂最新产物；还没编过或平台不支持时返回 false，原因写在状态里 */
		bool LoadLatest();

		[[nodiscard]] const std::string& GetStatus() const { return m_Status; }
		[[nodiscard]] int GetClassCount() const;

	private:
		void*       m_hModule{ nullptr };
		std::string m_Status{ "Scripts not loaded" };
	};
}
