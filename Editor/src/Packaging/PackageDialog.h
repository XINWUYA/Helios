#pragma once
/* 打包弹窗：跑的时候看进度，跑完看结果，环境不满足时看说明。四种状态共用同一张卡片，
 * 只有内容区不同；版式与启动页的 New Project 弹窗一致（无标题栏、12 圆角、定宽）。 */

#include <string>

namespace Helios::EditorUI
{
	enum class PackageDialogMode
	{
		Progress,     /* 正在打包：进度条 + 当前阶段 + 最近一行输出 */
		Success,      /* 成功：产物路径 */
		Failed,       /* 失败：最后几行输出 */
		Environment,  /* 环境不满足：说明原因，任务没开跑 */
	};

	struct PackageDialogModel
	{
		PackageDialogMode Mode{ PackageDialogMode::Progress };
		std::string Title;        /* 进度态的目标名，例如「打包 iOS 真机应用」 */
		std::string Stage;        /* 进度态：当前阶段 */
		std::string LastLine;     /* 进度态：最近一行输出 */
		float Fraction{ -1.0f };  /* 进度态：0~1，负数表示这一步拿不到百分比 */
		std::string Path;         /* 成功态：产物路径 */
		std::string Log;          /* 失败态：最后几行输出 */
		std::string Issue;        /* 环境不满足：给用户看的说明 */
	};

	/* request_open 为真时这一帧打开弹窗，要在宿主窗口的作用域里调。
	 * 返回 true 表示用户点了 Show in Finder，由调用方去定位产物。 */
	bool ShowPackageDialog(bool request_open, const PackageDialogModel& model);
}
