#pragma once
#include <cstdint>
#include <memory>
#include <imgui.h>

namespace Helios
{
	class DeviceTexture;

	/* 编辑器统一图标系统（单一数据源）：全部是矢量 —— 由 ImDrawList 直接绘制，没有外部资源、
	 * 跟分辨率无关；主描边跟着 UI 状态色走。UI 代码只引用 Icons::Id、不出现贴图路径字面量。 */
	namespace Icons
	{
		enum class Id : uint16_t
		{
			None = 0,
			Save, Play, Stop,
			Translate, Rotate, Scale,
			Menu, Add, Return, Filter,
			Directory, File, FileScene, FileMtlGraph, Visible,
			COUNT
		};

		struct IconDesc
		{
			const char* Name;      /* 语义名：用于 tooltip / 调试 */
			const char* PngPath;   /* 相对 Assets 的路径；改用字体图标后可置 nullptr */
			const char* Glyph;     /* 字体图标码点；暂无字体时为 nullptr */
		};

		/* 元数据表：新增图标只在这里加一行（Id 与表项一一对应） */
		const IconDesc& Get(Id id);

		/* 取图标贴图（内部集中缓存，同一图标全局只加载一次） */
		std::shared_ptr<DeviceTexture> GetTexture(Id id);

		/* 统一图标按钮：集中尺寸与 checked 高亮，替代各处手写 ImageButton */
		bool IconButton(Id id, const ImVec2& size, bool checked = false, const char* tooltip = nullptr);
	}
}
