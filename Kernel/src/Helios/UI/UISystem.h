#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <entt/entt.hpp>

#include "Helios/Common/Common.h"
#include "Helios/Common/Export.h"
#include "Helios/Core/KeyCodes.h"
#include "UIRuntime.h"

namespace Helios
{
	class Scene;
	class RenderView;

	namespace UI
	{
		/* 场景级运行时 UI：把画布实体映射成 RmlUi 文档，每帧布局、转发输入、按序渲染。
		 * 一个场景一份；进程级的接口与输入映射见 UIRuntime.h（不把 RmlUi 类型带给脚本）。 */
		class HELIOS_API UISystem final
		{
		public:
			explicit UISystem(Scene& scene);
			~UISystem();
			UISystem(const UISystem&) = delete;
			UISystem& operator=(const UISystem&) = delete;

			/* 每帧推进：同步画布实体、收控件值、更新布局 */
			void Tick();

			/* 把本场景的画布画进视图。调用方负责渲染目标与视口 */
			void Render(RenderView& view);

			/* 场景里有没有可画的画布 */
			[[nodiscard]] bool HasCanvas() const { return !m_Canvases.empty(); }

			/* ---- 输入（视口物理像素，原点在视口左上角）：返回 UI 是否消费了这次事件 ----
			 * 指针移动与离开发给所有画布（清理悬停），按键与点击按叠放顺序从上往下问 */
			bool ProcessMouseMove(float x, float y);
			bool ProcessMouseButtonDown(int button);
			bool ProcessMouseButtonUp(int button);
			bool ProcessMouseWheel(float delta);
			void ProcessMouseLeave();

			/* ---- 键盘（焦点 / Tab 导航 / 按钮回车都走这条） ---- */
			bool ProcessKeyDown(KeyCode key);
			bool ProcessKeyUp(KeyCode key);
			/* 文本输入：只有直接输入的字符，输入法与预编辑未接（P3） */
			bool ProcessTextInput(uint32_t codepoint);

			/* 指针是否落在 UI 控件上：按当前悬停元素判定，空白处（只剩文档底色）
			 * 算没落在 UI 上。脚本据此决定这一下给界面还是给游戏 */
			[[nodiscard]] bool IsPointerOverControl() const;
			/* 按坐标问同一个问题（视口物理像素）：触摸落下时用它给手指分路 */
			[[nodiscard]] bool IsPointOverControl(float x, float y) const;

			/* ---- 脚本通道 ---- */
			/* 控件值：摇杆按 name 给 <name>_x / <name>_y，滑杆按 name 给一个值 */
			[[nodiscard]] float GetNumber(entt::entity canvas, const std::string& name, float fallback = 0.0f) const;
			/* 按 id 改元素文本：脚本把数值显示出来，元素不在或文本没变就什么都不做 */
			void SetText(entt::entity canvas, const std::string& element_id, const std::string& text);

		private:
			/* 一个画布实体的运行时状态。元素树不进组件 —— 组件快照按值拷贝，
			 * 整棵树塞进去会让撤销与存盘失控（见设计文档 D2） */
			struct CanvasRuntime;

			/* 按组件增删建 / 拆画布，文档路径改了就重载 */
			void SyncCanvases();
			[[nodiscard]] CanvasRuntime* FindCanvas(entt::entity canvas);
			[[nodiscard]] const CanvasRuntime* FindCanvas(entt::entity canvas) const;

			/* 把方向键推的值喂给画布里的摇杆，再把各控件本帧的值收进 Numbers */
			void SyncJoysticks(CanvasRuntime& canvas);
			/* 滑杆当前值收进 Numbers：脚本按 name 取，不用自己解析控件 */
			void SyncSliders(CanvasRuntime& canvas);
			/* 磁盘上的 .rml / .rcss 变了：文档改了拆掉重建，样式改了就地重排 */
			void CheckHotReload();

			Scene& m_Scene;
			/* 轮询节流：每这么久查一次文件时间戳（帧数，60 帧约半秒） */
			uint32_t m_HotReloadTicks{ 0 };
			std::vector<UniquePtr<CanvasRuntime>> m_Canvases{};
			/* 画布尺寸取最近一次渲染的视图尺寸（物理像素） */
			int m_ViewWidth{ 1 };
			int m_ViewHeight{ 1 };
		};
	}
}
