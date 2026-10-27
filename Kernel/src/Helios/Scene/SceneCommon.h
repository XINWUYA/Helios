#pragma once

#include "HeliosConfig.h"
#include "Helios/Common/AssetPath.h"
#include "Helios/Common/PathUtils.h"
#include <filesystem>

namespace Helios
{
#define COMPONENT_CLASS(type)													\
	void SetDebugName(const std::string& name) { m_DebugName = #type + name; }	\
	const std::string& GetDebugName() const { return m_DebugName; }

/* 资产路径归一：相对路径按"项目根优先、引擎根回落"解释，绝对路径原样保留。
 * 注意：相对路径不能直接喂给 std::filesystem::relative（它会按进程工作目录再解析一遍）——
 * 否则"加载后再保存"会把引用写成 "../<工作目录>/…" 这种坏值、再打开时就丢了。 */
#define ABSOLUTE_PATH(path) PathToUtf8(ResolveAssetPath(PathFromUtf8(path)))
#define RELATIVE_PATH(path) PathToUtf8(ToAssetRelative(PathFromUtf8(path)))
}
