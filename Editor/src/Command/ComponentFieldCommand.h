#pragma once
#include <cstdint>
#include <string>
#include <typeindex>
#include <vector>
#include "Helios/Command/Command.h"
#include "Helios/Reflection/ComponentRegistry.h"
#include "Helios/Scene/Entity.h"

namespace Helios
{
	/* 修改组件字段的命令：按字段自己的取值方式（字节偏移或访问器）读写。属性面板每次字段编辑
	 * 生成一条、连续拖动合并后只留一条；持有的是字段描述副本而不是指针（不依赖注册表内部存储
	 * 的稳定性）。 */
	class ComponentFieldCommand final : public ICommand
	{
	public:
		/* 定长值字段：数值 / 向量 / 颜色 / 布尔 / 枚举 */
		ComponentFieldCommand(Entity entity, const ComponentDesc& desc, const FieldDesc& field,
		                      const void* before, const void* after);

		/* 文本字段：std::string 不能按字节拷贝 */
		ComponentFieldCommand(Entity entity, const ComponentDesc& desc, const FieldDesc& field,
		                      const std::string& before, const std::string& after);

		void Do() override;
		void Undo() override;

		/* 同一组件同一字段的连续改动可合并 */
		bool TryMerge(const ICommand& next) override;

		[[nodiscard]] const char* GetLabel() const override { return m_Label.c_str(); }

	private:
		void Write(const void* bytes, const std::string* text);
		/* 是否为同一组件类型的同一字段 */
		[[nodiscard]] bool IsSameField(const ComponentFieldCommand& other) const;

		Entity       m_Entity;
		std::type_index m_ComponentType;
		FieldDesc    m_Field{};
		HasFunc      m_Has{ nullptr };
		GetPtrFunc   m_GetPtr{ nullptr };

		std::vector<uint8_t> m_Before;
		std::vector<uint8_t> m_After;
		std::string m_BeforeText;
		std::string m_AfterText;
		bool m_IsText{ false };

		std::string m_Label;
	};
}
