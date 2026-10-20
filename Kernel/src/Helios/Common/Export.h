#pragma once

/* 跨模块导出宏。
 *
 * Kernel 是动态库，Editor 与脚本模块都链接它。三个平台走同一套标注：
 *   - macOS / Linux：默认全导出，标 HELIOS_API 只是显式声明意图（配合可见性收紧时也是必需的）；
 *   - Windows：DLL 默认什么都不导出，必须 __declspec(dllexport)；
 *     链接方要 __declspec(dllimport)，否则跨模块调用会走 thunk 或者干脆链不上。
 *
 * 标注对象：脚本或 Editor 会跨模块调用的类型与非内联函数。
 *   - 类上标一次，它的非内联成员（含 vtable / typeinfo）都跟着导出；
 *   - 模板与 inline 函数不标 —— 调用方自己实例化，标了反而制造重复符号。
 * 漏标在 Windows 上是**链接错误**，在 macOS 开可见性收紧时也是，不会留到运行期。 */
#if defined(PLATFORM_WINDOWS)
	#if defined(HELIOS_KERNEL_BUILD)
		#define HELIOS_API __declspec(dllexport)
	#else
		#define HELIOS_API __declspec(dllimport)
	#endif
#else
	#define HELIOS_API __attribute__((visibility("default")))
#endif
