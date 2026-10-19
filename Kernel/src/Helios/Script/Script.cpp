#include "Pch.h"
#include "Script.h"
#include "Helios/Scene/Scene.h"
#include "ScriptRuntime.h"

namespace Helios
{
	namespace
	{
		/* 贴图缓存：路径 → 已加载的贴图。静态放在引擎侧，脚本热重载不会把它清掉，
		 * 同一张图也不会因为多个脚本各要一次就反复上传。 */
		std::unordered_map<std::string, SharedPtr<DeviceTexture>>& TextureCache()
		{
			static std::unordered_map<std::string, SharedPtr<DeviceTexture>> s_Cache;
			return s_Cache;
		}
	}

	DeviceTexture* Script::LoadTexture(const std::string& asset_path)
	{
		if (asset_path.empty())
			return nullptr;

		auto& cache = TextureCache();
		const auto it = cache.find(asset_path);
		if (it != cache.end())
			return it->second.get();

		SharedPtr<DeviceTexture> texture = DeviceTexture::Create(asset_path);
		if (texture == nullptr)
		{
			CORE_LOG_ERROR("Script failed to load texture: {}.", asset_path);
			return nullptr;
		}

		return cache.emplace(asset_path, std::move(texture)).first->second.get();
	}

	Script* Script::GetScriptByName(const std::string& class_name) const
	{
		if (class_name.empty() || !m_Entity.HasComponent<ScriptComponent>())
			return nullptr;

		for (const ScriptSlot& slot : m_Entity.GetComponent<ScriptComponent>().m_Scripts)
		{
			if (slot.Instance != nullptr && slot.ClassName == class_name)
				return slot.Instance;
		}
		return nullptr;
	}

	float Script::GetDeltaTime() const
	{
		return m_Scene ? m_Scene->GetScriptRuntime().GetLastDeltaTime() : 0.0f;
	}

	float Script::GetTime() const
	{
		return m_Scene ? m_Scene->GetScriptRuntime().GetElapsedTime() : 0.0f;
	}

	Entity Script::FindEntity(const std::string& name) const
	{
		if (!m_Scene || name.empty())
			return {};

		const auto view = m_Scene->GetRegistry().view<NameComponent>();
		for (const entt::entity handle : view)
		{
			if (view.get<NameComponent>(handle).m_Name == name)
				return Entity{ handle, m_Scene };
		}
		return {};
	}

	Entity Script::CreateEntity(const std::string& name)
	{
		return m_Scene ? m_Scene->CreateEntity(name) : Entity{};
	}

	void Script::DestroySelf()
	{
		Destroy(m_Entity);
	}

	void Script::Destroy(Entity entity)
	{
		/* 走延迟销毁：一帧中间删实体不能当场删，见 Scene::QueueDestroy */
		if (m_Scene && entity)
			m_Scene->QueueDestroy(entity);
	}
}
