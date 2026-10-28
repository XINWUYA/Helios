#include "Pch.h"
#include "Jobs.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <HeliosJobs/JobSystem.h>
#include <HeliosJobs/ThreadQueue.h>

namespace Helios::Jobs
{
	struct TaskAccess
	{
		static Task Make(void* job, uint32_t owner) { return Task(job, owner); }
	};

	namespace
	{
		using HeliosJobs::JobSystem;

		std::mutex s_InitMutex;
		std::unique_ptr<JobSystem> s_System;
		HeliosJobs::ThreadQueue s_MainQueue;
		std::atomic<uint32_t> s_Generation{ 0 };
		uint32_t s_SystemId = 0;

		/* 后台任务的闭包放堆上：std::function 在 MSVC 上就有 64 字节，塞不进任务存储 */
		struct BackgroundPayload
		{
			std::function<void()> Work;
			std::function<void()> OnMain;
			uint32_t Generation = 0;
		};

		thread_local const BackgroundPayload* t_CurrentPayload = nullptr;

		void ForwardLog(HeliosJobs::LogLevel level, const char* message)
		{
			switch (level)
			{
			case HeliosJobs::LogLevel::Error:
				CORE_LOG_ERROR("{}", message);
				break;
			case HeliosJobs::LogLevel::Warning:
				CORE_LOG_WARN("{}", message);
				break;
			default:
				CORE_LOG_INFO("{}", message);
				break;
			}
		}

		bool IsStale(const BackgroundPayload& payload)
		{
			return payload.Generation != s_Generation.load(std::memory_order_acquire);
		}

		/* 主线程回调也不许把异常甩出帧循环 */
		void RunGuarded(const std::function<void()>& fn, const char* what)
		{
			try
			{
				fn();
			}
			catch (const std::exception& error)
			{
				CORE_LOG_ERROR("[Jobs] {} threw: {}", what, error.what());
			}
			catch (...)
			{
				CORE_LOG_ERROR("[Jobs] {} threw an unknown exception", what);
			}
		}

		JobSystem::Job* AsJob(void* job)
		{
			return static_cast<JobSystem::Job*>(job);
		}
	}

	bool Init(uint32_t worker_count)
	{
		std::lock_guard lock(s_InitMutex);
		if (s_System != nullptr)
			return true;

		HeliosJobs::JobSystemDesc desc;
		desc.WorkerCount = worker_count;
		desc.ThreadNamePrefix = "HeliosJob";
		desc.Log = &ForwardLog;

		/* 后台名额至少留 2 个：脚本编译这种长阻塞不该独占后台 */
		const uint32_t hardware = std::max(2u, std::thread::hardware_concurrency());
		const uint32_t workers = worker_count != 0 ? worker_count : hardware - 1;
		desc.BackgroundSlots = workers >= 3 ? std::max(2u, workers / 3) : 1u;

		s_System = std::make_unique<JobSystem>(desc);
		s_System->Adopt();
		++s_SystemId;
		CORE_LOG_INFO("[Jobs] {} workers, {} background slots", s_System->GetWorkerCount(), s_System->GetBackgroundSlots());
		return true;
	}

	void Shutdown()
	{
		std::unique_ptr<JobSystem> system;
		{
			std::lock_guard lock(s_InitMutex);
			system = std::move(s_System);
		}
		if (system == nullptr)
			return;

		/* 先让后台任务全部过期：排队的直接跳过，在跑的查 IsCancelled 自行收手 */
		InvalidateBackground();
		system->Emancipate();
		system.reset();
		s_MainQueue.Clear();
	}

	bool IsInitialized()
	{
		return s_System != nullptr;
	}

	uint32_t WorkerCount()
	{
		return s_System != nullptr ? s_System->GetWorkerCount() : 0;
	}

	uint32_t BackgroundSlots()
	{
		return s_System != nullptr ? s_System->GetBackgroundSlots() : 0;
	}

	bool IsMainThread()
	{
		return s_System != nullptr && s_System->IsAdoptedThread();
	}

	bool IsWorkerThread()
	{
		return s_System != nullptr && s_System->IsWorkerThread();
	}

	void ParallelForImpl(uint32_t count, uint32_t grain, const void* ctx,
		void (*invoke)(const void*, uint32_t, uint32_t))
	{
		if (invoke == nullptr || count == 0)
			return;

		/* 没建池、量不够切、或者在没收编的线程上：直接串行 */
		JobSystem* const system = s_System.get();
		if (system == nullptr || count <= grain || (!system->IsAdoptedThread() && !system->IsWorkerThread()))
		{
			invoke(ctx, 0, count);
			return;
		}

		JobSystem::Job* root = system->CreateParallelFor(nullptr, 0, count, grain,
			[ctx, invoke](uint32_t start, uint32_t length) { invoke(ctx, start, start + length); });
		if (root == nullptr)
		{
			invoke(ctx, 0, count);
			return;
		}
		system->RunAndWait(root);
	}

	uint32_t HelpWhileIdle(uint32_t budget_us)
	{
		if (s_System == nullptr || budget_us == 0)
			return 0;
		return s_System->HelpUntil(std::chrono::steady_clock::now() + std::chrono::microseconds(budget_us));
	}

	Task::~Task()
	{
		Reset();
	}

	Task::Task(Task&& other) noexcept
		: m_Job(std::exchange(other.m_Job, nullptr)), m_Owner(other.m_Owner)
	{
	}

	Task& Task::operator=(Task&& other) noexcept
	{
		if (this != &other)
		{
			Reset();
			m_Job = std::exchange(other.m_Job, nullptr);
			m_Owner = other.m_Owner;
		}
		return *this;
	}

	void* Task::Live() const
	{
		return (m_Job != nullptr && s_System != nullptr && m_Owner == s_SystemId) ? m_Job : nullptr;
	}

	bool Task::IsDone() const
	{
		void* const job = Live();
		return job == nullptr || JobSystem::IsFinished(AsJob(job));
	}

	void Task::Cancel()
	{
		if (void* const job = Live())
			s_System->RequestCancel(AsJob(job));
	}

	bool Task::SetPriority(Priority priority)
	{
		void* const job = Live();
		return job != nullptr && s_System->SetPriority(AsJob(job), static_cast<HeliosJobs::Priority>(priority));
	}

	void Task::Reset()
	{
		JobSystem::Job* job = AsJob(Live());
		m_Job = nullptr;
		if (job != nullptr)
			s_System->Release(job);
	}

	Task RunBackground(std::function<void()> work, Priority priority, std::function<void()> on_main)
	{
		if (!work)
			return {};

		auto payload = std::make_unique<BackgroundPayload>();
		payload->Work = std::move(work);
		payload->OnMain = std::move(on_main);
		payload->Generation = s_Generation.load(std::memory_order_acquire);

		JobSystem* const system = s_System.get();
		if (system == nullptr)
		{
			/* 没建池（检查程序、示例）：就地执行，回调同样走主线程队列 */
			RunGuarded(payload->Work, "Background task");
			if (payload->OnMain)
				PostToMain(std::move(payload->OnMain));
			return {};
		}

		/* 闭包持有 payload：被取消时不执行，但照样析构释放 */
		JobSystem::Job* job = system->CreateJob(nullptr,
			[payload = std::move(payload)](JobSystem& js, JobSystem::Job* self)
		{
			if (IsStale(*payload))
				return;

			t_CurrentPayload = payload.get();
			RunGuarded(payload->Work, "Background task");
			t_CurrentPayload = nullptr;

			if (payload->OnMain && !js.IsCancelled(self) && !IsStale(*payload))
			{
				const uint32_t generation = payload->Generation;
				PostToMain([on_main = std::move(payload->OnMain), generation]()
				{
					if (generation == s_Generation.load(std::memory_order_acquire))
						on_main();
				});
			}
		});

		if (job == nullptr)
		{
			CORE_LOG_ERROR("[Jobs] Job pool exhausted, background task dropped");
			return {};
		}

		JobSystem::Job* retained = system->RunBackgroundAndRetain(job, static_cast<HeliosJobs::Priority>(priority));
		return TaskAccess::Make(retained, s_SystemId);
	}

	bool IsCancelled()
	{
		if (t_CurrentPayload != nullptr && IsStale(*t_CurrentPayload))
			return true;
		JobSystem* const system = s_System.get();
		return system != nullptr && system->IsCancelled(JobSystem::CurrentJob());
	}

	void InvalidateBackground()
	{
		s_Generation.fetch_add(1, std::memory_order_acq_rel);
	}

	void PostToMain(std::function<void()> fn)
	{
		if (!fn)
			return;
		s_MainQueue.Post([fn = std::move(fn)]() { RunGuarded(fn, "Main thread callback"); });
	}

	uint32_t DrainMain(uint32_t budget_us)
	{
		return s_MainQueue.Drain(std::chrono::microseconds(budget_us));
	}
}
