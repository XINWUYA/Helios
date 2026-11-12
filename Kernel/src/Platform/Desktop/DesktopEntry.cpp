#include "Pch.h"
#include "Helios/Application/Application.h"
#include "Helios/Common/AssetPath.h"
#include "Helios/Core/Logger.h"

#if HELIOS_PLATFORM_WINDOWS && defined(HELIOS_DEBUG)
#include <crtdbg.h>
#endif

namespace Helios
{
	int RunApplication(int argc, char** argv, ApplicationFactory factory)
	{
#if HELIOS_PLATFORM_WINDOWS && defined(HELIOS_DEBUG)
		/* 调试版退出时自动报内存泄漏 */
		_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

		Logger::Init();
		/* 项目根：编译期默认值，--project 可覆盖 */
		ParseProjectArgument(argc, argv);

		{
			UniquePtr<Application> app = factory();
			app->Run();
		}

		Logger::Shutdown();
		return 0;
	}
}
