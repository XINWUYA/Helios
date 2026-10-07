#pragma once
/* 场景 gizmo（编辑器视口叠加层）：地面刻度网格 / 世界坐标轴 / 各类型实体图标。职责边界：
 * 只画"场景辅助和实体图标"；操作 gizmo（ImGuizmo）和视图指示器（ViewGizmo.h）不在本模块。
 * 新增实体图标 = 加几何 + 提交函数 + 在 Submit 里挂一行；相机只在 AxisPass 里接一行。 */

#include <glm/glm.hpp>

namespace Helios
{
	class RenderView;
	class EditorCamera;

	/* 场景 gizmo 的全局显隐（编辑器会话级，所有视口共用一份）。
	 * 绘制侧（SceneGizmos::Submit）只读、UI 侧（工具栏最右的 Gizmos 菜单）读写
	 * —— 单一数据源。 */
	struct ViewportGizmoOptions
	{
		bool MasterEnabled{ true };			/* 总开关：关则场景辅助与实体图标全部隐藏（分项状态保留） */
		bool ShowGrid{ true };				/* 地面刻度网格 */
		bool ShowWorldAxis{ true };			/* 世界坐标轴（原点三轴） */
		bool ShowLight{ true };				/* 光源图标（平行 / 点 / 聚光） */
		bool ShowCamera{ true };			/* 相机图标（视锥 + 机身盒） */
		bool ShowReflectionProbe{ true };	/* 反射探针盒 */
		bool ShowSprite{ true };			/* 精灵框 */
	};

	ViewportGizmoOptions& GetViewportGizmoOptions();

	namespace SceneGizmos
	{
		/* 提交本帧的场景 gizmo 叠加层 —— 在 AxisPass 的执行回调里、Bind() 之后调用：依次提交网格 /
		 * 世界轴 / 各类型实体图标（按 ViewportGizmoOptions 过滤）。执行期参数（线宽 / 屏幕展开 /
		 * 网格对齐）都在内部解决。 */
		void Submit(RenderView& render_view, const EditorCamera& camera);
	}
}
