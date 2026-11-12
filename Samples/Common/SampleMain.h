#pragma once

#include <string>

#include <Helios.h>

#include "Helios/ImGui/ImGuiLayer.h"

namespace Helios
{
	/* 示例的入口都一样：一个窗口、一层 ImGui 调试面板、一个示例层。
	 * 面板布局按示例名分文件，几个示例同时开不抢同一份 */
	template <typename TSampleLayer>
	int RunSample(int argc, char** argv, std::string name)
	{
		return RunApplication(argc, argv, [name = std::move(name)]
			{
				auto app = CreateUniquePtr<Application>(name, 1920, 1080, false);
				app->SetUILayer(CreateSharedPtr<ImGuiLayer>(ImGuiLayer::Config{ .ini_name = name }));
				app->PushLayer(CreateSharedPtr<TSampleLayer>());
				return app;
			});
	}
}
