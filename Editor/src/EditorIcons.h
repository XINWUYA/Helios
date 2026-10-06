#pragma once
#include <cstdint>
#include <imgui.h>

namespace Helios
{
	/* 编辑器统一图标系统（单一数据源）：全部是矢量 —— 由 ImDrawList 直接绘制，没有外部资源、
	 * 跟分辨率无关；主描边跟着 UI 状态色走。UI 代码只引用 Icons::Id、不出现贴图路径字面量。 */
	namespace Icons
	{
		enum class Id : uint16_t
		{
			None = 0,
			/* 文件与资源操作 */
			NewScene, OpenScene, Save, Import, NewAsset,
			/* 编辑历史 */
			Undo, Redo,
			/* 路径导航（资源管理器顶栏：回到上次路径 / 重进路径） */
			Back, Forward,
			/* 变换 */
			Translate, Rotate, Scale,
			/* 运行 */
			Play, Stop,
			/* 通用 */
			Menu, Add, Remove, Return, Filter, Search, Visible,
			/* 场景树节点：Scene 是根节点（当前场景），其余按实体持有的组件区分类型 */
			Scene, Entity, Model, Camera, Light, LightDirectional, LightPoint, LightSpot, ReflectionProbe, Sprite,
			Audio, Particle, Terrain,
			/* 组件卡头部：与实体类型图标分开，按"这是个什么组件"造型 */
			Transform, Tag,
			/* 面板/分组：统计类面板用 */
			Stats,
			/* 内容（资源浏览器）：文件夹管"在哪"，文件图标管"是什么" */
			Directory, File, FileImage, FileScene, FileMtlGraph, FileShader, FileModel,
			COUNT
		};

		/* 矢量绘制：在以 center 为中心、边长 size 的正方形内作图 */
		using IconDrawFunc = void (*)(ImDrawList* draw_list, const ImVec2& center, float size, ImU32 color);

		struct IconDesc
		{
			const char*  Name;      /* 语义名：用于调试与阅读 */
			IconDrawFunc Draw;
			float OpticalScale;     /* 最终 SVG 预览中的逐图光学尺寸 */
		};

		/* 元数据表：新增图标只在这里加一行（Id 与表项一一对应） */
		const IconDesc& Get(Id id);

		/* 绘制图标本体（不进入交互；按钮请用 IconButton） */
		void Draw(ImDrawList* draw_list, Id id, const ImVec2& center, float size, ImU32 color);

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
