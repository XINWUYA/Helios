#include "Pch.h"
#include "ShaderReloader.h"

#include <algorithm>

#include "Helios/Core/AssetManager.h"
#include "Helios/Renderer/Renderer.h"
#include "Helios/VirtualDevice/DeviceShader.h"

namespace Helios
{
	namespace
	{
		/* 轮询间隔：一秒一次，几十个文件的 stat 量级可忽略 */
		constexpr float kPollInterval = 1.0f;
		/* 文件刚写完的这段时间先不读，编辑器保存常分几笔写 */
		constexpr int kSettleMilliseconds = 150;

		double ElapsedMs(const std::chrono::steady_clock::time_point& start)
		{
			return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		}

		/* 读不到的文件记最小值：之后补建出来，时间对不上就算变化 */
		std::filesystem::file_time_type ReadStamp(const std::string& path)
		{
			std::error_code error;
			const auto stamp = std::filesystem::last_write_time(path, error);
			return error ? std::filesystem::file_time_type::min() : stamp;
		}

		/* 内嵌源码构造的实例没有文件，不参与热重载 */
		bool IsReloadable(const SharedPtr<DeviceShader>& shader)
		{
			return shader != nullptr && !shader->GetPath().empty() && !shader->GetDependencies().empty();
		}
	}

	void ShaderReloader::Tick(float delta_time)
	{
		if (!m_AutoReloadEnabled)
			return;

		m_PollTimer += delta_time;
		if (m_PollTimer >= kPollInterval)
		{
			m_PollTimer = 0.0f;
			Poll();
		}

		/* 一次只编一批：上一批换上之前，新发现的先排队 */
		if (!FinishBuild(false))
			return;

		if (!m_Pending.empty())
			StartBuild(true);
	}

	void ShaderReloader::ReloadChanged()
	{
		/* 在编的那批读的是开编时的内容，先换上；之后又改的下面这轮会再排上 */
		FinishBuild(true);

		Poll();
		if (!m_Pending.empty())
			StartBuild(false);
	}

	size_t ShaderReloader::ReloadAll()
	{
		FinishBuild(true);
		m_Pending.clear();

		std::vector<std::string> paths;
		for (const SharedPtr<DeviceShader>& shader : AssetManager::Instance().GetAll<DeviceShader>())
		{
			if (!IsReloadable(shader))
				continue;

			Watch& watch = m_Watches[shader->GetPath()];

			/* 内容跟已加载的一致就不编：只碰了文件或时间戳没跟上的，这里都能按字节判出来 */
			if (shader->ContentUnchanged())
			{
				watch.Dependencies = shader->GetDependencies();
				RecordStamps(watch);
				continue;
			}

			if (watch.Dependencies.empty())
				watch.Dependencies = shader->GetDependencies();
			RecordStamps(watch);
			paths.push_back(shader->GetPath());
		}

		std::vector<Target> targets;
		CollectTargets(paths, targets);
		if (targets.empty())
			return 0;

		const auto start_time = std::chrono::steady_clock::now();
		std::vector<Prepared> batch = BuildBatch(targets);
		return CommitBatch(batch, ElapsedMs(start_time));
	}

	void ShaderReloader::Poll()
	{
		const auto settle_edge = FileTime::clock::now() - std::chrono::milliseconds(kSettleMilliseconds);

		for (const SharedPtr<DeviceShader>& shader : AssetManager::Instance().GetAll<DeviceShader>())
		{
			if (!IsReloadable(shader))
				continue;

			const std::string& path = shader->GetPath();
			const auto [it, inserted] = m_Watches.try_emplace(path);
			Watch& watch = it->second;

			/* 第一次见到只记基线 */
			if (inserted)
			{
				watch.Dependencies = shader->GetDependencies();
				RecordStamps(watch);
				continue;
			}

			if (IsQueued(path))
				continue;

			bool changed = false;
			bool writing = false;
			for (const std::string& dependency : watch.Dependencies)
			{
				const FileTime stamp = ReadStamp(dependency);
				const auto recorded = watch.Stamps.find(dependency);
				if (recorded != watch.Stamps.end() && recorded->second == stamp)
					continue;

				changed = true;
				if (stamp != FileTime::min() && stamp > settle_edge)
					writing = true;
			}

			if (!changed || writing)
				continue;

			/* 只是碰了文件，或改回了已加载的内容：读一遍字节就能判定，别占编译也别换代 */
			if (shader->ContentUnchanged())
			{
				watch.Dependencies = shader->GetDependencies();
				RecordStamps(watch);
				continue;
			}

			m_Pending.push_back(path);
		}
	}

	void ShaderReloader::StartBuild(bool allow_async)
	{
		std::vector<std::string> paths(m_Pending.begin(), m_Pending.end());
		m_Pending.clear();

		/* 先记时间再开编：写成坏文件时不会每秒重试同一份内容，改好了时间变了自然会再来 */
		for (const std::string& path : paths)
			RecordStamps(m_Watches[path]);

		/* 一条路径可能挂着几个变体，各自带自己的 define 编 */
		std::vector<Target> targets;
		CollectTargets(paths, targets);
		if (targets.empty())
			return;

		if (!allow_async || Renderer::CurrentAPI() != RenderAPI::Metal)
		{
			const auto start_time = std::chrono::steady_clock::now();
			std::vector<Prepared> batch = BuildBatch(targets);
			CommitBatch(batch, ElapsedMs(start_time));
			return;
		}

		Build build;
		build.Targets = targets;
		build.StartTime = std::chrono::steady_clock::now();
		build.Result = std::async(std::launch::async,
			[targets]() { return BuildBatch(targets); });
		m_Building = std::move(build);
	}

	bool ShaderReloader::FinishBuild(bool wait)
	{
		if (!m_Building.has_value())
			return true;

		Build& build = *m_Building;
		if (!wait && build.Result.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
			return false;

		std::vector<Prepared> batch = build.Result.get();
		const double elapsed_ms = ElapsedMs(build.StartTime);
		m_Building.reset();

		CommitBatch(batch, elapsed_ms);
		return true;
	}

	void ShaderReloader::CollectTargets(const std::vector<std::string>& paths, std::vector<Target>& out) const
	{
		out.clear();
		if (paths.empty())
			return;

		/* 一份源码可能挂着几个变体：按路径把活着的实例都收进来，各自带自己的 define 重编 */
		for (const SharedPtr<DeviceShader>& shader : AssetManager::Instance().GetAll<DeviceShader>())
		{
			if (!IsReloadable(shader))
				continue;
			if (std::find(paths.begin(), paths.end(), shader->GetPath()) == paths.end())
				continue;
			out.push_back({ shader->GetPath(), shader->GetVariantDefines() });
		}
	}

	std::vector<ShaderReloader::Prepared> ShaderReloader::BuildBatch(const std::vector<Target>& targets)
	{
		std::vector<Prepared> batch;
		batch.reserve(targets.size());
		for (const Target& target : targets)
			batch.push_back({ target.Path, target.Defines,
				DeviceShader::PrepareReload(target.Path, target.Defines) });
		return batch;
	}

	size_t ShaderReloader::CommitBatch(std::vector<Prepared>& batch, double elapsed_ms)
	{
		/* 编译期间着色器可能已被清出缓存，按路径找活着的那个；同一路径的几个变体靠 define 区分 */
		std::unordered_map<std::string, std::vector<SharedPtr<DeviceShader>>> live;
		for (const SharedPtr<DeviceShader>& shader : AssetManager::Instance().GetAll<DeviceShader>())
		{
			if (shader != nullptr && !shader->GetPath().empty())
				live[shader->GetPath()].push_back(shader);
		}

		size_t reloaded = 0;
		for (Prepared& prepared : batch)
		{
			if (prepared.Shader == nullptr)
			{
				CORE_LOG_ERROR("[Shader] Reload failed, the previous version stays: {}", prepared.Path);
				continue;
			}

			/* 成败都换成这次读到的依赖表（换入会把它交换走，所以先记）：
			 * 编不过时新加的 include 也要接着盯，改好它才能触发下一次 */
			const auto watch = m_Watches.find(prepared.Path);
			if (watch != m_Watches.end())
				AdoptDependencies(watch->second, prepared.Shader->GetDependencies());

			const auto candidates = live.find(prepared.Path);
			if (candidates == live.end())
				continue;

			SharedPtr<DeviceShader> target;
			for (const SharedPtr<DeviceShader>& shader : candidates->second)
			{
				if (shader->GetVariantDefines() == prepared.Defines)
				{
					target = shader;
					break;
				}
			}
			if (target == nullptr)
				continue;

			const uint32_t generation_before = target->GetGeneration();
			if (!target->ApplyPrepared(*prepared.Shader))
				continue;

			if (target->GetGeneration() != generation_before)
			{
				++reloaded;
				CORE_LOG_INFO("[Shader] Reloaded {}", std::filesystem::path(prepared.Path).filename().string());
			}
		}

		if (reloaded > 0)
			CORE_LOG_INFO("[Shader] {} of {} reloaded in {:.1f} ms", reloaded, batch.size(), elapsed_ms);
		return reloaded;
	}

	void ShaderReloader::RecordStamps(Watch& watch) const
	{
		watch.Stamps.clear();
		for (const std::string& dependency : watch.Dependencies)
			watch.Stamps[dependency] = ReadStamp(dependency);
	}

	void ShaderReloader::AdoptDependencies(Watch& watch, const std::vector<std::string>& dependencies) const
	{
		if (dependencies.empty())
			return;

		std::unordered_map<std::string, FileTime> stamps;
		for (const std::string& dependency : dependencies)
		{
			const auto recorded = watch.Stamps.find(dependency);
			stamps[dependency] = recorded != watch.Stamps.end() ? recorded->second : ReadStamp(dependency);
		}
		watch.Dependencies = dependencies;
		watch.Stamps = std::move(stamps);
	}

	bool ShaderReloader::IsQueued(const std::string& path) const
	{
		if (std::find(m_Pending.begin(), m_Pending.end(), path) != m_Pending.end())
			return true;

		if (!m_Building.has_value())
			return false;

		return std::any_of(m_Building->Targets.begin(), m_Building->Targets.end(),
			[&path](const Target& target) { return target.Path == path; });
	}
}
