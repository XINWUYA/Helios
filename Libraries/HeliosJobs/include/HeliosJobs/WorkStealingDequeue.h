/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Modified for Helios: moved to namespace HeliosJobs.
 */

#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace HeliosJobs
{
	/* 定长无锁偷取队列（Chase-Lev）：自己从底部 push / pop，别的线程从顶部 steal。
	 * 内存序沿用 Filament 原实现，seq_cst 不要随手放宽。 */
	template <typename TYPE, size_t COUNT>
	class WorkStealingDequeue
	{
		static_assert(!(COUNT & (COUNT - 1)), "COUNT must be a power of two");
		static_assert(std::is_trivially_destructible_v<TYPE>);
		static constexpr size_t MASK = COUNT - 1;

		using index_t = int64_t;

		std::atomic<index_t> m_Top{ 0 };
		std::atomic<index_t> m_Bottom{ 0 };
		/* 槽位用 relaxed 原子：偷取方可能读到正被覆盖的旧槽，CAS 失败会丢弃它 */
		std::atomic<TYPE> m_Items[COUNT]{};

		TYPE GetItemAt(index_t index) noexcept { return m_Items[index & MASK].load(std::memory_order_relaxed); }
		void SetItemAt(index_t index, TYPE item) noexcept { m_Items[index & MASK].store(item, std::memory_order_relaxed); }

	public:
		/* 只能由队列所属线程调用 */
		void Push(TYPE item) noexcept
		{
			const index_t bottom = m_Bottom.load(std::memory_order_relaxed);
			SetItemAt(bottom, item);
			m_Bottom.store(bottom + 1, std::memory_order_seq_cst);
		}

		/* 只能由队列所属线程调用；空队列返回 TYPE() */
		TYPE Pop() noexcept
		{
			const index_t bottom = m_Bottom.fetch_sub(1, std::memory_order_seq_cst) - 1;
			assert(bottom >= -1);

			index_t top = m_Top.load(std::memory_order_seq_cst);
			if (top < bottom)
				return GetItemAt(bottom);

			TYPE item{};
			if (top == bottom)
			{
				/* 最后一个元素可能正被偷：跟偷取方抢同一个 top */
				item = GetItemAt(bottom);
				if (m_Top.compare_exchange_strong(top, top + 1,
					std::memory_order_seq_cst, std::memory_order_relaxed))
				{
					++top;
				}
				else
				{
					item = TYPE();
				}
			}
			else
			{
				assert(top - bottom == 1);
			}

			m_Bottom.store(top, std::memory_order_seq_cst);
			return item;
		}

		/* 任何线程都能调用；空队列返回 TYPE() */
		TYPE Steal() noexcept
		{
			for (;;)
			{
				index_t top = m_Top.load(std::memory_order_seq_cst);
				const index_t bottom = m_Bottom.load(std::memory_order_seq_cst);
				if (top >= bottom)
					return TYPE();

				const TYPE item(GetItemAt(top));
				if (m_Top.compare_exchange_strong(top, top + 1,
					std::memory_order_seq_cst, std::memory_order_relaxed))
				{
					return item;
				}
			}
		}

		[[nodiscard]] size_t GetCount() const noexcept
		{
			const index_t bottom = m_Bottom.load(std::memory_order_relaxed);
			const index_t top = m_Top.load(std::memory_order_relaxed);
			return bottom > top ? static_cast<size_t>(bottom - top) : 0;
		}
	};
}
