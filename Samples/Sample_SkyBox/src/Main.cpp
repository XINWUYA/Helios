#include "Pch.h"
#include "TestApp.h"
#include "Helios/Common/AssetPath.h"

#ifdef _WIN32
#define _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <crtdbg.h>

/* 启用内存泄漏检测工具 */
void StartMemoryLeakDetector()
{
	// Enable run-time memory check for debug builds.
#ifdef HELIOS_DEBUG
	//_CRTDBG_LEAK_CHECK_DF: Perform automatic leak checking at program exit through a call to _CrtDumpMemoryLeaks and generate an error 
	//report if the application failed to free all the memory it allocated. OFF: Do not automatically perform leak checking at program exit.
	_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);

	//the following statement is used to trigger a breakpoint when memory leak happens
	//comment it out if there is no memory leak report;
	//_CrtSetBreakAlloc(2499);
#endif
}

void EndMemoryLeakDetector()
{
#ifdef HELIOS_DEBUG
	_CrtDumpMemoryLeaks();
#endif
}
#else
void StartMemoryLeakDetector() {}
void EndMemoryLeakDetector() {}
#endif

int main(int argc, char** argv)
{
	StartMemoryLeakDetector();

	Helios::Logger::Init();
	EDITOR_LOG("Helios Kernel 1.0.0");

	/* 项目根：编译期默认值，--project 可覆盖 */
	Helios::ParseProjectArgument(argc, argv);

	{
		// See the profiler result in: edge://tracing/
		// More suggested: https://ui.perfetto.dev/
		// Just Drag the json file in.
		PROFILER_BEGIN_SESSION("Startup", "PerfettoWriter-Startup.json");
		auto app = Helios::CreateApplication();
		PROFILER_END_SESSION();

		PROFILER_BEGIN_SESSION("Runtime", "PerfettoWriter-Runtime.json");
		app->Run();
		PROFILER_END_SESSION();
	}

	Helios::Logger::Shutdown();
	EndMemoryLeakDetector();

	return 0;
}
