#pragma once
#include <cstdint>

namespace Helios
{
	/* 面板注册表：窗口名和默认停靠的唯一数据源。窗口名集中成常量、要改只改这个文件；带工厂和
	 * 可见性的完整面板注册（IEditorPanel）不归本模块管。 */
	namespace Panel
	{
		/* 窗口键 = ImGui 窗口名（这里都无 ### 后缀，故即整个名字）。
		 * 一旦定名不得再改，否则用户的 imgui.ini 布局会失效。 */
		inline constexpr const char* kSceneHierarchy  = "Scene Hierarchy";
		inline constexpr const char* kProperties      = "Properties";
		inline constexpr const char* kScene           = "Scene";
		inline constexpr const char* kModel           = "Model";
		inline constexpr const char* kModelHelper     = "Model Helper";
		inline constexpr const char* kStatInfo        = "Stat Info";
		/* 资源浏览器：目录树 + 目录内容合成一个面板（原先 File List / Resource Browser 是两个页签） */
		inline constexpr const char* kResourceBrowser = "Resource Browser";
		/* 渲染图可视化页：依赖图 + 各 Pass 的中间渲染结果 */
		inline constexpr const char* kFrameGraph      = "Frame Graph";

		/* 默认停靠的语义槽位，与 BuildDefaultLayout 里的 DockNode 划分一一对应 */
		enum class DockSlot : uint8_t
		{
			Center = 0,   /* 中央视口 */
			LeftTop,      /* 左栏上半（层级树） */
			LeftBottom,   /* 左栏下半（资产浏览器） */
			RightTop,     /* 右栏上半（属性 / 模型参数） */
			RightBottom,  /* 右栏下半（统计） */
		};

		struct Desc
		{
			const char* Id;     /* 窗口键 */
			DockSlot    Slot;   /* 默认停靠槽位 */
		};

		/* 默认布局表：调整默认分栏只改这里 */
		inline constexpr Desc kDefaultLayout[] = {
			{ kSceneHierarchy,  DockSlot::LeftTop     },
			{ kResourceBrowser, DockSlot::LeftBottom  },
			{ kScene,           DockSlot::Center      },
			{ kModel,           DockSlot::Center      },
			{ kFrameGraph,      DockSlot::Center      },
			{ kProperties,      DockSlot::RightTop    },
			{ kModelHelper,     DockSlot::RightTop    },
			{ kStatInfo,        DockSlot::RightBottom },
		};
	}
}
