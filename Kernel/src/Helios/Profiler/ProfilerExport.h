#pragma once
#include <string>

#include "Helios/Profiler/Profiler.h"

namespace Helios
{
	/* 录制导出的纯数据件：与 UI 无关，CPU / GPU 共用格式化工具，headless 可断言 */
	namespace ProfilerExport
	{
		/* GPU 录制导出为 CSV 宽表：一行一帧，帧号 / 根节点占用 + 每个作用域节点两列。
		 * 各阶段之间会交叠，几列不能相加。 */
		std::string BuildRecordingCsv(const GPUTimeScopeRecorder& recorder);

		/* 落盘路径收尾：系统保存框不保证补后缀，名称未以 .csv 结尾时补上 */
		std::string EnsureRecordingExtension(const std::string& path);
	}
}
