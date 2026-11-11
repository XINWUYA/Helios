#pragma once
#include <string>
#include <utility>
#include <vector>

namespace Helios
{
	namespace UIEditor
	{
		/* 在 .rcss 文本里改一条声明：找 `#id { ... }` 这一块，属性在就替换值，不在就插入。
		 * 只认单一 id 选择器的块；选择器是复合形式（`.btn:hover`、逗号并列）时不动文本，
		 * 通过 out_found = false 让调用方提示“无法定位”。
		 * 返回文本是否被改动。 */
		bool PatchCssDeclaration(std::string& text, const std::string& id,
			const std::string& property, const std::string& value, bool& out_found);

		/* 从 `#id { ... }` 里删掉一条声明。本来就没有也算达成（返回 false，out_found 为真）。
		 * 块不存在时 out_found = false。 */
		bool RemoveCssDeclaration(std::string& text, const std::string& id,
			const std::string& property, bool& out_found);

		/* 文档里还没有 `#id` 块时追加一块；已有就不动并返回 false。 */
		bool AppendCssBlock(std::string& text, const std::string& id,
			const std::vector<std::pair<std::string, std::string>>& declarations);
	}
}
