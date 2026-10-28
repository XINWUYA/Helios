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
 * Modified for Helios: derived from filament/libs/utils JobSystem. Added background
 * queue with priorities, slots and cancellation; lock-free job pool; thread-local
 * thread state; main thread only helps with frame jobs.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

#include "HeliosJobs/WorkStealingDequeue.h"

namespace HeliosJobs
{
	inline constexpr size_t kCacheLineSize = 64;

	enum class LogLevel : uint8_t
	{
		Info = 0,
		Warning,
		Error,
	};

	/* 后台任务优先级，取任务时从高往低 */
	enum class Priority : uint8_t
	{
		High = 0,
		Normal,
		Low,
	};
	inline constexpr uint32_t kPriorityCount = 3;

	struct JobSystemDesc
	{
		/* worker 数，0 表示硬件线程数减 1，最多 32 */
		uint32_t WorkerCount = 0;
		/* 能收编的外部线程数，主线程算一个 */
		uint32_t AdoptableCount = 1;
		/* 同时在跑的后台任务上限，0 表示 worker 数的三分之一（至少 1） */
		uint32_t BackgroundSlots = 0;
		/* 收编线程上单个任务超过这个时长就告警，0 表示不检查 */
		uint32_t AdoptedJobWarnUs = 2000;
		const char* ThreadNamePrefix = "HeliosJob";
		void (*Log)(LogLevel level, const char* message) = nullptr;
	};

	/* 两类任务共用一组 worker：
	 *   帧内任务进每线程偷取队列，收编线程（主线程）等待或空闲时只会偷这一类；
	 *   后台任务进带优先级的共享队列，只由 worker 在名额内执行，执行期间线程降为后台优先级。 */
	class JobSystem
	{
		static constexpr uint32_t kMaxJobCount = 1u << 14;
		static constexpr uint32_t kJobCountMask = kMaxJobCount - 1;
		static constexpr uint32_t kBackgroundBit = 1u << 22;
		static constexpr uint32_t kCancelBit = 1u << 23;
		static constexpr uint32_t kWaiterShift = 24;
		static constexpr uint16_t kNoParent = 0x7FFF;
		static constexpr uint32_t kMaxWorkers = 32;
		static_assert(kMaxJobCount <= kNoParent);

		using WorkQueue = WorkStealingDequeue<uint16_t, kMaxJobCount>;

	public:
		class Job;
		using ThreadId = uint8_t;
		using JobFunc = void (*)(void* storage, JobSystem& js, Job* job);
		static constexpr ThreadId kInvalidThreadId = 0xFF;

		class alignas(kCacheLineSize) Job
		{
		public:
			static constexpr size_t kStorageSize = 48;

			Job() noexcept {}
			Job(const Job&) = delete;
			Job& operator=(const Job&) = delete;

		private:
			friend class JobSystem;

			void* Storage[kStorageSize / sizeof(void*)];
			JobFunc Function = nullptr;
			uint16_t Parent = kNoParent;
			mutable ThreadId Id = kInvalidThreadId;
			mutable std::atomic<uint8_t> RefCount{ 1 };
			std::atomic<uint32_t> RunningJobCount{ 1 };
		};
		static_assert(sizeof(Job) == kCacheLineSize);

		explicit JobSystem(const JobSystemDesc& desc = {}) noexcept;
		~JobSystem();
		JobSystem(const JobSystem&) = delete;
		JobSystem& operator=(const JobSystem&) = delete;

		/* 把当前线程收编进来（主线程必须收编才能投递和等待帧内任务） */
		void Adopt();
		void Emancipate();

		/* 建任务，任何线程都能调用；任务池用完返回 nullptr */
		Job* Create(Job* parent, JobFunc func) noexcept;
		Job* CreateJob(Job* parent = nullptr) noexcept { return Create(parent, nullptr); }

		/* functor 签名 void(JobSystem&, Job*)，大小不超过 48 字节，被取消时不调用但照样析构 */
		template <typename T>
		Job* CreateJob(Job* parent, T functor) noexcept;

		/* 分片并行：fn(start, count) 会被多个线程同时调用，必须 const 可调用，
		 * 闭包不超过 24 字节（大的按指针捕获）。返回根任务，交给 Run / RunAndWait。 */
		template <typename F>
		Job* CreateParallelFor(Job* parent, uint32_t start, uint32_t count, uint32_t grain, F fn) noexcept;

		/* 还没投递的任务直接作废 */
		void Cancel(Job*& job) noexcept;
		static Job* Retain(Job* job) noexcept;
		void Release(Job*& job) noexcept;

		/* 帧内任务：进当前线程的偷取队列；后台任务里调用会自动转成同档后台任务 */
		void Run(Job*& job) noexcept;
		void Run(Job*&& job) noexcept { Job* p = job; Run(p); }
		Job* RunAndRetain(Job* job) noexcept;

		/* 后台任务：任何线程都能投递，主线程永远不会执行它 */
		void RunBackground(Job*& job, Priority priority = Priority::Normal) noexcept;
		void RunBackground(Job*&& job, Priority priority = Priority::Normal) noexcept { Job* p = job; RunBackground(p, priority); }
		Job* RunBackgroundAndRetain(Job* job, Priority priority = Priority::Normal) noexcept;

		/* 等待期间顺手执行同类任务；收编线程上只偷帧内任务 */
		void WaitAndRelease(Job*& job) noexcept;
		void RunAndWait(Job*& job) noexcept;
		void RunAndWait(Job*&& job) noexcept { Job* p = job; RunAndWait(p); }

		[[nodiscard]] static bool IsFinished(const Job* job) noexcept;
		/* 协作式取消：排队中的直接跳过，在跑的由任务自己查 IsCancelled，子任务跟着父任务取消 */
		void RequestCancel(Job* job) noexcept;
		[[nodiscard]] bool IsCancelled(const Job* job) const noexcept;
		/* 还在排队的后台任务换档，返回是否找到 */
		bool SetPriority(const Job* job, Priority priority) noexcept;

		/* 收编线程空闲时偷帧内小任务，到点就停，从不睡眠也不碰后台任务 */
		uint32_t HelpUntil(std::chrono::steady_clock::time_point deadline) noexcept;

		[[nodiscard]] static Job* CurrentJob() noexcept;
		[[nodiscard]] static ThreadId GetThreadId(const Job* job) noexcept { return job->Id; }
		[[nodiscard]] static bool IsInBackground() noexcept;
		[[nodiscard]] bool IsAdoptedThread() const noexcept;
		[[nodiscard]] bool IsWorkerThread() const noexcept;
		[[nodiscard]] uint32_t GetWorkerCount() const noexcept { return m_WorkerCount; }
		[[nodiscard]] uint32_t GetBackgroundSlots() const noexcept { return m_BackgroundSlots; }
		[[nodiscard]] uint32_t GetBackgroundQueued() const noexcept { return m_BackgroundQueued.load(std::memory_order_relaxed); }
		[[nodiscard]] uint32_t GetBackgroundRunning() const noexcept { return m_BackgroundRunning.load(std::memory_order_relaxed); }

		static void SetThreadName(const char* name) noexcept;

	private:
		struct alignas(kCacheLineSize) ThreadState
		{
			WorkQueue Queue;

			alignas(kCacheLineSize) JobSystem* Owner = nullptr;
			std::thread Thread;
			uint32_t Random = 1;
			ThreadId Index = 0;
			bool IsWorker = false;
		};

		template <typename F>
		struct ParallelForData
		{
			std::atomic<uint32_t> NextOffset{ 0 };
			uint32_t Start = 0;
			uint32_t Count = 0;
			uint32_t Chunk = 1;
			std::atomic<uint32_t> ActiveWorkers{ 1 };
			F Fn;

			ParallelForData(uint32_t start, uint32_t count, uint32_t chunk, F fn) noexcept
				: Start(start), Count(count), Chunk(chunk), Fn(std::move(fn)) {}

			void Process()
			{
				uint32_t offset;
				while ((offset = NextOffset.fetch_add(Chunk, std::memory_order_relaxed)) < Count)
					std::as_const(Fn)(Start + offset, Count - offset < Chunk ? Count - offset : Chunk);
			}

			/* 最后一个干完活的线程负责析构，根任务的存储要等子任务全结束才回收 */
			void FinishWorker() noexcept
			{
				if (ActiveWorkers.fetch_sub(1, std::memory_order_acq_rel) == 1)
					this->~ParallelForData();
			}
		};

		ThreadState* GetState() const noexcept;
		Job* AllocateJob() noexcept;
		void DestroyJob(const Job* job) noexcept;
		static void IncRef(const Job* job) noexcept;
		void DecRef(const Job* job) noexcept;

		void Put(ThreadState& state, Job* job) noexcept;
		void PutBackground(Job* job, Priority priority) noexcept;
		Job* PopFrame(ThreadState& state) noexcept;
		Job* StealFrame(ThreadState& state) noexcept;
		Job* TakeBackground(bool ignore_slots, bool& owns_slot, Priority& priority) noexcept;

		bool ExecuteFrame(ThreadState& state) noexcept;
		bool ExecuteBackgroundHelp(ThreadState& state) noexcept;
		bool ExecuteAny(ThreadState& state) noexcept;
		void ExecuteJob(ThreadState& state, Job* job, bool stolen) noexcept;
		void RunBackgroundJob(ThreadState& state, Job* job, bool owns_slot, Priority priority) noexcept;
		void Finish(Job* job) noexcept;

		bool HasFrameWork() const noexcept;
		bool HasBackgroundWork(bool ignore_slots) const noexcept;
		uint32_t WaitFor(std::unique_lock<std::mutex>& lock, Job* job, bool background) noexcept;
		void WakeWorker() noexcept;
		void WakeWaiters() noexcept;
		void WorkerLoop(ThreadState* state);
		void DrainOnExit() noexcept;

		uint32_t ParallelWidth() const noexcept;
		void ReportException() const noexcept;
		void Log(LogLevel level, const char* message) const noexcept;

		static thread_local ThreadState* t_State;
		static thread_local Job* t_CurrentJob;
		static thread_local bool t_InBackground;
		static thread_local Priority t_BackgroundPriority;

		/* 等待与唤醒：worker 等新任务，等待方等任务完成，两边分开避免互相吞掉通知 */
		std::mutex m_WaitLock;
		std::condition_variable m_WorkerCondition;
		std::condition_variable m_WaiterCondition;
		std::atomic<int32_t> m_ActiveJobs{ 0 };
		std::atomic<uint32_t> m_SleepingWorkers{ 0 };
		std::atomic<bool> m_ExitRequested{ false };

		/* 任务池：定长数组 + 带版本号的无锁空闲链 */
		std::unique_ptr<Job[]> m_Jobs;
		std::unique_ptr<std::atomic<uint16_t>[]> m_FreeNext;
		alignas(kCacheLineSize) std::atomic<uint64_t> m_FreeHead{ 0 };

		/* 后台队列：量少且长，一把锁足够 */
		alignas(kCacheLineSize) std::mutex m_BackgroundLock;
		std::deque<uint16_t> m_Background[kPriorityCount];
		std::atomic<uint32_t> m_BackgroundQueued{ 0 };
		std::atomic<uint32_t> m_BackgroundRunning{ 0 };

		std::unique_ptr<ThreadState[]> m_ThreadStates;
		std::atomic<uint32_t> m_AdoptedCount{ 0 };
		uint32_t m_ThreadStateCount = 0;
		uint32_t m_WorkerCount = 0;
		uint32_t m_BackgroundSlots = 1;
		uint32_t m_AdoptedJobWarnUs = 0;
		std::string m_NamePrefix;
		void (*m_Log)(LogLevel, const char*) = nullptr;
	};

	template <typename T>
	JobSystem::Job* JobSystem::CreateJob(Job* parent, T functor) noexcept
	{
		static_assert(sizeof(T) <= Job::kStorageSize, "functor too large, capture by pointer");
		static_assert(alignof(T) <= alignof(void*), "functor over-aligned");

		Job* job = Create(parent, [](void* storage, JobSystem& js, Job* self)
		{
			T* const that = static_cast<T*>(storage);
			if (!js.IsCancelled(self))
			{
				try
				{
					(*that)(js, self);
				}
				catch (...)
				{
					js.ReportException();
				}
			}
			that->~T();
		});

		if (job != nullptr)
			new (job->Storage) T(std::move(functor));
		return job;
	}

	template <typename F>
	JobSystem::Job* JobSystem::CreateParallelFor(Job* parent, uint32_t start, uint32_t count, uint32_t grain, F fn) noexcept
	{
		using Data = ParallelForData<F>;
		static_assert(sizeof(Data) <= Job::kStorageSize, "parallel-for functor too large, capture by pointer");
		static_assert(std::is_invocable_v<const F&, uint32_t, uint32_t>, "functor must be const-invocable as fn(start, count)");

		if (count == 0 || count > 0x7FFFFFFFu)
			return nullptr;

		/* 块大小至少 grain，总块数封顶 4096，线程间用原子游标动态领取 */
		const uint32_t min_chunk = (count + 4095u) / 4096u;
		const uint32_t chunk = grain > min_chunk ? grain : (min_chunk > 0 ? min_chunk : 1);

		Job* root = Create(parent, [](void* storage, JobSystem& js, Job* self)
		{
			Data* const data = static_cast<Data*>(storage);
			const uint32_t chunks = (data->Count + data->Chunk - 1) / data->Chunk;
			const uint32_t width = js.ParallelWidth();
			const uint32_t helpers = (chunks < width ? chunks : width) - 1;

			data->ActiveWorkers.store(1 + helpers, std::memory_order_relaxed);
			for (uint32_t index = 0; index < helpers; ++index)
			{
				Job* helper = js.Create(self, [](void* helper_storage, JobSystem& helper_js, Job* helper_job)
				{
					Data* const shared = static_cast<Data*>(static_cast<void**>(helper_storage)[0]);
					if (!helper_js.IsCancelled(helper_job))
					{
						try { shared->Process(); }
						catch (...) { helper_js.ReportException(); }
					}
					shared->FinishWorker();
				});

				if (helper == nullptr)
				{
					/* 池满就少开几个帮手，活照样干完 */
					data->ActiveWorkers.fetch_sub(helpers - index, std::memory_order_relaxed);
					break;
				}
				helper->Storage[0] = data;
				js.Run(helper);
			}

			if (!js.IsCancelled(self))
			{
				try { data->Process(); }
				catch (...) { js.ReportException(); }
			}
			data->FinishWorker();
		});

		if (root != nullptr)
			new (root->Storage) Data(start, count, chunk, std::move(fn));
		return root;
	}
}
