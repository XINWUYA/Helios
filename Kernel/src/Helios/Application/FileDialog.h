#pragma once
#include "Helios/Common/Export.h"
namespace Helios
{
	/* 系统文件弹窗 */
	class HELIOS_API FileDialog
	{
	public:
		/* 通过文件窗口选取指定类型文件路径 */
		static std::string OpenFile(const char* filter = "*.*");
		/* 保存文件到指定路径；default_name = 弹窗里预填的文件名（可空） */
		static std::string SaveFile(const char* filter = "*.*", const char* default_name = nullptr);
	};

	/* 打开到指定文件目录 */
	HELIOS_API bool OpenFileExplorer(const char* path);
}
