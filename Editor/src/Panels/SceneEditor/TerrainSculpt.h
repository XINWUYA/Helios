#pragma once
#include <cstdint>
#include "Helios/Scene/Entity.h"

namespace Helios::GrassPaint
{
	enum class Mode : uint8_t { None, Add, Erase };
	struct Settings
	{
		Mode Brush{ Mode::None };
		float Radius{ 8.0f };
		float Strength{ 0.7f };
	};
	inline Settings& GetSettings()
	{
		static Settings settings;
		return settings;
	}
}

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

	enum class ExportStatus : uint8_t { Idle, Pending, Succeeded, Failed };
	struct ExportRequest
	{
		Entity Target;
		uint64_t Serial{ 0 };
		ExportStatus Status{ ExportStatus::Idle };
		bool Consumed{ false };
		bool BindAsset{ true };
	};

	inline ExportRequest& GetExportRequest()
	{
		static ExportRequest s_Request;
		return s_Request;
	}

	/* 请求绑到实体，换选中不会把高度写进另一块地形。 */
	inline uint64_t RequestExport(Entity target, bool bind_asset = true)
	{
		ExportRequest& request = GetExportRequest();
		if (!target || request.Status == ExportStatus::Pending)
			return 0;
		request.Target = target;
		++request.Serial;
		request.Status = ExportStatus::Pending;
		request.Consumed = false;
		request.BindAsset = bind_asset;
		return request.Serial;
	}

	inline Entity ConsumeExportRequest()
	{
		ExportRequest& request = GetExportRequest();
		if (request.Status != ExportStatus::Pending || request.Consumed)
			return {};
		request.Consumed = true;
		return request.Target;
	}

	inline void CompleteExport(uint64_t serial, bool succeeded)
	{
		ExportRequest& request = GetExportRequest();
		if (request.Serial == serial && request.Status == ExportStatus::Pending)
			request.Status = succeeded ? ExportStatus::Succeeded : ExportStatus::Failed;
	}
}
