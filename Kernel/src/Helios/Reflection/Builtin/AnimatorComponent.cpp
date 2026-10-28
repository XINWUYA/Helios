#include "Pch.h"
#include "Helios/Reflection/ComponentRegistration.h"
#include "Helios/Scene/AnimationClip.h"
#include "Helios/Scene/Components.h"
#include "Helios/Scene/SceneCommon.h"
#include "Helios/Core/AssetManager.h"
#include <tinyxml2.h>

namespace Helios
{
	namespace
	{
		void SaveAnimator(tinyxml2::XMLElement* element, const void* component)
		{
			const auto& animator = *static_cast<const AnimatorComponent*>(component);
			if (animator.m_Clip != nullptr && !animator.m_Clip->GetPath().empty())
				element->SetAttribute("Clip", RELATIVE_PATH(animator.m_Clip->GetPath()).c_str());

			element->SetAttribute("Speed", animator.m_Speed);
			element->SetAttribute("Loop", animator.m_Loop);
			element->SetAttribute("AutoPlay", animator.m_AutoPlay);
		}

		void LoadAnimator(const tinyxml2::XMLElement* element, void* component)
		{
			auto& animator = *static_cast<AnimatorComponent*>(component);
			animator.m_Speed = element->FloatAttribute("Speed", 1.0f);
			animator.m_Loop = element->BoolAttribute("Loop", true);
			animator.m_AutoPlay = element->BoolAttribute("AutoPlay", false);

			const char* path = element->Attribute("Clip");
			if (path == nullptr || *path == '\0')
				return;

			/* 属性里存的是相对资产根的形态，入口即转绝对（与模型 / 材质同一规则） */
			animator.m_Clip = AssetManager::Instance().GetOrLoad<AnimationClip>(ABSOLUTE_PATH(path));
			if (animator.m_Clip == nullptr)
				CORE_LOG_WARN("Failed to load animation clip: {}.", path);
		}
	}

	void RegisterAnimatorComponent()
	{
		MakeRegistrar<AnimatorComponent>("Animator")
			.SaveExtra(&SaveAnimator)
			.LoadExtra(&LoadAnimator)
			.Register();
	}
}
