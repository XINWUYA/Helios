#include "Pch.h"
#include "Helios/Reflection/ComponentRegistration.h"
#include "Helios/Scene/Components.h"
#include "Helios/Scene/SceneCommon.h"
#include "Helios/VirtualDevice/DeviceTexture.h"
#include <tinyxml2.h>

namespace Helios
{
	namespace
	{
		/* 探针的字段表之外的持久化内容：天空盒纹理与烘焙结果缓存路径。
		 * 对象为空时不写属性，读的时候也接受属性缺失。 */
		void SaveProbeExtra(tinyxml2::XMLElement* element, const void* component)
		{
			const auto& probe = static_cast<const ReflectionProbeComponent*>(component)->m_ReflectionProbe;
			if (probe == nullptr)
				return;

			const SharedPtr<DeviceTexture> skybox = probe->GetSkyBoxTexture();
			if (skybox != nullptr)
				element->SetAttribute("SkyBoxPath", RELATIVE_PATH(skybox->GetPath()).c_str());

			/* 缓存路径只在有烘焙结果时才落盘：否则场景会指向一份与当前状态不符的缓存 */
			const std::string& cache_path = probe->GetBakeCachePath();
			if (probe->IsBaked() && !cache_path.empty())
				element->SetAttribute("BakeCachePath", cache_path.c_str());
		}

		void LoadProbeExtra(const tinyxml2::XMLElement* element, void* component)
		{
			auto& probe = static_cast<ReflectionProbeComponent*>(component)->m_ReflectionProbe;
			if (probe == nullptr)
				return;

			/* 同上：天空盒路径按相对资产根存储，入口即转绝对 */
			if (const char* path = element->Attribute("SkyBoxPath"); path != nullptr && *path != '\0')
				probe->SetSkyBoxTexture(DeviceTexture::Create(ABSOLUTE_PATH(path)));

			/* 命中缓存就直接恢复烘焙结果：加载后无需再烘焙 */
			if (const char* cache_path = element->Attribute("BakeCachePath"); cache_path != nullptr && *cache_path != '\0')
			{
				probe->SetBakeCachePath(cache_path);
				if (!probe->LoadBakeCache(ABSOLUTE_PATH(cache_path)))
					CORE_LOG_WARN("ReflectionProbe: bake cache '{}' is unavailable, it will be baked again", cache_path);
			}
		}

		/* 探针为空时整个组件块不显示 */
		bool HasProbeObject(const void* component)
		{
			return static_cast<const ReflectionProbeComponent*>(component)->m_ReflectionProbe != nullptr;
		}

		/* 从天空贴图烘焙时不捕获场景：只跟场景捕获有关的字段（每帧重烤 / 层掩码）跟着隐藏 */
		bool CapturesScene(const void* component)
		{
			const auto& probe = static_cast<const ReflectionProbeComponent*>(component)->m_ReflectionProbe;
			return probe != nullptr && !probe->GetBakeConfig().BakeFromSkyTexture;
		}

		/* 分帧捕获只在「实时 + 捕获场景」下有意义：贴图烘焙一次做完，关掉实时也不分帧 */
		bool RealtimeSceneCapture(const void* component)
		{
			const auto& probe = static_cast<const ReflectionProbeComponent*>(component)->m_ReflectionProbe;
			return probe != nullptr && probe->NeedsRealtimeBake();
		}
	}

	void RegisterReflectionProbeComponent()
	{
		/* 反射探针：烘焙参数以结构体整体读写，故用配置访问器逐个字段暴露 */
		MakeRegistrar<ReflectionProbeComponent>("ReflectionProbe")
			.SaveExtra(&SaveProbeExtra)
			.LoadExtra(&LoadProbeExtra)
			/* 烘焙来源放最前：它决定后面哪些字段有意义（贴图烘焙 vs 捕获场景） */
			.Group("Capture")
			.Field(MakeConfigAccessor<&ReflectionProbeComponent::m_ReflectionProbe,
					&ReflectionProbe::GetBakeConfig, &ReflectionProbe::SetBakeConfig,
					&ReflectionProbe::BakeConfig::BakeFromSkyTexture>(),
				"BakeFromSkyTexture", FieldType::Bool)
			.Field(MakeAccessor<&ReflectionProbeComponent::m_ReflectionProbe,
					&ReflectionProbe::IsRealtime, &ReflectionProbe::SetRealtime>(),
				"Realtime", FieldType::Bool)
			.When(MakePredicateCondition(&CapturesScene))
			/* 实时捕获分帧：每帧只烘一面，省掉六分之五的场景绘制 */
			.Field(MakeAccessor<&ReflectionProbeComponent::m_ReflectionProbe,
					&ReflectionProbe::IsCaptureFacePerFrame, &ReflectionProbe::SetCaptureFacePerFrame>(),
				"CaptureFacePerFrame", FieldType::Bool)
			.When(MakePredicateCondition(&RealtimeSceneCapture))
			.Field(MakeAccessor<&ReflectionProbeComponent::m_ReflectionProbe,
					&ReflectionProbe::GetBoxSize, &ReflectionProbe::SetBoxSize>(),
				"BoxSize", FieldType::Vec3, 0.1f, 10.0f)
			.Field(MakeAccessor<&ReflectionProbeComponent::m_ReflectionProbe,
					&ReflectionProbe::GetBlendDistance, &ReflectionProbe::SetBlendDistance>(),
				"BlendDistance", FieldType::Float, 0.01f)
			/* 探针关心的层：烘焙只捕获这些层，视图也只在与自身可见层相交时使用它 */
			.Field(MakeAccessor<&ReflectionProbeComponent::m_ReflectionProbe,
					&ReflectionProbe::GetCullingMask, &ReflectionProbe::SetCullingMask>(),
				"CullingMask", FieldType::LayerMask)
			.When(MakePredicateCondition(&CapturesScene))
			.Group("Bake")
			.Field(MakeConfigAccessor<&ReflectionProbeComponent::m_ReflectionProbe,
					&ReflectionProbe::GetBakeConfig, &ReflectionProbe::SetBakeConfig,
					&ReflectionProbe::BakeConfig::EnvSize>(),
				"EnvSize", FieldType::Int)
			.Field(MakeConfigAccessor<&ReflectionProbeComponent::m_ReflectionProbe,
					&ReflectionProbe::GetBakeConfig, &ReflectionProbe::SetBakeConfig,
					&ReflectionProbe::BakeConfig::IrradianceSize>(),
				"IrradianceSize", FieldType::Int)
			.Field(MakeConfigAccessor<&ReflectionProbeComponent::m_ReflectionProbe,
					&ReflectionProbe::GetBakeConfig, &ReflectionProbe::SetBakeConfig,
					&ReflectionProbe::BakeConfig::PrefilterSize>(),
				"PrefilterSize", FieldType::Int)
			.Field(MakeConfigAccessor<&ReflectionProbeComponent::m_ReflectionProbe,
					&ReflectionProbe::GetBakeConfig, &ReflectionProbe::SetBakeConfig,
					&ReflectionProbe::BakeConfig::PrefilterMipLevels>(),
				"PrefilterMipLevels", FieldType::Int)
			.Field(MakeConfigAccessor<&ReflectionProbeComponent::m_ReflectionProbe,
					&ReflectionProbe::GetBakeConfig, &ReflectionProbe::SetBakeConfig,
					&ReflectionProbe::BakeConfig::BakeDiffuse>(),
				"BakeDiffuse", FieldType::Bool)
			.Field(MakeConfigAccessor<&ReflectionProbeComponent::m_ReflectionProbe,
					&ReflectionProbe::GetBakeConfig, &ReflectionProbe::SetBakeConfig,
					&ReflectionProbe::BakeConfig::BakeSpecular>(),
				"BakeSpecular", FieldType::Bool)
			.Visible(&HasProbeObject)
			.Register();
	}
}
