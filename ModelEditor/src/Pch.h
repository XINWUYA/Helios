#pragma once

/* === 深度约定（必须在任何 glm 头文件之前定义） ===
 * 与 Kernel/src/Pch.h 保持一致，保证所有 target 使用同一套 glm 配置（避免 ODR）。 */
#ifndef GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#endif

/* 确保Windows Visual Studio正确处理UTF-8编码的源文件 */
#ifdef _MSC_VER
#pragma execution_character_set("utf-8")
#endif

#include <imgui.h>
#include <imgui_internal.h>

#include <Helios.h>
