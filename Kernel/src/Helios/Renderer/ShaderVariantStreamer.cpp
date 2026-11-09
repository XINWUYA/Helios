#include "Pch.h"
#include "ShaderVariantStreamer.h"

#include <chrono>

#include "Helios/Core/AssetManager.h"
#include "Helios/Core/Logger.h"
#include "Helios/Renderer/Renderer.h"
#include "Helios/Renderer/ShaderVariants.h"
#include "Helios/Scene/SceneCommon.h"
#include "Helios/VirtualDevice/DeviceShader.h"

namespace Helios
{
	namespace
	{
		/* 后台同时跑几个：一个变体几百毫秒，开太多会跟主线程抢 CPU */
		constexpr size_t kMaxInFlight = 2;
	}

	ShaderVariantStreamer& ShaderVariantStreamer::Instance()
	{
		static ShaderVariantStreamer instance;
		return instance;
	}

	std::string ShaderVariantStreamer::MakeKey(const std::string& path, const ShaderVariantDefines& defines)
	{
		return path + "#" + std::to_string(ShaderVariantFingerprint(defines));
	}

	bool ShaderVariantStreamer::CanBuildAsync()
	{
		return AsyncEnabled() && Renderer::CurrentAPI() == RenderAPI::Metal;
	}

	bool ShaderVariantStreamer::AsyncEnabled()
	{
		static const bool enabled = []
		{
			const char* value = std::getenv("HELIOS_VARIANT_ASYNC");
			return value == nullptr || std::strcmp(value, "0") != 0;
		}();
		return enabled;
	}

	SharedPtr<DeviceShader> ShaderVariantStreamer::BuildCpu(const Item& item)
	{
		/* 只建 CPU 侧：源码转换与反射都在后台跑，设备对象回主线程 */
		try
		{
			return DeviceShader::Create(item.Path, item.Defines);
		}
		catch (...)
		{
			return nullptr;
		}
	}

	bool ShaderVariantStreamer::Publish(const Item& item, const SharedPtr<DeviceShader>& shader)
	{
		if (shader == nullptr)
			return false;

		/* 设备对象在主线程建：与后台线程分工，退出时也就没有悬空依赖 */
		if (!shader->BuildDeviceObjects())
			return false;

		return AssetManager::Instance().Insert<DeviceShader>(item.Path,
			ShaderVariantConfig{ item.Defines }, shader);
	}

	bool ShaderVariantStreamer::Request(const std::string& path, const ShaderVariantDefines& defines)
	{
		if (path.empty())
			return false;

		if (AssetManager::Instance().Find<DeviceShader>(path, ShaderVariantConfig{ defines }) != nullptr)
			return true;

		const std::string key = MakeKey(path, defines);
		if (m_Queued.count(key) != 0 || m_Failed.count(key) != 0)
			return false;

		/* 严格模式不现编：清单外或者产物没带上，都是打包漏了，报一次就够了 */
		if (m_Strict)
		{
			if (m_Failed.insert(key).second)
			{
				CORE_LOG_ERROR("[Shader] {}：{} {}", m_Manifest.count(key) != 0
					? "清单里有但没产物" : "不在清单里", path, ShaderVariantDefinesToString(defines));
			}
			return false;
		}

		/* 同步档：当场编完再返回，调用方紧接着再查一次就能拿到精确变体 */
		if (!AsyncEnabled())
			return Preload(path, defines);
		m_Queued.insert(key);
		m_Pending.push_back(Item{ path, defines });
		return false;
	}

	bool ShaderVariantStreamer::Preload(const std::string& path, const ShaderVariantDefines& defines)
	{
		if (path.empty())
			return false;

		const Item item{ path, defines };
		if (AssetManager::Instance().Find<DeviceShader>(path, ShaderVariantConfig{ defines }) != nullptr)
			return true;

		return Publish(item, BuildCpu(item));
	}

	void ShaderVariantStreamer::SetStrictManifest(const std::vector<ShaderVariantItem>& items)
	{
		m_Manifest.clear();
		for (const ShaderVariantItem& item : items)
		{
			if (!item.Path.empty())
				m_Manifest.insert(MakeKey(ABSOLUTE_PATH(item.Path), item.Defines));
		}

		m_Strict = true;
		m_Failed.clear();
		CORE_LOG_INFO("[Shader] 变体清单生效：{} 条，缺失只报错不编译", m_Manifest.size());
	}

	void ShaderVariantStreamer::ClearStrictManifest()
	{
		m_Manifest.clear();
		m_Strict = false;
		m_Failed.clear();
	}

	size_t ShaderVariantStreamer::LoadStrictManifest(const std::string& filepath)
	{
		const std::vector<ShaderVariantItem> items = ShaderVariantCatalog::Load(filepath);
		if (items.empty())
		{
			CORE_LOG_WARN("[Shader] 变体清单为空或读不到：{}", filepath);
			return 0;
		}

		SetStrictManifest(items);
		return items.size();
	}

	void ShaderVariantStreamer::Tick()
	{
		/* 先收上一批：到点了就入库，没到就等下一帧 */
		for (auto it = m_InFlight.begin(); it != m_InFlight.end();)
		{
			if (it->second.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
			{
				++it;
				continue;
			}

			const Item item = it->first;
			SharedPtr<DeviceShader> shader = it->second.get();
			const std::string key = MakeKey(item.Path, item.Defines);
			m_Queued.erase(key);

			if (shader == nullptr)
			{
				m_Failed.insert(key);
				CORE_LOG_ERROR("[Shader] 变体编译失败，先用近似版本 {}", item.Path);
			}
			else if (Publish(item, shader))
			{
				++m_Completed;
			}

			it = m_InFlight.erase(it);
		}

		const bool async = CanBuildAsync();
		uint32_t budget = m_FrameBudget;
		while (budget-- > 0 && !m_Pending.empty())
		{
			if (async && m_InFlight.size() >= kMaxInFlight)
				break;

			const Item item = std::move(m_Pending.front());
			m_Pending.pop_front();

			if (async)
			{
				m_InFlight.emplace_back(item, std::async(std::launch::async, [item]()
					{
						return BuildCpu(item);
					}));
				continue;
			}

			/* 主线程编：GL 的上下文绑在这一条线程上，只能当场做完 */
			SharedPtr<DeviceShader> shader = BuildCpu(item);
			if (shader != nullptr && !shader->BuildDeviceObjects())
				shader = nullptr;
			const std::string key = MakeKey(item.Path, item.Defines);
			m_Queued.erase(key);
			if (shader == nullptr)
			{
				m_Failed.insert(key);
				continue;
			}
			if (AssetManager::Instance().Insert<DeviceShader>(
				item.Path, ShaderVariantConfig{ item.Defines }, shader))
			{
				++m_Completed;
			}
		}
	}
}
