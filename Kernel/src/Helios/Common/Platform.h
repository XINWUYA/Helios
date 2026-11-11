#pragma once

/* 平台判定集中在这里：构建脚本按目标系统注入一个 PLATFORM_* 宏，
 * 其它代码只用下面的 HELIOS_PLATFORM_*，不再自己认宏。 */

#if defined(PLATFORM_IOS)
#define HELIOS_PLATFORM_IOS 1
#else
#define HELIOS_PLATFORM_IOS 0
#endif

#if defined(PLATFORM_MACOS)
#define HELIOS_PLATFORM_MACOS 1
#else
#define HELIOS_PLATFORM_MACOS 0
#endif

#if defined(PLATFORM_WINDOWS)
#define HELIOS_PLATFORM_WINDOWS 1
#else
#define HELIOS_PLATFORM_WINDOWS 0
#endif

#if defined(PLATFORM_LINUX)
#define HELIOS_PLATFORM_LINUX 1
#else
#define HELIOS_PLATFORM_LINUX 0
#endif

#if defined(PLATFORM_ANDROID)
#define HELIOS_PLATFORM_ANDROID 1
#else
#define HELIOS_PLATFORM_ANDROID 0
#endif

/* 苹果系共用系统框架，Metal 相关分支按它放宽 */
#define HELIOS_PLATFORM_APPLE (HELIOS_PLATFORM_MACOS || HELIOS_PLATFORM_IOS)
/* 桌面才有窗口系统、文件对话框与运行期着色器编译 */
#define HELIOS_PLATFORM_DESKTOP (HELIOS_PLATFORM_WINDOWS || HELIOS_PLATFORM_MACOS || HELIOS_PLATFORM_LINUX)
/* 移动端：触摸输入、无运行期脚本编译、GPU 资源要能重建 */
#define HELIOS_PLATFORM_MOBILE (HELIOS_PLATFORM_IOS || HELIOS_PLATFORM_ANDROID)

/* 两个宏同时定义说明构建脚本的平台分支写错了，早点拦住比查编译产物省事 */
#if HELIOS_PLATFORM_IOS && HELIOS_PLATFORM_MACOS
#error "PLATFORM_IOS and PLATFORM_MACOS are mutually exclusive"
#endif

#if !HELIOS_PLATFORM_APPLE && !HELIOS_PLATFORM_WINDOWS && !HELIOS_PLATFORM_LINUX && !HELIOS_PLATFORM_ANDROID
#error "No PLATFORM_* macro defined"
#endif
