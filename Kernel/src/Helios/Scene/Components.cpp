#include "Pch.h"
#include "Components.h"
#include "Entity.h"
#include "Scene.h"
#include "SceneCommon.h"	/* ABSOLUTE_PATH */
#include <Helios/Core/AssetManager.h>
#include <Helios/VirtualDevice/DeviceShader.h>

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

	/* 内建草地材质：参数就是着色器里的默认值。没有可改的入口，整场共享一份 */
	SharedPtr<Material> GrassComponent::GetDefaultMaterial()
	{
		static const SharedPtr<Material> s_Default = []()
		{
			const auto shader = AssetManager::Instance().GetOrLoad<DeviceShader>(
				ABSOLUTE_PATH("Shaders/MaterialShaders/Grass.glsl"));
			return shader != nullptr ? Material::Create(shader) : nullptr;
		}();
		return s_Default;
	}

	void GrassComponent::OnAdded(const Scene& scene, Entity& entity)
	{
		(void)scene;
		(void)entity;

		/* 面板加进来的组件没有材质来源，先给内建外观，不然这片草地等于不存在 */
		if (m_MaterialSlot.pMaterial == nullptr)
			m_MaterialSlot.pMaterial = GetDefaultMaterial();
	}

	SharedPtr<Material> TerrainComponent::GetDefaultMaterial()
	{
		static const SharedPtr<Material> s_Default = []()
		{
			const auto shader = AssetManager::Instance().GetOrLoad<DeviceShader>(
				ABSOLUTE_PATH("Shaders/MaterialShaders/Terrain.glsl"));
			return shader != nullptr ? Material::Create(shader) : nullptr;
		}();
		return s_Default;
	}

	void TerrainComponent::OnAdded(const Scene& scene, Entity& entity)
	{
		(void)scene;
		(void)entity;

		/* 给一份自己的副本：高度图 / 范围这些参数是按地形来的，共用一份会互相覆盖 */
		if (m_MaterialSlot.pMaterial == nullptr || m_MaterialSlot.pMaterial == GetDefaultMaterial())
		{
			const SharedPtr<Material> source = m_MaterialSlot.pMaterial != nullptr
				? m_MaterialSlot.pMaterial : GetDefaultMaterial();
			m_MaterialSlot.pMaterial = source != nullptr ? source->Clone() : nullptr;
			m_MaterialSlot.SourceAssetPath.clear();
			m_MaterialSlot.IsInstance = false;	/* 不写进场景，加载后由高度场重新填 */
		}
	}
}
