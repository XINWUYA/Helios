#pragma once
#include <cstdint>
#include <imgui.h>

namespace Helios
{
	/* 编辑器统一图标系统：图标来自 EditorRes/icons 目录的 SVG 文件，文件名和枚举名相同，
	 * 由 ImDrawList 直接绘制、跟分辨率无关；UI 代码只引用 Icons::Id，不写资源路径。 */
	namespace Icons
	{
		enum class Id : uint16_t
		{
			None = 0,
			/* 文件与资源操作 */
			NewScene, OpenScene, Save, Import, Export, NewAsset,
			/* 编辑历史 */
			Undo, Redo,
			/* 路径导航（资源管理器顶栏：回到上次路径 / 重进路径） */
			Back, Forward,
			/* 变换 */
			Translate, Rotate, Scale,
			/* 运行 */
			Play, Stop,
			/* GPU 计时面板的两种观测方式：实时（活性脉冲）/ 录制（REC 圆点） */
			Live, Record,
			/* GPU 计时面板录制回放的帧导航（性能分析图标族，按设计稿
			 * game-engine-icons-final-v13-performance）：上一帧 / 下一帧
			 * （胶片格 + 方向箭头）、峰值（peak）/ 谷值（valley）折线图 */
			PreviousFrame, NextFrame, Peak, Valley,
			/* CPU 录制选项：同时写追踪文本（chrome://tracing 时间线 JSON） */
			TraceText,
			/* 通用 */
			Menu, Add, Remove, Delete, Return, ReturnRoot, Filter, Search, Visible, Hidden,
			/* 场景树节点：Scene 是根节点（当前场景），其余按实体持有的组件区分类型 */
			Scene, Entity, Model, Cube, Sphere, Plane, Shape3D, Camera, Light, LightDirectional, LightPoint, LightSpot, ReflectionProbe, Sprite,
			Audio, Particle, Terrain,
			/* 组件卡头部：与实体类型图标分开，按"这是个什么组件"造型 */
			Transform, Tag,
			/* 渲染层组件卡：多层堆叠 + 勾选（表示层掩码） */
			LayerMask,
			/* 面板/分组：统计类面板用 */
			Stats,
			/* 面板/分组：渲染图可视化页（Frame Graph）用 */
			FrameGraph,
			/* 面板/分组：日志窗口用 */
			Log,
			/* 面板工具动作：适配视图（把整张图收进视野）/ 预览垂直翻转（Frame Graph 的镜像开关） */
			FitView, FlipVertical,
			/* 弹窗窗口控制（CPU 图表弹窗的行尾按钮）：最大化铺满主视口 / 还原为浮动窗 */
			Maximize, Restore,
			/* 侧边栏展开 / 收起（时间轴右侧属性栏开关） */
			ExpandSidebar, CollapseSidebar,
			/* 抓取当前帧（实时视图：冻结数据供分析） */
			Capture,
			/* 内容（资源浏览器）：文件夹管"在哪"，文件图标管"是什么" */
			Directory, File, FileImage, FileSvg, FileScene, FileMtlGraph, FileShader, FileModel, FileMaterial, FileProbe,
			/* 视口辅助（Gizmos 显隐菜单）：工具栏按钮 + 弹层分项 */
			Gizmos, Grid, WorldAxis,
			/* 调试视图（工具栏按钮 + 弹层档位） */
			DebugView,
			/* 调试视图弹层的档位图形（分区各成族：Surface = 圆角方块、Lighting = 圆盘、
			 * Analysis = 叠层方块）—— 与 DebugView 卡片同族的小图形 */
			Albedo, Normal, Roughness, Metallic, SpecularColor, AmbientOcclusion, Emission, Ambient,
			Diffuse, Specular, Shadow, IndirectDiffuse, IndirectSpecular, Overdraw, Mipmap, Probe,
			/* 面板工具动作：材质参数浮层的开关（模型编辑窗口右上角） */
			Sliders,
			/* 动画：片段 / 轨道 / 暂停 / 曲线 / 预览 / 循环播放（播放与停止在运行组） */
			AnimationClip, Animation, Pause, Curve, Preview, Loop,
			/* 后处理：链 / 后处理对象 / 常见效果（Bloom 已接入，其余效果组件接入后直接用） */
			RenderPass, PostProcess, Bloom, ColorGrading, DepthOfField, MotionBlur, AntiAliasing, Vignette,
			COUNT
		};

		/* 矢量绘制：在以 center 为中心、边长 size 的正方形内作图 */
		using IconDrawFunc = void (*)(ImDrawList* draw_list, const ImVec2& center, float size, ImU32 color);

		struct IconDesc
		{
			const char*  Name;      /* 语义名：用于调试与阅读 */
			IconDrawFunc Draw;
		};

		/* 图标元数据表：与 Id 枚举一一对应，Draw 是加载失败时的回退画法 */
		const IconDesc& Get(Id id);

		/* 这张图标的 SVG 是否真的从 EditorRes/icons 加载到了（自检用：文件名写错会静默走回退） */
		[[nodiscard]] bool HasSvg(Id id);

		/* 绘制图标本体（不进入交互；按钮请用 IconButton）。
		 * apply_optical_scale = false 时不吃每图的光学尺寸（按给定尺寸原样画）——
		 * 缩略图这类"要和矢量预览同尺度"的场合用它（光学尺寸是给小尺寸 UI 调视觉重量的）。 */
		void Draw(ImDrawList* draw_list, Id id, const ImVec2& center, float size, ImU32 color,
			bool apply_optical_scale = true);

		/* 搜索框前置放大镜；Begin/End 包住对应的 InputText，自动预留图标空间。 */
		void BeginSearchInput();
		void EndSearchInput();

		/* 前置图标（输入框 / 下拉框左端的小图标）：用 Begin/End 包住控件、图标画在左端中部（控件
		 * 矩形由调用方给 —— 下拉弹层打开时 GetItemRect 取到的是弹层项）。注意：Begin/End 靠撑大
		 * FramePadding 让位，而弹层的内边距正是取自 FramePadding.x —— 带弹层的下拉框别用（自己画预览），
		 * 只给输入框这类没弹层的控件用。 */
		void BeginLeadingIcon();
		void DrawLeadingIcon(Id id, const ImVec2& frame_min, const ImVec2& frame_max);
		void EndLeadingIcon();

		/* 前置图标占掉的横向宽度：自己排版的控件（如下拉框）要把这一档算进自己的宽度里，
		 * 否则文字会被挤到箭头底下。 */
		float LeadingIconSpace();

		/* 统一图标按钮：集中尺寸、扁平底色、checked 高亮与 tooltip */
		bool IconButton(Id id, const ImVec2& size, bool checked = false, const char* tooltip = nullptr);
	}
}
