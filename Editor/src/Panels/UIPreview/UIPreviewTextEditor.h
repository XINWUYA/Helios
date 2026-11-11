#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

namespace Helios::UIEditor
{
	/* 面板右侧的代码列：编辑当前 .rml 或它配套的 .rcss。
	 * 只管缓冲与显示，读写文件和历史都归面板 —— 存盘要进撤销记录。 */
	class UIPreviewTextEditor final
	{
	public:
		/* 换文档：回到 Document 页并读盘 */
		void SetDocument(const std::string& document_path);

		[[nodiscard]] bool IsEditingStyle() const { return m_EditingStyle; }
		[[nodiscard]] bool IsDirty() const { return m_Text != m_TextSaved; }

		/* 当前编辑的文件（相对项目根）与磁盘上那份文本 */
		[[nodiscard]] std::string GetPath() const { return CurrentPath(); }
		[[nodiscard]] std::string GetStylePath() const { return StylePath(); }
		[[nodiscard]] const std::string& GetText() const { return m_Text; }
		[[nodiscard]] const std::string& GetSavedText() const { return m_TextSaved; }

		/* 磁盘上的文件变了：编辑缓冲没改动才跟着刷新 */
		void ReloadIfClean();

		/* 把一组声明写进一段 .rcss 文本里的 #id 块：纯函数，读写文件归面板。
		 * 找不到块 written = -1，否则给出改动的条数 */
		[[nodiscard]] static std::string PatchText(std::string source, const std::string& id,
			const std::vector<std::pair<std::string, std::string>>& declarations, int& written);

		/* 面板把新文本写到磁盘之后回灌缓冲：不算未保存改动 */
		void SetText(const std::string& text);

		/* 画代码列：位置与尺寸由面板排版给出。存盘统一在属性卡的保存入口，
		 * 这里只留 Document / Style 切换 */
		void Draw(const ImVec2& position, const ImVec2& size);

	private:
		void DrawToolbar();
		void DrawSource();
		void Load();
		[[nodiscard]] std::string CurrentPath() const;
		/* 文档同目录同名的 .rcss（不存在返回空） */
		[[nodiscard]] std::string StylePath() const;

		std::string m_DocumentPath{};
		/* 编辑的是文档本身还是配套样式 */
		bool m_EditingStyle{ false };
		/* 顶部「Document / Style」滑动切换的动画位置 */
		float m_KindSlide{ 0.0f };
		std::string m_Text{};
		std::string m_TextSaved{};
		std::filesystem::file_time_type m_TextStamp{};
	};
}
