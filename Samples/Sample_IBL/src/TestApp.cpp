#include "TestApp.h"
#include "SampleIBL.h"
#include "Helios/ImGui/ImGuiLayer.h"

namespace Helios
{
TestApp::TestApp() : Application("Sample_IBL", 1920, 1080, false)
{
	/* 示例的调试面板走 ImGui，布局单独一份，几个 Sample 同时开不抢同一份 */
	SetUILayer(CreateSharedPtr<ImGuiLayer>(ImGuiLayer::Config{ .ini_name = "Sample_IBL" }));
	PushLayer(CreateSharedPtr<SampleIBLLayer>());
}

UniquePtr<Application> CreateApplication()
{
	return CreateUniquePtr<TestApp>();
}

}