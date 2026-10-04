#pragma once

/* === 深度约定（必须在任何 glm 头文件之前定义） ===
 * 与 Kernel/src/Pch.h 保持一致，保证所有 target 使用同一套 glm 配置（避免 ODR）。 */
#ifndef GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#endif

/* Ensure Windows Visual Studio correctly handles UTF-8 encoded source files */
#ifdef _MSC_VER
#pragma execution_character_set("utf-8")
#endif

/* === Third-party UI (stable, very large) === */
#include <imgui.h>
#include <imgui_internal.h>

/* === Engine umbrella header (pre-compiled by Kernel) === */
/*
 * Note: We only include the lightweight HeliosCore.h rather than the full Helios.h.
 * Helios.h pulls in heavy headers (Renderer, Scene, FrameGraph, etc.)
 * which may not be needed by every source file and can cause excessive
 * symbol bloat in the precompiled header.
 */
#include <Helios.h>
