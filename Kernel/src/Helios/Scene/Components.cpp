#include "Pch.h"
#include "Components.h"
#include "Entity.h"
#include "Scene.h"

namespace Helios
{
	
	/* ReflectionProbeComponent构造时注册到ReflectionProbeManager */
	void ReflectionProbeComponent::OnAdded(const Scene& scene, Entity& entity)
	{
		/* 探针没有实体之外的命名来源，用实体名标注它：既让烘焙的
		 * DebugGroup / 纹理名可读，也让缓存文件默认名与之对应。 */
		if (m_ReflectionProbe)
		{
			if (entity.HasComponent<NameComponent>())
				m_ReflectionProbe->SetDebugName(entity.GetComponent<NameComponent>().m_Name);

			if (auto* manager = scene.GetReflectionProbeManager().get())
				manager->RegisterProbe(m_ReflectionProbe);
		}
	}

	/* ReflectionProbeComponent移出时从ReflectionProbeManager中移除 */
	void ReflectionProbeComponent::OnRemoved(const Scene& scene, Entity& entity)
	{
		if (auto* manager = scene.GetReflectionProbeManager().get())
			if (m_ReflectionProbe)
				manager->UnregisterProbe(m_ReflectionProbe);
	}
}
