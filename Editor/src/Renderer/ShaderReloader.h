#pragma once
#include <chrono>
#include <deque>
#include <filesystem>
#include <future>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "Helios/Common/Common.h"

namespace Helios
{
	class DeviceShader;

	/* Shader 热重载：盯住已加载着色器的依赖文件，改了就地重载，由编辑器每帧驱动。
	 * 改公共库会波及一批 shader，整批在后台编完后同一帧一起换上，画面不会新旧混用。 */
	class ShaderReloader
	{
	public:
		/* 每帧调用。自动开关关掉时什么都不做 */
		void Tick(float delta_time);

		void SetAutoReloadEnabled(bool enabled) { m_AutoReloadEnabled = enabled; }
		[[nodiscard]] bool IsAutoReloadEnabled() const { return m_AutoReloadEnabled; }

		/* 立刻检查一轮并同步提交（不等轮询，也不看自动开关）。后台在编的那批先等它编完换上 */
		void ReloadChanged();

		/* 不看修改时间，按内容比对重载全部已缓存的着色器，返回真正换了内容的个数 */
		size_t ReloadAll();

		/* 排队与后台编译中的总个数 */
		[[nodiscard]] size_t GetPendingCount() const
		{
			return m_Pending.size() + (m_Building.has_value() ? m_Building->Paths.size() : 0u);
		}

	private:
		using FileTime = std::filesystem::file_time_type;

		/* 每个着色器各自盯的文件与看到的修改时间。时间戳不能跨着色器共用：
		 * 同一个库被 A、B 引用，B 记下新时间不代表 A 已经编过这版 */
		struct Watch
		{
			/* 最近一次尝试编译时的依赖（编译失败也算，新加的 include 要接着盯） */
			std::vector<std::string> Dependencies{};
			/* 文件不存在记 FileTime::min()，补建出来就算变化 */
			std::unordered_map<std::string, FileTime> Stamps{};
		};

		struct Prepared
		{
			std::string Path{};
			SharedPtr<DeviceShader> Shader{};
		};

		struct Build
		{
			std::vector<std::string> Paths{};
			std::future<std::vector<Prepared>> Result{};
			std::chrono::steady_clock::time_point StartTime{};
		};

		/* 扫一遍已加载的着色器，把依赖变了的排进队列 */
		void Poll();
		/* 把队列整批开编：Metal 且允许时走后台线程，GL 只能当场做（编译要用调用线程的上下文） */
		void StartBuild(bool allow_async);
		/* 后台那批编完了就整批换上；wait 为真时等它编完。返回是否已经没有在编的批次 */
		bool FinishBuild(bool wait);
		/* 逐个建好待换入的实例，可在后台线程跑 */
		static std::vector<Prepared> BuildBatch(const std::vector<std::string>& paths);
		/* 同一帧把整批换进活着的对象，返回真正换了内容的个数 */
		size_t CommitBatch(std::vector<Prepared>& batch, double elapsed_ms);

		/* 记下当前修改时间：开编之前记，编译期间又改了下一轮自然会再来 */
		void RecordStamps(Watch& watch) const;
		/* 换成这次编译读到的依赖表：老文件沿用开编前的时间，新文件记当前时间 */
		void AdoptDependencies(Watch& watch, const std::vector<std::string>& dependencies) const;
		[[nodiscard]] bool IsQueued(const std::string& path) const;

		std::unordered_map<std::string, Watch> m_Watches{};
		/* 等待编译的着色器路径（按发现顺序） */
		std::deque<std::string> m_Pending{};
		std::optional<Build> m_Building{};
		bool m_AutoReloadEnabled{ false };
		float m_PollTimer{ 0.0f };
	};
}
