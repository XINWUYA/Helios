#pragma once
#include <functional>
#include <string>

#include "Command/Command.h"
#include "ElementProperties.h"
#include "PreviewSelection.h"

namespace Helios::UIEditor
{
	/* 一次元素变换的历史：按 key（id + 先序下标）存取整套状态，撤销时不看当前选中是谁。
	 * 拖动过程本身不入栈，松手时把改动前 / 改动后一起交进来记一条。 */
	class ElementTransformCommand final : public ICommand
	{
	public:
		using ApplyFunc = std::function<bool(const PreviewSelection::ElementKey&,
			const PreviewSelection::ElementState&)>;

		ElementTransformCommand(PreviewSelection::ElementKey key, PreviewSelection::ElementState before,
			PreviewSelection::ElementState after, ApplyFunc apply);

		void Do() override;
		void Undo() override;
		/* 同一个元素的连续改动并成一条：拖到一半松手再接着拖也只留一条 */
		bool TryMerge(const ICommand& next) override;

		[[nodiscard]] const char* GetLabel() const override { return "Game UI element"; }
		/* 改的是 UI 文档里的元素，不是场景内容 */
		[[nodiscard]] bool AffectsSceneDocument() const override { return false; }

	private:
		PreviewSelection::ElementKey   m_Key;
		PreviewSelection::ElementState m_Before{};
		PreviewSelection::ElementState m_After{};
		ApplyFunc                     m_Apply;
	};

	/* 一次元素属性改动：inline 样式、元素属性、文本内容都走这一条。
	 * 属性描述表是静态的，存指针即可；撤销按 key 定位元素，不看当前选中。 */
	class ElementPropertyCommand final : public ICommand
	{
	public:
		using ApplyFunc = std::function<bool(const PreviewSelection::ElementKey&,
			const ElementPropertyDesc&, const std::string&)>;

		ElementPropertyCommand(PreviewSelection::ElementKey key, const ElementPropertyDesc& desc,
			std::string before, std::string after, ApplyFunc apply);

		void Do() override;
		void Undo() override;
		/* 同一元素同一条属性的连续输入并成一条 */
		bool TryMerge(const ICommand& next) override;

		[[nodiscard]] const char* GetLabel() const override { return "Game UI property"; }
		[[nodiscard]] bool AffectsSceneDocument() const override { return false; }

	private:
		PreviewSelection::ElementKey m_Key;
		const ElementPropertyDesc*   m_Desc{ nullptr };
		std::string                  m_Before{};
		std::string                  m_After{};
		ApplyFunc                    m_Apply;
	};

	/* 一份 UI 文本的整段改写：写回 .rcss、代码区存盘都算这一条。
	 * 存的是整段文本，撤销就是原样写回去，解析细节留给文件本身。 */
	class UITextCommand final : public ICommand
	{
	public:
		using WriteFunc = std::function<bool(const std::string& text)>;

		UITextCommand(const char* label, std::string before, std::string after, WriteFunc write);

		void Do() override;
		void Undo() override;
		/* 写盘失败：命令栈据此不记历史 */
		[[nodiscard]] bool DidFail() const override { return m_Failed; }

		[[nodiscard]] const char* GetLabel() const override { return m_Label; }
		[[nodiscard]] bool AffectsSceneDocument() const override { return false; }

	private:
		const char* m_Label;
		std::string m_Before;
		std::string m_After;
		WriteFunc   m_Write;
		bool        m_Failed{ false };
	};
}
