#include <Helios.h>

#include "Helios/Application/GameLayer.h"
#include "Helios/Application/ProjectConfig.h"

/* 通用的玩家外壳：直接跑当前项目的启动场景，场景、脚本和画质都从项目里读，换项目不用改这里 */
int main(int argc, char** argv)
{
	return Helios::RunApplication(argc, argv, []
		{
			using namespace Helios;

			auto app = CreateUniquePtr<Application>("Helios Player");
			/* 资源根在应用构造时才定下来（随包工程优先），标题等它定了再取项目名 */
			app->GetWindow().SetWindowTitle(LoadProjectConfig().Name);
			app->PushLayer(CreateSharedPtr<GameLayer>());
			return app;
		});
}
