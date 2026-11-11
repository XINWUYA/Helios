#include "Pch.h"
#include "App/EditorApp.h"
#include "Packaging/Packager.h"

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

	Helios::Logger::Init();
	EDITOR_LOG("Helios Kernel 1.0.0");

	/* 项目根先定下来：打包要把它写进包里，别的入口也用它 */
	const bool project_specified = Helios::ParseProjectArgument(argc, argv);

	/* 打包入口的命令行形态：Build 菜单那几项走的是同一套实现，
	 * 这条给 CI 与没有界面的环境用，跑完直接退出。 */
	for (int i = 1; i < argc; ++i)
	{
		const std::string arg = argv[i];

		Helios::Build::PackageTarget target;
		bool packaging = true;
		if (arg == "--package-ios-sim")
			target = Helios::Build::PackageTarget::IOSSimulatorApp;
		else if (arg == "--package-ios-device")
			target = Helios::Build::PackageTarget::IOSDeviceApp;
		else if (arg == "--generate-ios-xcode")
			target = Helios::Build::PackageTarget::IOSXcodeProject;
		else
			packaging = false;

		if (!packaging)
			continue;

		const Helios::Build::PackageResult result = Helios::Build::Run(target, Helios::g_ProjectPath);
		if (result.Success)
			EDITOR_LOG_INFO("[Package] 完成：{}", result.OutputPath);
		else
			EDITOR_LOG_ERROR("[Package] 失败：\n{}", result.LogTail);

		Helios::Logger::Shutdown();
		return result.Success ? 0 : 1;
	}

	/* 耗时追踪文本默认不写：运行时在性能面板（Stat Info → CPU Timings）的 "Trace Text"
	 * 开关里控制写盘；要抓启动阶段就临时用 PROFILER_BEGIN_SESSION / PROFILER_END_SESSION
	 * 包住下面两段。 */
	auto app = Helios::CreateEditorApplication(!project_specified);
	app->Run();

	Helios::Logger::Shutdown();
	EndMemoryLeakDetector();

	return 0;
}
