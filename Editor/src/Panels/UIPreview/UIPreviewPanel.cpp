#include "Pch.h"
#include "UIPreviewPanel.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

#include "Common/EditorCommon.h"
#include "Common/EditorSettings.h"
#include "Helios/Application/ProjectConfig.h"
#include "Helios/Core/Input.h"
#include "Panels/PanelWindow.h"
#include "EditorUI/EditorIcons.h"
#include "EditorUI/EditorTheme.h"
#include "EditorUI/ImGuiExtensions.h"
#include "EditorUI/PanelChrome.h"
#include "EditorUI/PanelRegistry.h"
#include "RmlPatch.h"
#include "CssPatch.h"
#include "RmlPatch.h"
#include "UIEditCommands.h"

#include <RmlUi/Core.h>

namespace Helios::UIEditor
{
	namespace
	{
		/* 读一份文本资产（相对项目根）：写回样式与「自己写的那份」比对都用它 */
		std::string ReadTextFile(const std::string& relative_path)
		{
			std::ifstream file(g_ProjectPath / relative_path, std::ios::binary);
			if (!file)
				return {};
			return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		}

		/* 项目里的 .rml 清单：预览面板的下拉用。跳过缓存与脚本目录，按路径排序 */
		void CollectDocuments(std::vector<std::string>& out_documents)
		{
			namespace fs = std::filesystem;
			out_documents.clear();

			std::error_code error;
			for (fs::recursive_directory_iterator it(g_ProjectPath, fs::directory_options::skip_permission_denied, error), end;
				it != end; it.increment(error))
			{
				if (error)
				{
					error.clear();
					continue;
				}

				if (it->is_directory())
				{
					const std::string name = it->path().filename().string();
					if (name == "Cache" || name == "Scripts" || name == ".helios" || name == ".helios-trash")
						it.disable_recursion_pending();
					continue;
				}

				if (it->path().extension() == ".rml")
				{
					const std::filesystem::path relative = std::filesystem::relative(it->path(), g_ProjectPath, error);
					if (!error && !relative.empty())
						out_documents.push_back(relative.generic_string());
				}
			}

			std::sort(out_documents.begin(), out_documents.end());
		}
	}

	CommandStack& UIPreviewPanel::CurrentStack()
	{
		UniquePtr<CommandStack>& stack = m_Stacks[m_Path];
		if (stack == nullptr)
			stack = CreateUniquePtr<CommandStack>();
		return *stack;
	}

	const CommandStack* UIPreviewPanel::FindStack() const
	{
		const auto found = m_Stacks.find(m_Path);
		return found != m_Stacks.end() ? found->second.get() : nullptr;
	}

	bool UIPreviewPanel::Undo()
	{
		if (m_Path.empty() || !CurrentStack().Undo())
			return false;

		/* 撤销改了元素：面板上的数值跟着回读，下一次拖动才不从旧值起算 */
		SyncSelectionNumbers();
		m_Edited = true;
		return true;
	}

	bool UIPreviewPanel::Redo()
	{
		if (m_Path.empty() || !CurrentStack().Redo())
			return false;

		SyncSelectionNumbers();
		m_Edited = true;
		return true;
	}

	bool UIPreviewPanel::HasUnsavedChanges() const
	{
		return m_Edited || m_TextEditor.IsDirty();
	}

	bool UIPreviewPanel::CanUndo() const
	{
		const CommandStack* stack = FindStack();
		return stack != nullptr && stack->CanUndo();
	}

	bool UIPreviewPanel::CanRedo() const
	{
		const CommandStack* stack = FindStack();
		return stack != nullptr && stack->CanRedo();
	}

	const char* UIPreviewPanel::GetUndoLabel() const
	{
		const CommandStack* stack = FindStack();
		return stack != nullptr ? stack->GetUndoLabel() : nullptr;
	}

	const char* UIPreviewPanel::GetRedoLabel() const
	{
		const CommandStack* stack = FindStack();
		return stack != nullptr ? stack->GetRedoLabel() : nullptr;
	}

	/* 起手：记下选中元素与它此刻的整套状态，松手时跟新状态比一比 */
	void UIPreviewPanel::BeginGesture()
	{
		m_GestureKey = {};
		if (m_pCanvas == nullptr || !m_pCanvas->HasSelection())
			return;
		if (!m_pCanvas->GetSelectionState(m_GestureBefore))
			return;

		m_GestureKey.Id = m_pCanvas->GetSelectionId();
		m_GestureKey.Index = m_pCanvas->GetSelectionIndex();
	}

	void UIPreviewPanel::CommitGesture()
	{
		if (m_pCanvas == nullptr || (m_GestureKey.Id.empty() && m_GestureKey.Index < 0))
			return;

		const PreviewSelection::ElementKey key = m_GestureKey;
		m_GestureKey = {};

		PreviewSelection::ElementState after;
		if (m_pCanvas->GetSelectionId() != key.Id
			|| m_pCanvas->GetSelectionIndex() != key.Index
			|| !m_pCanvas->GetSelectionState(after))
			return;

		/* 没挪动就不记历史，免得空点一下也占一条撤销 */
		if (std::memcmp(&after, &m_GestureBefore, sizeof(after)) == 0)
			return;

		/* 撤销要能写到别的元素上，这里只交 key 和应用函数，不碰当前选中 */
		const PreviewSelection::ElementKey anchored = EnsureElementId(key);
		CurrentStack().Execute(CreateUniquePtr<ElementTransformCommand>(anchored, m_GestureBefore, after,
			[this, canvas = m_pCanvas.get()](const PreviewSelection::ElementKey& element_key,
				const PreviewSelection::ElementState& state)
			{
				if (!canvas->ApplyElementState(element_key, state))
					return false;

				MarkTransformDirty(element_key);
				return true;
			}));
	}

	void UIPreviewPanel::LogOperation(const char* action) const
	{
		/* 同时落一份到 <项目>/.helios/ui_preview.log：终端日志会被测试运行覆盖，
		 * 单独一份才查得清「刚才那一下到底写了什么」 */
		const auto append_to_file = [](const std::string& line)
		{
			std::error_code error;
			std::filesystem::create_directories(g_ProjectPath / ".helios", error);
			std::ofstream file(g_ProjectPath / ".helios" / "ui_preview.log",
				std::ios::app | std::ios::binary);
			if (file)
				file << line << "\n";
		};

		if (m_pCanvas == nullptr || !m_pCanvas->HasSelection())
		{
			EDITOR_LOG_INFO("UI preview: {} (no selection)", action);
			return;
		}

		float left = 0.0f;
		float top = 0.0f;
		m_pCanvas->GetSelectionPosition(left, top);
		float translate_x = 0.0f;
		float translate_y = 0.0f;
		m_pCanvas->GetSelectionTranslate(translate_x, translate_y);

		/* 再补一行几何：布局盒与复合后的四角。两行一对，
		 * 差值是变换产生的位移，能分辨「布局错了」还是「变换算错了」 */
		float box_x = 0.0f;
		float box_y = 0.0f;
		float box_width = 0.0f;
		float box_height = 0.0f;
		m_pCanvas->GetSelectionBox(box_x, box_y, box_width, box_height);

		float quad[4][2] = {};
		m_pCanvas->GetSelectionRenderQuad(quad);

		{
			char line[512];
			std::snprintf(line, sizeof(line),
				"layout #%s box (%.1f,%.1f) %.1fx%.1f quad (%.1f,%.1f) (%.1f,%.1f) (%.1f,%.1f) (%.1f,%.1f)",
				m_pCanvas->GetSelectionId().c_str(), static_cast<double>(box_x),
				static_cast<double>(box_y), static_cast<double>(box_width),
				static_cast<double>(box_height),
				static_cast<double>(quad[0][0]), static_cast<double>(quad[0][1]),
				static_cast<double>(quad[1][0]), static_cast<double>(quad[1][1]),
				static_cast<double>(quad[2][0]), static_cast<double>(quad[2][1]),
				static_cast<double>(quad[3][0]), static_cast<double>(quad[3][1]));
			append_to_file(std::string(action) + " " + line);
		}

		EDITOR_LOG_INFO("UI preview layout #{} box ({:.1f},{:.1f}) {:.1f}x{:.1f} quad "
			"({:.1f},{:.1f}) ({:.1f},{:.1f}) ({:.1f},{:.1f}) ({:.1f},{:.1f})",
			m_pCanvas->GetSelectionId(), box_x, box_y, box_width, box_height,
			static_cast<double>(quad[0][0]), static_cast<double>(quad[0][1]),
			static_cast<double>(quad[1][0]), static_cast<double>(quad[1][1]),
			static_cast<double>(quad[2][0]), static_cast<double>(quad[2][1]),
			static_cast<double>(quad[3][0]), static_cast<double>(quad[3][1]));

		EDITOR_LOG_INFO("UI preview: {} #{} left {:.1f} top {:.1f} translate {:.1f},{:.1f} "
			"rotate {:.1f} scale {:.2f} anchor {:.2f},{:.2f} ({})",
			action, m_pCanvas->GetSelectionId(), left, top, translate_x, translate_y,
			static_cast<double>(m_pCanvas->GetSelectionRotation()),
			static_cast<double>(m_pCanvas->GetSelectionScale()),
			static_cast<double>(m_pCanvas->GetAnchorX()),
			static_cast<double>(m_pCanvas->GetAnchorY()),
			m_pCanvas->IsSelectionStatic() ? "static" : "positioned");
	}

	/* 把选中元素的变换折成 .rcss 声明：写回样式、属性栏预览都按这一份 */
	UIPreviewPanel::DirtyEntry& UIPreviewPanel::DirtyFor(const PreviewSelection::ElementKey& key)
	{
		const std::string owner = key.Id + "#" + std::to_string(key.Index);
		DirtyEntry& entry = m_Dirty[owner];
		entry.Key = key;
		return entry;
	}

	/* 没有 id 的元素先分一个：写回样式与撤销都靠 id 定位，下标在文档重载后就不可靠了 */
	PreviewSelection::ElementKey UIPreviewPanel::EnsureElementId(const PreviewSelection::ElementKey& key)
	{
		if (!key.Id.empty() || m_pCanvas == nullptr)
			return key;

		const std::string tag = m_pCanvas->GetElementTag(key);
		const std::string stem = tag.empty() ? "element" : tag;
		for (int serial = 1; serial < 10000; ++serial)
		{
			const std::string candidate = stem + "-" + std::to_string(serial);
			if (m_pCanvas->HasElementId(candidate))
				continue;
			if (!m_pCanvas->SetElementId(key, candidate))
				return key;

			CORE_LOG_INFO("UI 元素没有 id，已分配 {}", candidate);

			PreviewSelection::ElementKey renamed = key;
			renamed.Id = candidate;
			/* 新 id 本身也要写回 .rml，不然重开文档又没了 */
			DirtyFor(renamed).Attributes.push_back("id");
			return renamed;
		}
		return key;
	}

	void UIPreviewPanel::MarkPropertyDirty(const PreviewSelection::ElementKey& key,
		const ElementPropertyDesc& desc, bool removed)
	{
		DirtyEntry& entry = DirtyFor(EnsureElementId(key));

		switch (desc.Kind)
		{
		case ElementPropertyKind::Style:
			entry.Style = true;
			if (removed
				&& std::find(entry.RemovedStyles.begin(), entry.RemovedStyles.end(), desc.Key)
					== entry.RemovedStyles.end())
				entry.RemovedStyles.push_back(desc.Key);
			break;

		case ElementPropertyKind::Attribute:
			if (std::find(entry.Attributes.begin(), entry.Attributes.end(), desc.Key) == entry.Attributes.end())
				entry.Attributes.push_back(desc.Key);
			break;

		case ElementPropertyKind::Text:
			entry.Text = true;
			break;
		}

		m_Edited = true;
	}

	void UIPreviewPanel::MarkTransformDirty(const PreviewSelection::ElementKey& key)
	{
		/* 变换写到元素上就是几条本地样式，保存时整份刷回即可 */
		DirtyFor(EnsureElementId(key)).Style = true;
		m_Edited = true;
	}

	/* 脏元素逐个写回：每个文件只拼一次、只写一次、只记一条历史。
	 * 任一文件写失败就保留脏标记，下次保存重试（重复写同一条声明是幂等的） */
	bool UIPreviewPanel::FlushDirty()
	{
		if (m_pCanvas == nullptr || m_Dirty.empty())
			return true;

		bool ok = true;

		/* ---- 样式：这次的改动都在同一份 .rcss 里 ---- */
		const std::string style_path = m_TextEditor.GetStylePath();
		if (!style_path.empty())
		{
			const std::string before = ReadTextFile(style_path);
			std::string text = before;
			int written = 0;

			for (const auto& [owner, entry] : m_Dirty)
			{
				/* 清空过的声明要从文件里删掉 */
				for (const std::string& name : entry.RemovedStyles)
				{
					bool found = false;
					if (RemoveCssDeclaration(text, entry.Key.Id, name, found))
						++written;
				}

				if (!entry.Style)
					continue;

				std::vector<std::pair<std::string, std::string>> locals;
				m_pCanvas->CollectElementStyles(entry.Key, locals);
				if (locals.empty())
					continue;

				/* 没有 #id 块就补一块：不然后面每条声明都无处可写 */
				bool block_found = false;
				for (const auto& [name, value] : locals)
				{
					bool found = false;
					if (PatchCssDeclaration(text, entry.Key.Id, name, value, found))
						++written;
					block_found = block_found || found;
				}
				if (!block_found && AppendCssBlock(text, entry.Key.Id, locals))
					++written;
			}

			if (written > 0 && text != before
				&& !ExecuteTextCommand("Game UI style", style_path, before, text))
			{
				SetStatusNote("样式写回失败，改动已保留");
				ok = false;
			}
		}

		/* ---- 文档：属性与文本同样一次成文 ---- */
		if (!m_Path.empty())
		{
			const std::string before = ReadTextFile(m_Path);
			std::string text = before;
			int written = 0;

			for (const auto& [owner, entry] : m_Dirty)
			{
				for (const std::string& name : entry.Attributes)
				{
					ElementPropertyDesc desc{};
					desc.Key = name.c_str();
					desc.Kind = ElementPropertyKind::Attribute;

					std::string value;
					if (!m_pCanvas->ReadElementPropertyValue(entry.Key, desc, value))
						continue;

					bool found = false;
					if (PatchRmlAttribute(text, entry.Key.Id, name, value, found))
						++written;
				}

				if (!entry.Text)
					continue;

				ElementPropertyDesc desc{};
				desc.Kind = ElementPropertyKind::Text;

				std::string value;
				if (!m_pCanvas->ReadElementPropertyValue(entry.Key, desc, value))
					continue;

				bool found = false;
				if (PatchRmlText(text, entry.Key.Id, value, found))
					++written;
			}

			if (written > 0 && text != before
				&& !ExecuteTextCommand("Game UI document", m_Path, before, text))
			{
				SetStatusNote("文档写回失败，改动已保留");
				ok = false;
			}
		}

		return ok;
	}

	bool UIPreviewPanel::SaveAll()
	{
		if (m_pCanvas == nullptr || !m_pCanvas->IsOpen())
			return false;

		/* 写 .rml 会让文档重载、元素换新：先记下选中，重载后按 id 选回来 */
		m_RestoreSelectionId = m_pCanvas->HasSelection() ? m_pCanvas->GetSelectionId() : std::string();

		bool ok = true;
		if (m_TextEditor.IsDirty() && !SaveText())
			ok = false;

		if (!FlushDirty())
			ok = false;

		if (!ok)
		{
			/* 写成功的那部分重试是幂等的，脏标记一律留着 */
			if (m_StatusNote.empty())
				SetStatusNote("有改动没写成，已保留待重试");
			return false;
		}

		m_Dirty.clear();
		m_Edited = false;
		return true;
	}

	void UIPreviewPanel::SetStatusNote(const char* note)
	{
		m_StatusNote = note != nullptr ? note : "";
		m_StatusNoteTime = 4.0f;
		CORE_LOG_WARN("UI 保存：{}", m_StatusNote);
	}

	/* 写 UI 文本文件的唯一入口：命令的 Do / Undo 都走这里，
	 * 顺带把缓冲和「自己写的内容」对齐，热重载回来时才分得清内外部改动 */
	bool UIPreviewPanel::WriteUITextFile(const std::string& path, const std::string& text)
	{
		if (path.empty())
			return false;

		std::ofstream file(g_ProjectPath / path, std::ios::binary | std::ios::trunc);
		if (!file)
		{
			CORE_LOG_ERROR("UI 文档保存失败：{}", path);
			return false;
		}

		file << text;
		file.close();

		m_WrittenText[path] = text;
		if (m_TextEditor.GetPath() == path)
			m_TextEditor.SetText(text);
		return true;
	}

	bool UIPreviewPanel::ExecuteTextCommand(const char* label, const std::string& path,
		const std::string& before, const std::string& after)
	{
		if (path.empty() || before == after)
			return true;

		return CurrentStack().Execute(CreateUniquePtr<UITextCommand>(label, before, after,
			[this, path](const std::string& text) { return WriteUITextFile(path, text); }));
	}

	bool UIPreviewPanel::SaveText()
	{
		if (!m_TextEditor.IsDirty())
			return true;

		const bool written = ExecuteTextCommand("Game UI text", m_TextEditor.GetPath(),
			m_TextEditor.GetSavedText(), m_TextEditor.GetText());
		if (written)
			CORE_LOG_INFO("UI 文档已保存：{}", m_TextEditor.GetPath());
		return written;
	}

	bool UIPreviewPanel::IsOwnWrite(const std::string& path) const
	{
		const auto found = m_WrittenText.find(path);
		if (found == m_WrittenText.end())
			return false;

		return ReadTextFile(path) == found->second;
	}

	void UIPreviewPanel::SaveSettings()
	{
		/* 读改写：这里只动 UIPreview 这一段，其它设置保持文件里的现值 */
		EditorSettings::State state = EditorSettings::Load();
		state.UIPreview.Document = m_Path;
		state.UIPreview.Resolution = m_Resolution;
		state.UIPreview.Guides = m_Guides;
		state.UIPreview.Editing = m_Editing;
		state.UIPreview.Tree = m_ShowTree;
		state.UIPreview.CodeFraction = m_CodeFraction;
		state.UIPreview.TreeFraction = m_TreeFraction;
		state.UIPreview.InspectorFraction = m_InspectorFraction;
		EditorSettings::Save(state);
	}

	void UIPreviewPanel::OpenDocument(const std::string& document_path)
	{
		if (m_pCanvas == nullptr)
			m_pCanvas = CreateUniquePtr<PreviewCanvas>();

		if (!m_pCanvas->Open(document_path))
		{
			CORE_LOG_WARN("UI preview failed: {}", m_pCanvas->GetError());
			m_FailedPath = document_path;
			std::error_code error;
			m_FailedStamp = std::filesystem::last_write_time(g_ProjectPath / document_path, error);
			return;
		}
		m_FailedPath.clear();
		m_Path = document_path;
		SaveSettings();
		/* 换了文档就把缩放与平移归位，免得新文档一进来就停在上一份的放大位置上 */
		m_Zoom = 1.0f;
		m_Pan = ImVec2(0.0f, 0.0f);
		/* 变换数值与手势属于上一份文档的元素；历史按文档分开存，切回来还在 */
		m_Rotate = 0.0f;
		m_Scale = 1.0f;
		m_HandleDrag = -1;
		m_PointerDown = false;
		m_GestureKey = {};
		/* 元素表、折叠状态、待写回清单与滚动位置都属于上一份文档 */
		m_Elements.clear();
		m_TreeCollapsed.clear();
		m_LastTreeIndex = -1;
		m_Edited = false;
		m_Dirty.clear();
		m_TextEditor.SetDocument(document_path);
	}

	/* 面板设置只读一次：分辨率、参考线、代码列这些。上次那份文档单独记着，
	 * 由 RestoreLastDocument 决定要不要打开 */
	void UIPreviewPanel::LoadSettingsOnce()
	{
		if (m_SettingsLoaded)
			return;
		m_SettingsLoaded = true;

		const EditorSettings::State state = EditorSettings::Load();
		m_Resolution = state.UIPreview.Resolution;
		m_Guides = state.UIPreview.Guides;
		m_Editing = state.UIPreview.Editing;
		m_ShowTree = state.UIPreview.Tree;
		m_CodeFraction = state.UIPreview.CodeFraction;
		m_TreeFraction = state.UIPreview.TreeFraction;
		m_InspectorFraction = state.UIPreview.InspectorFraction;
		m_LastDocument = state.UIPreview.Document;
	}

	void UIPreviewPanel::RestoreLastDocument()
	{
		/* 已经从资源浏览器 / Canvas 卡打开了某份文档时不再覆盖：
		 * 这里再开一次会把刚点的那份顶掉 */
		if (!m_Path.empty() || m_LastDocument.empty())
			return;

		if (std::find(m_Documents.begin(), m_Documents.end(), m_LastDocument) != m_Documents.end())
			OpenDocument(m_LastDocument);
	}

	/* 外部打开：绝对路径先折成相对项目根的写法，再走同一套加载 */
	void UIPreviewPanel::OpenDocumentAt(const std::string& path)
	{
		/* 先把设置读进来：OpenDocument 会 SaveSettings，用默认值写回去会把用户的
		 * 分辨率 / 参考线 / 代码列设置盖掉 */
		if (m_Documents.empty())
			CollectDocuments(m_Documents);
		LoadSettingsOnce();

		std::error_code error;
		std::filesystem::path relative = PathFromUtf8(path);
		if (relative.is_absolute())
			relative = std::filesystem::relative(relative, g_ProjectPath, error);

		if (error || relative.empty())
		{
			CORE_LOG_WARN("Game UI 打不开：{}", path);
			return;
		}

		const std::string document = relative.generic_string();

		/* 能不能开由文件本身说了算：下拉清单只服务下拉，不该挡住入口 */
		std::error_code exists_error;
		if (!std::filesystem::is_regular_file(g_ProjectPath / document, exists_error))
		{
			CORE_LOG_WARN("Game UI 找不到文档：{}", document);
			return;
		}

		/* 清单是上次扫的：新文件补进去，下拉里才有对应的项可选中 */
		if (std::find(m_Documents.begin(), m_Documents.end(), document) == m_Documents.end())
		{
			CollectDocuments(m_Documents);
			if (std::find(m_Documents.begin(), m_Documents.end(), document) == m_Documents.end())
			{
				m_Documents.push_back(document);
				std::sort(m_Documents.begin(), m_Documents.end());
			}
		}

		/* 同一份文档也要确认画布是开着的：面板收起时画布被关掉了，
		 * 这时再双击同一个文件不能什么都不做 */
		if (document != m_Path || m_pCanvas == nullptr || !m_pCanvas->IsOpen())
		{
			CORE_LOG_INFO("Game UI 打开文档：{}", document);
			OpenDocument(document);
		}
	}

	/* 两种状态的切换点：进交互态收掉编辑现场（拖拽、手柄、选框），
	 * 回设计态给控件发一次 leave，悬停样式不会僵在那里 */
	void UIPreviewPanel::EnterPreviewMode(bool preview)
	{
		if (m_PreviewMode == preview)
			return;

		m_PreviewMode = preview;
		m_PointerDown = false;

		if (m_pCanvas == nullptr)
			return;

		if (preview)
		{
			if (m_HandleDrag >= 0)
				m_pCanvas->EndSelectionDrag();
			m_pCanvas->ClearSelection();
		}
		else
		{
			m_pCanvas->ProcessMouseLeave();
		}

		m_HandleDrag = -1;
	}

	/* 预览面板：顶部选文档 + 重新载入，下面按比例铺满画面。
	 * 真实渲染在 Update 里（OnUpdate 阶段），这里只负责尺寸、交互与显示 */
	void UIPreviewPanel::OnImGuiRender()
	{
		if (!Panel::IsVisible(Panel::kGameUI))
		{
			/* 面板收起来了就不该再算「拿着焦点」，撤销交回场景 */
			m_Focused = false;
			return;
		}

		/* 面板自己排版：画面与代码按区域撑满，不要滚动条 */
		PanelWindow window(Panel::kGameUI, &Panel::VisibleFlag(Panel::kGameUI), ImGuiWindowFlags_NoScrollbar);
		if (!window)
		{
			m_Focused = false;
			return;
		}

		/* 每帧重算：没有文档可画时也要落到 false，撤销归属才不会卡在面板上 */
		m_Focused = false;

		if (m_Documents.empty())
			CollectDocuments(m_Documents);

		LoadSettingsOnce();
		RestoreLastDocument();

		/* 没有任何记录时自动挑一份，省一次点击。打不开的那份等它再存一次才重试，
		 * 否则坏文件会把日志刷满 */
		if (m_Path.empty() && !m_Documents.empty() && ShouldRetryOpen(m_Documents.front()))
			OpenDocument(m_Documents.front());

		static constexpr struct { const char* Label; uint32_t Width; uint32_t Height; } kResolutions[] = {
			{ "Fit panel", 0, 0 },
			{ "1280 x 720", 1280, 720 },
			{ "1920 x 1080", 1920, 1080 },
			{ "2560 x 1440", 2560, 1440 },
		};

		const ImGuiStyle& style = ImGui::GetStyle();
		const float icon_size = ImGui::GetFrameHeight();
		const float resolution_width = ImGui::CalcTextSize("2560 x 1440").x + icon_size * 2.6f;
		float reserved = ImGui::CalcTextSize("Reload").x + style.FramePadding.x * 2.0f
			+ ImGui::CalcTextSize("Rescan").x + style.FramePadding.x * 2.0f
			+ resolution_width + icon_size * 3.0f
			+ style.ItemSpacing.x * 6.0f;

		/* ---- 工具行：文档 / 重载 / 重扫（编辑区开关在没开时出现） ---- */
		ImGui::SetNextItemWidth(std::max(140.0f, ImGui::GetContentRegionAvail().x - reserved));
		if (ImGui::BeginCombo("##ui-preview-doc", m_Path.empty() ? "Select a .rml" : m_Path.c_str()))
		{
			for (const std::string& document : m_Documents)
			{
				if (ImGui::Selectable(document.c_str(), document == m_Path))
					OpenDocument(document);
			}
			ImGui::EndCombo();
		}

		ImGui::SameLine();
		if (ImGui::Button("Reload") && !m_Path.empty())
			OpenDocument(m_Path);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Reload the document from disk");

		ImGui::SameLine();
		if (ImGui::Button("Rescan"))
		{
			CollectDocuments(m_Documents);
			if (!m_Path.empty()
				&& std::find(m_Documents.begin(), m_Documents.end(), m_Path)
					== m_Documents.end())
				m_Path.clear();
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Rescan the project for .rml documents");

		/* 右端依次是：分辨率下拉、元素树开关、参考线开关、编辑区开关（最右） */
		const float right_edge = ImGui::GetWindowContentRegionMax().x;
		const float toggle_x = right_edge - icon_size;
		const float grid_x = toggle_x - style.ItemSpacing.x - icon_size;
		const float tree_x = grid_x - style.ItemSpacing.x - icon_size;
		const float resolution_x = tree_x - style.ItemSpacing.x - resolution_width;

		ImGui::SameLine(std::max(0.0f, resolution_x));
		ImGui::SetNextItemWidth(resolution_width);
		if (ImGui::BeginCombo("##ui-preview-resolution", kResolutions[m_Resolution].Label))
		{
			for (int index = 0; index < static_cast<int>(IM_ARRAYSIZE(kResolutions)); ++index)
			{
				if (ImGui::Selectable(kResolutions[index].Label, index == m_Resolution))
				{
					m_Resolution = index;
					SaveSettings();
				}
			}
			ImGui::EndCombo();
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Preview canvas size");

		ImGui::SameLine(std::max(0.0f, tree_x));
		if (Icons::IconButton(Icons::Id::LayerMask, ImVec2(icon_size, icon_size), m_ShowTree,
			m_ShowTree ? "Hide the hierarchy and properties" : "Show the hierarchy and properties"))
		{
			m_ShowTree = !m_ShowTree;
			SaveSettings();
		}

		ImGui::SameLine(std::max(0.0f, grid_x));
		if (Icons::IconButton(Icons::Id::Grid, ImVec2(icon_size, icon_size), m_Guides,
			m_Guides ? "Hide the guides" : "Show the guides"))
		{
			m_Guides = !m_Guides;
			SaveSettings();
		}

		ImGui::SameLine(std::max(0.0f, toggle_x));
		if (Icons::IconButton(m_Editing ? Icons::Id::CollapseSidebar : Icons::Id::ExpandSidebar,
			ImVec2(icon_size, icon_size), m_Editing,
			m_Editing ? "Hide the code editor" : "Show the code editor"))
		{
			m_Editing = !m_Editing;
			SaveSettings();
		}

		/* 让光标落到下一行：各列是按屏幕坐标自己摆的，压着工具行会叠在一起 */
		ImGui::Spacing();

		/* ---- 四列：元素树 / 预览 / 属性 / 代码 ---- */
		const ImVec2 content_min = ImGui::GetCursorScreenPos();
		const ImVec2 content_size = ImGui::GetContentRegionAvail();
		if (content_size.x <= 0.0f || content_size.y <= 0.0f)
			return;

		/* 每帧一拍：元素树与属性列共用同一份顺序（树收起时属性列也要拿到） */
		if (m_pCanvas != nullptr && m_pCanvas->IsOpen())
			m_pCanvas->CollectElements(m_Elements);
		else
			m_Elements.clear();

		/* 三条分隔条既画线也接拖拽：列宽可调 */
		const float splitter_width = 6.0f;

		/* 左列：元素树 */
		float tree_width = 0.0f;
		if (m_ShowTree)
		{
			m_TreeFraction = std::clamp(m_TreeFraction, 0.12f, 0.40f);
			tree_width = std::clamp(content_size.x * m_TreeFraction, 160.0f,
				std::max(160.0f, content_size.x * 0.45f));
		}

		const float left_span = m_ShowTree ? tree_width + splitter_width : 0.0f;

		/* 侧栏关掉时属性列一起收；有选中才出现，点画布空白取消选中也跟着收回 */
		const bool show_inspector = m_ShowTree && m_pCanvas != nullptr && m_pCanvas->HasSelection();
		const float right_splitters = (show_inspector ? splitter_width : 0.0f)
			+ (m_Editing ? splitter_width : 0.0f);

		/* 右列两块的预算：预览至少留 160，剩下的才分给属性和代码 */
		const float right_budget = std::max(0.0f,
			content_size.x - left_span - 160.0f - right_splitters);

		m_InspectorFraction = std::clamp(m_InspectorFraction, 0.16f, 0.45f);
		const float inspector_width = show_inspector
			? std::min(std::max(200.0f, content_size.x * m_InspectorFraction), right_budget)
			: 0.0f;

		float code_width = 0.0f;
		if (m_Editing)
		{
			const float code_budget = std::max(0.0f, right_budget - inspector_width);
			m_CodeFraction = std::clamp(m_CodeFraction, 0.20f, 0.65f);
			code_width = std::min(std::max(200.0f, content_size.x * m_CodeFraction), code_budget);
		}

		const float right_span = (inspector_width > 0.0f ? inspector_width + splitter_width : 0.0f)
			+ (m_Editing ? code_width + splitter_width : 0.0f);
		const ImVec2 preview_size(std::max(160.0f, content_size.x - left_span - right_span), content_size.y);
		const ImVec2 preview_min(content_min.x + left_span, content_min.y);
		const ImVec2 preview_max(preview_min.x + preview_size.x, preview_min.y + preview_size.y);
		const ImVec2 inspector_min(preview_max.x + splitter_width, content_min.y);

		/* 代码列紧跟在属性列后面；属性列收起来时它就贴到预览右边 */
		const float code_start_x = inspector_width > 0.0f
			? inspector_min.x + inspector_width + splitter_width
			: preview_max.x + splitter_width;
		const ImVec2 code_min(code_start_x, content_min.y);

		ImDrawList* draw_list = ImGui::GetWindowDrawList();
		draw_list->AddRectFilled(preview_min, preview_max,
			ImGui::GetColorU32(EditorTheme::Token::Neutral0), Scaled(6.0f));

		/* 三条竖分隔条同一套画法：拖它改相邻列的比例，松手才写盘 */
		const auto draw_splitter = [&](float x, const char* id, float& fraction, bool invert)
		{
			ImGui::SetCursorScreenPos(ImVec2(x, content_min.y));
			ImGui::InvisibleButton(id, ImVec2(splitter_width, content_size.y), ImGuiButtonFlags_MouseButtonLeft);
			const bool hovered = ImGui::IsItemHovered();
			const bool active = ImGui::IsItemActive();
			if (hovered || active)
				ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
			if (active)
			{
				const float step = ImGui::GetIO().MouseDelta.x / std::max(1.0f, content_size.x);
				fraction += invert ? -step : step;
			}
			/* 拖动过程中每帧写一次太频繁 */
			if (ImGui::IsItemDeactivated())
				SaveSettings();

			ImVec4 divider = EditorTheme::Token::Neutral7;
			divider.w = (hovered || active) ? 0.85f : 0.35f;
			draw_list->AddLine(ImVec2(x + splitter_width * 0.5f, content_min.y + 4.0f),
				ImVec2(x + splitter_width * 0.5f, content_min.y + content_size.y - 4.0f),
				ImGui::GetColorU32(divider), 1.0f);
		};

		/* 左列整列都是元素树 */
		if (m_ShowTree)
		{
			ImGui::SetCursorScreenPos(content_min);
			DrawElementTree(ImVec2(tree_width, content_size.y));
			draw_splitter(content_min.x + tree_width, "##ui-tree-splitter", m_TreeFraction, false);
		}

		/* 画面区域就是整列：分辨率与参考线已经移到上方工具行 */
		const ImVec2 image_area_min = preview_min;
		const ImVec2 image_area_size = preview_size;
		const ImVec2 image_area_max = preview_max;

		const bool has_document = m_pCanvas != nullptr && m_pCanvas->IsOpen();
		const bool has_texture = has_document && m_pCanvas->GetTexture() != nullptr;

		float source_aspect = 16.0f / 9.0f;
		if (m_Resolution > 0)
			source_aspect = static_cast<float>(kResolutions[m_Resolution].Width)
				/ static_cast<float>(kResolutions[m_Resolution].Height);
		else if (has_texture && m_pCanvas->GetHeight() > 0)
			source_aspect = static_cast<float>(m_pCanvas->GetWidth()) / static_cast<float>(m_pCanvas->GetHeight());

		ImVec2 image_size = image_area_size;
		if (image_size.x / image_size.y > source_aspect)
			image_size.x = image_size.y * source_aspect;
		else
			image_size.y = image_size.x / source_aspect;

		const ImVec2 image_min(image_area_min.x + (image_area_size.x - image_size.x) * 0.5f,
			image_area_min.y + (image_area_size.y - image_size.y) * 0.5f);

		/* 视图矩形 = 适配矩形按缩放放大，再按平移量偏移；平移量夹住不让画面跑出边框 */
		m_Zoom = std::clamp(m_Zoom, 0.25f, 6.0f);
		const ImVec2 view_size(image_size.x * m_Zoom, image_size.y * m_Zoom);
		const ImVec2 max_pan(std::max(0.0f, (view_size.x - image_area_size.x) * 0.5f),
			std::max(0.0f, (view_size.y - image_area_size.y) * 0.5f));
		m_Pan.x = std::clamp(m_Pan.x, -max_pan.x, max_pan.x);
		m_Pan.y = std::clamp(m_Pan.y, -max_pan.y, max_pan.y);
		const ImVec2 view_min(image_min.x + (image_size.x - view_size.x) * 0.5f + m_Pan.x,
			image_min.y + (image_size.y - view_size.y) * 0.5f + m_Pan.y);

		/* 跟随面板：按物理像素开目标（Retina 上差 2 倍，不放大就糊）；固定分辨率：按目标宽高渲 */
		if (m_Resolution > 0)
		{
			m_Width = kResolutions[m_Resolution].Width;
			m_Height = kResolutions[m_Resolution].Height;
			if (m_pCanvas != nullptr)
				m_pCanvas->SetDensityRatio(1.0f);
		}
		else
		{
			const float pixel_scale = ImGuiToPhysicalScale();
			m_Width = static_cast<uint32_t>(std::max(1.0f, image_size.x * pixel_scale));
			m_Height = static_cast<uint32_t>(std::max(1.0f, image_size.y * pixel_scale));
			if (m_pCanvas != nullptr)
				m_pCanvas->SetDensityRatio(pixel_scale);
		}

		if (has_texture)
		{
			/* 画面纹理压在最底下，顶栏图标和选框都得盖在它上面 */
			draw_list->ChannelsSplit(2);
			draw_list->ChannelsSetCurrent(1);

			/* 画面顶栏：最左是保存，接着是变换方式，右上角是预览开关 */
			const float overlay_icon = ImGui::GetFrameHeight();
			{
				ImVec2 button_min(preview_min.x + 6.0f, preview_min.y + 6.0f);

				/* 保存不属于变换，交互态下也留着 */
				const bool can_save = HasUnsavedChanges();
				ImGui::SetCursorScreenPos(button_min);
				ImGui::BeginDisabled(!can_save);
				if (Icons::IconButton(Icons::Id::Save, ImVec2(overlay_icon, overlay_icon), false,
					can_save ? "Save text, style and document" : "No unsaved changes"))
					SaveAll();
				ImGui::EndDisabled();
				button_min.x += overlay_icon + 6.0f;

				ImGui::BeginDisabled(m_PreviewMode);
				const Icons::Id modes[] = { Icons::Id::Translate, Icons::Id::Rotate, Icons::Id::Scale };
				const char* tips[] = { "Move the element", "Rotate the element", "Scale the element" };
				for (int index = 0; index < 3; ++index)
				{
					ImGui::SetCursorScreenPos(button_min);
					if (Icons::IconButton(modes[index], ImVec2(overlay_icon, overlay_icon),
						m_TransformMode == index, tips[index]))
						m_TransformMode = index;
					button_min.x += overlay_icon + 2.0f;
				}
				ImGui::EndDisabled();
			}

			const ImVec2 preview_button_min(preview_max.x - overlay_icon - 6.0f, preview_min.y + 6.0f);
			ImGui::SetCursorScreenPos(preview_button_min);
			if (Icons::IconButton(Icons::Id::Preview, ImVec2(overlay_icon, overlay_icon), m_PreviewMode,
				m_PreviewMode ? "Exit preview" : "Preview the UI"))
				EnterPreviewMode(!m_PreviewMode);

			/* 顶栏这一条压在画面上：点图标时不该再把事件传给底下的控件 */
			const bool over_overlay = ImGui::IsMouseHoveringRect(
				ImVec2(preview_min.x + 4.0f, preview_min.y + 4.0f),
				ImVec2(preview_max.x - 4.0f, preview_min.y + 8.0f + overlay_icon));

			/* 画面区先占一个不可见按钮：ImGui 默认「拖窗口空白就挪窗」，
			 * 不占住的话摇杆拖到一半会变成拖窗口。占满整块预览区，缩放后也能拖 */
			ImGui::SetCursorScreenPos(image_area_min);
			ImGui::InvisibleButton("##ui-preview-surface", image_area_size, ImGuiButtonFlags_MouseButtonLeft);
			const bool hovering = ImGui::IsItemHovered() && !over_overlay;
			m_Focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

			/* 中键拖动平移、Ctrl / ⌘ + 滚轮缩放（普通滚轮留给 UI 自己滚动） */
			const ImGuiIO& io = ImGui::GetIO();
			if (hovering && io.KeyCtrl && io.MouseWheel != 0.0f)
				m_Zoom *= std::pow(1.12f, io.MouseWheel);
			if (hovering && ImGui::IsMouseClicked(2))
				m_Panning = true;
			if (m_Panning)
			{
				m_Pan.x += io.MouseDelta.x;
				m_Pan.y += io.MouseDelta.y;
				if (!ImGui::IsMouseDown(2))
					m_Panning = false;
			}

			/* 纹理像素与屏幕像素的换算：缩放一起算进去，命中与描框才对得上 */
			const float to_tex_x = static_cast<float>(m_pCanvas->GetWidth())
				/ std::max(1.0f, image_size.x * m_Zoom);
			const float to_tex_y = static_cast<float>(m_pCanvas->GetHeight())
				/ std::max(1.0f, image_size.y * m_Zoom);
			const auto to_texture = [&](const ImVec2& position)
			{
				return ImVec2((position.x - view_min.x) * to_tex_x, (position.y - view_min.y) * to_tex_y);
			};

			draw_list->PushClipRect(image_area_min, image_area_max, true);
			draw_list->ChannelsSetCurrent(0);
			draw_list->AddImage((ImTextureID)m_pCanvas->GetTexture().get(), view_min,
				ImVec2(view_min.x + view_size.x, view_min.y + view_size.y), ImVec2(0, 1), ImVec2(1, 0));
			draw_list->ChannelsSetCurrent(1);

			/* 外框是「输入归谁」的提示：设计态中性灰，交互态运行绿。
			 * 缩放后设备边界可能出画，取可见部分描边，不会画到画面外面 */
			{
				const ImVec2 frame_min(std::max(view_min.x, image_area_min.x),
					std::max(view_min.y, image_area_min.y));
				const ImVec2 frame_max(std::min(view_min.x + view_size.x, image_area_max.x),
					std::min(view_min.y + view_size.y, image_area_max.y));
				if (frame_max.x > frame_min.x && frame_max.y > frame_min.y)
				{
					draw_list->AddRect(frame_min, frame_max, ImGui::GetColorU32(m_PreviewMode
						? EditorTheme::Token::UIFrameInteract : EditorTheme::Token::UIFrameDesign),
						0.0f, 0, 1.5f);
				}
			}

			/* 参考线：8dp 网格（太密就跳过）+ 安全区（90% / 80%）+ 中线。交互态整层不画 */
			if (m_Guides && !m_PreviewMode)
			{
				const float grid_step = 8.0f / to_tex_x;
				if (grid_step >= 4.0f)
				{
					const ImU32 grid_color = ImGui::GetColorU32(EditorTheme::Token::UIGuideGrid);
					for (float x = view_min.x; x <= view_min.x + view_size.x; x += grid_step)
						draw_list->AddLine(ImVec2(x, view_min.y), ImVec2(x, view_min.y + view_size.y), grid_color);
					for (float y = view_min.y; y <= view_min.y + view_size.y; y += grid_step)
						draw_list->AddLine(ImVec2(view_min.x, y), ImVec2(view_min.x + view_size.x, y), grid_color);
				}

				const ImU32 safe_color = ImGui::GetColorU32(EditorTheme::Token::UISafeArea);
				for (const float inset : { 0.05f, 0.10f })
				{
					draw_list->AddRect(
						ImVec2(view_min.x + view_size.x * inset, view_min.y + view_size.y * inset),
						ImVec2(view_min.x + view_size.x * (1.0f - inset), view_min.y + view_size.y * (1.0f - inset)),
						safe_color);
				}

				const ImU32 center_color = ImGui::GetColorU32(EditorTheme::Token::UIGuideAxis);
				draw_list->AddLine(ImVec2(view_min.x + view_size.x * 0.5f, view_min.y),
					ImVec2(view_min.x + view_size.x * 0.5f, view_min.y + view_size.y), center_color);
				draw_list->AddLine(ImVec2(view_min.x, view_min.y + view_size.y * 0.5f),
					ImVec2(view_min.x + view_size.x, view_min.y + view_size.y * 0.5f), center_color);
			}

			const ImVec2 mouse = io.MousePos;
			/* 手柄命中优先于控件：命中直接起手，不要求修饰键 */
			int grabbed_handle = -1;
			if (!m_PreviewMode && hovering && m_pCanvas->HasSelection())
			{
				{
					/* 与绘制用同一份复合四角，父级动过之后命中仍然准 */
					float hit_quad[4][2];
					m_pCanvas->GetSelectionRenderQuad(hit_quad);
					const ImVec2 corners[4] = {
						ImVec2(view_min.x + hit_quad[0][0] / to_tex_x, view_min.y + hit_quad[0][1] / to_tex_y),
						ImVec2(view_min.x + hit_quad[1][0] / to_tex_x, view_min.y + hit_quad[1][1] / to_tex_y),
						ImVec2(view_min.x + hit_quad[2][0] / to_tex_x, view_min.y + hit_quad[2][1] / to_tex_y),
						ImVec2(view_min.x + hit_quad[3][0] / to_tex_x, view_min.y + hit_quad[3][1] / to_tex_y),
					};
					const auto place = [&](const ImVec2& corner) { return corner; };

					ImVec2 handles[5];
					int handle_count = 0;
					if (m_TransformMode == 2)
					{
						for (int index = 0; index < 4; ++index)
							handles[handle_count++] = place(corners[index]);
					}
					else if (m_TransformMode == 1)
					{
						const ImVec2 top_middle((corners[0].x + corners[1].x) * 0.5f, (corners[0].y + corners[1].y) * 0.5f);
						const float radians = m_Rotate * 3.14159265f / 180.0f;
						handles[handle_count++] = ImVec2(top_middle.x + std::sin(radians) * 22.0f,
							top_middle.y - std::cos(radians) * 22.0f);
					}
					else
					{
						handles[handle_count++] = place(ImVec2((corners[0].x + corners[1].x) * 0.5f, corners[0].y));
						handles[handle_count++] = place(ImVec2(corners[1].x, (corners[1].y + corners[2].y) * 0.5f));
						handles[handle_count++] = place(ImVec2((corners[3].x + corners[2].x) * 0.5f, corners[2].y));
						handles[handle_count++] = place(ImVec2(corners[0].x, (corners[0].y + corners[3].y) * 0.5f));
					}

					for (int index = 0; index < handle_count; ++index)
					{
						if (std::abs(mouse.x - handles[index].x) <= 8.0f
							&& std::abs(mouse.y - handles[index].y) <= 8.0f)
						{
							grabbed_handle = index;
							break;
						}
					}

					/* 移动模式：框内按下也算抓住，拖动整块（不必非要点到边上的小手柄）。
					 * 用四角的包围矩形近似，旋转过之后可能略宽一点，够用 */
					if (grabbed_handle < 0 && m_TransformMode == 0)
					{
						float min_x = corners[0].x;
						float max_x = corners[0].x;
						float min_y = corners[0].y;
						float max_y = corners[0].y;
						for (const ImVec2& corner : corners)
						{
							min_x = std::min(min_x, corner.x);
							max_x = std::max(max_x, corner.x);
							min_y = std::min(min_y, corner.y);
							max_y = std::max(max_y, corner.y);
						}
						if (mouse.x >= min_x && mouse.x <= max_x && mouse.y >= min_y && mouse.y <= max_y)
							grabbed_handle = 0;
					}
				}
			}

			if (hovering && ImGui::IsMouseClicked(0) && grabbed_handle >= 0)
			{
				BeginGesture();
				m_pCanvas->BeginSelectionDrag();
				LogOperation("drag begin");
				m_HandleDrag = grabbed_handle;

				if (m_TransformMode == 2)
				{
					/* 缩放期间锚点保持中心，不要在这里改 transform-origin：
					 * 已经带旋转/缩放的元素换原点会瞬移，看起来像「一按就翻过去」。
					 * 对角锚定要连带位置补偿，等这套补偿算清楚再开 */
					m_pCanvas->SetSelectionAnchor(0.5f, 0.5f);
				}

				if (m_TransformMode == 1)
				{
					/* 绕「看得见的中心」转：元素被拖远之后，布局盒中心已经不是它的中心了，
					 * 拿布局盒去算角度会越转越偏 */
					float quad[4][2];
					m_pCanvas->GetSelectionRenderQuad(quad);
					const ImVec2 center(
						view_min.x + (quad[0][0] + quad[1][0] + quad[2][0] + quad[3][0]) * 0.25f / to_tex_x,
						view_min.y + (quad[0][1] + quad[1][1] + quad[2][1] + quad[3][1]) * 0.25f / to_tex_y);
					m_GrabAngle = std::atan2(mouse.y - center.y, mouse.x - center.x);
				}
			}
			else if (hovering && ImGui::IsMouseClicked(0))
			{
				const ImVec2 local = to_texture(mouse);

				if (m_PreviewMode)
				{
					/* 交互态：按下直接给控件，选中与选框都不参与 */
					m_pCanvas->ProcessMouseMove(local.x, local.y);
					m_pCanvas->ProcessMouseButtonDown(0);
					m_PointerDown = true;
				}
				else if (m_pCanvas->SelectAt(local.x, local.y))
				{
					/* 设计态：左键只用来选中元素，一个事件都不往控件里发 */
					LogOperation("select");
					SyncSelectionNumbers();
				}
			}

			if (!m_PreviewMode && m_HandleDrag >= 0)
			{
				if (m_TransformMode == 1)
				{
					/* 按「指针相对元素中心的角度差」旋转：绕多少圈都能转，柄也始终在手边 */
					float quad[4][2];
					m_pCanvas->GetSelectionRenderQuad(quad);
					{
						const ImVec2 center(
							view_min.x + (quad[0][0] + quad[1][0] + quad[2][0] + quad[3][0]) * 0.25f / to_tex_x,
							view_min.y + (quad[0][1] + quad[1][1] + quad[2][1] + quad[3][1]) * 0.25f / to_tex_y);
						const float angle = std::atan2(mouse.y - center.y, mouse.x - center.x);

						/* 逐帧累加，角度差先收进 (-π, π]：指针跨过 ±π 那条线时，
						 * 直接相减会拿到 ±360°，元素会突然翻一圈 */
						const float delta = std::remainder(angle - m_GrabAngle, 6.28318531f);
						m_Rotate += delta * 57.29578f;
						m_GrabAngle = angle;

						m_pCanvas->SetSelectionTransform(m_Rotate, m_Scale);
						m_pCanvas->SetSelectionTransformState(m_Rotate, m_Scale);
					}
				}
				else if (m_TransformMode == 2)
				{
					/* 左侧两个角（0 = 左上、3 = 左下）向外拉是负的 dx，方向要反过来，
					 * 才能做到「往外拉 = 放大」 */
					const float direction = (m_HandleDrag == 0 || m_HandleDrag == 3)
						? -1.0f : 1.0f;
					m_Scale = std::clamp(
						m_Scale * (1.0f + direction * io.MouseDelta.x * 0.004f), 0.1f, 8.0f);
					m_pCanvas->SetSelectionTransform(m_Rotate, m_Scale);
				}
				else
				{
					m_pCanvas->DragSelection(io.MouseDelta.x * to_tex_x, io.MouseDelta.y * to_tex_y);
				}

				if (ImGui::IsMouseReleased(0))
				{
					m_pCanvas->EndSelectionDrag();
					CommitGesture();
					LogOperation("drag end");
					m_HandleDrag = -1;
				}
			}

			/* 指针只喂交互态：设计态一个事件都不发给控件，命中走自己的 SelectAt / HitTest */
			if (m_PreviewMode)
			{
				const ImVec2 local = to_texture(mouse);
				if (m_PointerDown)
				{
					m_pCanvas->ProcessMouseMove(local.x, local.y);
					if (ImGui::IsMouseReleased(0))
					{
						m_pCanvas->ProcessMouseButtonUp(0);
						m_PointerDown = false;
					}
				}
				else if (hovering)
				{
					m_pCanvas->ProcessMouseMove(local.x, local.y);
				}
				else
				{
					m_pCanvas->ProcessMouseLeave();
				}

				if (hovering && !io.KeyCtrl && io.MouseWheel != 0.0f)
					m_pCanvas->ProcessMouseWheel(io.MouseWheel);
			}

			/* 元素检查：光标下的元素描框并给一条信息，超出父级用警示色。预览态不画 */
			if (!m_PreviewMode && hovering)
			{
				const ImVec2 local = to_texture(mouse);
				ElementHit hit;
				/* 悬停的正好是已选中的元素时不再画 hover 框：会和选中的框叠成两层 */
				if (m_pCanvas->HitTest(local.x, local.y, hit) && hit.Id != m_pCanvas->GetSelectionId())
				{
					/* 边框按渲染四边形画：元素被平移/旋转过之后，布局盒已经不在画面上那个位置了 */
					const ImVec2 quad[4] = {
						ImVec2(view_min.x + hit.Corners[0][0] / to_tex_x, view_min.y + hit.Corners[0][1] / to_tex_y),
						ImVec2(view_min.x + hit.Corners[1][0] / to_tex_x, view_min.y + hit.Corners[1][1] / to_tex_y),
						ImVec2(view_min.x + hit.Corners[2][0] / to_tex_x, view_min.y + hit.Corners[2][1] / to_tex_y),
						ImVec2(view_min.x + hit.Corners[3][0] / to_tex_x, view_min.y + hit.Corners[3][1] / to_tex_y),
					};
					const bool overflow = hit.OverflowX || hit.OverflowY;
					const ImU32 color = ImGui::GetColorU32(overflow
						? EditorTheme::Token::Warning : EditorTheme::Token::Accent);
					draw_list->AddQuad(quad[0], quad[1], quad[2], quad[3], color, 1.5f);

					/* 标签里的坐标也用渲染后的左上角，和画面看到的位置对得上 */
					const float tex_x = std::min(std::min(quad[0].x, quad[1].x), std::min(quad[2].x, quad[3].x));
					const float tex_y = std::min(std::min(quad[0].y, quad[1].y), std::min(quad[2].y, quad[3].y));

					char label[192];
					std::snprintf(label, sizeof(label), "%s%s%s%s  %.0f x %.0f  (%.0f, %.0f)",
						hit.Id.empty() ? "" : "#", hit.Id.c_str(),
						hit.ClassName.empty() ? "" : "  .",
						hit.ClassName.empty() ? "" : hit.ClassName.c_str(),
						static_cast<double>(hit.Width), static_cast<double>(hit.Height),
						static_cast<double>((tex_x - view_min.x) * to_tex_x),
						static_cast<double>((tex_y - view_min.y) * to_tex_y));

					const ImVec2 text_size = ImGui::CalcTextSize(label);
					ImVec2 label_pos(mouse.x + 14.0f, mouse.y + 18.0f);
					label_pos.x = std::min(label_pos.x, image_area_max.x - text_size.x - 8.0f);
					label_pos.y = std::min(label_pos.y, image_area_max.y - text_size.y - 6.0f);
					const ImVec2 background_min(label_pos.x - 6.0f, label_pos.y - 4.0f);
					const ImVec2 background_max(label_pos.x + text_size.x + 6.0f, label_pos.y + text_size.y + 4.0f);
					ImVec4 background = EditorTheme::Token::Neutral0;
					background.w = 0.92f;
					draw_list->AddRectFilled(background_min, background_max, ImGui::GetColorU32(background), 4.0f);
					draw_list->AddRect(background_min, background_max, color, 4.0f);
					draw_list->AddText(label_pos, ImGui::GetColorU32(ImGuiCol_Text), label);
				}
			}

			/* 状态贴在画面左下角：元素数常驻，有缺失资源或警告就顶替成那一句 */
			{
				const std::vector<std::string>& missing = m_pCanvas->GetMissingFiles();
				const std::string& message = m_pCanvas->GetLastMessage();

				/* 保存失败的提示优先占这一行，几秒后自己退回 */
				if (m_StatusNoteTime > 0.0f)
					m_StatusNoteTime -= ImGui::GetIO().DeltaTime;

				char status[192];
				bool warning = false;
				if (!m_StatusNote.empty() && m_StatusNoteTime > 0.0f)
				{
					std::snprintf(status, sizeof(status), "%s", m_StatusNote.c_str());
					warning = true;
				}
				else if (m_pCanvas->HasSelection())
				{
					float x = 0.0f;
					float y = 0.0f;
					float width = 0.0f;
					float height = 0.0f;
					m_pCanvas->GetSelectionBox(x, y, width, height);
					const std::string id = m_pCanvas->GetSelectionId();
					float left = 0.0f;
					float top = 0.0f;
					m_pCanvas->GetSelectionPosition(left, top);
					std::snprintf(status, sizeof(status),
						"%s%s   %.0f x %.0f   left %.0f  top %.0f   rot %.0f  scale %.2f",
						id.empty() ? "selection" : "#", id.c_str(),
						static_cast<double>(width), static_cast<double>(height),
						static_cast<double>(left), static_cast<double>(top),
						static_cast<double>(m_Rotate), static_cast<double>(m_Scale));
				}
				else if (!missing.empty())
				{
					std::snprintf(status, sizeof(status), "Missing %zu file(s): %s%s",
						missing.size(), missing.front().c_str(), missing.size() > 1 ? " ..." : "");
					warning = true;
				}
				else if (!message.empty())
				{
					std::snprintf(status, sizeof(status), "%s", message.c_str());
					warning = true;
				}
				else
				{
					std::snprintf(status, sizeof(status), m_PreviewMode ? "Preview · %zu elements" : "%zu elements",
						m_pCanvas->GetElementCount());
				}

				const ImVec2 text_size = ImGui::CalcTextSize(status);
				const ImVec2 text_pos(image_area_min.x + 8.0f,
					image_area_max.y - text_size.y - 8.0f);
				ImVec4 background = EditorTheme::Token::Neutral0;
				background.w = 0.82f;
				draw_list->AddRectFilled(ImVec2(text_pos.x - 5.0f, text_pos.y - 3.0f),
					ImVec2(text_pos.x + text_size.x + 5.0f, text_pos.y + text_size.y + 3.0f),
					ImGui::GetColorU32(background), 4.0f);
				draw_list->AddText(text_pos, ImGui::GetColorU32(warning
					? EditorTheme::Token::Warning : EditorTheme::Token::Neutral7), status);
			}

			/* 选中的元素再描一圈（比 hover 更实），并把名字与尺寸放进状态行 */
			float selection_x = 0.0f;
			float selection_y = 0.0f;
			float selection_width = 0.0f;
			float selection_height = 0.0f;
			if (!m_PreviewMode && m_pCanvas->HasSelection()
				&& m_pCanvas->GetSelectionBox(selection_x, selection_y, selection_width, selection_height))
			{
				const ImU32 accent = ImGui::GetColorU32(EditorTheme::Token::Accent);

				/* 四个角直接取画布算好的结果：自身与每一层祖先的平移/旋转/缩放都复合在里面，
				 * 父级动过之后选框也能贴合 */
				float render_quad[4][2];
				m_pCanvas->GetSelectionRenderQuad(render_quad);
				const ImVec2 quad[4] = {
					ImVec2(view_min.x + render_quad[0][0] / to_tex_x, view_min.y + render_quad[0][1] / to_tex_y),
					ImVec2(view_min.x + render_quad[1][0] / to_tex_x, view_min.y + render_quad[1][1] / to_tex_y),
					ImVec2(view_min.x + render_quad[2][0] / to_tex_x, view_min.y + render_quad[2][1] / to_tex_y),
					ImVec2(view_min.x + render_quad[3][0] / to_tex_x, view_min.y + render_quad[3][1] / to_tex_y),
				};
				const float radians = m_Rotate * 3.14159265f / 180.0f;
				const float cos_value = std::cos(radians);
				const float sin_value = std::sin(radians);
				draw_list->AddQuad(quad[0], quad[1], quad[2], quad[3], accent, 1.5f);

				/* 手柄：按当前模式给出备选点，悬停时高亮 */
				const ImVec2 box_center((quad[0].x + quad[2].x) * 0.5f, (quad[0].y + quad[2].y) * 0.5f);
				const float handle_half = 3.5f;
				const ImVec2 mouse_pos = ImGui::GetIO().MousePos;
				const auto draw_handle = [&](const ImVec2& position)
				{
					const bool hot = std::abs(mouse_pos.x - position.x) <= handle_half + 3.0f
						&& std::abs(mouse_pos.y - position.y) <= handle_half + 3.0f;
					draw_list->AddRectFilled(ImVec2(position.x - handle_half, position.y - handle_half),
						ImVec2(position.x + handle_half, position.y + handle_half),
						hot ? accent : ImGui::GetColorU32(ImGuiCol_Text));
					draw_list->AddRect(ImVec2(position.x - handle_half, position.y - handle_half),
						ImVec2(position.x + handle_half, position.y + handle_half), accent);
				};

				if (m_TransformMode == 2)
				{
					/* 缩放：四个角 */
					draw_handle(quad[0]);
					draw_handle(quad[1]);
					draw_handle(quad[3]);
					draw_handle(quad[2]);
				}
				else if (m_TransformMode == 1)
				{
					/* 旋转：顶边中点往上一段画一个圆 */
					const ImVec2 stem(ImVec2((quad[0].x + quad[1].x) * 0.5f, (quad[0].y + quad[1].y) * 0.5f));
					/* 柄沿框的本地「上」方向伸出，框转它跟着转，长度恒定 */
					const ImVec2 knob(stem.x + sin_value * 22.0f, stem.y - cos_value * 22.0f);
					const bool hot = std::abs(mouse_pos.x - knob.x) <= 8.0f
						&& std::abs(mouse_pos.y - knob.y) <= 8.0f;
					draw_list->AddLine(stem, knob, accent, 1.0f);
					draw_list->AddCircleFilled(knob, 5.0f, hot ? accent : ImGui::GetColorU32(ImGuiCol_Text));
					draw_list->AddCircle(knob, 5.0f, accent);
				}
				else
				{
					/* 移动：四边中点 + 中心 */
					draw_handle(ImVec2((quad[0].x + quad[1].x) * 0.5f, (quad[0].y + quad[1].y) * 0.5f));
					draw_handle(ImVec2((quad[1].x + quad[2].x) * 0.5f, (quad[1].y + quad[2].y) * 0.5f));
					draw_handle(ImVec2((quad[3].x + quad[2].x) * 0.5f, (quad[3].y + quad[2].y) * 0.5f));
					draw_handle(ImVec2((quad[0].x + quad[3].x) * 0.5f, (quad[0].y + quad[3].y) * 0.5f));
					draw_list->AddCircleFilled(box_center, 2.5f, accent);
				}

				/* 旋转 / 缩放时把实际锚点点出来：元素就是绕它转，位置不对一眼能看见 */
				if (m_TransformMode == 1 || m_TransformMode == 2)
				{
					float anchor_x = 0.0f;
					float anchor_y = 0.0f;
					if (m_pCanvas->GetSelectionAnchorPoint(anchor_x, anchor_y))
					{
						const ImVec2 anchor(view_min.x + anchor_x / to_tex_x, view_min.y + anchor_y / to_tex_y);
						draw_list->AddLine(ImVec2(anchor.x - 7.0f, anchor.y), ImVec2(anchor.x + 7.0f, anchor.y),
							accent, 1.0f);
						draw_list->AddLine(ImVec2(anchor.x, anchor.y - 7.0f), ImVec2(anchor.x, anchor.y + 7.0f),
							accent, 1.0f);
						draw_list->AddCircle(anchor, 3.5f, accent, 12, 1.5f);
					}
				}
			}

			draw_list->PopClipRect();
			draw_list->ChannelsMerge();

			/* 打字：交互态、面板拿着键盘、且 ImGui 自己没有输入框在用时才喂给控件 */
			if (m_PreviewMode && m_Focused && !ImGui::GetIO().WantTextInput)
			{
				for (const ImWchar character : ImGui::GetIO().InputQueueCharacters)
					m_pCanvas->ProcessTextInput(static_cast<uint32_t>(character));
			}
		}
		else if (m_Path.empty())
		{
			const char* hint = "Pick a .rml document to preview";
			const ImVec2 text_size = ImGui::CalcTextSize(hint);
			draw_list->AddText(ImVec2(image_area_min.x + (image_area_size.x - text_size.x) * 0.5f,
				image_area_min.y + (image_area_size.y - text_size.y) * 0.5f),
				ImGui::GetColorU32(EditorTheme::Token::Neutral7), hint);
		}


		/* ---- 属性列：贴着预览右侧，选中元素的盒子与变换 ---- */
		if (inspector_width > 0.0f)
		{
			ImGui::SetCursorScreenPos(inspector_min);
			DrawInspector(ImVec2(inspector_width, content_size.y));
			draw_splitter(preview_max.x, "##ui-inspector-splitter", m_InspectorFraction, true);
		}

		/* ---- 最右列：代码区 ---- */
		if (!m_Editing || code_width <= 0.0f)
			return;

		draw_splitter(code_min.x - splitter_width, "##ui-code-splitter", m_CodeFraction, true);

		m_TextEditor.Draw(ImVec2(code_min.x, content_min.y), ImVec2(code_width, content_size.y));
	}

	namespace
	{
		/* 树行标签：有 id 就写 #id，没有退回「标签 + 首个 class」——
		 * 布局用的 div 大多没有 id，光看标签分不出谁是谁 */
		std::string ElementLabel(const PreviewSelection::ElementNode& node)
		{
			if (!node.Id.empty())
				return "#" + node.Id;

			std::string label = node.Tag;
			if (!node.ClassName.empty())
			{
				const size_t end = node.ClassName.find(' ');
				label += " ." + node.ClassName.substr(0, end);
			}
			return label;
		}

		std::string ToLowerCopy(const std::string& text)
		{
			std::string out = text;
			for (char& character : out)
				character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
			return out;
		}

		/* 颜色值从拾色器写回：带透明度的给八位，和 CSS 里的写法一致 */
		std::string FormatCssColour(const ImVec4& colour)
		{
			const auto channel = [](float value)
			{
				return static_cast<int>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
			};

			char buffer[16];
			if (channel(colour.w) >= 255)
			{
				std::snprintf(buffer, sizeof(buffer), "#%02x%02x%02x",
					channel(colour.x), channel(colour.y), channel(colour.z));
			}
			else
			{
				std::snprintf(buffer, sizeof(buffer), "#%02x%02x%02x%02x",
					channel(colour.x), channel(colour.y), channel(colour.z), channel(colour.w));
			}
			return buffer;
		}

		/* "#RGB" / "#RRGGBB" / "#RRGGBBAA" / "rgb(...)" 解析成色块用的颜色。
		 * 只服务预览：认不出来就不画色块，写回仍然按输入框里的原文 */
		bool ParseCssColour(const std::string& text, ImU32& out)
		{
			const auto hex_digit = [](char character) -> int
			{
				if (character >= '0' && character <= '9') return character - '0';
				if (character >= 'a' && character <= 'f') return character - 'a' + 10;
				if (character >= 'A' && character <= 'F') return character - 'A' + 10;
				return -1;
			};

			size_t index = 0;
			while (index < text.size() && std::isspace(static_cast<unsigned char>(text[index])))
				++index;

			if (index < text.size() && text[index] == '#')
			{
				std::string digits;
				for (size_t scan = index + 1; scan < text.size() && hex_digit(text[scan]) >= 0; ++scan)
					digits.push_back(text[scan]);

				int channels[4] = { 0, 0, 0, 255 };
				if (digits.size() == 3)
				{
					for (int i = 0; i < 3; ++i)
						channels[i] = hex_digit(digits[i]) * 17;
				}
				else if (digits.size() == 6 || digits.size() == 8)
				{
					for (size_t i = 0; i < digits.size() / 2; ++i)
						channels[i] = hex_digit(digits[i * 2]) * 16 + hex_digit(digits[i * 2 + 1]);
				}
				else
				{
					return false;
				}

				out = IM_COL32(channels[0], channels[1], channels[2], channels[3]);
				return true;
			}

			/* rgb(r, g, b) / rgba(r, g, b, a)：命名色不展开，认不出来交给上面的失败分支 */
			const size_t open = text.find('(', index);
			if (open == std::string::npos)
				return false;

			const std::string name = text.substr(index, open - index);
			if (name != "rgb" && name != "rgba")
				return false;

			const size_t close = text.find(')', open);
			if (close == std::string::npos)
				return false;

			int channels[4] = { 0, 0, 0, 255 };
			int count = 0;
			size_t cursor = open + 1;
			while (count < 4 && cursor < close)
			{
				while (cursor < close
					&& (text[cursor] == ',' || std::isspace(static_cast<unsigned char>(text[cursor]))))
					++cursor;
				if (cursor >= close)
					break;

				float value = 0.0f;
				bool has_dot = false;
				while (cursor < close && (std::isdigit(static_cast<unsigned char>(text[cursor])) || text[cursor] == '.'))
				{
					has_dot = has_dot || text[cursor] == '.';
					value = value * 10.0f + static_cast<float>(text[cursor] - '0');
					++cursor;
				}

				/* 第四个数是 alpha，可能是 0~1 的小数 */
				channels[count] = (count == 3 && has_dot)
					? static_cast<int>(value * 255.0f + 0.5f)
					: static_cast<int>(value + 0.5f);
				++count;
			}

			if (count < 3)
				return false;

			out = IM_COL32(channels[0], channels[1], channels[2], channels[3]);
			return true;
		}
	}

	void UIPreviewPanel::SyncSelectionNumbers()
	{
		if (m_pCanvas == nullptr || !m_pCanvas->HasSelection())
			return;

		/* 只读：把画布里的数值取回面板。点选不再往元素上写变换，
		 * 免得瞄一眼元素就给它加上 transform 本地声明 */
		m_Rotate = m_pCanvas->GetSelectionRotation();
		m_Scale = m_pCanvas->GetSelectionScale();
		m_pCanvas->SetSelectionTransformState(m_Rotate, m_Scale);
	}

	namespace
	{
		/* 选中行的祖先都要展开：否则滚到那一行也还是被折在上层里看不见 */
		void ExpandAncestors(std::vector<bool>& collapsed,
			const std::vector<PreviewSelection::ElementNode>& nodes, int index)
		{
			if (index < 0 || static_cast<size_t>(index) >= nodes.size())
				return;

			int depth = nodes[static_cast<size_t>(index)].Depth;
			for (int parent = index - 1; parent >= 0 && depth > 0; --parent)
			{
				const size_t at = static_cast<size_t>(parent);
				if (nodes[at].Depth >= depth)
					continue;

				depth = nodes[at].Depth;
				if (at < collapsed.size())
					collapsed[at] = false;
			}
		}
	}

	void UIPreviewPanel::DrawElementTree(const ImVec2& size)
	{
		ImGui::BeginChild("##ui-element-tree", size, true);

		/* 标题行：和面板头部同一套版式，这列装的是什么一眼看得出 */
		const PanelChrome::HeaderRow header = PanelChrome::BeginHeaderRow(Icons::Id::FileUI);
		PanelChrome::DrawHeaderTitle(header, "Hierarchy");
		PanelChrome::EndHeaderRow(header);

		if (m_pCanvas == nullptr || !m_pCanvas->IsOpen())
		{
			ImGui::TextDisabled("No document");
			ImGui::EndChild();
			return;
		}

		/* 搜索框占满一行：按 id / 标签 / class 过滤，下面的树跟着收窄 */
		ImGui::SetNextItemWidth(-FLT_MIN);
		Icons::BeginSearchInput();
		ImGuiExt::InputTextString("##ui-tree-search", m_TreeFilter);
		Icons::EndSearchInput();
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Filter by id, tag or class");

		/* 折叠状态与每帧的元素表对齐；选中变了自动把祖先摊开 */
		if (m_TreeCollapsed.size() != m_Elements.size())
			m_TreeCollapsed.assign(m_Elements.size(), false);

		const int selected = m_pCanvas->GetSelectionIndex();
		if (selected != m_LastTreeIndex)
			ExpandAncestors(m_TreeCollapsed, m_Elements, selected);

		/* 树自己滚，标题行钉在框顶 */
		ImGui::BeginChild("##ui-element-rows", ImVec2(0.0f, 0.0f), false);

		/* 交互态的输入全归控件，树只看不改 */
		ImGui::BeginDisabled(m_PreviewMode);

		/* 行内容的右沿（和层级树同一笔账：内边距与边框取大的那个）：
		 * 名字放不下就省略号收尾，不给面板撑出横向滚动 */
		const ImGuiWindow* tree_window = ImGui::GetCurrentWindow();
		const float row_right = tree_window->InnerRect.Max.x
			- std::max(tree_window->WindowPadding.x, tree_window->WindowBorderSize);

		/* 搜索：命中的行连同它的祖先链一起留下，其余整枝跳过 */
		const bool filtering = !m_TreeFilter.empty();
		std::vector<bool> visible;
		bool has_match = true;
		if (filtering)
		{
			const std::string needle = ToLowerCopy(m_TreeFilter);
			visible.assign(m_Elements.size(), false);
			has_match = false;

			std::vector<int> ancestors;
			for (size_t index = 0; index < m_Elements.size(); ++index)
			{
				const PreviewSelection::ElementNode& node = m_Elements[index];
				if (ancestors.size() <= static_cast<size_t>(node.Depth))
					ancestors.resize(static_cast<size_t>(node.Depth) + 1, -1);

				if (ToLowerCopy(ElementLabel(node)).find(needle) != std::string::npos)
				{
					visible[index] = true;
					has_match = true;
					for (int depth = 0; depth < node.Depth; ++depth)
					{
						const int parent = ancestors[static_cast<size_t>(depth)];
						if (parent >= 0)
							visible[static_cast<size_t>(parent)] = true;
					}
				}

				ancestors[static_cast<size_t>(node.Depth)] = static_cast<int>(index);
			}
		}

		if (!has_match)
			ImGui::TextDisabled("No match");

		/* 平表按深度补缩进：Indent 与 TreePush 是同一笔账，画出来和场景层级树一致。
		 * 折起来的行，它的后代一路跳过，直到回到同层或更浅的一行 */
		int indent_depth = 0;
		int hidden_below_depth = -1;
		for (size_t index = 0; index < m_Elements.size(); ++index)
		{
			const PreviewSelection::ElementNode& node = m_Elements[index];

			/* 搜索状态只看命中与祖先链，折叠不起作用 */
			if (filtering)
			{
				if (!visible[index])
					continue;
			}
			else if (hidden_below_depth >= 0)
			{
				if (node.Depth > hidden_below_depth)
					continue;
				hidden_below_depth = -1;
			}

			while (indent_depth < node.Depth)
			{
				ImGui::Indent();
				++indent_depth;
			}
			while (indent_depth > node.Depth)
			{
				ImGui::Unindent();
				--indent_depth;
			}

			const bool is_selected = static_cast<int>(index) == selected;
			const bool folded = filtering ? false : m_TreeCollapsed[index];

			/* 开合状态每帧按自己的表定：自动展开祖先与点箭头共用这一份 */
			if (node.HasChildren)
				ImGui::SetNextItemOpen(!folded, ImGuiCond_Always);

			/* NoTreePushOnOpen：缩进是自己用 Indent 补的，节点不用再 TreePush ——
			 * 平表里没有对应的 TreePop，少了这一位每帧会漏一个 ID 到栈上 */
			ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanFullWidth
				| ImGuiTreeNodeFlags_NoTreePushOnOpen;
			if (is_selected)
				flags |= ImGuiTreeNodeFlags_Selected;
			if (!node.HasChildren)
				flags |= ImGuiTreeNodeFlags_Leaf;

			/* 悬停选中行时 ImGui 画的也是 HeaderHovered（会把选中色盖掉） */
			if (is_selected)
				ImGui::PushStyleColor(ImGuiCol_HeaderHovered, EditorTheme::RowHoverSelected);

			ImGui::PushID(static_cast<int>(index));
			/* 空标签：行内容自己画（图标 + 名字），和层级树是同一套版式 */
			const bool opened = ImGui::TreeNodeEx("##node", flags, "%s", "");

			if (is_selected)
				ImGui::PopStyleColor();

			/* 选中与滚动定位都得赶在画行内容之前：画完「上一项」就换人了 */
			if (ImGui::IsItemClicked())
			{
				m_pCanvas->SelectIndex(index);
				SyncSelectionNumbers();
			}
			if (is_selected && m_LastTreeIndex != selected)
				ImGui::SetScrollHereY(0.5f);

			/* 用户点了箭头：把结果记回来（ImGui 这一帧已经翻过面了）。
			 * 搜索时箭头恒展开，别让点击污染折叠表 */
			if (!filtering && node.HasChildren && opened != !folded)
				m_TreeCollapsed[index] = !opened;

			PanelChrome::DrawTreeRowLabel(node.Depth == 0 ? Icons::Id::FileUI : Icons::Id::Entity,
				ElementLabel(node), false, EditorTheme::Token::Text, row_right);
			ImGui::PopID();

			if (node.HasChildren && !opened)
				hidden_below_depth = node.Depth;
		}

		/* 缩进收回到顶层，别留给后面的属性栏 */
		while (indent_depth > 0)
		{
			ImGui::Unindent();
			--indent_depth;
		}
		ImGui::EndDisabled();

		/* 点了行之后选中可能已经变了（点 body 等于取消选中），下一帧按新的算 */
		m_LastTreeIndex = m_pCanvas->GetSelectionIndex();
		ImGui::EndChild();
		ImGui::EndChild();
	}

	bool UIPreviewPanel::DrawInspectorFloat(const char* label, float& value, float speed, const char* format)
	{
		const float value_width = ImGuiExt::BeginPropertyRow(label);
		ImGui::SetNextItemWidth(value_width);
		const bool changed = ImGui::DragFloat("##value", &value, speed, 0.0f, 0.0f, format);
		ImGuiExt::EndPropertyRow();
		return changed;
	}

	void UIPreviewPanel::BeginInspectorEdit()
	{
		if (m_pCanvas != nullptr && m_pCanvas->HasSelection())
			m_pCanvas->GetSelectionState(m_InspectorBefore);
	}

	void UIPreviewPanel::CommitInspectorEdit()
	{
		if (m_pCanvas == nullptr || !m_pCanvas->HasSelection())
			return;

		/* 拖动过程中改的值已经生效，这里只把「起点 → 现在」记成一条命令 */
		m_GestureBefore = m_InspectorBefore;
		m_GestureKey.Id = m_pCanvas->GetSelectionId();
		m_GestureKey.Index = m_pCanvas->GetSelectionIndex();
		CommitGesture();
	}

	void UIPreviewPanel::DrawInspector(const ImVec2& size)
	{
		/* 和左边的树列一样包一层圆角边框；内边距走主题默认，卡片与属性面板对得上 */
		ImGui::BeginChild("##ui-inspector", size, true);

		const bool has_selection = m_pCanvas != nullptr && m_pCanvas->HasSelection();
		const int selected = has_selection ? m_pCanvas->GetSelectionIndex() : -1;
		const bool has_node = selected >= 0 && static_cast<size_t>(selected) < m_Elements.size();

		if (!has_node)
		{
			ImGui::TextDisabled("No element selected");
			ImGui::EndChild();
			return;
		}

		PreviewSelection::ElementKey key;
		key.Id = m_pCanvas->GetSelectionId();
		key.Index = selected;

		/* 换元素清输入缓冲：缓冲按属性 key 存，留着会把上一个元素的值带过来 */
		const std::string owner = key.Id + "#" + std::to_string(key.Index);
		if (m_PropertyEditsOwner != owner)
		{
			m_PropertyEditsOwner = owner;
			m_PropertyEdits.clear();
			m_ColourRow.clear();
			m_ColourBefore.clear();
		}

		/* 面板头：元素名 + 盒子，和属性面板的头部同一个版式 */
		const PreviewSelection::ElementNode& node = m_Elements[static_cast<size_t>(selected)];
		const PanelChrome::HeaderRow header = PanelChrome::BeginHeaderRow(Icons::Id::Entity);
		PanelChrome::DrawHeaderTitle(header, ElementLabel(node),
			std::max(0.0f, header.Right - header.TitleX));
		PanelChrome::EndHeaderRow(header);

		ImGui::TextDisabled("%.0f x %.0f  (%.0f, %.0f)",
			static_cast<double>(node.Width), static_cast<double>(node.Height),
			static_cast<double>(node.X), static_cast<double>(node.Y));

		/* 交互态改的是控件，属性栏不给动 */
		ImGui::BeginDisabled(m_PreviewMode);

		/* 变换最高频，放在最上面；它不是 RmlUi 属性（自己记状态），单独一张卡 */
		DrawTransformCard();

		/* 属性卡：RmlUi 那边能改的属性按分类各成一张，顺序同描述表 */
		m_Properties.clear();
		m_pCanvas->CollectSelectionProperties(m_Properties);

		size_t index = 0;
		while (index < m_Properties.size())
		{
			const char* category = m_Properties[index].Desc->Category;
			const size_t begin = index;
			while (index < m_Properties.size()
				&& std::strcmp(m_Properties[index].Desc->Category, category) == 0)
				++index;

			const PanelChrome::Card card = PanelChrome::BeginCard(category);
			if (card.Open)
			{
				for (size_t row = begin; row < index; ++row)
					DrawPropertyRow(key, m_Properties[row]);
			}
			PanelChrome::EndCard(card);
		}

		ImGui::EndDisabled();
		ImGui::EndChild();
	}

	void UIPreviewPanel::DrawPropertyRow(const PreviewSelection::ElementKey& key,
		const ElementPropertyValue& property)
	{
		const ElementPropertyDesc& desc = *property.Desc;

		/* 只读项（id / class）：只显示，不给改 */
		if (desc.ReadOnly)
		{
			ImGuiExt::BeginPropertyRow(desc.Label);
			ImGui::TextDisabled("%s", property.Value.empty() ? "-" : property.Value.c_str());
			ImGuiExt::EndPropertyRow();
			return;
		}

		/* 文本内容单独一套：整行宽、折行显示，点开才换成输入框 */
		if (desc.Editor == ElementPropertyEditor::Area)
		{
			DrawTextAreaRow(key, desc, property);
			return;
		}

		const std::string row_key = desc.Kind == ElementPropertyKind::Text ? "#text" : desc.Key;

		PropertyEdit& edit = m_PropertyEdits[row_key];
		/* 没在编辑时跟着元素上的值走：撤销、外部重载都能同步回来。
		 * 拾色器开着的那一行例外 —— 回灌会把量化后的值又喂回拾色器，拖起来发跳 */
		if (!edit.Active && m_ColourRow != row_key
			&& (!edit.Synced || edit.Text != property.Value))
		{
			edit.Text = property.Value;
			edit.Synced = true;
		}

		const float value_width = ImGuiExt::BeginPropertyRow(desc.Label);

		/* 上一帧写回被文档拒了：这一帧把控件描成警示色，悬停给出说明 */
		const bool invalid = edit.WriteFailed;
		if (invalid)
			ImGui::PushStyleColor(ImGuiCol_Border, EditorTheme::Token::Warning);

		bool changed = false;
		switch (desc.Editor)
		{
		case ElementPropertyEditor::Text:
			ImGui::SetNextItemWidth(value_width);
			changed = ImGuiExt::InputTextString("##value", edit.Text);
			break;

		case ElementPropertyEditor::Color:
		{
			/* 色块既是当前值的预览，也是拾色器的入口；写回仍按输入框里的原文，
			 * 只有真的从拾色器里选了色才换成 #rrggbb(aa) */
			ImU32 swatch = 0;
			const bool parsed = ParseCssColour(edit.Text, swatch);
			ImVec4 current = parsed ? ImGui::ColorConvertU32ToFloat4(swatch) : ImVec4(1.0f, 1.0f, 1.0f, 1.0f);

			const float swatch_size = ImGui::GetFrameHeight();
			ImGui::ColorButton("##swatch", current,
				ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
				ImVec2(swatch_size, swatch_size));
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Pick a colour");
			if (ImGui::IsItemClicked())
			{
				m_ColourRow = row_key;
				m_ColourBefore = property.Value;
				ImGui::OpenPopup("##colour-picker");
			}

			ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
			changed = ImGuiExt::InputTextString("##value", edit.Text);

			if (ImGui::BeginPopup("##colour-picker"))
			{
				ImGui::TextDisabled("%s", desc.Label);
				if (ImGui::ColorPicker4("##picker", &current.x,
					ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoSidePreview))
				{
					edit.Text = FormatCssColour(current);
					changed = true;
				}
				ImGui::EndPopup();
			}
			else if (m_ColourRow == row_key)
			{
				/* 弹层关了：这一段调色合成一条历史 */
				m_ColourRow.clear();
				if (edit.Text != m_ColourBefore)
					CommitPropertyEdit(key, desc, m_ColourBefore, edit.Text);
				m_ColourBefore.clear();
			}
			break;
		}

		case ElementPropertyEditor::Choice:
			ImGui::SetNextItemWidth(value_width);
			if (ImGui::BeginCombo("##value", edit.Text.empty() ? "-" : edit.Text.c_str()))
			{
				for (const char* const* choice = desc.Choices; choice != nullptr && *choice != nullptr; ++choice)
				{
					const bool picked = edit.Text == *choice;
					if (ImGui::Selectable(*choice, picked))
					{
						edit.Text = *choice;
						changed = true;
					}
					if (picked)
						ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}
			break;
		}

		const bool activated = ImGui::IsItemActivated();
		const bool deactivated = ImGui::IsItemDeactivated();
		const bool finished = ImGui::IsItemDeactivatedAfterEdit();

		/* 值被文档拒了：悬停给一句说明，不然用户只看到预览没反应 */
		if (invalid && ImGui::IsItemHovered())
			ImGui::SetTooltip("The document rejected this value");

		ImGuiExt::EndPropertyRow();
		if (invalid)
			ImGui::PopStyleColor();

		/* 下拉一次就是一次操作，不攒历史 */
		if (desc.Editor == ElementPropertyEditor::Choice)
		{
			if (changed && edit.Text != property.Value)
			{
				edit.WriteFailed = !ApplyPropertyValue(key, desc, edit.Text);
				if (!edit.WriteFailed)
					CommitPropertyEdit(key, desc, property.Value, edit.Text);
			}
			return;
		}

		if (activated)
		{
			edit.Active = true;
			edit.Before = property.Value;
		}

		/* 打字即生效：预览立刻跟着变，历史等失焦再记一条 */
		if (changed)
			edit.WriteFailed = !ApplyPropertyValue(key, desc, edit.Text);

		/* 失焦就交还同步权：撤销改了值，下一帧缓冲会跟着走 */
		if (deactivated)
			edit.Active = false;

		/* 被拒的值不进历史：撤销回退一段没生效过的改动没有意义 */
		if (finished && !edit.WriteFailed && edit.Text != edit.Before)
			CommitPropertyEdit(key, desc, edit.Before, edit.Text);
	}

	/* 文本内容：平时按列宽折行显示（左上起排，看得到全文），点一下才换成输入框。
	 * ImGui 的多行输入框不折行、也不裁自己，长文本只能这样看全 */
	void UIPreviewPanel::DrawTextAreaRow(const PreviewSelection::ElementKey& key,
		const ElementPropertyDesc& desc, const ElementPropertyValue& property)
	{
		const std::string row_key = "#text";

		PropertyEdit& edit = m_PropertyEdits[row_key];
		if (!edit.Active && (!edit.Synced || edit.Text != property.Value))
		{
			edit.Text = property.Value;
			edit.Synced = true;
		}

		const ImGuiStyle& style = ImGui::GetStyle();

		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("%s", desc.Label);
		ImGui::Spacing();

		const float width = ImGui::GetContentRegionAvail().x;
		const ImVec2 box_min = ImGui::GetCursorScreenPos();
		const float wrap_width = std::max(1.0f, width - style.FramePadding.x * 2.0f);

		if (edit.Editing)
		{
			/* 刚点开的那一行：这一帧把键盘焦点交给输入框 */
			if (m_FocusTextRow == row_key)
			{
				ImGui::SetKeyboardFocusHere();
				m_FocusTextRow.clear();
			}

			const float box_height = ImGui::GetTextLineHeight() * 4.0f + style.FramePadding.y * 2.0f;
			ImGui::PushClipRect(box_min, ImVec2(box_min.x + width, box_min.y + box_height), true);
			const bool changed = ImGuiExt::InputTextMultilineString("##value", edit.Text,
				ImVec2(width, box_height));
			const bool deactivated = ImGui::IsItemDeactivated();
			const bool finished = ImGui::IsItemDeactivatedAfterEdit();
			ImGui::PopClipRect();

			if (changed)
				ApplyPropertyValue(key, desc, edit.Text);

			if (deactivated)
			{
				edit.Editing = false;
				edit.Active = false;
			}
			if (finished && edit.Text != edit.Before)
				CommitPropertyEdit(key, desc, edit.Before, edit.Text);
			return;
		}

		/* 显示态：按列宽折行，框高跟着内容走 */
		const ImVec2 text_size = ImGui::CalcTextSize(edit.Text.c_str(), nullptr, false, wrap_width);
		const float box_height = std::max(text_size.y, ImGui::GetTextLineHeight())
			+ style.FramePadding.y * 2.0f;

		ImDrawList* draw_list = ImGui::GetWindowDrawList();
		const ImVec2 box_max(box_min.x + width, box_min.y + box_height);
		draw_list->AddRectFilled(box_min, box_max,
			ImGui::GetColorU32(ImGuiCol_FrameBg), style.FrameRounding);
		draw_list->AddRect(box_min, box_max,
			ImGui::GetColorU32(ImGuiCol_Border), style.FrameRounding);

		ImGui::SetCursorScreenPos(ImVec2(box_min.x + style.FramePadding.x, box_min.y + style.FramePadding.y));
		ImGui::PushTextWrapPos(box_min.x + style.FramePadding.x + wrap_width);
		if (edit.Text.empty())
			ImGui::TextDisabled("(empty)");
		else
			ImGui::TextUnformatted(edit.Text.c_str());
		ImGui::PopTextWrapPos();

		/* 整块接住点击：点一下就进编辑 */
		ImGui::SetCursorScreenPos(box_min);
		ImGui::InvisibleButton("##text-box", ImVec2(width, box_height));
		if (ImGui::IsItemHovered())
		{
			ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
			ImGui::SetTooltip("Click to edit");
		}
		if (ImGui::IsItemClicked())
		{
			edit.Editing = true;
			edit.Active = true;
			edit.Before = property.Value;
			m_FocusTextRow = row_key;
		}
	}

	bool UIPreviewPanel::ApplyPropertyValue(const PreviewSelection::ElementKey& key,
		const ElementPropertyDesc& desc, const std::string& value)
	{
		if (m_pCanvas == nullptr || !m_pCanvas->ApplyElementProperty(key, desc, value))
			return false;

		m_Edited = true;
		return true;
	}

	void UIPreviewPanel::CommitPropertyEdit(const PreviewSelection::ElementKey& key,
		const ElementPropertyDesc& desc, const std::string& before, const std::string& after)
	{
		if (m_pCanvas == nullptr || before == after)
			return;

		/* 没有 id 的元素先分一个，命令与写回都要锚点 */
		const PreviewSelection::ElementKey anchored = EnsureElementId(key);

		/* 元素上已经生效（上面每帧写的就是它），这里补一条历史。
		 * 标脏放在回调里：撤销、重做走过同一条路，同样算"文件还没跟上" */
		if (!CurrentStack().Execute(CreateUniquePtr<ElementPropertyCommand>(anchored, desc, before, after,
			[this, canvas = m_pCanvas.get()](const PreviewSelection::ElementKey& element_key,
				const ElementPropertyDesc& property, const std::string& value)
			{
				if (!canvas->ApplyElementProperty(element_key, property, value))
					return false;

				MarkPropertyDirty(element_key, property, value.empty());
				return true;
			})))
			return;
	}

	void UIPreviewPanel::DrawTransformCard()
	{
		const PanelChrome::Card card = PanelChrome::BeginCard("Transform", Icons::Id::Translate);

		if (card.Open)
		{
			/* 元素自带的 matrix / skew / 3D 变换面板表达不了：先说清楚，再让用户决定 */
			if (m_pCanvas->IsSelectionTransformOpaque())
			{
				ImGui::PushTextWrapPos(0.0f);
				ImGui::TextDisabled("This element has a transform this panel cannot represent; editing here will replace it.");
				ImGui::PopTextWrapPos();
			}

			const auto edit_started = [this]()
			{
				if (ImGui::IsItemActivated())
					BeginInspectorEdit();
			};
			const auto edit_finished = [this]()
			{
				if (ImGui::IsItemDeactivatedAfterEdit())
					CommitInspectorEdit();
			};

			if (m_pCanvas->IsSelectionStatic())
			{
				/* 静态元素写 left/top 会被布局忽略，位置只能靠 translate */
				float translate_x = 0.0f;
				float translate_y = 0.0f;
				m_pCanvas->GetSelectionTranslate(translate_x, translate_y);

				bool moved = DrawInspectorFloat("Translate X", translate_x, 0.5f, "%.1f dp");
				edit_started();
				edit_finished();
				moved |= DrawInspectorFloat("Translate Y", translate_y, 0.5f, "%.1f dp");
				edit_started();
				edit_finished();
				if (moved)
					m_pCanvas->SetSelectionTranslate(translate_x, translate_y);
			}
			else
			{
				float left = 0.0f;
				float top = 0.0f;
				m_pCanvas->GetSelectionPosition(left, top);

				bool moved = DrawInspectorFloat("Left", left, 0.5f, "%.1f dp");
				edit_started();
				edit_finished();
				moved |= DrawInspectorFloat("Top", top, 0.5f, "%.1f dp");
				edit_started();
				edit_finished();
				if (moved)
					m_pCanvas->SetSelectionPosition(left, top);
			}

			bool turned = DrawInspectorFloat("Rotate", m_Rotate, 0.5f, "%.1f deg");
			edit_started();
			edit_finished();
			turned |= DrawInspectorFloat("Scale", m_Scale, 0.01f, "%.2f");
			edit_started();
			edit_finished();
			if (turned)
			{
				m_pCanvas->SetSelectionTransform(m_Rotate, m_Scale);
				m_pCanvas->SetSelectionTransformState(m_Rotate, m_Scale);
			}
		}

		PanelChrome::EndCard(card);
	}

	bool UIPreviewPanel::ProcessKeyDown(KeyCode key)
	{
		const bool command = Input::IsKeyPressedRaw(Key::LeftSuper)
			|| Input::IsKeyPressedRaw(Key::LeftControl);

		/* 设计态：撤销与存盘这些编辑动作归面板自己，按键不下发 */
		if (!m_PreviewMode)
		{
			/* Ctrl / ⌘ + Z：撤这份文档的上一条改动。没得撤也吃掉，
			 * 不能漏到场景那边把别的东西撤掉 */
			if (m_Focused && key == Key::Z && command)
			{
				Undo();
				return true;
			}

			/* Ctrl / ⌘ + S：和卡头上的保存同一个入口，面板拿着焦点就吃下来 */
			if (m_Focused && key == Key::S && command && !m_Path.empty())
			{
				SaveAll();
				return true;
			}

			return false;
		}

		/* 交互态：按键全给控件（Tab 切焦点、方向键导航、回车点按钮） */
		return m_Focused && m_pCanvas != nullptr && m_pCanvas->IsOpen()
			&& m_pCanvas->ProcessKeyDown(key);
	}

	bool UIPreviewPanel::ProcessKeyUp(KeyCode key)
	{
		if (!m_PreviewMode)
			return false;

		return m_Focused && m_pCanvas != nullptr && m_pCanvas->IsOpen()
			&& m_pCanvas->ProcessKeyUp(key);
	}

	bool UIPreviewPanel::ShouldRetryOpen(const std::string& path) const
	{
		if (m_FailedPath != path)
			return true;

		std::error_code error;
		const std::filesystem::file_time_type stamp =
			std::filesystem::last_write_time(g_ProjectPath / path, error);
		return !error && stamp != m_FailedStamp;
	}

	/* 预览面板的推进：磁盘改了就重载，再按上次画出来的尺寸渲一帧。
	 * 面板不可见时整段跳过 —— 预览的离屏目标不该在没人看的时候还占着 GPU */
	void UIPreviewPanel::Update()
	{
		if (!Panel::IsVisible(Panel::kGameUI))
		{
			/* 面板关掉就把画布收了：2560x1440 的 RGBA8 目标近 14MB，不该长期占着 */
			if (m_pCanvas != nullptr && m_pCanvas->IsOpen())
				m_pCanvas->Close();
			return;
		}

		/* 重新打开面板时按原路径恢复；上次打得开的文档坏了就等它再存一次 */
		if (m_pCanvas == nullptr || !m_pCanvas->IsOpen())
		{
			if (!m_Path.empty() && ShouldRetryOpen(m_Path))
				OpenDocument(m_Path);
			if (m_pCanvas == nullptr || !m_pCanvas->IsOpen())
				return;
		}

		switch (m_pCanvas->ReloadIfChanged())
		{
		case PreviewCanvas::Change::Document:
		{
			/* 结构变了：元素 id 未必还在，外部改动一律作废历史；自己刚存的那份不算 */
			CORE_LOG_INFO("Game UI 文档已重载：{}", m_Path);
			const bool own_write = IsOwnWrite(m_Path);
			if (!own_write)
				CurrentStack().Clear();
			m_TextEditor.ReloadIfClean();

			/* 自己保存换来的重载：把选中按 id 接回去，属性列不会跟着收起 */
			if (own_write && !m_RestoreSelectionId.empty()
				&& m_pCanvas->SelectById(m_RestoreSelectionId))
				SyncSelectionNumbers();
			m_RestoreSelectionId.clear();
			/* 重载就是回到磁盘那份，元素上没写回的东西已经跟着没了 */
			m_Edited = false;
			m_Dirty.clear();
			break;
		}
		case PreviewCanvas::Change::Style:
		{
			/* 自己写下去的那份热重载回来不算外部改动；别处改了才清历史 */
			const std::string style_path = m_TextEditor.GetStylePath();
			if (!style_path.empty() && !IsOwnWrite(style_path))
			{
				CORE_LOG_INFO("Game UI 样式被外部改动，撤销记录清空：{}", style_path);
				CurrentStack().Clear();
			}
			/* 编辑缓冲没被改过才跟着刷新，有未保存的改动不动它 */
			m_TextEditor.ReloadIfClean();
			break;
		}
		default:
			break;
		}

		/* 尺寸是上一帧画面时量的：面板刚打开、页签还在后台时还量不到，
		 * 先按默认尺寸渲一帧，免得打开后一直空白等下一帧 */
		if (m_Width == 0 || m_Height == 0)
		{
			m_Width = 1280;
			m_Height = 720;
		}
		m_pCanvas->Render(m_Width, m_Height);
	}

}
