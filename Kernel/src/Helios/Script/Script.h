#pragma once
#include <string>
#include <type_traits>
#include <vector>
#include "Helios/Common/Common.h"
#include "Helios/Common/Assert.h"          /* Entity / Components 里的 ASSERT 靠它 */
#include "Helios/Reflection/ComponentRegistry.h"
#include "Helios/Scene/Entity.h"
#include "Helios/Scene/Components.h"

namespace Helios
{
	class Scene;

	/* 脚本基类：实例只活在 Runtime 模式里，由 ScriptRuntime 统一创建和销毁。
	 * 脚本写自己的成员当参数，用 HELIOS_SCRIPT_FIELD 声明哪些能被面板编辑与存盘。 */
	class HELIOS_API Script
	{
	public:
		virtual ~Script() = default;

		virtual void OnCreate() {}
		virtual void OnUpdate(float) {}
		virtual void OnDestroy() {}

		[[nodiscard]] Entity GetEntity() const { return m_Entity; }
		[[nodiscard]] SharedPtr<Scene> GetScene() const { return m_Scene; }

		/* ---- 组件 ---- */
		template <typename T>
		T& GetComponent() { return m_Entity.GetComponent<T>(); }

		template <typename T>
		[[nodiscard]] bool HasComponent() const { return m_Entity.HasComponent<T>(); }

		template <typename T>
		T& AddComponent() { return m_Entity.AddComponent<T>(); }

		template <typename T>
		void RemoveComponent() { m_Entity.RemoveComponent<T>(); }

		/* ---- 同实体上的其他脚本 ---- */
		/* 按类型取：实例是本库里的对象，dynamic_cast 就能判，不用维护类名到类型的映射 */
		template <typename T>
		T* GetScript()
		{
			static_assert(std::is_base_of_v<Script, T>, "GetScript needs a Helios::Script type");

			if (!HasComponent<ScriptComponent>())
				return nullptr;

			for (ScriptSlot& slot : GetComponent<ScriptComponent>().m_Scripts)
			{
				if (slot.Instance == nullptr)
					continue;
				if (T* typed = dynamic_cast<T*>(slot.Instance))
					return typed;
			}
			return nullptr;
		}

		/* 按类名取：脚本之间互相不知道类型时用这个 */
		[[nodiscard]] Script* GetScriptByName(const std::string& class_name) const;

		/* ---- 时间 ---- */
		/* 上一帧步长与进 Runtime 以来的累计秒数 */
		[[nodiscard]] float GetDeltaTime() const;
		[[nodiscard]] float GetTime() const;

		/* ---- 场景 ---- */
		/* 按名字找第一个同名实体，找不到返回空句柄 */
		[[nodiscard]] Entity FindEntity(const std::string& name) const;
		Entity CreateEntity(const std::string& name);

		/* 按资产路径取贴图：同一个路径只加载一次，跨脚本共享。
		 * 路径可以直接用 HELIOS_SCRIPT_FIELD_ASSET 声明的参数读出来。 */
		DeviceTexture* LoadTexture(const std::string& asset_path);

		/* 销毁排到本帧末：OnUpdate 里删自己不会被遍历打断 */
		void DestroySelf();
		void Destroy(Entity entity);

	private:
		Entity m_Entity{};
		SharedPtr<Scene> m_Scene{};

		friend class ScriptRuntime;
	};

	/* ---- 脚本字段 ----
	 * 读写回调由脚本库生成，所以卸载库之前必须先清空注册表（否则回调指向已释放的内存）。 */

	using ScriptFieldGetFunc = void (*)(Script* script, void* out_value);
	using ScriptFieldSetFunc = void (*)(Script* script, const void* in_value);

	/* 枚举字段的名字表：取一次存下来（脱离脚本库，面板与存档都不依赖它） */
	using ScriptEnumNamesFunc = std::vector<std::string> (*)();

	struct ScriptFieldDesc
	{
		const char*           Name{ nullptr };
		FieldType             Type{ FieldType::Float };
		ScriptFieldGetFunc    Get{ nullptr };
		ScriptFieldSetFunc    Set{ nullptr };
		ScriptEnumNamesFunc   GetEnumNames{ nullptr };
	};

	/* 成员指针作模板参数：编译期常量，回调 lambda 才不用捕获（无捕获才能转函数指针）。
	 * 脚本字段直接写 HELIOS_SCRIPT_FIELD，不用关心这一层。 */
	namespace Detail
	{
		template <typename T> struct ScriptMemberOwner;
		template <typename C, typename M> struct ScriptMemberOwner<M C::*> { using Type = C; };

		template <typename T> struct ScriptMemberValue;
		template <typename C, typename M> struct ScriptMemberValue<M C::*> { using Type = M; };

		template <auto Member>
		ScriptFieldDesc MakeScriptFieldTyped(const char* name, FieldType type)
		{
			using T = typename ScriptMemberOwner<decltype(Member)>::Type;
			using M = typename ScriptMemberValue<decltype(Member)>::Type;

			static_assert(std::is_base_of_v<Script, T>, "Script field owner must derive from Helios::Script");

			ScriptFieldDesc field;
			field.Name = name;
			field.Type = type;
			field.Get = [](Script* script, void* out_value)
			{
				*static_cast<M*>(out_value) = static_cast<T*>(script)->*Member;
			};
			field.Set = [](Script* script, const void* in_value)
			{
				static_cast<T*>(script)->*Member = *static_cast<const M*>(in_value);
			};
			return field;
		}

		/* 枚举字段：值按 int 存，名字表从 magic_enum 取，面板据此画下拉 */
		template <auto Member, typename Enum>
		ScriptFieldDesc MakeScriptEnumField(const char* name)
		{
			static_assert(std::is_enum_v<Enum>, "Use HELIOS_SCRIPT_FIELD_ENUM with an enum type");

			ScriptFieldDesc field = MakeScriptFieldTyped<Member>(name, FieldType::Enum);
			field.GetEnumNames = []() -> std::vector<std::string> { return GetEnumNames<Enum>(); };
			return field;
		}
	}

	/* 脚本类：名字、工厂、字段表。Create / Destroy 都在脚本库里，保证 new 与 delete 同侧。 */
	struct ScriptClassDesc
	{
		const char*                  Name{ nullptr };
		Script*                    (*Create)(void){ nullptr };
		void                       (*Destroy)(Script* script){ nullptr };
		std::vector<ScriptFieldDesc> Fields;
	};

	template <typename T>
	ScriptClassDesc MakeScriptClassDesc(const char* name, std::vector<ScriptFieldDesc> fields = {})
	{
		static_assert(std::is_base_of_v<Script, T>, "Script class must derive from Helios::Script");
		static_assert(std::is_default_constructible_v<T>, "Script class must be default constructible");

		ScriptClassDesc desc;
		desc.Name = name;
		desc.Create = []() -> Script* { return new T(); };
		desc.Destroy = [](Script* script) { delete static_cast<T*>(script); };
		desc.Fields = std::move(fields);
		return desc;
	}

	/* 属性文本与实例字段互转：UI 按文本存参数，运行时灌进实例，两边共用这一对函数。
	 * 类型与字段不匹配时写入不做任何事，读出返回空串。 */
	HELIOS_API bool ApplyScriptPropertyText(Script& script, const ScriptFieldDesc& field, const std::string& text);
	HELIOS_API std::string ReadScriptPropertyText(Script& script, const ScriptFieldDesc& field);
}
