#include "Pch.h"

#ifdef _WIN32
#define _CRTDBG_MAP_ALLOC
#include <crtdbg.h>

/* Windows内存泄漏检测工具 */
void StartMemoryLeakDetector()
{
#ifdef HELIOS_DEBUG
	_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif
}

void EndMemoryLeakDetector()
{
#ifdef HELIOS_DEBUG
	_CrtDumpMemoryLeaks();
#endif
}
#else
/* Mac/Linux下暂无内存泄漏检测 */
void StartMemoryLeakDetector() {}
void EndMemoryLeakDetector() {}
#endif

int main(int argc, char** argv)
{
	StartMemoryLeakDetector();

	/* todo: 根据argc/argv获取工程路径 */

	Helios::Logger::Init();
	EDITOR_LOG("Helios Kernel 1.0.0");

	/* 耗时追踪文本默认不写：运行时在性能面板（Stat Info → CPU Timings）的 "Trace Text"
	 * 开关里控制写盘；要抓启动阶段就临时用 PROFILER_BEGIN_SESSION / PROFILER_END_SESSION
	 * 包住下面两段。 */
	auto app = Helios::CreateApplication();
	app->Run();

	Helios::Logger::Shutdown();
	EndMemoryLeakDetector();

	return 0;
}
