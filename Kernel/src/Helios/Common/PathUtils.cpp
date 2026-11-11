#include "Pch.h"
#include "PathUtils.h"

#if HELIOS_PLATFORM_APPLE
#include <mach-o/dyld.h>
#elif HELIOS_PLATFORM_WINDOWS
#include <windows.h>
#endif

namespace Helios
{
	std::filesystem::path GetExecutablePath()
	{
#if HELIOS_PLATFORM_APPLE
		/* 第一次调用拿长度，第二次才填内容：路径可能比栈上的缓冲长 */
		uint32_t size = 0;
		_NSGetExecutablePath(nullptr, &size);
		if (size == 0)
			return {};

		std::string buffer(size, '\0');
		if (_NSGetExecutablePath(buffer.data(), &size) != 0)
			return {};

		/* 返回的是调用时的样子，里面可能带符号链接与 .. ，先规范化再当基准目录 */
		std::error_code error;
		const std::filesystem::path resolved = std::filesystem::weakly_canonical(buffer.c_str(), error);
		return error ? std::filesystem::path(buffer.c_str()) : resolved;
#elif HELIOS_PLATFORM_WINDOWS
		std::wstring buffer(MAX_PATH, L'\0');
		while (true)
		{
			const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
			if (length == 0)
				return {};

			/* 缓冲不够时返回值等于缓冲长度，加长重来 */
			if (length < buffer.size())
			{
				buffer.resize(length);
				return std::filesystem::path(buffer);
			}

			buffer.resize(buffer.size() * 2);
		}
#elif HELIOS_PLATFORM_LINUX
		std::error_code error;
		const std::filesystem::path path = std::filesystem::read_symlink("/proc/self/exe", error);
		return error ? std::filesystem::path{} : path;
#else
		return {};
#endif
	}
}
