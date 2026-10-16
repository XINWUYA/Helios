#include "Pch.h"
#include "Helios/Reflection/ComponentRegistration.h"
#include "Helios/ImGui/ImGuiExtensions.h"
#include "Helios/Scene/Components.h"
#include "Helios/Scene/SceneCommon.h"
#include "Helios/VirtualDevice/DeviceTexture.h"
#include <tinyxml2.h>

namespace Helios
{
	namespace
	{
		/* 贴图是资源引用：字段表只认 POD 与字符串，路径读写单独处理；
		 * 对象为空时不写属性，读的时候也接受属性缺失。 */
		void SaveSpriteTexture(tinyxml2::XMLElement* element, const void* component)
		{
			const auto& texture = static_cast<const SpriteComponent*>(component)->m_Texture;
			if (texture != nullptr)
				element->SetAttribute("TexturePath", RELATIVE_PATH(texture->GetPath()).c_str());
		}

		void LoadSpriteTexture(const tinyxml2::XMLElement* element, void* component)
		{
			const char* path = element->Attribute("TexturePath");
			if (path != nullptr && *path != '\0')
				/* 属性里存的是相对资产根的形态，入口即转绝对（对象内部路径恒为绝对） */
				static_cast<SpriteComponent*>(component)->m_Texture = DeviceTexture::Create(ABSOLUTE_PATH(path));
		}

		/* 贴图是资源引用，需要拖入与悬停预览，故整块自定义绘制；
		 * BaseColor 与 TilingFactor 走 schema 字段（见下方注册）。
		 * 返回是否改动：换贴图要生成快照命令（撤销与场景脏标记都靠它）。 */
		bool DrawSpriteBlock(void* raw)
		{
			auto& component = *static_cast<SpriteComponent*>(raw);
			const SharedPtr<DeviceTexture> before = component.m_Texture;
			ImGuiExt::DrawTextureUI("Texture", component.m_Texture);
			return component.m_Texture != before;
		}
	}

	void RegisterSpriteComponent()
	{
		MakeRegistrar<SpriteComponent>("Sprite")
			.SaveExtra(&SaveSpriteTexture)
			.LoadExtra(&LoadSpriteTexture)
			.Field(&SpriteComponent::m_BaseColor, "BaseColor", FieldType::Color)
			.Field(&SpriteComponent::m_TilingFactor, "TilingFactor", FieldType::Float, 0.1f, 1.0f)
			/* 未设置贴图时 TilingFactor 无意义，条件不满足则不显示（值保留） */
			.When(FieldCondition{ 0, FieldType::Bool, ConditionOp::IsTrue, 0.0,
				[](const void* component)
				{
					return static_cast<const SpriteComponent*>(component)->m_Texture != nullptr;
				} })
			.CustomDraw(&DrawSpriteBlock)
			.Register();
	}
}
