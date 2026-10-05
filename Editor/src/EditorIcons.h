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
			/* 文件 */
			NewScene, OpenScene, Save,
			/* 编辑历史 */
			Undo, Redo,
			/* 变换 */
			Translate, Rotate, Scale,
			/* 运行 */
			Play, Stop,
			/* 通用 */
			Menu, Add, Remove, Return, Filter, Visible,
			/* 场景树节点：Scene 是根节点（当前场景），其余按实体持有的组件区分类型 */
			Scene, Camera, LightDirectional, LightPoint, LightSpot, ReflectionProbe, Model, Sprite, Entity,
			/* 组件卡头部：与实体类型图标分开，按"这是个什么组件"造型 */
			Transform, Tag,
			/* 面板/分组：统计类面板用 */
			Stats,
			/* 内容（资源浏览器）：文件夹管"在哪"，文件图标管"是什么" */
			Directory, File, FileImage, FileScene, FileMtlGraph,
			COUNT
		};

		/* 矢量绘制：在以 center 为中心、边长 size 的正方形内作图 */
		using IconDrawFunc = void (*)(ImDrawList* draw_list, const ImVec2& center, float size, ImU32 color);

		struct IconDesc
		{
			const char*  Name;      /* 语义名：用于调试与阅读 */
			IconDrawFunc Draw;
		};

		/* 元数据表：新增图标只在这里加一行（Id 与表项一一对应） */
		const IconDesc& Get(Id id);

		/* 绘制图标本体（不进入交互；按钮请用 IconButton） */
		void Draw(ImDrawList* draw_list, Id id, const ImVec2& center, float size, ImU32 color);

		/* 统一图标按钮：集中尺寸、扁平底色、checked 高亮与 tooltip */
		bool IconButton(Id id, const ImVec2& size, bool checked = false, const char* tooltip = nullptr);
	}
}
