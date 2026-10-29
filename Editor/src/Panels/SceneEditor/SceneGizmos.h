#pragma once
/* 场景 gizmo（编辑器视口叠加层）：地面刻度网格 / 世界坐标轴 / 各类型实体图标。职责边界：
 * 只画"场景辅助和实体图标"；操作 gizmo（TransformGizmo.h）和视图指示器（ViewGizmo.h）不在本模块。
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
		bool ShowGrid{ true };				/* 地面刻度网格 */
		bool ShowWorldAxis{ true };			/* 世界坐标轴（原点三轴） */
		bool ShowLight{ true };				/* 光源图标（平行 / 点 / 聚光） */
		bool ShowCamera{ true };			/* 相机图标（视锥 + 机身盒） */
		bool ShowReflectionProbe{ true };	/* 反射探针盒 */
		bool ShowSprite{ true };			/* 精灵框 */
	};

	ViewportGizmoOptions& GetViewportGizmoOptions();

	/* 操作 gizmo（平移 / 旋转 / 缩放）的帧状态：UI 侧（SceneEditorLayer）每帧写，
	 * 绘制侧（SceneGizmos::Submit）读。手柄是实体网格，绘制发生在渲染 Pass 里，
	 * 交互发生在 ImGui 阶段 —— 这是两者之间唯一的通道。 */
	struct TransformGizmoState
	{
		bool Visible{ false };
		/* 拥有这条 gizmo 的视口相机：模型编辑器也用 EditorCamera，靠它区分，
		 * 场景里的手柄不会画到模型预览上 */
		const EditorCamera* Camera{ nullptr };
		glm::mat4 World{ 1.0f };	/* 实体世界变换（手柄原点 = 平移列，轴基 = 三列归一化） */
		int32_t Operation{ -1 };	/* 0 = 平移 / 1 = 旋转 / 2 = 缩放，-1 = 不画 */
		bool LocalSpace{ true };	/* 轴基取实体旋转（true）还是世界轴（false） */
		int32_t Highlight{ -1 };	/* 高亮手柄：0 / 1 / 2 = 轴，3 = 轴心，4..6 = 平面，7 = 白色外环 */

		/* 旋转拖拽中的角度弧：半径 <= 0 表示本帧不画。起始角与扫过角都落在
		 * 法线为 ArcAxis 的环平面内（基由 RingBasis 给出，与命中侧同源） */
		glm::vec3 ArcAxis{ 0.0f, 0.0f, 1.0f };
		float ArcStart{ 0.0f };
		float ArcSweep{ 0.0f };
		float ArcRadius{ 0.0f };
		int32_t ArcAxisIndex{ -1 };	/* 扇区颜色跟这个轴走，-1 表示用高亮橙 */

		/* 平移拖拽中的轨迹：从起点到当前原点画虚线，Distance 是沿轴分量（自由拖动取长度），
		 * 供屏幕上的距离标签使用。Active 为 false 时两者都不画 */
		bool DragActive{ false };
		glm::vec3 DragStart{ 0.0f };
		glm::vec3 DragCurrent{ 0.0f };
		float DragDistance{ 0.0f };
		float DragScale{ 0.0f };	/* 缩放拖拽倍数，0 表示不在缩放 */
	};

	TransformGizmoState& GetTransformGizmoState();

	/* 每逻辑像素在单位距离上的世界尺寸（屏幕恒定尺寸的换算系数）：操作 gizmo 的渲染与拾取
	 * 共用同一份，避免两边各算一次而漂移。 */
	float ComputeGizmoScreenScale(const EditorCamera& camera);

	namespace SceneGizmos
	{
		/* 提交本帧的场景 gizmo 叠加层 —— 在 AxisPass 的执行回调里、Bind() 之后调用：依次提交网格 /
		 * 世界轴 / 各类型实体图标（按 ViewportGizmoOptions 过滤），调试视图选了 Skeleton 时
		 * 最后叠骨骼节点与连线。执行期参数（线宽 / 屏幕展开 / 网格对齐）都在内部解决。 */
		void Submit(RenderView& render_view, const EditorCamera& camera);

		/* 提交操作 gizmo（平移箭头 / 旋转环 / 缩放方块）：实体网格 + 统一色材质，
		 * 关深度测试所以永远画在场景前面。operation < 0 或布局退化时不画。 */
		void SubmitTransformGizmo(RenderView& render_view, const glm::mat4& entity_world,
			int32_t operation, int32_t highlight, float gizmo_screen_scale);

		/* 子网格高亮（模型预览：悬停子模型卡时用）：把场景里模型的指定网格段用强调色
		 * 平铺再画一遍 —— 深度测试跟场景一致，只染该段看得见的表面，被别的物体挡住的不透出来。
		 * 在 Submit 之后、线框之前调用（线框画在最上层）；调用方保证 segment_index >= 0。 */
		void SubmitModelSegmentHighlight(RenderView& render_view, int segment_index);
	}
}
