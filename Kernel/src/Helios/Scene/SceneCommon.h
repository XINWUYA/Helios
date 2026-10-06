#pragma once

#include "HeliosConfig.h"
#include "Helios/Common/PathUtils.h"
#include <filesystem>

namespace Helios
{
#define COMPONENT_CLASS(type)													\
	void SetDebugName(const std::string& name) { m_DebugName = #type + name; }	\
	const std::string& GetDebugName() const { return m_DebugName; }

const std::filesystem::path g_AssetsPath = PathFromUtf8(ASSETS_PATH);
#define ABSOLUTE_PATH(path) PathToUtf8(g_AssetsPath / PathFromUtf8(path))
#define RELATIVE_PATH(path) PathToUtf8(std::filesystem::relative(PathFromUtf8(path), g_AssetsPath))

}
