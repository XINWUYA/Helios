#include "HeliosJobs/ThreadQueue.h"

namespace HeliosJobs
{
	void ThreadQueue::Post(std::function<void()> fn)
	{
		if (!fn)
			return;

		std::lock_guard lock(m_Mutex);
		m_Items.push_back(std::move(fn));
	}

	uint32_t ThreadQueue::Drain(std::chrono::microseconds budget)
	{
		const auto deadline = std::chrono::steady_clock::now() + budget;
		const bool limited = budget.count() > 0;

		/* 一次取一条再执行：回调里继续 Post 不会重入锁，也能被同一轮预算接着处理 */
		uint32_t executed = 0;
		for (;;)
		{
			std::function<void()> fn;
			{
				std::lock_guard lock(m_Mutex);
				if (m_Items.empty())
					break;
				fn = std::move(m_Items.front());
				m_Items.pop_front();
			}

			fn();
			++executed;

			if (limited && std::chrono::steady_clock::now() >= deadline)
				break;
		}
		return executed;
	}

	size_t ThreadQueue::Pending() const
	{
		std::lock_guard lock(m_Mutex);
		return m_Items.size();
	}

	void ThreadQueue::Clear()
	{
		std::deque<std::function<void()>> items;
		{
			std::lock_guard lock(m_Mutex);
			items.swap(m_Items);
		}
	}
}
