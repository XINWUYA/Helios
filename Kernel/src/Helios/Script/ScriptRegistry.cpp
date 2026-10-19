#include "Pch.h"
#include "ScriptRegistry.h"

namespace Helios
{
	ScriptRegistry& ScriptRegistry::Instance()
	{
		static ScriptRegistry s_Instance;
		return s_Instance;
	}

	void ScriptRegistry::Register(ScriptClassDesc desc)
	{
		if (desc.Name == nullptr || desc.Create == nullptr)
			return;

		/* 同名覆盖：热重载后重新注册同名类不该堆两份 */
		for (ScriptClassDesc& existing : m_Classes)
		{
			if (std::string(existing.Name) == desc.Name)
			{
				existing = std::move(desc);
				return;
			}
		}

		m_Classes.emplace_back(std::move(desc));
	}

	const ScriptClassDesc* ScriptRegistry::Find(const std::string& name) const
	{
		if (name.empty())
			return nullptr;

		for (const ScriptClassDesc& desc : m_Classes)
		{
			if (desc.Name != nullptr && name == desc.Name)
				return &desc;
		}
		return nullptr;
	}
}
