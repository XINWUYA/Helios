#include "Pch.h"
#include "SampleScene.h"
#include "Helios/Common/AssetPath.h"
#include "Helios/ImGui/ImGuiLayer.h"

namespace Helios
{
	class SceneApp final : public Application
	{
	public:
		SceneApp() : Application("Sample_Scene", 1920, 1080, false)
		{
			/* 示例的调试面板走 ImGui，布局单独一份，几个 Sample 同时开不抢同一份 */
			SetUILayer(CreateSharedPtr<ImGuiLayer>(ImGuiLayer::Config{ .ini_name = "Sample_Scene" }));
			PushLayer(CreateSharedPtr<SampleSceneLayer>());
		}
	};

	UniquePtr<Application> CreateApplication()
	{
		return CreateUniquePtr<SceneApp>();
	}
}

int main(int argc, char** argv)
{
	Helios::Logger::Init();
	Helios::ParseProjectArgument(argc, argv);

	Helios::CreateApplication()->Run();

	Helios::Logger::Shutdown();
	return 0;
}
