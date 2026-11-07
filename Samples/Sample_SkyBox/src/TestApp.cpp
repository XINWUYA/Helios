#include "TestApp.h"
#include "SampleSkyBox.h"
#include "Helios/ImGui/ImGuiLayer.h"

namespace Helios
{
TestApp::TestApp() : Application("Sample_SkyBox", 1920, 1080, false)
{
	/* 示例的调试面板走 ImGui，布局单独一份，几个 Sample 同时开不抢同一份 */
	SetUILayer(CreateSharedPtr<ImGuiLayer>(ImGuiLayer::Config{ .ini_name = "Sample_SkyBox" }));
	PushLayer(CreateSharedPtr<SampleSkyBox>());
}

UniquePtr<Application> CreateApplication()
{
	return CreateUniquePtr<TestApp>();
}

}