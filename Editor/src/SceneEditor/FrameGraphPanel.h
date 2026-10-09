#pragma once
#include <string>
#include <vector>
#include <imgui.h>

namespace Helios
{
	class FrameGraph;

	/* FrameGraph 可视化页：整页 = 所选相机当前帧的渲染图 —— Pass 行布局（每个 Pass 一行，行头和
	 * 背景合并 + 标题；输出资源卡叠在区域内，缩略图 = 执行完当刻的快照；可缩放 / 平移 / 点选；
	 * 默认只显示生效的 Pass；抓取开关由 SceneEditorLayer 落地）。连线相邻直连、其余绕左侧总线；
	 * 同名资源副本跨行对齐（虚线关联）。 */
	class FrameGraphPanel final
	{
	public:
		/* 可视化目标：一个相机对应的渲染图（Graph 与相机同生命周期，跨帧稳定） */
		struct CameraEntry
		{
			std::string Name;              /* 显示名（实体名 / 编辑器相机） */
			FrameGraph* Graph{ nullptr };  /* 该相机的渲染图 */
		};

		/* 画整页。cameras 为本帧可选的相机（编辑器相机在前、场景相机随后）；
		 * visible 与窗口右上角关闭按钮同源（View 菜单共用同一个 bool）。 */
		void OnImGuiRenderer(const std::vector<CameraEntry>& cameras, bool& visible);

		/* 本帧 UI 是否请求抓取（下一帧渲染时生效） */
		[[nodiscard]] bool IsCaptureRequested() const { return m_CaptureRequested; }
		/* 抓取目标 = 当前选中的相机渲染图（未选择 / 无相机时为 nullptr） */
		[[nodiscard]] const FrameGraph* GetCaptureTarget() const { return m_SelectedGraph; }

		/* 上一帧依赖图节点的屏幕矩形（自检 / 自动化验证用） */
		struct NodeRect
		{
			std::string Name;
			bool IsPassNode{ false };
			ImVec2 Min{ 0.0f, 0.0f };
			ImVec2 Max{ 0.0f, 0.0f };
			/* 节点内直接显示的信息行（"宽 x 高  格式"；无快照图的节点为空） */
			std::string InfoText;
		};
		[[nodiscard]] const std::vector<NodeRect>& GetLastNodeRects() const { return m_LastNodeRects; }

		/* 上一帧各节点快照图的屏幕矩形（按节点顺序；Pass 节点 = 第一路输出、
		 * 资源节点 = 最近快照。PassName 为空表示资源节点；自检 / 自动化验证用） */
		struct OutputRect
		{
			std::string PassName;
			std::string Name;
			ImVec2 Min{ 0.0f, 0.0f };
			ImVec2 Max{ 0.0f, 0.0f };
		};
		[[nodiscard]] const std::vector<OutputRect>& GetLastOutputRects() const { return m_LastOutputRects; }

		/* 上一帧各 Pass 背景区域（行头与背景合并）的屏幕矩形（按行序；自检用） */
		struct LaneRect
		{
			std::string PassName;
			ImVec2 Min{ 0.0f, 0.0f };
			ImVec2 Max{ 0.0f, 0.0f };
		};
		[[nodiscard]] const std::vector<LaneRect>& GetLastLaneRects() const { return m_LastLaneRects; }

		/* 上一帧两类节点类型图标的屏幕矩形（绘制时记录；自检 / 自动化验证用）：
		 * Pass 节点 = 叠层菱形板 + 箭头（图标集 render-pass 样式）、
		 * 资源节点 = 节点卡 + 宝石（图标集 resource-node 样式） */
		struct IconRect
		{
			std::string Name;      /* 卡片名（Pass 名 / 资源名） */
			std::string LaneName;  /* 所属行的 Pass 名（资源卡区分同名副本） */
			bool IsPassIcon{ false };
			ImVec2 Min{ 0.0f, 0.0f };
			ImVec2 Max{ 0.0f, 0.0f };
		};
		[[nodiscard]] const std::vector<IconRect>& GetLastIconRects() const { return m_LastIconRects; }

			/* 当前选中的 Pass 名（聚焦的是 Pass 背景区域时 = 该名字；否则空。
		 * 聚焦卡的描边用强调色标记） */
		[[nodiscard]] const std::string& GetSelectedPassName() const { return m_SelectedPassName; }

		/* 聚焦的卡片（点击任意卡；archify 式交互：高亮它的整条传递链 —— 上游产出
		 * 到下游消费的全部卡与连线，其余元素变暗；再点同一张卡或空白处取消）。
		 * Name 为空 = 未聚焦 */
		struct FocusRef
		{
			std::string LaneName;  /* 所属行的 Pass 名 */
			std::string Name;      /* 卡片名（Pass 名 / 资源名） */
			bool IsPass{ false };  /* Pass 背景区域 or 资源卡 */
		};
		[[nodiscard]] const FocusRef& GetFocusedCard() const { return m_Focus; }

		/* 搜索过滤（archify 的节点查找）：非空时只亮名称匹配（不区分大小写的子串）
		 * 的卡、其余变暗；UI 上是工具行的输入框，这里给程序化入口供自检 */
		void SetSearchFilter(const std::string& filter);
		[[nodiscard]] const std::string& GetSearchFilter() const { return m_SearchFilter; }

		/* 阶段标识色查询（自检 / 自动化验证用）：名字命中阶段关键字就返回饱和标识色（节点描边用的
		 * 那个）；没命中返回 nullptr（用默认的卡片 / 面板色和 Border 描边）。 */
		[[nodiscard]] static const ImVec4* GetStageColor(const std::string& node_name);

	private:
		/* 依赖图画布：布局 + 绘制 + 交互（节点内嵌快照缩略图网格） */
		void DrawGraphCanvas(FrameGraph& frame_graph, float canvas_height);
		/* 把当前视图变换重置为「适配画布」 */
		void RequestFit() { m_Zoom = 0.0f; }

		bool m_CaptureEnabled{ true };      /* 抓取开关（面板可见时生效） */
		bool m_ShowCulled{ false };         /* 是否显示被剔除的节点（默认只看生效节点） */
		bool m_CaptureRequested{ false };
		std::string m_SelectedPassName{};
		FocusRef m_Focus{};                 /* 聚焦的卡片（见 GetFocusedCard） */
		std::string m_SearchFilter{};       /* 搜索过滤（见 SetSearchFilter） */
		char m_SearchBuffer[64]{};          /* 工具行输入框的驻留缓冲 */
		/* 所选相机的渲染图（按指针匹配；渲染图与相机同生命周期。相机被删除 / 选中不在
		 * 本帧列表时回落到首个条目） */
		FrameGraph* m_SelectedGraph{ nullptr };
		float m_Zoom{ 0.0f };               /* 0 = 尚未适配（本帧做一次 fit） */
		ImVec2 m_Pan{ 0.0f, 0.0f };         /* 视图平移（屏幕像素） */
		bool m_IsDragging{ false };         /* 左键拖拽中（平移画布） */
		float m_DraggedDistance{ 0.0f };
		std::vector<NodeRect> m_LastNodeRects{};
		std::vector<OutputRect> m_LastOutputRects{};
		std::vector<LaneRect> m_LastLaneRects{};
		std::vector<IconRect> m_LastIconRects{};
	};
}
