#pragma once
#include <cstdint>

#include "Helios/Common/Export.h"

namespace Helios
{
	namespace UI
	{
		class UIRenderInterface;
		class UIFileInterface;
		class UISystemInterface;
		class UIFrameRenderer;

		/* RmlUi 的全局状态：接口、字体与渲染层。进程内一份，第一次建画布时初始化 */
		HELIOS_API void Initialise();
		HELIOS_API void Shutdown();
		[[nodiscard]] HELIOS_API bool IsInitialised();

		/* .rml / .rcss 改完自动重载。只有编辑器打开：游戏构建不该轮询磁盘 */
		HELIOS_API void SetHotReloadEnabled(bool enable);
		[[nodiscard]] HELIOS_API bool IsHotReloadEnabled();

		/* 每个画布宿主一个序号，拼进 Context 名字：编辑世界与运行世界的句柄会撞 */
		HELIOS_API uint32_t NextHostSerial();

		/* 进程级的接口实现与落地方：引擎内部取用点，脚本侧不该依赖它们 */
		HELIOS_API UIRenderInterface& RenderInterface();
		HELIOS_API UIFileInterface& FileInterface();
		HELIOS_API UISystemInterface& SystemInterface();
		HELIOS_API UIFrameRenderer& FrameRenderer();
	}
}
