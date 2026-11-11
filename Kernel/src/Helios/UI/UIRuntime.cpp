#include "Pch.h"
#include "UIRuntime.h"

#include <RmlUi/Core.h>

#include "ElementJoystick.h"
#include "UIFrameRenderer.h"
#include "UIRenderInterface.h"
#include "UIRmlInterfaces.h"

#include "Helios/Scene/SceneCommon.h"

namespace Helios
{
	namespace UI
	{
		namespace
		{
			/* 接口实现是进程级单例：换场景、换项目都不重建 */
			UISystemInterface g_SystemInterface;
			UIFileInterface g_FileInterface;
			UIRenderInterface g_RenderInterface;
			UIFrameRenderer g_FrameRenderer;
			Rml::ElementInstancerGeneric<ElementJoystick> g_JoystickInstancer;

			bool g_Initialised{ false };
			/* 默认关：游戏构建不轮询磁盘，编辑器启动时打开 */
			bool g_HotReload{ false };
			std::atomic<uint32_t> g_NextHostSerial{ 1 };
		}

		void Initialise()
		{
			if (g_Initialised)
				return;

			Rml::SetSystemInterface(&g_SystemInterface);
			Rml::SetFileInterface(&g_FileInterface);
			Rml::SetRenderInterface(&g_RenderInterface);
			if (!Rml::Initialise())
			{
				CORE_LOG_ERROR("RmlUi initialisation failed");
				return;
			}

			g_FrameRenderer.Init();
			/* 自定义标签：<joystick name="move"> 由元素工厂建对应的控件 */
			Rml::Factory::RegisterElementInstancer("joystick", &g_JoystickInstancer);

			/* 运行期只需要一套拉丁字体；中文按设计文档的计划等 OFL 字库到位再接 */
			Rml::LoadFontFace(Rml::String(ABSOLUTE_PATH("EditorRes/fonts/opensans/OpenSans-Regular.ttf").c_str()));
			Rml::LoadFontFace(Rml::String(ABSOLUTE_PATH("EditorRes/fonts/opensans/OpenSans-SemiBold.ttf").c_str()));

			g_Initialised = true;
		}

		void Shutdown()
		{
			if (!g_Initialised)
				return;

			/* 先放掉自己持有的 GPU 资源，再关 RmlUi；最后摘接口，别让它的析构拿着失效指针 */
			g_FrameRenderer.Shutdown();
			g_RenderInterface.Shutdown();
			Rml::Shutdown();
			Rml::SetRenderInterface(nullptr);
			Rml::SetFileInterface(nullptr);
			Rml::SetSystemInterface(nullptr);
			g_Initialised = false;
		}

		bool IsInitialised()
		{
			return g_Initialised;
		}

		void SetHotReloadEnabled(bool enable)
		{
			g_HotReload = enable;
		}

		bool IsHotReloadEnabled()
		{
			return g_HotReload;
		}

		uint32_t NextHostSerial()
		{
			return g_NextHostSerial.fetch_add(1);
		}

		UIRenderInterface& RenderInterface()
		{
			return g_RenderInterface;
		}

		UIFileInterface& FileInterface()
		{
			return g_FileInterface;
		}

		UISystemInterface& SystemInterface()
		{
			return g_SystemInterface;
		}

		UIFrameRenderer& FrameRenderer()
		{
			return g_FrameRenderer;
		}
	}
}
