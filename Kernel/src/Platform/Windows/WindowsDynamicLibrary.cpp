#include "Pch.h"
#include "Platform/DynamicLibrary.h"

#include <windows.h>

namespace Helios::Platform
{
	void* LoadDynamicLibrary(const std::string& path, std::string& error)
	{
		HMODULE module = ::LoadLibraryA(path.c_str());
		if (module == nullptr)
			error = "LoadLibrary failed with code " + std::to_string(::GetLastError());
		return module;
	}
}
