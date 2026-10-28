#include "Pch.h"
#include "ComponentEditors.h"
#include "ImGui/ImGuiExtensions.h"
#include "Helios/Reflection/ComponentRegistry.h"
#include "Helios/Scene/Components.h"
#include "Helios/VirtualDevice/DeviceTexture.h"

namespace Helios
{
	namespace
	{
		/* 贴图是资源引用，需要拖入与悬停预览，故整块自定义绘制；
		 * BaseColor 与 TilingFactor 走 schema 字段（见注册表）。
		 * 返回是否改动：换贴图要生成快照命令（撤销与场景脏标记都靠它）。 */
		bool DrawSpriteBlock(void* raw)
		{
			auto& component = *static_cast<SpriteComponent*>(raw);
			const SharedPtr<DeviceTexture> before = component.m_Texture;
			ImGuiExt::DrawTextureUI("Texture", component.m_Texture);
			return component.m_Texture != before;
		}
	}

	void RegisterSpriteComponentEditor()
	{
		ComponentRegistry::Instance().SetCustomDraw(typeid(SpriteComponent), &DrawSpriteBlock);
	}
}
