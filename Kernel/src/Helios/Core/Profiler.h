#pragma once
#include <atomic>
#include <fstream>
#include <mutex>
#include "Helios/Core/CPUTimerProfiler.h"
#if TRACY_ENABLE
#include "tracy/Tracy.hpp"
#include "client/TracyScoped.hpp"
#endif

namespace Helios
{
	struct ProfileResult
	{
		std::string Name; // 标记名
		std::chrono::duration<double, std::micro> StartTime; // 开始时间
		std::chrono::microseconds ElapsedTime; // 耗时
		std::thread::id ThreadId; // 所属线程
	};

	// 耗时分析
	class HELIOS_API TimeCostProfiler
	{
	public:
		TimeCostProfiler(const TimeCostProfiler&) = delete;
		TimeCostProfiler(TimeCostProfiler&&) = delete;

		static TimeCostProfiler& Instance();

		void BeginSession(const std::string& name, const std::string& outfile = "ProfileResult-TimeCost.json");
		void EndSession();

		/* 会话是否在写盘中（运行期开关的状态；UI 开关 / 调用方据此判断） */
		[[nodiscard]] bool IsSessionActive() const { return m_bSessionActive.load(std::memory_order_relaxed); }

		void WriteProfileResult(const ProfileResult& result);

	private:
		TimeCostProfiler() = default;
		~TimeCostProfiler();

		void WriteHeader();
		void WriteFooter();

		void InternalEndSession();

		struct SessionInfo
		{
			std::string Name;
		};

		std::mutex m_Mutex{};
		SharedPtr<SessionInfo> m_pSessionInfo{ nullptr };
		std::ofstream m_OutputStream{};
		/* 无锁快径标记：会话关着时 WriteProfileResult 直接返回（不构造 json、不抢锁） */
		std::atomic<bool> m_bSessionActive{ false };
	};

	// 分析计时器
	class HELIOS_API ProfilerTimer
	{
	public:
		ProfilerTimer(const char* name);
		~ProfilerTimer();

		void Stop();

	private:
		const char* m_pName;
		std::chrono::time_point<std::chrono::steady_clock> m_StartTimePoint{};
		bool m_Stopped{ false };
	};

	namespace ProfilerUtils
	{
		template <size_t N>
		struct ChangeResult
		{
			char Data[N];
		};

		/* expr 在 index 处是否是 token（编译期前缀判断） */
		template <size_t N>
		constexpr bool HasTokenAt(const char(&expr)[N], size_t index, const char* token)
		{
			for (size_t k = 0; token[k] != '\0'; ++k)
			{
				if (index + k >= N || expr[index + k] != token[k])
					return false;
			}
			return true;
		}

		/* 把编译器函数签名裁成「类::函数名」（PROFILE_FUNCTION 的标签源）：剥掉返回类型与
		 * virtual/static 等修饰符、去掉参数表；顺带剥 MSVC 的 "__cdecl " 前缀与 clang 的
		 * "(anonymous namespace)" 标记、双引号换单引号（追踪 JSON 的 name 安全）。
		 * 无参数表的自定义标签（"Update Layers" 等）原样保留。 */
		template <size_t N>
		constexpr auto CleanupFunctionLabel(const char(&expr)[N])
		{
			ChangeResult<N> result = {};

			/* 一、逐字符清理：剥两个固定标记、引号转单引号 */
			size_t out = 0;
			for (size_t i = 0; i < N && expr[i] != '\0';)
			{
				if (HasTokenAt(expr, i, "__cdecl "))
				{
					i += 8;
					continue;
				}
				if (HasTokenAt(expr, i, "(anonymous namespace)"))
				{
					i += 21;
					continue;
				}
				result.Data[out++] = expr[i] == '"' ? '\'' : expr[i];
				++i;
			}

			/* 二、取第一个 '(' 之前、类名起点开始的一段（无 '(' 视为自定义标签，原样返回） */
			size_t paren = out;
			for (size_t k = 0; k < out; ++k)
			{
				if (result.Data[k] == '(')
				{
					paren = k;
					break;
				}
			}
			if (paren == out)
				return result;

			/* 从 '(' 往左找名字起点：跳过模板尖括号内的空格（返回类型里的 "int, int>" 等） */
			size_t start = 0;
			size_t depth = 0;
			for (size_t k = paren; k-- > 0;)
			{
				const char ch = result.Data[k];
				if (ch == '>')
					++depth;
				else if (ch == '<' && depth > 0)
					--depth;
				else if (ch == ' ' && depth == 0)
				{
					start = k + 1;
					break;
				}
			}
			/* 匿名命名空间清理后可能带前导 "::"：跳过 */
			if (start + 1 < paren && result.Data[start] == ':' && result.Data[start + 1] == ':')
				start += 2;

			size_t write = 0;
			for (size_t k = start; k < paren; ++k)
				result.Data[write++] = result.Data[k];
			result.Data[write] = '\0';
			return result;
		}
	}

}

// See the profiler result in: edge://tracing/
// More suggested: https://ui.perfetto.dev/
// Just Drag the json file in.
/* HELIOS_PROFILE：追踪文本（TimeCostProfiler 的 JSON）的编译期开关 —— 0（默认）= 不编译计时器、
 * 会话调用为空操作；1 = 编译计时器，写不写盘由运行期全局开关控制（面板 Trace Text，会话开着才写）。
 * 面板 CPU Timings 的采集不受本开关影响 —— 两条路径都接、标签一致（见 PROFILE_LABEL_LINE）；
 * 编辑器在 Editor/CMakeLists.txt 定义 =1，Sample 按需自定义。默认不写。 */
#ifndef HELIOS_PROFILE
	#define HELIOS_PROFILE 0
#endif

// Resolve which function signature macro will be used. Note that this only
// is resolved when the (pre)compiler starts, so the syntax highlighting
// could mark the wrong one in your editor!
#if defined(__GNUC__) || (defined(__MWERKS__) && (__MWERKS__ >= 0x3000)) || (defined(__ICC) && (__ICC >= 600)) || defined(__ghs__)
	#define FUNC_SIG __PRETTY_FUNCTION__
#elif defined(__DMC__) && (__DMC__ >= 0x810)
	#define FUNC_SIG __PRETTY_FUNCTION__
#elif (defined(__FUNCSIG__) || (_MSC_VER))
	#define FUNC_SIG __FUNCSIG__
#elif (defined(__INTEL_COMPILER) && (__INTEL_COMPILER >= 600)) || (defined(__IBMCPP__) && (__IBMCPP__ >= 500))
	#define FUNC_SIG __FUNCTION__
#elif defined(__BORLANDC__) && (__BORLANDC__ >= 0x550)
	#define FUNC_SIG __FUNC__
#elif defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 199901)
	#define FUNC_SIG __func__
#elif defined(__cplusplus) && (__cplusplus >= 201103)
	#define FUNC_SIG __func__
#else
	#define FUNC_SIG "FUNC_SIG unknown!"
#endif

#if TRACY_ENABLE
	#define PROFILE_SCOPE(name) ZoneScopedN(name)
	#define PROFILE_SCOPE_ARGS_LINE(fmt, line, ...)  char label_##line[100]; snprintf(label_##line, 100, fmt, __VA_ARGS__); const char* label_##line##_const = label_##line; PROFILE_SCOPE(label_##line##_const);
	#define PROFILE_SCOPE_ARGS(fmt, ...) PROFILE_SCOPE_ARGS_LINE(fmt, __LINE__, __VA_ARGS__)
	#define PROFILE_FUNCTION() ZoneScoped

#else
	/* 标签准备（HELIOS_PROFILE 的两条路径共用）：编译期把编译器签名裁成「类::函数名」，
	 * 并且 static —— 采集器只存标签指针、帧末结算时才读内容；函数局部（非 static）的
	 * constexpr 在 -O0 下物化在栈帧上，函数一返回指针就悬垂（标签会变成乱码 / "?"）。 */
	#define PROFILE_LABEL_LINE(name, line) static constexpr auto fixed_name_##line = ::Helios::ProfilerUtils::CleanupFunctionLabel(name)

	#if HELIOS_PROFILE
		// 开启一个耗时分析（写追踪文本；实际写不写由运行期全局开关控制）
		#define PROFILER_BEGIN_SESSION(name, filepath) ::Helios::TimeCostProfiler::Instance().BeginSession(name, filepath)
		// 关闭一个耗时分析
		#define PROFILER_END_SESSION() ::Helios::TimeCostProfiler::Instance().EndSession()
		// 分析当前指令段耗时：面板采集 + 追踪文本计时器（会话关着时 ProfilerTimer 空转）；
		// file / line = 插桩点位置（悬浮读数显示"文件名:行号"）
		#define PROFILE_SCOPE_LINE(name, file, line) PROFILE_LABEL_LINE(name, line); \
												::Helios::ProfilerTimer timer_##line(fixed_name_##line.Data); \
												const ::Helios::ScopedCPUTimerScope helios_cpu_profiler_scope_##line(fixed_name_##line.Data, file, line)
	#else
		/* 未定义总开关（Kernel / Sample 的默认路径）：会话调用为空操作；作用域仍接面板
		 * 采集，标签与开关打开时完全一致 —— 面板读到的内容不随构建开关变形。
		 * 作用域展开为对象声明，须处于语句上下文 —— 仓内插桩点均满足（带分号）。 */
		#define PROFILER_BEGIN_SESSION(name, filepath)
		#define PROFILER_END_SESSION()
		#define PROFILE_SCOPE_LINE(name, file, line) PROFILE_LABEL_LINE(name, line); \
												const ::Helios::ScopedCPUTimerScope helios_cpu_profiler_scope_##line(fixed_name_##line.Data, file, line)
	#endif

	/* 行号 / 文件名先经这层"普通转发"展开再进粘连与调用 —— `##` 会抑制参数展开，
	 * __LINE__ 直接进 PROFILE_SCOPE_LINE 会以字面 token 参与粘连（名字糊成
	 * <前缀>___LINE__）；__FILE__ 同层展开成字符串字面量（插桩点文件，悬浮读数用）。 */
	#define PROFILE_SCOPE_LINE_EXPAND(name, line) PROFILE_SCOPE_LINE(name, __FILE__, line)
	#define PROFILE_SCOPE(name) PROFILE_SCOPE_LINE_EXPAND(name, __LINE__)
	// 分析当前函数耗时
	#define PROFILE_FUNCTION() PROFILE_SCOPE(FUNC_SIG)
#endif