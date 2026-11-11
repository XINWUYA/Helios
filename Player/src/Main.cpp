#include "Pch.h"

#include "PlayerLayer.h"

#include "Helios/Application/Application.h"
#include "Helios/Common/AssetPath.h"
#include "Helios/Core/Logger.h"

namespace Helios
{
	/* 客户端实现：Application 负责窗口与渲染器，场景层负责载入与出图 */
	UniquePtr<Application> CreateApplication()
	{
		UniquePtr<Application> app = CreateUniquePtr<Application>("Helios Player");
		app->PushLayer(CreateSharedPtr<PlayerLayer>());
		return app;
	}
}

#if !HELIOS_PLATFORM_IOS

int main(int argc, char** argv)
{
	Helios::Logger::Init();

	/* 项目根：编译期默认值，--project 可覆盖 */
	Helios::ParseProjectArgument(argc, argv);

	{
		auto app = Helios::CreateApplication();
		app->Run();
	}

	Helios::Logger::Shutdown();
	return 0;
}

#endif
