#pragma once

#include <string>

#include "Helios/Common/Export.h"

namespace Helios::Platform
{
	/* 加载动态库，失败返回空并把原因写进 error。
	 * 不提供卸载：脚本类和字段回调一直指着这份映像，卸了就是悬空 */
	HELIOS_API void* LoadDynamicLibrary(const std::string& path, std::string& error);
}
