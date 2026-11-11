#include "Pch.h"
#include "UIEditCommands.h"

namespace Helios::UIEditor
{
	ElementTransformCommand::ElementTransformCommand(PreviewSelection::ElementKey key,
		PreviewSelection::ElementState before, PreviewSelection::ElementState after, ApplyFunc apply)
		: m_Key(std::move(key)), m_Before(before), m_After(after), m_Apply(std::move(apply))
	{
	}

	void ElementTransformCommand::Do()
	{
		if (m_Apply)
			m_Apply(m_Key, m_After);
	}

	void ElementTransformCommand::Undo()
	{
		if (m_Apply)
			m_Apply(m_Key, m_Before);
	}

	bool ElementTransformCommand::TryMerge(const ICommand& next)
	{
		const auto* other = dynamic_cast<const ElementTransformCommand*>(&next);
		if (other == nullptr || other->m_Key.Id != m_Key.Id || other->m_Key.Index != m_Key.Index)
			return false;

		m_After = other->m_After;
		return true;
	}

	ElementPropertyCommand::ElementPropertyCommand(PreviewSelection::ElementKey key,
		const ElementPropertyDesc& desc, std::string before, std::string after, ApplyFunc apply)
		: m_Key(std::move(key)), m_Desc(&desc), m_Before(std::move(before)),
		m_After(std::move(after)), m_Apply(std::move(apply))
	{
	}

	void ElementPropertyCommand::Do()
	{
		if (m_Apply)
			m_Apply(m_Key, *m_Desc, m_After);
	}

	void ElementPropertyCommand::Undo()
	{
		if (m_Apply)
			m_Apply(m_Key, *m_Desc, m_Before);
	}

	bool ElementPropertyCommand::TryMerge(const ICommand& next)
	{
		const auto* other = dynamic_cast<const ElementPropertyCommand*>(&next);
		if (other == nullptr || other->m_Desc != m_Desc)
			return false;
		if (other->m_Key.Id != m_Key.Id || other->m_Key.Index != m_Key.Index)
			return false;

		m_After = other->m_After;
		return true;
	}

	UITextCommand::UITextCommand(const char* label, std::string before, std::string after, WriteFunc write)
		: m_Label(label), m_Before(std::move(before)), m_After(std::move(after)), m_Write(std::move(write))
	{
	}

	void UITextCommand::Do()
	{
		m_Failed = (m_Write == nullptr) || !m_Write(m_After);
	}

	void UITextCommand::Undo()
	{
		m_Failed = (m_Write == nullptr) || !m_Write(m_Before);
	}
}
