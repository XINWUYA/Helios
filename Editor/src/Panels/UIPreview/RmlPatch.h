#pragma once
#include <string>

namespace Helios::UIEditor
{
	/* 写进 .rml 之前要先转义：文本与属性值共用这一套（& < > "） */
	std::string EscapeRmlText(const std::string& text);

	/* 按 id 改 rml 开标签里的某个属性，没有就补一条。
	 * 找到了元素返回 true（即使值没变），out_found 告诉调用方 id 在不在文档里。 */
	bool PatchRmlAttribute(std::string& text, const std::string& id, const std::string& name,
		const std::string& value, bool& out_found);

	/* 按 id 改元素的文本内容：只适用纯文本元素，自闭合标签写不了 */
	bool PatchRmlText(std::string& text, const std::string& id, const std::string& content,
		bool& out_found);
}
