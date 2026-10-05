#pragma once
#include <vector>
#include "EditorIcons.h"
#include "Helios/Reflection/ComponentRegistry.h"

namespace Helios
{
	/* 实体预设：「新建实体」菜单的条目。一份预设 = 名字 + 图标 + 若干组件添加动作；新增预设只需
	 * 在注册表补一段，菜单不用动。 */
	struct EntityTemplateDesc
	{
		const char*           Name{ nullptr };   /* 预设名，同时用作新建实体的名字 */
		Icons::Id             Icon{ Icons::Id::Entity };   /* 「新建实体」下拉里的条目图标 */
		std::vector<AddFunc>  Components;        /* 依次添加到新实体上的组件 */
	};

	class EntityTemplateRegistry
	{
	public:
		static EntityTemplateRegistry& Instance();

		[[nodiscard]] const std::vector<EntityTemplateDesc>& All() const { return m_Templates; }

		void Add(EntityTemplateDesc desc) { m_Templates.emplace_back(std::move(desc)); }

	private:
		EntityTemplateRegistry() = default;

		std::vector<EntityTemplateDesc> m_Templates;
	};

	/* 链式注册构建器：以临时对象使用，末尾 .Register() 提交 */
	class EntityTemplateRegistrar
	{
	public:
		explicit EntityTemplateRegistrar(const char* name);

		/* 「新建实体」下拉里的条目图标（不设则用通用实体图标） */
		EntityTemplateRegistrar& Icon(Icons::Id icon);

		/* 追加一个组件（含需要构造参数的情形） */
		EntityTemplateRegistrar& Component(AddFunc add);

		void Register();

	private:
		EntityTemplateDesc m_Desc;
	};
}
