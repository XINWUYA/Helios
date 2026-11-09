#include "Pch.h"
#include "ScriptRuntime.h"
#include "Helios/Scene/Components.h"
#include "Helios/Scene/Scene.h"
#include "ScriptRegistry.h"

namespace Helios
{
	namespace
	{
		/* 字段缓冲：容得下 Bool/Int/Float/Vec2/Vec3/Vec4/Color，字符串单独走。 */
		constexpr size_t kPropertyBufferSize = 64;

		/* 热重载时按「实体 + 第几个挂载点」存下来的实例字段 */
		struct SavedInstance
		{
			entt::entity                Entity{ entt::null };
			size_t                      Slot{ 0 };
			std::string                 ClassName;
			std::vector<ScriptProperty> Properties;
		};

		/* 建实例的待办项：OnCreate 里可能改挂载点，先把名单收完再动手 */
		struct PendingInstance
		{
			entt::entity Entity{ entt::null };
			size_t       Slot{ 0 };
			std::string  ClassName;
			int          Order{ 0 };
		};
	}

	bool ApplyScriptPropertyText(Script& script, const ScriptFieldDesc& field, const std::string& text)
	{
		if (field.Set == nullptr || text.empty())
			return false;

		/* 字符串与引用类字段都是文本：资源存路径、实体存名字 */
		if (field.Type == FieldType::String || field.Type == FieldType::AssetRef
			|| field.Type == FieldType::EntityRef)
		{
			std::string value = text;
			field.Set(&script, &value);
			return true;
		}

		alignas(16) uint8_t buffer[kPropertyBufferSize] = {};
		try
		{
			switch (field.Type)
			{
			case FieldType::Bool:      *reinterpret_cast<bool*>(buffer)      = (text == "1" || text == "true"); break;
			case FieldType::Int:
			case FieldType::Enum:      *reinterpret_cast<int*>(buffer)       = std::stoi(text); break;
			case FieldType::Float:     *reinterpret_cast<float*>(buffer)     = std::stof(text); break;
			case FieldType::Vec2:      *reinterpret_cast<glm::vec2*>(buffer) = ToVec2(text); break;
			case FieldType::Vec3:      *reinterpret_cast<glm::vec3*>(buffer) = ToVec3(text); break;
			case FieldType::Vec4:
			case FieldType::Color:     *reinterpret_cast<glm::vec4*>(buffer) = ToVec4(text); break;
			case FieldType::LayerMask: *reinterpret_cast<LayerMask*>(buffer) = static_cast<LayerMask>(std::stoul(text)); break;
			default:                   return false;
			}
		}
		catch (const std::exception&)
		{
			/* 文本坏了就保留字段原值，不让一条脏属性挡住整个场景加载 */
			return false;
		}

		field.Set(&script, buffer);
		return true;
	}

	std::string ReadScriptPropertyText(Script& script, const ScriptFieldDesc& field)
	{
		if (field.Get == nullptr)
			return {};

		if (field.Type == FieldType::String || field.Type == FieldType::AssetRef
			|| field.Type == FieldType::EntityRef)
		{
			std::string value;
			field.Get(&script, &value);
			return value;
		}

		alignas(16) uint8_t buffer[kPropertyBufferSize] = {};
		field.Get(&script, buffer);

		switch (field.Type)
		{
		case FieldType::Bool:      return *reinterpret_cast<const bool*>(buffer) ? "1" : "0";
		case FieldType::Int:
		case FieldType::Enum:      return std::to_string(*reinterpret_cast<const int*>(buffer));
		case FieldType::Float:     return ToString(*reinterpret_cast<const float*>(buffer));
		case FieldType::Vec2:      return ToString(*reinterpret_cast<const glm::vec2*>(buffer));
		case FieldType::Vec3:      return ToString(*reinterpret_cast<const glm::vec3*>(buffer));
		case FieldType::Vec4:
		case FieldType::Color:     return ToString(*reinterpret_cast<const glm::vec4*>(buffer));
		case FieldType::LayerMask: return std::to_string(*reinterpret_cast<const LayerMask*>(buffer));
		default:                   return {};
		}
	}

	entt::entity ScriptRuntime::GetInstanceEntity(size_t index) const
	{
		return (index < m_Instances.size()) ? m_Instances[index].Entity : entt::null;
	}

	void ScriptRuntime::Start(Scene& scene)
	{
		if (m_bRunning)
			return;

		m_bRunning = true;
		m_pScene = &scene;

		std::vector<PendingInstance> pending;
		const auto view = scene.GetRegistry().view<ScriptComponent>();
		for (const entt::entity entity : view)
		{
			const ScriptComponent& component = view.get<ScriptComponent>(entity);
			for (size_t slot_index = 0; slot_index < component.m_Scripts.size(); ++slot_index)
			{
				const ScriptSlot& slot = component.m_Scripts[slot_index];
				pending.push_back({ entity, slot_index, slot.ClassName, slot.m_Order });
			}
		}

		/* 全局执行顺序：Order 小的先跑。同值用 stable_sort 保持收集顺序（实体句柄 + 挂载顺序）。 */
		std::stable_sort(pending.begin(), pending.end(),
			[](const PendingInstance& left, const PendingInstance& right) { return left.Order < right.Order; });

		for (const PendingInstance& item : pending)
		{
			if (!scene.GetRegistry().valid(item.Entity))
				continue;

			ScriptComponent* component = scene.GetRegistry().try_get<ScriptComponent>(item.Entity);
			if (component == nullptr || item.Slot >= component->m_Scripts.size())
				continue;

			ScriptSlot& slot = component->m_Scripts[item.Slot];
			/* 中途被删掉或被换了类：这一条不再成立 */
			if (slot.ClassName != item.ClassName)
				continue;

			CreateInstance(scene, item.Entity, item.Slot, slot);
		}
	}

	void ScriptRuntime::CreateInstance(Scene& scene, entt::entity entity, size_t slot_index, ScriptSlot& slot)
	{
		/* 重进 Runtime 时先清掉上一次残留的指针 */
		slot.Instance = nullptr;

		const ScriptClassDesc* desc = ScriptRegistry::Instance().Find(slot.ClassName);
		if (desc == nullptr || desc->Create == nullptr || desc->Destroy == nullptr)
		{
			if (!slot.ClassName.empty())
				CORE_LOG_WARN("Script class not found: {}.", slot.ClassName);
			return;
		}

		Script* object = desc->Create();
		if (object == nullptr)
			return;

		const SharedPtr<Scene> scene_ptr = scene.shared_from_this();
		object->m_Scene = scene_ptr;
		object->m_Entity = Entity{ entity, scene_ptr };

		/* 参数灌进实例：按当前字段表逐项匹配，名字对不上的（旧场景、改过名的字段）直接跳过 */
		for (const ScriptProperty& property : slot.Properties)
		{
			for (const ScriptFieldDesc& field : desc->Fields)
			{
				if (field.Name != nullptr && property.Name == field.Name)
				{
					ApplyScriptPropertyText(*object, field, property.Value);
					break;
				}
			}
		}

		slot.Instance = object;
		m_Instances.push_back({ entity, slot_index, object, desc->Destroy });
		++m_CreatedCount;

		/* OnCreate 里抛异常同样不能穿出去：记下来，这个实例不参与后续 tick */
		try
		{
			object->OnCreate();
		}
		catch (const std::exception& error)
		{
			RecordFault(m_Instances.back(), error.what());
		}
		catch (...)
		{
			RecordFault(m_Instances.back(), "unknown exception");
		}
	}

	/* 热重载：字段值按挂载点存下来，实例重建后按名字回灌。临时状态（计时器、缓存的引用）
	 * 本来就不指望跨库保留，只有 HELIOS_SCRIPT_FIELD 声明过的成员能活过来。 */
	bool ScriptRuntime::Reload(Scene& scene, const std::function<bool()>& swap_module)
	{
		if (!swap_module)
			return false;

		if (!m_bRunning)
			return swap_module();

		std::vector<SavedInstance> saved;
		for (const Instance& instance : m_Instances)
		{
			if (instance.Object == nullptr || !scene.GetRegistry().valid(instance.Entity))
				continue;

			const ScriptComponent* component = scene.GetRegistry().try_get<ScriptComponent>(instance.Entity);
			if (component == nullptr || instance.Slot >= component->m_Scripts.size())
				continue;

			const ScriptSlot& slot = component->m_Scripts[instance.Slot];
			SavedInstance entry;
			entry.Entity = instance.Entity;
			entry.Slot = instance.Slot;
			entry.ClassName = slot.ClassName;

			if (const ScriptClassDesc* desc = ScriptRegistry::Instance().Find(entry.ClassName))
			{
				for (const ScriptFieldDesc& field : desc->Fields)
					entry.Properties.emplace_back(field.Name, field.Type, ReadScriptPropertyText(*instance.Object, field));
			}
			saved.emplace_back(std::move(entry));
		}

		Stop();

		const bool swapped = swap_module();

		Start(scene);

		for (const SavedInstance& entry : saved)
		{
			if (!scene.GetRegistry().valid(entry.Entity))
				continue;

			ScriptComponent* component = scene.GetRegistry().try_get<ScriptComponent>(entry.Entity);
			if (component == nullptr || entry.Slot >= component->m_Scripts.size())
				continue;

			ScriptSlot& slot = component->m_Scripts[entry.Slot];
			if (slot.Instance == nullptr || slot.ClassName != entry.ClassName)
				continue;

			const ScriptClassDesc* desc = ScriptRegistry::Instance().Find(slot.ClassName);
			if (desc == nullptr)
				continue;

			for (const ScriptProperty& property : entry.Properties)
			{
				for (const ScriptFieldDesc& field : desc->Fields)
				{
					/* 回灌口径与着色器那边一致：同名同类型才带回来，类型变了当新字段处理。
					 * 文本值的大小由类型定，所以类型对上就够了 */
					if (property.Name == field.Name && property.Type == field.Type)
					{
						ApplyScriptPropertyText(*slot.Instance, field, property.Value);
						break;
					}
				}
			}
		}

		return swapped;
	}

	void ScriptRuntime::Tick(Scene& scene, float delta_time)
	{
		if (!m_bRunning)
			return;

		m_LastDeltaTime = delta_time;
		m_ElapsedTime += delta_time;

		for (Instance& instance : m_Instances)
		{
			if (instance.Object == nullptr || instance.Broken)
				continue;

			/* OnUpdate 里可能把实体或挂载点删了：逐个校验，别让实例操作已经没了的对象 */
			if (!scene.GetRegistry().valid(instance.Entity))
				continue;

			const ScriptComponent* component = scene.GetRegistry().try_get<ScriptComponent>(instance.Entity);
			if (component == nullptr || instance.Slot >= component->m_Scripts.size())
				continue;
			if (component->m_Scripts[instance.Slot].Instance != instance.Object)
				continue;

			/* 脚本自己抛的异常不能穿出场景推进：那会把编辑器整个带走。
			 * 捕获后停掉这个实例，编辑器会在下一帧暂停运行并提示 */
			try
			{
				instance.Object->OnUpdate(delta_time);
			}
			catch (const std::exception& error)
			{
				RecordFault(instance, error.what());
			}
			catch (...)
			{
				RecordFault(instance, "unknown exception");
			}
		}
	}

	void ScriptRuntime::Stop()
	{
		if (!m_bRunning)
			return;

		m_bRunning = false;

		/* 逆序销毁：后创建的先走，与构造顺序相反 */
		for (auto it = m_Instances.rbegin(); it != m_Instances.rend(); ++it)
		{
			DestroyInstance(*it);

			/* 挂载点还活着就把实例指针摘掉，编辑模式下没人该再碰它 */
			if (m_pScene != nullptr && m_pScene->GetRegistry().valid(it->Entity))
			{
				ScriptComponent* component = m_pScene->GetRegistry().try_get<ScriptComponent>(it->Entity);
				if (component != nullptr && it->Slot < component->m_Scripts.size())
					component->m_Scripts[it->Slot].Instance = nullptr;
			}
		}

		m_Instances.clear();
		m_pScene = nullptr;

		/* 上一次运行的异常不带到下一次 */
		m_Fault = Fault{};
		m_HasFault = false;
	}

	void ScriptRuntime::DestroyInstancesFor(entt::entity entity)
	{
		for (auto it = m_Instances.begin(); it != m_Instances.end();)
		{
			if (it->Entity != entity)
			{
				++it;
				continue;
			}

			DestroyInstance(*it);
			it = m_Instances.erase(it);
		}
	}

	void ScriptRuntime::DestroyInstance(Instance& instance)
	{
		if (instance.Object == nullptr)
			return;

		/* 收尾也不许抛：Stop 走在关编辑器 / 切场景的路径上，崩在这里会连带丢编辑内容 */
		try
		{
			instance.Object->OnDestroy();

			if (instance.Destroy != nullptr)
				instance.Destroy(instance.Object);
		}
		catch (const std::exception& error)
		{
			CORE_LOG_ERROR("Script threw while shutting down: {}.", error.what());
		}
		catch (...)
		{
			CORE_LOG_ERROR("Script threw while shutting down: unknown exception.");
		}

		instance.Object = nullptr;
	}

	/* 记异常：带上实体、槽位与类名，停掉这个实例，等编辑器取走 */
	void ScriptRuntime::RecordFault(Instance& instance, const char* message)
	{
		m_Fault = Fault{};
		m_Fault.Entity = instance.Entity;
		m_Fault.Slot = instance.Slot;

		if (m_pScene != nullptr && m_pScene->GetRegistry().valid(instance.Entity))
		{
			const ScriptComponent* component = m_pScene->GetRegistry().try_get<ScriptComponent>(instance.Entity);
			if (component != nullptr && instance.Slot < component->m_Scripts.size())
				m_Fault.ClassName = component->m_Scripts[instance.Slot].ClassName;
		}

		m_Fault.Message = (message != nullptr) ? message : std::string();
		m_HasFault = true;
		instance.Broken = true;

		CORE_LOG_ERROR("Script '{}' threw: {}. The instance stays stopped until play mode restarts.",
			m_Fault.ClassName, m_Fault.Message);
	}

	bool ScriptRuntime::ConsumeFault(Fault& out)
	{
		if (!m_HasFault)
			return false;

		out = std::move(m_Fault);
		m_Fault = Fault{};
		m_HasFault = false;
		return true;
	}
}
