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
 * Modified for Helios: see JobSystem.h.
 */

#include "HeliosJobs/JobSystem.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <random>

#if defined(_WIN32)
	#ifndef NOMINMAX
		#define NOMINMAX
	#endif
	#include <windows.h>
#else
	#include <pthread.h>
#endif

namespace HeliosJobs
{
	thread_local JobSystem::ThreadState* JobSystem::t_State = nullptr;
	thread_local JobSystem::Job* JobSystem::t_CurrentJob = nullptr;
	thread_local bool JobSystem::t_InBackground = false;
	thread_local Priority JobSystem::t_BackgroundPriority = Priority::Normal;

	namespace
	{
		/* 后台任务期间把线程降级：macOS 上会优先落到能效核，Windows 降一档优先级 */
		void SetBackgroundQos(bool background) noexcept
		{
#if defined(__APPLE__)
			pthread_set_qos_class_self_np(background ? QOS_CLASS_UTILITY : QOS_CLASS_USER_INITIATED, 0);
#elif defined(_WIN32)
			SetThreadPriority(GetCurrentThread(), background ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_NORMAL);
#else
			(void)background;
#endif
		}

		uint32_t NextRandom(uint32_t& state) noexcept
		{
			state = static_cast<uint32_t>((static_cast<uint64_t>(state) * 48271u) % 0x7FFFFFFFu);
			return state;
		}
	}

	void JobSystem::SetThreadName(const char* name) noexcept
	{
#if defined(__APPLE__)
		pthread_setname_np(name);
#elif defined(__linux__)
		pthread_setname_np(pthread_self(), name);
#elif defined(_WIN32)
		const int length = MultiByteToWideChar(CP_UTF8, 0, name, -1, nullptr, 0);
		if (length <= 0)
			return;
		std::wstring wide(static_cast<size_t>(length), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, name, -1, wide.data(), length);
		SetThreadDescription(GetCurrentThread(), wide.c_str());
#else
		(void)name;
#endif
	}

	JobSystem::JobSystem(const JobSystemDesc& desc) noexcept
	{
		uint32_t workers = desc.WorkerCount;
		if (workers == 0)
		{
			const unsigned hardware = std::thread::hardware_concurrency();
			workers = hardware > 1 ? hardware - 1 : 1;
		}
		workers = std::clamp<uint32_t>(workers, 1, kMaxWorkers);

		const uint32_t adoptable = std::clamp<uint32_t>(desc.AdoptableCount, 1, kInvalidThreadId - kMaxWorkers);
		const uint32_t slots = desc.BackgroundSlots != 0 ? desc.BackgroundSlots : std::max<uint32_t>(1, workers / 3);

		m_WorkerCount = workers;
		m_BackgroundSlots = std::clamp<uint32_t>(slots, 1, workers);
		m_AdoptedJobWarnUs = desc.AdoptedJobWarnUs;
		m_NamePrefix = (desc.ThreadNamePrefix != nullptr && desc.ThreadNamePrefix[0] != '\0') ? desc.ThreadNamePrefix : "HeliosJob";
		m_Log = desc.Log;

		/* 空闲链用 1 起的编号，0 表示链尾 */
		m_Jobs.reset(new Job[kMaxJobCount]);
		m_FreeNext.reset(new std::atomic<uint16_t>[kMaxJobCount]);
		for (uint32_t index = 0; index < kMaxJobCount; ++index)
			m_FreeNext[index].store(static_cast<uint16_t>(index + 1 < kMaxJobCount ? index + 2 : 0), std::memory_order_relaxed);
		m_FreeHead.store(1, std::memory_order_release);

		m_ThreadStateCount = workers + adoptable;
		m_ThreadStates.reset(new ThreadState[m_ThreadStateCount]);

		std::random_device seed;
		for (uint32_t index = 0; index < m_ThreadStateCount; ++index)
		{
			ThreadState& state = m_ThreadStates[index];
			state.Owner = this;
			state.Index = static_cast<ThreadId>(index);
			state.IsWorker = index < workers;
			state.Random = std::max<uint32_t>(1, seed() % 0x7FFFFFFFu);
		}

		for (uint32_t index = 0; index < workers; ++index)
			m_ThreadStates[index].Thread = std::thread(&JobSystem::WorkerLoop, this, &m_ThreadStates[index]);
	}

	JobSystem::~JobSystem()
	{
		m_ExitRequested.store(true, std::memory_order_seq_cst);
		{
			std::lock_guard lock(m_WaitLock);
		}
		m_WorkerCondition.notify_all();
		m_WaiterCondition.notify_all();

		for (uint32_t index = 0; index < m_WorkerCount; ++index)
		{
			if (m_ThreadStates[index].Thread.joinable())
				m_ThreadStates[index].Thread.join();
		}

		DrainOnExit();

		if (t_State != nullptr && t_State->Owner == this)
			t_State = nullptr;
	}

	void JobSystem::Adopt()
	{
		if (GetState() != nullptr)
			return;

		const uint32_t adopted = m_AdoptedCount.fetch_add(1, std::memory_order_relaxed);
		const uint32_t index = m_WorkerCount + adopted;
		if (index >= m_ThreadStateCount)
		{
			m_AdoptedCount.fetch_sub(1, std::memory_order_relaxed);
			Log(LogLevel::Error, "HeliosJobs: no adoptable thread slot left");
			return;
		}
		t_State = &m_ThreadStates[index];
	}

	void JobSystem::Emancipate()
	{
		if (GetState() != nullptr)
			t_State = nullptr;
	}

	JobSystem::ThreadState* JobSystem::GetState() const noexcept
	{
		return (t_State != nullptr && t_State->Owner == this) ? t_State : nullptr;
	}

	bool JobSystem::IsAdoptedThread() const noexcept
	{
		const ThreadState* state = GetState();
		return state != nullptr && !state->IsWorker;
	}

	bool JobSystem::IsWorkerThread() const noexcept
	{
		const ThreadState* state = GetState();
		return state != nullptr && state->IsWorker;
	}

	JobSystem::Job* JobSystem::CurrentJob() noexcept
	{
		return t_CurrentJob;
	}

	bool JobSystem::IsInBackground() noexcept
	{
		return t_InBackground;
	}

	// ---------------------------------------------------------------------------------------------
	// 任务池

	JobSystem::Job* JobSystem::AllocateJob() noexcept
	{
		uint64_t head = m_FreeHead.load(std::memory_order_acquire);
		for (;;)
		{
			const uint32_t id = static_cast<uint32_t>(head);
			if (id == 0)
				return nullptr;

			const uint64_t next = m_FreeNext[id - 1].load(std::memory_order_relaxed);
			const uint64_t new_head = (((head >> 32) + 1) << 32) | next;
			if (m_FreeHead.compare_exchange_weak(head, new_head, std::memory_order_acquire, std::memory_order_acquire))
				return new (&m_Jobs[id - 1]) Job();
		}
	}

	void JobSystem::DestroyJob(const Job* job) noexcept
	{
		const uint32_t id = static_cast<uint32_t>(job - m_Jobs.get()) + 1;
		uint64_t head = m_FreeHead.load(std::memory_order_relaxed);
		for (;;)
		{
			m_FreeNext[id - 1].store(static_cast<uint16_t>(head), std::memory_order_relaxed);
			const uint64_t new_head = (((head >> 32) + 1) << 32) | id;
			if (m_FreeHead.compare_exchange_weak(head, new_head, std::memory_order_release, std::memory_order_relaxed))
				return;
		}
	}

	void JobSystem::IncRef(const Job* job) noexcept
	{
		job->RefCount.fetch_add(1, std::memory_order_relaxed);
	}

	void JobSystem::DecRef(const Job* job) noexcept
	{
		const uint8_t count = job->RefCount.fetch_sub(1, std::memory_order_acq_rel);
		assert(count > 0);
		if (count == 1)
			DestroyJob(job);
	}

	// ---------------------------------------------------------------------------------------------
	// 队列

	bool JobSystem::HasFrameWork() const noexcept
	{
		return m_ActiveJobs.load(std::memory_order_seq_cst) > 0;
	}

	bool JobSystem::HasBackgroundWork(bool ignore_slots) const noexcept
	{
		if (m_BackgroundQueued.load(std::memory_order_seq_cst) == 0)
			return false;
		return ignore_slots || m_BackgroundRunning.load(std::memory_order_seq_cst) < m_BackgroundSlots;
	}

	void JobSystem::WakeWorker() noexcept
	{
		/* 没人睡就不碰锁：投递方（通常是主线程）的热路径只剩几个原子操作 */
		if (m_SleepingWorkers.load(std::memory_order_seq_cst) == 0)
			return;
		{
			std::lock_guard lock(m_WaitLock);
		}
		m_WorkerCondition.notify_one();
	}

	void JobSystem::WakeWaiters() noexcept
	{
		{
			std::lock_guard lock(m_WaitLock);
		}
		m_WaiterCondition.notify_all();
	}

	void JobSystem::Put(ThreadState& state, Job* job) noexcept
	{
		const size_t index = static_cast<size_t>(job - m_Jobs.get());
		state.Queue.Push(static_cast<uint16_t>(index + 1));
		m_ActiveJobs.fetch_add(1, std::memory_order_seq_cst);
		WakeWorker();
	}

	void JobSystem::PutBackground(Job* job, Priority priority) noexcept
	{
		job->RunningJobCount.fetch_or(kBackgroundBit, std::memory_order_relaxed);
		{
			std::lock_guard lock(m_BackgroundLock);
			m_Background[static_cast<uint32_t>(priority)].push_back(static_cast<uint16_t>(job - m_Jobs.get()));
			m_BackgroundQueued.fetch_add(1, std::memory_order_seq_cst);
		}
		WakeWorker();
	}

	JobSystem::Job* JobSystem::PopFrame(ThreadState& state) noexcept
	{
		const uint16_t index = state.Queue.Pop();
		if (index == 0)
			return nullptr;
		m_ActiveJobs.fetch_sub(1, std::memory_order_relaxed);
		return &m_Jobs[index - 1];
	}

	JobSystem::Job* JobSystem::StealFrame(ThreadState& state) noexcept
	{
		const uint32_t thread_count = m_WorkerCount + m_AdoptedCount.load(std::memory_order_relaxed);
		if (thread_count < 2)
			return nullptr;

		do
		{
			const uint32_t victim = NextRandom(state.Random) % thread_count;
			if (victim == state.Index)
				continue;

			const uint16_t index = m_ThreadStates[victim].Queue.Steal();
			if (index != 0)
			{
				m_ActiveJobs.fetch_sub(1, std::memory_order_relaxed);
				return &m_Jobs[index - 1];
			}
		} while (HasFrameWork());

		return nullptr;
	}

	JobSystem::Job* JobSystem::TakeBackground(bool ignore_slots, bool& owns_slot, Priority& priority) noexcept
	{
		owns_slot = false;
		if (m_BackgroundQueued.load(std::memory_order_relaxed) == 0)
			return nullptr;

		/* 先占名额再取任务，长任务最多占住 m_BackgroundSlots 个 worker */
		if (!ignore_slots)
		{
			uint32_t running = m_BackgroundRunning.load(std::memory_order_relaxed);
			do
			{
				if (running >= m_BackgroundSlots)
					return nullptr;
			} while (!m_BackgroundRunning.compare_exchange_weak(running, running + 1, std::memory_order_seq_cst));
			owns_slot = true;
		}

		Job* job = nullptr;
		{
			std::lock_guard lock(m_BackgroundLock);
			for (uint32_t level = 0; level < kPriorityCount; ++level)
			{
				std::deque<uint16_t>& queue = m_Background[level];
				if (queue.empty())
					continue;

				job = &m_Jobs[queue.front()];
				queue.pop_front();
				m_BackgroundQueued.fetch_sub(1, std::memory_order_seq_cst);
				priority = static_cast<Priority>(level);
				break;
			}
		}

		if (job == nullptr && owns_slot)
		{
			m_BackgroundRunning.fetch_sub(1, std::memory_order_seq_cst);
			owns_slot = false;
		}
		return job;
	}

	// ---------------------------------------------------------------------------------------------
	// 执行

	void JobSystem::ExecuteJob(ThreadState& state, Job* job, bool stolen) noexcept
	{
		Job* const previous = t_CurrentJob;
		t_CurrentJob = job;
		job->Id = state.Index;

		if (job->Function != nullptr)
		{
			/* 只盯收编线程偷来的任务：自己投递的那份本来就是它自己的活 */
			const bool timed = stolen && !state.IsWorker && m_AdoptedJobWarnUs != 0;
			const auto start = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};

			try
			{
				job->Function(job->Storage, *this, job);
			}
			catch (...)
			{
				ReportException();
			}

			if (timed)
			{
				const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
				if (elapsed > static_cast<long long>(m_AdoptedJobWarnUs))
				{
					char message[128];
					std::snprintf(message, sizeof(message),
						"HeliosJobs: a job ran %lld us on an adopted thread, split it into smaller pieces", static_cast<long long>(elapsed));
					Log(LogLevel::Warning, message);
				}
			}
		}

		job->Id = kInvalidThreadId;
		t_CurrentJob = previous;
		Finish(job);
	}

	void JobSystem::RunBackgroundJob(ThreadState& state, Job* job, bool owns_slot, Priority priority) noexcept
	{
		const bool outer = !t_InBackground;
		const Priority previous_priority = t_BackgroundPriority;
		t_BackgroundPriority = priority;
		if (outer)
		{
			t_InBackground = true;
			SetBackgroundQos(true);
		}

		ExecuteJob(state, job, false);

		if (outer)
		{
			SetBackgroundQos(false);
			t_InBackground = false;
		}
		t_BackgroundPriority = previous_priority;

		if (owns_slot)
		{
			m_BackgroundRunning.fetch_sub(1, std::memory_order_seq_cst);
			if (m_BackgroundQueued.load(std::memory_order_seq_cst) != 0)
				WakeWorker();
		}
	}

	bool JobSystem::ExecuteFrame(ThreadState& state) noexcept
	{
		if (Job* job = PopFrame(state))
		{
			ExecuteJob(state, job, false);
			return true;
		}
		if (Job* job = StealFrame(state))
		{
			ExecuteJob(state, job, true);
			return true;
		}
		return false;
	}

	bool JobSystem::ExecuteBackgroundHelp(ThreadState& state) noexcept
	{
		/* 已经在后台任务里：只帮后台的忙，并且不再占名额（自己手里已经有一个） */
		if (Job* job = PopFrame(state))
		{
			ExecuteJob(state, job, false);
			return true;
		}

		bool owns_slot = false;
		Priority priority = Priority::Normal;
		Job* job = TakeBackground(true, owns_slot, priority);
		if (job == nullptr)
			return false;

		RunBackgroundJob(state, job, owns_slot, priority);
		return true;
	}

	bool JobSystem::ExecuteAny(ThreadState& state) noexcept
	{
		if (ExecuteFrame(state))
			return true;

		bool owns_slot = false;
		Priority priority = Priority::Normal;
		Job* job = TakeBackground(false, owns_slot, priority);
		if (job == nullptr)
			return false;

		RunBackgroundJob(state, job, owns_slot, priority);
		return true;
	}

	void JobSystem::Finish(Job* job) noexcept
	{
		bool notify = false;
		do
		{
			const uint32_t value = job->RunningJobCount.fetch_sub(1, std::memory_order_acq_rel);
			const uint32_t running = value & kJobCountMask;
			assert(running > 0);
			if (running != 1)
				break;

			if ((value >> kWaiterShift) != 0)
				notify = true;

			Job* const parent = job->Parent == kNoParent ? nullptr : &m_Jobs[job->Parent];
			DecRef(job);
			job = parent;
		} while (job != nullptr);

		if (notify)
			WakeWaiters();
	}

	void JobSystem::WorkerLoop(ThreadState* state)
	{
		t_State = state;
		const std::string name = m_NamePrefix + std::to_string(state->Index);
		SetThreadName(name.c_str());
		SetBackgroundQos(false);

		while (!m_ExitRequested.load(std::memory_order_relaxed))
		{
			if (ExecuteAny(*state))
				continue;

			std::unique_lock lock(m_WaitLock);
			m_SleepingWorkers.fetch_add(1, std::memory_order_seq_cst);
			while (!m_ExitRequested.load(std::memory_order_relaxed) && !HasFrameWork() && !HasBackgroundWork(false))
				m_WorkerCondition.wait(lock);
			m_SleepingWorkers.fetch_sub(1, std::memory_order_seq_cst);
		}

		t_State = nullptr;
	}

	void JobSystem::DrainOnExit() noexcept
	{
		/* worker 已全部退出：剩下的任务以取消状态走一遍，让闭包正常析构 */
		auto discard = [this](Job* job)
		{
			job->RunningJobCount.fetch_or(kCancelBit, std::memory_order_relaxed);
			if (job->Function != nullptr)
				job->Function(job->Storage, *this, job);
			Finish(job);
		};

		for (uint32_t index = 0; index < m_ThreadStateCount; ++index)
		{
			while (const uint16_t id = m_ThreadStates[index].Queue.Pop())
				discard(&m_Jobs[id - 1]);
		}

		for (std::deque<uint16_t>& queue : m_Background)
		{
			while (!queue.empty())
			{
				const uint16_t id = queue.front();
				queue.pop_front();
				discard(&m_Jobs[id]);
			}
		}
		m_ActiveJobs.store(0, std::memory_order_relaxed);
		m_BackgroundQueued.store(0, std::memory_order_relaxed);
	}

	// ---------------------------------------------------------------------------------------------
	// 公开接口

	JobSystem::Job* JobSystem::Create(Job* parent, JobFunc func) noexcept
	{
		Job* const job = AllocateJob();
		if (job == nullptr)
		{
			Log(LogLevel::Error, "HeliosJobs: job pool exhausted");
			return nullptr;
		}

		uint16_t parent_index = kNoParent;
		if (parent != nullptr)
		{
			const uint32_t parent_count = parent->RunningJobCount.fetch_add(1, std::memory_order_relaxed);
			assert((parent_count & kJobCountMask) > 0);
			(void)parent_count;
			parent_index = static_cast<uint16_t>(parent - m_Jobs.get());
		}

		job->Function = func;
		job->Parent = parent_index;
		return job;
	}

	void JobSystem::Cancel(Job*& job) noexcept
	{
		if (job == nullptr)
			return;

		/* 带取消标记走一遍函数：包装层会跳过用户代码，只做析构 */
		job->RunningJobCount.fetch_or(kCancelBit, std::memory_order_relaxed);
		if (job->Function != nullptr)
			job->Function(job->Storage, *this, job);
		Finish(job);
		job = nullptr;
	}

	JobSystem::Job* JobSystem::Retain(Job* job) noexcept
	{
		if (job != nullptr)
			IncRef(job);
		return job;
	}

	void JobSystem::Release(Job*& job) noexcept
	{
		if (job != nullptr)
			DecRef(job);
		job = nullptr;
	}

	void JobSystem::Run(Job*& job) noexcept
	{
		if (job == nullptr)
			return;

		/* 后台任务派生的子任务和未收编线程投递的任务都走后台队列，主线程永远碰不到 */
		ThreadState* const state = GetState();
		if (t_InBackground || state == nullptr)
			PutBackground(job, t_InBackground ? t_BackgroundPriority : Priority::High);
		else
			Put(*state, job);
		job = nullptr;
	}

	JobSystem::Job* JobSystem::RunAndRetain(Job* job) noexcept
	{
		Job* const retained = Retain(job);
		Run(job);
		return retained;
	}

	void JobSystem::RunBackground(Job*& job, Priority priority) noexcept
	{
		if (job == nullptr)
			return;
		PutBackground(job, priority);
		job = nullptr;
	}

	JobSystem::Job* JobSystem::RunBackgroundAndRetain(Job* job, Priority priority) noexcept
	{
		Job* const retained = Retain(job);
		RunBackground(job, priority);
		return retained;
	}

	uint32_t JobSystem::WaitFor(std::unique_lock<std::mutex>& lock, Job* job, bool background) noexcept
	{
		const bool has_work = background ? HasBackgroundWork(true) : HasFrameWork();
		if (has_work || m_ExitRequested.load(std::memory_order_relaxed))
			return job->RunningJobCount.load(std::memory_order_acquire);

		uint32_t value = job->RunningJobCount.fetch_add(1u << kWaiterShift, std::memory_order_relaxed);
		if ((value & kJobCountMask) != 0)
			m_WaiterCondition.wait(lock);
		value = job->RunningJobCount.fetch_sub(1u << kWaiterShift, std::memory_order_acquire);
		return value;
	}

	void JobSystem::WaitAndRelease(Job*& job) noexcept
	{
		if (job == nullptr)
			return;

		ThreadState* const state = GetState();
		if (state == nullptr)
		{
			/* 未收编线程不帮忙，只阻塞等 */
			std::unique_lock lock(m_WaitLock);
			while (!IsFinished(job) && !m_ExitRequested.load(std::memory_order_relaxed))
			{
				const uint32_t value = job->RunningJobCount.fetch_add(1u << kWaiterShift, std::memory_order_relaxed);
				if ((value & kJobCountMask) != 0)
					m_WaiterCondition.wait(lock);
				job->RunningJobCount.fetch_sub(1u << kWaiterShift, std::memory_order_acquire);
			}
			lock.unlock();
			Release(job);
			return;
		}

		const bool background = t_InBackground;
		if (!state->IsWorker && (job->RunningJobCount.load(std::memory_order_relaxed) & kBackgroundBit) != 0)
			Log(LogLevel::Warning, "HeliosJobs: adopted thread is blocking on a background job, poll IsFinished instead");

		do
		{
			const bool executed = background ? ExecuteBackgroundHelp(*state) : ExecuteFrame(*state);
			if (!executed)
			{
				if (IsFinished(job))
					break;

				std::unique_lock lock(m_WaitLock);
				const uint32_t value = WaitFor(lock, job, background);
				if ((value & kJobCountMask) == 0 || m_ExitRequested.load(std::memory_order_relaxed))
					break;
			}
		} while (!IsFinished(job) && !m_ExitRequested.load(std::memory_order_relaxed));

		Release(job);
	}

	void JobSystem::RunAndWait(Job*& job) noexcept
	{
		Job* retained = RunAndRetain(job);
		job = nullptr;
		WaitAndRelease(retained);
	}

	bool JobSystem::IsFinished(const Job* job) noexcept
	{
		return (job->RunningJobCount.load(std::memory_order_acquire) & kJobCountMask) == 0;
	}

	void JobSystem::RequestCancel(Job* job) noexcept
	{
		if (job != nullptr)
			job->RunningJobCount.fetch_or(kCancelBit, std::memory_order_relaxed);
	}

	bool JobSystem::IsCancelled(const Job* job) const noexcept
	{
		while (job != nullptr)
		{
			if ((job->RunningJobCount.load(std::memory_order_relaxed) & kCancelBit) != 0)
				return true;
			job = job->Parent == kNoParent ? nullptr : &m_Jobs[job->Parent];
		}
		return false;
	}

	bool JobSystem::SetPriority(const Job* job, Priority priority) noexcept
	{
		if (job == nullptr)
			return false;

		const uint16_t id = static_cast<uint16_t>(job - m_Jobs.get());
		std::lock_guard lock(m_BackgroundLock);
		for (std::deque<uint16_t>& queue : m_Background)
		{
			const auto iter = std::find(queue.begin(), queue.end(), id);
			if (iter == queue.end())
				continue;

			queue.erase(iter);
			m_Background[static_cast<uint32_t>(priority)].push_back(id);
			return true;
		}
		return false;
	}

	uint32_t JobSystem::HelpUntil(std::chrono::steady_clock::time_point deadline) noexcept
	{
		ThreadState* const state = GetState();
		if (state == nullptr || t_InBackground)
			return 0;

		uint32_t executed = 0;
		while (std::chrono::steady_clock::now() < deadline && ExecuteFrame(*state))
			++executed;
		return executed;
	}

	uint32_t JobSystem::ParallelWidth() const noexcept
	{
		/* 后台里的分片也只能在名额内铺开；帧内按全部 worker 加调用线程算 */
		return t_InBackground ? m_BackgroundSlots : m_WorkerCount + 1;
	}

	void JobSystem::ReportException() const noexcept
	{
		Log(LogLevel::Error, "HeliosJobs: job threw an exception");
	}

	void JobSystem::Log(LogLevel level, const char* message) const noexcept
	{
		if (m_Log != nullptr)
			m_Log(level, message);
	}
}
