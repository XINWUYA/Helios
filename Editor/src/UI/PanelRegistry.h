#pragma once
#include <cstdint>

namespace Helios
{
	/* 面板注册表：窗口名、默认停靠、开关菜单与可见性的唯一数据源。
	 * 新增一个停靠窗口 = 这里加常量和一行 kPanels，面板自己用 PanelWindow 提交窗口。 */
	namespace Panel
	{
		/* 窗口键 = ImGui 窗口名（这里都无 ### 后缀，故即整个名字）。
		 * 一旦定名不得再改，否则用户的 imgui.ini 布局会失效。 */
		inline constexpr const char* kSceneHierarchy  = "Scene Hierarchy";
		inline constexpr const char* kProperties      = "Properties";
		inline constexpr const char* kScene           = "Scene";
		/* 游戏画面：运行世界主相机的渲染结果，默认与 Scene 成页签 */
		inline constexpr const char* kGame            = "Game";
		inline constexpr const char* kModel           = "Model";
		inline constexpr const char* kStatInfo        = "Stat Info";
		/* 资源浏览器：目录树 + 目录内容合成一个面板（原先 File List / Resource Browser 是两个页签） */
		inline constexpr const char* kResourceBrowser = "Resource Browser";
		/* 渲染图可视化页：依赖图 + 各 Pass 的中间渲染结果 */
		inline constexpr const char* kFrameGraph      = "Frame Graph";
		/* 日志窗口：Kernel / Editor 两个 logger 的输出（捕获 sink 实时填充） */
		inline constexpr const char* kLog            = "Log";
		/* 时间轴：选中实体的动画片段编辑（播放头 / 关键帧 / 录制） */
		inline constexpr const char* kTimeline       = "Timeline";
		/* 脚本执行顺序：全场景脚本挂载点的 Order 总表（从 Scripts 菜单打开，默认不占布局） */
		inline constexpr const char* kScriptOrder    = "Script Order";
		inline constexpr const char* kEditorCamera   = "Editor Camera";
		/* 着色器变体：源码声明的维度、已加载的变体与各自加载耗时 */
		inline constexpr const char* kShaderVariants = "Shader Variants";

		/* 默认停靠的语义槽位，与 BuildDefaultLayout 里的 DockNode 划分一一对应 */
		enum class DockSlot : uint8_t
		{
			Center = 0,   /* 中央视口 */
			LeftTop,      /* 左栏上半（层级树） */
			LeftBottom,   /* 左栏下半（资产浏览器） */
			RightTop,     /* 右栏上半（属性 / 模型参数） */
			RightBottom,  /* 右栏下半（统计） */
			CenterBottom, /* 中栏下半（日志窗口） */
			Floating,     /* 不进默认布局，打开时浮动 */
		};

		/* 开关放在哪个菜单里：None = 常驻窗口，没有关闭按钮 */
		enum class Menu : uint8_t
		{
			None = 0,
			Window,
			Scripts,
		};

		struct Desc
		{
			const char*   Id;                 /* 窗口键 */
			DockSlot      Slot;               /* 默认停靠槽位 */
			Menu          ToggleMenu;         /* 开关所在菜单 */
			const char*   MenuLabel;          /* 菜单里的名字 */
			bool          DefaultVisible;     /* 启动时开不开 */
			const char*   Tooltip{ nullptr }; /* 菜单项悬停说明 */
		};

		/* 全部停靠窗口：调整默认分栏、菜单开关都只改这里 */
		inline constexpr Desc kPanels[] = {
			{ kSceneHierarchy,  DockSlot::LeftTop,      Menu::None,    nullptr,          true },
			{ kResourceBrowser, DockSlot::LeftBottom,   Menu::None,    nullptr,          true },
			{ kScene,           DockSlot::Center,       Menu::Window,  "Scene Viewport", true },
			{ kGame,            DockSlot::Center,       Menu::Window,  "Game",           true,
				"Game view: the primary camera of the running world" },
			{ kModel,           DockSlot::Center,       Menu::Window,  "Model Viewport", false },
			{ kFrameGraph,      DockSlot::Center,       Menu::Window,  "Frame Graph",    true },
			{ kProperties,      DockSlot::RightTop,     Menu::None,    nullptr,          true },
			{ kStatInfo,        DockSlot::RightBottom,  Menu::Window,  "Stat Info",      true },
			{ kLog,             DockSlot::CenterBottom, Menu::Window,  "Log",            true },
			{ kTimeline,        DockSlot::CenterBottom, Menu::Window,  "Timeline",       true },
			{ kEditorCamera,    DockSlot::Floating,     Menu::Window,  "Editor Camera",  false,
				"Live read-only parameters of the scene editor camera" },
			{ kScriptOrder,     DockSlot::Floating,     Menu::Scripts, "Script Order",   false,
				"Global script execution order, sorted by Order" },
			{ kShaderVariants,  DockSlot::Floating,     Menu::Window,  "Shader Variants", false,
				"Declared variant dimensions and loaded variants per shader" },
		};

		/* 窗口开关的唯一存放处：PanelWindow、菜单和各层读写的都是这一份。
		 * 只登记了开关菜单的窗口才有意义；常驻窗口恒为 true。 */
		bool& VisibleFlag(const char* id);
		inline bool IsVisible(const char* id) { return VisibleFlag(id); }
		inline void SetVisible(const char* id, bool visible) { VisibleFlag(id) = visible; }

		/* 在当前打开的菜单里按表画出这一组窗口的开关项 */
		void DrawToggleMenuItems(Menu menu);
	}
}
