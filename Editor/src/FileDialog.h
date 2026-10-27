#pragma once
#include <string>
namespace Helios
{
	/* 系统文件弹窗（编辑器专用：引擎运行时不依赖平台 UI） */
	class FileDialog
	{
	public:
		/* 通过文件窗口选取指定类型文件路径 */
		static std::string OpenFile(const char* filter = "*.*");
		/* 保存文件到指定路径；default_name = 弹窗里预填的文件名（可空） */
		static std::string SaveFile(const char* filter = "*.*", const char* default_name = nullptr);
		/* 选目录（打开工程 / 新工程放哪）：title 是弹窗里的一句提示（可空） */
		static std::string OpenFolder(const char* title = nullptr);
	};

	/* 打开到指定文件目录 */
	bool OpenFileExplorer(const char* path);
}
