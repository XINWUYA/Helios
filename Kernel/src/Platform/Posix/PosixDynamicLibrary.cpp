#include "Pch.h"
#include "Platform/DynamicLibrary.h"

#include <dlfcn.h>

namespace Helios::Platform
{
	void* LoadDynamicLibrary(const std::string& path, std::string& error)
	{
		void* module = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
		if (module == nullptr)
		{
			const char* reason = dlerror();
			error = reason != nullptr ? reason : "unknown error";
		}
		return module;
	}
}
