#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "Command.h"
#include "Helios/Common/Common.h"
#include "Helios/Scene/Entity.h"

namespace Helios
{
	struct ComponentDesc;
	struct FieldDesc;

	/* 把运行世界里调好的组件值搬回编辑世界。按字段表逐项复制，不整块 Capture / Restore ——
	 * 相机、光源、模型、草地这些组件身上挂着属于自己世界的实例，整块搬会让两份世界共用对象。
	 * 运行期删掉再新建会复用句柄，所以名字对不上就不认。 */
	class ApplyRuntimeStateCommand final : public ICommand
	{
	public:
		/* 采一份运行实体的字段值准备写进编辑实体。没有可搬的值时返回空，out_reason 说明原因 */
		static UniquePtr<ApplyRuntimeStateCommand> Create(Entity runtime_entity, Entity edit_entity,
			std::string& out_reason);

		void Do() override;
		void Undo() override;
		[[nodiscard]] const char* GetLabel() const override { return "Apply Runtime State"; }

		/* 会写过去的字段数：界面据此给提示 */
		[[nodiscard]] size_t GetFieldCount() const { return m_FieldCount; }

	private:
		struct FieldValue
		{
			const FieldDesc*     Field{ nullptr };
			std::vector<uint8_t> Before;
			std::vector<uint8_t> After;
		};
		struct ComponentValues
		{
			const ComponentDesc*     Desc{ nullptr };
			std::vector<FieldValue>  Fields;
		};

		void Apply(bool after);

		Entity m_EditEntity;
		std::vector<ComponentValues> m_Components;
		size_t m_FieldCount{ 0 };
	};
}
