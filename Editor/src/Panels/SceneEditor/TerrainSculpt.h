#pragma once
#include <cstdint>

namespace Helios::TerrainSculpt
{
	/* 笔刷模式：None 时不接管视口左键 */
	enum class Mode : uint8_t
	{
		None,
		Raise,
		Lower,
		Smooth,
	};

	/* 笔刷参数：半径按世界米算（进地形时换算成局部），强度是每秒改变多少米，
	 * 抹平模式下是每秒混向邻域均值的比例 */
	struct Settings
	{
		Mode Brush{ Mode::None };
		float Radius{ 12.0f };
		float Strength{ 8.0f };
	};

	/* 工具状态放在头文件里：面板卡与视口两处读同一份，检查脚本按文件清单单独编译时
	 * 也不用为它额外加一个 .cpp。函数内静态在多个编译单元里是同一个对象 */
	inline Settings& GetSettings()
	{
		static Settings s_Settings;
		return s_Settings;
	}

	/* 卡片上的按钮只置一个请求：导出要知道当前选中的是哪个实体，交给编辑器层做 */
	inline bool& ExportRequested()
	{
		static bool s_Requested = false;
		return s_Requested;
	}

	inline void RequestExport()
	{
		ExportRequested() = true;
	}

	inline bool ConsumeExportRequest()
	{
		const bool requested = ExportRequested();
		ExportRequested() = false;
		return requested;
	}
}
