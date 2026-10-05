#include "Pch.h"
#include "EditorResourceBrowser.h"
#include "EditorCommon.h"
#include "EditorIcons.h"
#include "PanelChrome.h"
#include "PanelRegistry.h"
#include "Helios/ImGui/EditorTheme.h"

namespace Helios
{
	namespace
	{
		/* 目录状态检查间隔：兼顾刷新及时性与遍历开销 */
		constexpr float kDirectoryCheckInterval = 0.5f;

		/* 资源根目录的显示名：取最后一级目录名（路径以分隔符结尾时 filename() 为空） */
		std::string AssetsRootName()
		{
			const std::string folder_name = g_AssetsPath.filename().string();
			return folder_name.empty() ? g_AssetsPath.string() : folder_name;
		}

		/* 统计资源目录（含根目录）的目录数量与最新写入时间。
		 * 新增 / 删除文件会更新其所在目录的时间戳，因此只需读目录，不必 stat 每个文件。 */
		void CollectDirectoryStamp(size_t& out_count, std::filesystem::file_time_type& out_newest)
		{
			std::error_code error;

			/* 资源根目录自身：直接在根下新增 / 删除文件只更新它的时间戳 */
			out_count = 1;
			out_newest = std::filesystem::last_write_time(g_AssetsPath, error);

			for (const auto& entry : std::filesystem::recursive_directory_iterator(
				g_AssetsPath, std::filesystem::directory_options::skip_permission_denied))
			{
				std::error_code entry_error;
				if (!entry.is_directory(entry_error))
					continue;

				++out_count;

				const auto write_time = entry.last_write_time(entry_error);
				if (!entry_error && write_time > out_newest)
					out_newest = write_time;
			}
		}
	}

	/* 文件类型 -> 图标：与层级面板共用同一套图标语言，
	 * 图片用矢量图（缩略图在 14px 行高下糊成一团） */
	Icons::Id EditorResourceBrowser::ResolveFileIcon(FileType type)
	{
		switch (type)
		{
		case FileType::Folder:   return Icons::Id::Directory;
		case FileType::Image:    return Icons::Id::Sprite;
		case FileType::Scene:    return Icons::Id::FileScene;
		case FileType::MtlGraph: return Icons::Id::FileMtlGraph;
		default:                 return Icons::Id::File;
		}
	}

	/* 本帧的过滤结果：自底向上一次遍历，命中的节点与它们的祖先都算可见
	 * （祖先不留下的话，命中的文件会挂在看不见的目录里）。
	 * 过滤词为空时全部命中 —— 于是「不显示」只有"不在集合里"这一条判据。 */
	void EditorResourceBrowser::FillBrowserFilter()
	{
		PROFILE_FUNCTION();

		m_FilterNeedle = ToLowercase(m_Filter);
		m_VisibleNodes.clear();

		std::function<bool(const SharedPtr<FileNode>&)> visit = [&](const SharedPtr<FileNode>& node) -> bool
		{
			if (node == nullptr)
				return false;

			bool visible = ContainsCaseInsensitive(node->FilePath.c_str(), m_FilterNeedle);
			for (const auto& child : node->ChildNodes)
				visible = visit(child) || visible; /* 注意顺序：先把孩子算完 */

			if (visible)
				m_VisibleNodes.insert(node.get());

			return visible;
		};

		visit(m_RootFileNodeTree);
	}

	bool EditorResourceBrowser::HasDirectoryChanged() const
	{
		size_t count = 0;
		std::filesystem::file_time_type newest{};
		CollectDirectoryStamp(count, newest);

		return count != m_DirectoryCount || newest != m_NewestDirectoryWriteTime;
	}

	void EditorResourceBrowser::UpdateDirectoryStamp()
	{
		CollectDirectoryStamp(m_DirectoryCount, m_NewestDirectoryWriteTime);
	}

	SharedPtr<EditorResourceBrowser::FileNode> EditorResourceBrowser::FindNode(
		const SharedPtr<FileNode>& node, const std::string& path)
	{
		if (node == nullptr)
			return nullptr;

		if (node->FilePath == path)
			return node;

		for (const auto& child : node->ChildNodes)
		{
			if (auto found = FindNode(child, path))
				return found;
		}

		return nullptr;
	}

	void EditorResourceBrowser::OnImGuiRenderer()
	{
		PROFILE_FUNCTION();

		/* 资源可能被编辑器之外的操作改动（新建场景、另存为、外部增删），定期比对目录状态 */
		m_RefreshElapsed += ImGui::GetIO().DeltaTime;
		if (m_RefreshElapsed >= kDirectoryCheckInterval)
		{
			m_RefreshElapsed = 0.0f;
			if (HasDirectoryChanged())
				m_IsDirty = true;
		}

        /* 创建根文件目录节点 */
		if (m_IsDirty)
		{
			/* 记住浏览位置，重建后按路径找回，避免刷新把用户弹回根目录 */
			const std::string current_path = (m_CurrentFileNode != nullptr) ? m_CurrentFileNode->FilePath : std::string();

			m_RootFileNodeTree = CreateSharedPtr<FileNode>(g_AssetsPath.string(), "", FileType::Folder, 0, -1);
			BuildFileNodeTree(m_RootFileNodeTree);
			UpdateDirectoryStamp();

			m_CurrentFileNode = current_path.empty() ? nullptr : FindNode(m_RootFileNodeTree, current_path);
			if (!m_CurrentFileNode)
				m_CurrentFileNode = m_RootFileNodeTree;   /* 未浏览过，或原目录已被删除 */
		}

		/* 过滤结果每帧现算：过滤词与目录树两者都可能变 */
		FillBrowserFilter();

		/* 文件目录树窗口：头部给出资源根目录名，树里按文件类型给图标 */
		ImGui::Begin(Panel::kFileList);
		{
			const PanelChrome::HeaderRow header = PanelChrome::BeginHeaderRow(Icons::Id::Directory);
			PanelChrome::DrawHeaderTitle(header, AssetsRootName());
			PanelChrome::EndHeaderRow(header);

			/* 遍历文件节点构建UI */
			BuildFileUIListTreeSimple(m_RootFileNodeTree);
		}
		ImGui::End();

		/* 详细资源列表窗口 */
		ImGui::Begin(Panel::kResourceBrowser);
		{
			if (!m_CurrentFileNode)
				m_CurrentFileNode = m_RootFileNodeTree; /* 默认为根节点 */

			ShowBrowserHeader();
			ShowBrowserToolbar();
			ShowBrowserContent();
		}
		ImGui::End();
	}

	/* 资源浏览器头部：当前目录名 + 项数 + 视图菜单（缩略图尺寸 / 间距） */
	void EditorResourceBrowser::ShowBrowserHeader()
	{
		PROFILE_FUNCTION();

		const PanelChrome::HeaderRow header = PanelChrome::BeginHeaderRow(Icons::Id::Directory);
		const float button_size = header.Height;

		/* 项数贴右端，与视图菜单各占一个动作位 */
		char count_text[32] = {};
		snprintf(count_text, sizeof(count_text), "%d items",
			static_cast<int>(m_CurrentFileNode->ChildNodes.size()));

		const float count_width = ImGui::CalcTextSize(count_text).x;
		PanelChrome::PlaceHeaderAction(header, count_width, 1);
		ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::TextDim);
		ImGui::TextUnformatted(count_text);
		ImGui::PopStyleColor();

		/* 目录名：按到计数文字左侧为止裁剪 */
		PanelChrome::DrawHeaderTitle(header, m_CurrentFileNode->FileName,
			header.Right - header.TitleX - PanelChrome::HeaderActionWidth(count_width)
				- PanelChrome::HeaderActionWidth(button_size) - ImGui::GetStyle().ItemInnerSpacing.x);

		/* 视图菜单：缩略图尺寸与间距 */
		PanelChrome::PlaceHeaderAction(header, button_size);
		if (Icons::IconButton(Icons::Id::Menu, ImVec2(button_size, button_size), false, "View options"))
			ImGui::OpenPopup("BrowserViewOptions");

		if (ImGui::BeginPopup("BrowserViewOptions"))
		{
			ImGui::SetNextItemWidth(140.0f);
			ImGui::SliderFloat("Thumbnail", &m_ThumbnailSize, 16.0f, 128.0f);
			ImGui::SetNextItemWidth(140.0f);
			ImGui::SliderFloat("Spacing", &m_ThumbnailPadding, 0.0f, 32.0f);
			ImGui::EndPopup();
		}

		PanelChrome::EndHeaderRow(header);
	}

	/* 工具行：上一级 + 过滤框 */
	void EditorResourceBrowser::ShowBrowserToolbar()
	{
		PROFILE_FUNCTION();

		const ImGuiStyle& style = ImGui::GetStyle();
		const PanelChrome::HeaderRow toolbar = PanelChrome::BeginHeaderRow(Icons::Id::None);
		const float button_size = toolbar.Height;

		/* 已经在根目录时把按钮置灰而不是隐藏 —— 位置固定，按钮才不会跳动 */
		const bool at_root = (*m_CurrentFileNode) == (*m_RootFileNodeTree);
		if (at_root)
		{
			ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
			ImGui::PushStyleVar(ImGuiStyleVar_Alpha, style.Alpha * style.DisabledAlpha);
		}

		if (Icons::IconButton(Icons::Id::Return, ImVec2(button_size, button_size), false, "Parent folder"))
			m_CurrentFileNode = m_CurrentFileNode->ParentNode.lock();

		if (at_root)
		{
			ImGui::PopStyleVar();
			ImGui::PopItemFlag();
		}

		/* 过滤框吃掉工具行剩下的宽度 */
		ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
		ImGui::InputTextWithHint("##ResourceFilter", "Filter assets...", m_Filter, sizeof(m_Filter));

		PanelChrome::EndHeaderRow(toolbar);
	}

	/* 主体：缩略图够大就用网格，否则退回详细列表 */
	void EditorResourceBrowser::ShowBrowserContent()
	{
		PROFILE_FUNCTION();

		const std::string needle = ToLowercase(m_Filter);
		const float panel_width = ImGui::GetContentRegionAvail().x;

		if (m_ThumbnailSize > 32.0f)
		{
			const float cell_size = m_ThumbnailSize + m_ThumbnailPadding;
			int column_count = static_cast<int>(panel_width / cell_size);
			if (column_count < 1)
				column_count = 1;

			ImGui::Columns(column_count, nullptr, false);

			for (const auto& child_node : m_CurrentFileNode->ChildNodes)
			{
				/* 通过筛选的才需要显示 */
				if (!ContainsCaseInsensitive(child_node->FilePath.c_str(), needle))
					continue;

				ImGui::PushID(child_node->FileName.c_str());
				{
					ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::Token::Clear);
					{
						/* 图标（图片文件直接用缩略图） */
						ImGui::ImageButton((ImTextureID)child_node->Icon.get(),
							ImVec2(m_ThumbnailSize, m_ThumbnailSize), ImVec2(0, 1), ImVec2(1, 0));

						/* 拖拽 */
						if (ImGui::BeginDragDropSource())
						{
							const char* item_path = child_node->FilePath.c_str();
							ImGui::SetDragDropPayload("RESOURCE_BROWSER_ITEM", item_path, strlen(item_path) + 1);
							ImGui::EndDragDropSource();
						}
					}
					ImGui::PopStyleColor();

					/* 双击进入文件夹 */
					if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
					{
						if (child_node->Type == FileType::Folder)
							m_CurrentFileNode = child_node;
					}

					/* 右键菜单 */
					if (ImGui::BeginPopupContextItem())
					{
						if (ImGui::MenuItem("Show in file explorer"))
						{
							auto path = g_AssetsPath / child_node->FilePath;
							path = absolute(path).make_preferred();
							OpenFileExplorer(path.string().c_str());
						}

						ImGui::EndPopup();
					}

					/* 文件名 */
					ImGui::TextWrapped("%s", child_node->FileName.c_str());

					ImGui::NextColumn();
				}
				ImGui::PopID();
			}

			ImGui::Columns(1);
			return;
		}

		/* 列表模式：三列（名字 / 类型 / 大小），名字列带文件类型图标 */
		ImGui::BeginTable("Assets List", 3);
		{
			ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_NoHide);
			ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableHeadersRow();

			BuildFileUIListTreeDetail(m_CurrentFileNode);
		}
		ImGui::EndTable();
	}

	void EditorResourceBrowser::BuildFileNodeTree(const SharedPtr<FileNode>& parent_node)
	{
		for (auto& directory_entry : std::filesystem::directory_iterator(g_AssetsPath / parent_node->FilePath))
		{
			const auto& path = directory_entry.path();
			auto relative_path = GetRelativePath(g_AssetsPath, path);
			auto filename = relative_path.filename();

			/* 创建一个文件节点 */
			auto file_node = CreateSharedPtr<FileNode>();
			file_node->ParentNode = parent_node;
			file_node->FileName = filename.string();
			file_node->FilePath = relative_path.string();
			file_node->Depth = parent_node->Depth + 1;

			if (directory_entry.is_directory()) /* 文件夹，递归目录 */
			{
                file_node->Type = FileType::Folder;
				file_node->Icon = Icons::GetTexture(Icons::Id::Directory);
				parent_node->ChildNodes.emplace_back(file_node);
				BuildFileNodeTree(file_node);
			}
			else
			{
				auto ext = filename.extension().string();
				transform(ext.begin(), ext.end(), ext.begin(), ::toupper);
				if (ext == ".JPG" || ext == ".PNG" || ext == ".DDS" || ext == ".TGA" || ext == ".BMP")
				{
                    file_node->Type = FileType::Image;

					auto filepath = std::filesystem::path(file_node->FilePath);
					auto relative_path = g_AssetsPath / filepath;
					file_node->Icon = TextureAssetManager::Instance().GetOrCreateTexture(relative_path.generic_string());
				}
				else if (ext == ".SCN")
				{
                    file_node->Type = FileType::Scene;
					file_node->Icon = Icons::GetTexture(Icons::Id::FileScene);

				}
				else if (ext == ".MTLGRAPH")
				{
                    file_node->Type = FileType::MtlGraph;
					file_node->Icon = Icons::GetTexture(Icons::Id::FileMtlGraph);
				}
				else
				{
                    file_node->Type = FileType::Default;
					file_node->Icon = Icons::GetTexture(Icons::Id::File);
				}

				file_node->FileSize = static_cast<float>(std::filesystem::file_size(path)) / 1024.0f;
				parent_node->ChildNodes.emplace_back(file_node);
			}
		}
		m_IsDirty = false;
	}

	void EditorResourceBrowser::BuildFileUIListTreeDetail(const SharedPtr<FileNode>& node)
	{
		for (const auto& child_node : node->ChildNodes)
		{
			if (m_VisibleNodes.count(child_node.get()) == 0)
				continue;

			const auto file_ext = ExtractFileSuffix(child_node->FileName);
			const bool is_folder = (child_node->Type == FileType::Folder);

			if (is_folder && !m_FilterNeedle.empty())
				ImGui::SetNextItemOpen(true, ImGuiCond_Always); /* 过滤时全展开，否则搜到了也看不见 */

			ImGui::PushID(child_node.get());

			ImGui::TableNextRow();
			ImGui::TableNextColumn();

			/* 空标签 + 自绘"图标 + 名字"：与层级树、文件树同一套行内容 */
			const ImGuiTreeNodeFlags flags = is_folder
				? ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanFullWidth
				: ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
			const bool is_opened = ImGui::TreeNodeEx("##node", flags, "%s", "");

			/* 右键菜单：在系统文件夹中显示 */
			if (ImGui::BeginPopupContextItem())
			{
				if (ImGui::MenuItem("Show in file explorer"))
				{
					auto path = g_AssetsPath / child_node->FilePath;
					path = absolute(path).make_preferred();
					OpenFileExplorer(path.string().c_str());
				}

				ImGui::EndPopup();
			}

			PanelChrome::DrawTreeRowLabel(ResolveFileIcon(child_node->Type), child_node->FileName);

			/* 文件类型 */
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(file_ext.c_str());

			/* 文件大小 */
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(is_folder ? "" : (ToString(child_node->FileSize, 2) + "KB").c_str());

			if (is_folder && is_opened)
			{
				BuildFileUIListTreeDetail(child_node);
				ImGui::TreePop();
			}

			ImGui::PopID();
		}
	}

	void EditorResourceBrowser::BuildFileUIListTreeSimple(const SharedPtr<FileNode>& node)
	{
		for (const auto& child_node : node->ChildNodes)
		{
			if (m_VisibleNodes.count(child_node.get()) == 0)
				continue;

			const bool is_folder = (child_node->Type == FileType::Folder);

			if (is_folder && !m_FilterNeedle.empty())
				ImGui::SetNextItemOpen(true, ImGuiCond_Always); /* 过滤时全展开 */

			ImGui::PushID(child_node.get());

			const ImGuiTreeNodeFlags flags = is_folder
				? ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanFullWidth
				: ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
			const bool is_opened = ImGui::TreeNodeEx("##node", flags, "%s", "");

			/* 点目录（不是点展开箭头）时把它设为当前浏览目录 */
			if (is_folder && ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
				m_CurrentFileNode = child_node;

			PanelChrome::DrawTreeRowLabel(ResolveFileIcon(child_node->Type), child_node->FileName);

			if (is_folder && is_opened)
			{
				BuildFileUIListTreeSimple(child_node);
				ImGui::TreePop();
			}

			ImGui::PopID();
		}
	}

	std::filesystem::path EditorResourceBrowser::GetRelativePath(const std::filesystem::path& dir, const std::filesystem::path& path)
	{
		return path.lexically_relative(dir);
	}
}
