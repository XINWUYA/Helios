#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>
#include "Helios/Common/Common.h"

/* 引擎唯一的任务入口，内核是 Libraries/HeliosJobs（取自 Filament JobSystem）。
 * 帧内分片走 ParallelFor，编辑器长任务走 RunBackground，结果经 PostToMain 回主线程。
 * 主线程只会顺手偷帧内小任务，永远不执行后台任务。 */
namespace Helios::Jobs
{
	enum class Priority : uint8_t
	{
		High = 0,
		Normal,
		Low,
	};

	/* 在主线程调用：建线程池并收编主线程。worker_count 为 0 时取硬件线程数减 1 */
	HELIOS_API bool Init(uint32_t worker_count = 0);
	/* 作废全部后台任务、等在跑的收尾、回收线程，重复调用无副作用 */
	HELIOS_API void Shutdown();

	[[nodiscard]] HELIOS_API bool IsInitialized();
	[[nodiscard]] HELIOS_API uint32_t WorkerCount();
	[[nodiscard]] HELIOS_API uint32_t BackgroundSlots();
	[[nodiscard]] HELIOS_API bool IsMainThread();
	[[nodiscard]] HELIOS_API bool IsWorkerThread();

	/* 每个 ParallelFor 调用点一份：记每个元素的耗时，估出来太小就直接串行 */
	struct ParallelForSite
	{
		std::atomic<float> NsPerItem{ 0.0f };
		std::atomic<uint32_t> Calls{ 0 };
	};

	/* 分片并行的非模板入口，返回时全部分片完成 */
	HELIOS_API void ParallelForImpl(uint32_t count, uint32_t grain, const void* ctx,
		void (*invoke)(const void*, uint32_t begin, uint32_t end), ParallelForSite* site);

	/* 用法：Jobs::ParallelFor(n, 256, [&](uint32_t begin, uint32_t end) { ... });
	 * 每片只写自己的区间；分片会被多个线程同时调用，单片要短，主线程也会来帮忙。
	 * 首次调用照常并行并测耗时，之后估计总量不到 50us 的直接在调用线程串行。 */
	template <typename Fn>
	void ParallelFor(uint32_t count, uint32_t grain, Fn&& fn)
	{
		using Body = std::remove_reference_t<Fn>;
		static ParallelForSite s_Site;
		ParallelForImpl(count, grain, &fn, [](const void* ctx, uint32_t begin, uint32_t end)
		{
			(*static_cast<const Body*>(ctx))(begin, end);
		}, &s_Site);
	}

	/* 主线程空闲时偷帧内小任务，最多占用 budget_us 微秒 */
	HELIOS_API uint32_t HelpWhileIdle(uint32_t budget_us);

	/* 后台任务的终态。Pending 表示还没结束：排队中、在跑、或正要开始都算 */
	enum class TaskState : uint8_t
	{
		Pending,
		Completed, /* 正常跑完 */
		Skipped,   /* 没跑成：取消、过期、池满被丢、退出清理 */
	};

	struct TaskControl;

	/* 后台任务句柄：查终态、可阻塞等待；析构时放手不取消。
	 * 终态记在共享控制块里，句柄放手、线程池重建之后照样读得到。 */
	class HELIOS_API Task
	{
	public:
		Task() = default;
		~Task();
		Task(Task&& other) noexcept;
		Task& operator=(Task&& other) noexcept;
		Task(const Task&) = delete;
		Task& operator=(const Task&) = delete;

		/* 句柄是否还连着任务池里的任务（池满被丢、没建池就地执行时为空） */
		[[nodiscard]] bool IsValid() const { return m_Job != nullptr; }
		[[nodiscard]] TaskState State() const;
		[[nodiscard]] bool IsDone() const { return State() != TaskState::Pending; }
		/* 阻塞到不再是 Pending。排队中的任务会先提到 High：
		 * 主线程不执行后台任务，排在长任务后面会等很久。
		 * 只给主线程或外部线程用，worker 里等会占着名额自锁。 */
		void Wait() const;
		/* 排队中的直接跳过，在跑的要任务自己查 Jobs::IsCancelled() */
		void Cancel();
		/* 还在排队时换档，比如缩略图滚出视野就降到 Low */
		bool SetPriority(Priority priority);
		void Reset();

	private:
		friend struct TaskAccess;
		Task(void* job, uint32_t owner, SharedPtr<TaskControl> control)
			: m_Job(job), m_Owner(owner), m_Control(std::move(control)) {}

		/* 句柄记下建池代号：线程池重建后旧句柄一律视为已完成，不碰新任务池 */
		[[nodiscard]] void* Live() const;

		void* m_Job = nullptr;
		uint32_t m_Owner = 0;
		SharedPtr<TaskControl> m_Control{};
	};

	/* 投递后台任务：work 在 worker 上跑，on_main 在 work 完成后回主线程执行（被取消就不回）。
	 * 返回的句柄总是有效：池满被丢或没建池就地执行时，State 也会落到终态 */
	HELIOS_API Task RunBackground(std::function<void()> work, Priority priority = Priority::Normal,
		std::function<void()> on_main = {});
	/* 后台任务里查自己是否被取消或已过期 */
	[[nodiscard]] HELIOS_API bool IsCancelled();
	/* 让之前提交的后台任务全部过期：不再执行、不再回主线程。
	 * 目前只有 Jobs::Shutdown 在调用；切项目接上它之前，消费者要先能处理"完成但没结果"。 */
	HELIOS_API void InvalidateBackground();

	/* 主线程回调队列：任何线程都能投，主线程每帧按预算执行 */
	HELIOS_API void PostToMain(std::function<void()> fn);
	HELIOS_API uint32_t DrainMain(uint32_t budget_us = 0);
}
