#pragma once
#include <string>
#include <vector>
#include "Helios/Script/Script.h"

namespace Helios
{
	/* 脚本类注册表：脚本库加载时往这里注册，卸载前整表清空。
	 * 与组件注册表分开两张表 —— 脚本类随库生灭，组件是编译期固定的。 */
	class HELIOS_API ScriptRegistry
	{
	public:
		static ScriptRegistry& Instance();

		void Register(ScriptClassDesc desc);
		[[nodiscard]] const ScriptClassDesc* Find(const std::string& name) const;
		[[nodiscard]] const std::vector<ScriptClassDesc>& All() const { return m_Classes; }

		/* 清空：卸载脚本库之前必须调用，表里的回调全在库里 */
		void Clear() { m_Classes.clear(); }

		/* 整表替换：加载新库失败时把备份放回去 */
		void Restore(std::vector<ScriptClassDesc> classes) { m_Classes = std::move(classes); }

	private:
		ScriptRegistry() = default;

		std::vector<ScriptClassDesc> m_Classes;
	};
}

/* 在脚本源文件末尾注册：字段列表可空。
 *
 *     class Rotator : public Helios::Script { float m_Speed = 90.0f; };
 *     HELIOS_SCRIPT(Rotator, HELIOS_SCRIPT_FIELD(&Rotator::m_Speed, "Speed", Helios::FieldType::Float))
 *
 * 展开成一个静态对象，库加载时（dlopen 的静态初始化）跑一次。 */
#define HELIOS_SCRIPT(Class, ...)                                                       \
	namespace                                                                           \
	{                                                                                   \
		struct Class##ScriptBootstrap                                                    \
		{                                                                                \
			Class##ScriptBootstrap()                                                     \
			{                                                                            \
				::Helios::ScriptRegistry::Instance().Register(                            \
					::Helios::MakeScriptClassDesc<Class>(#Class, { __VA_ARGS__ }));       \
			}                                                                            \
		} g_##Class##ScriptBootstrap;                                                    \
	}

#define HELIOS_SCRIPT_FIELD(Member, Name, Type) ::Helios::Detail::MakeScriptFieldTyped<Member>(Name, Type)

/* 枚举字段：面板画下拉，存档存序号 */
#define HELIOS_SCRIPT_FIELD_ENUM(Member, Name, EnumType) \
	::Helios::Detail::MakeScriptEnumField<Member, EnumType>(Name)

/* 资源引用：存资产路径，面板可拖入资源浏览器里的文件 */
#define HELIOS_SCRIPT_FIELD_ASSET(Member, Name) \
	::Helios::Detail::MakeScriptFieldTyped<Member>(Name, ::Helios::FieldType::AssetRef)

/* 实体引用：存实体名字，面板可拖入层级面板里的节点 */
#define HELIOS_SCRIPT_FIELD_ENTITY(Member, Name) \
	::Helios::Detail::MakeScriptFieldTyped<Member>(Name, ::Helios::FieldType::EntityRef)
