#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>

namespace HeliosJobs
{
	/* 投给某个线程执行的回调队列：任何线程都能 Post，所属线程按时间预算 Drain。
	 * 后台任务的收尾（建 GPU 对象、改场景、刷 UI）都经它回主线程。 */
	class ThreadQueue
	{
	public:
		void Post(std::function<void()> fn);

		/* 预算用完就停，剩下的按原顺序留到下次；budget 为 0 表示全部执行 */
		uint32_t Drain(std::chrono::microseconds budget = std::chrono::microseconds::zero());

		[[nodiscard]] size_t Pending() const;
		void Clear();

	private:
		mutable std::mutex m_Mutex;
		std::deque<std::function<void()>> m_Items;
	};
}
