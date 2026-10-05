#include "Pch.h"
#include "EditorResourceBrowser.h"
#include "EditorCommon.h"
#include "EditorIcons.h"
#include "PanelChrome.h"
#include "PanelRegistry.h"
#include "Command/AssetFileOps.h"
#include "Command/CreateAssetFolderCommand.h"
#include "Command/DeleteAssetsCommand.h"
#include "Command/RenameAssetCommand.h"
#include "Helios/Application/FileDialog.h"
#include "Helios/ImGui/EditorTheme.h"
#include <utility>
#include <vector>

namespace Helios
{
	namespace
	{
		/* 目录状态检查间隔：兼顾刷新及时性与遍历开销 */
		constexpr float kDirectoryCheckInterval = 0.5f;

		/* 目录树 / 内容区的宽度分配：比例的可拖范围，以及两栏各自的最小宽度
		 * （目录树至少要放得下一个目录名，内容区至少要放得下一列缩略图） */
		constexpr float kMinTreePaneRatio = 0.12f;
		constexpr float kMaxTreePaneRatio = 0.70f;
		constexpr float kMinTreePaneWidth = 96.0f;
		constexpr float kMinContentWidth = 160.0f;

		/* 缩略图缩放范围（顶部一行的滑动条） */
		constexpr float kMinThumbnailSize = 16.0f;
		constexpr float kMaxThumbnailSize = 128.0f;

		/* 网格里两格之间的横向间距：定死，不做成可调项（原本有个 Spacing 滑动条，
		 * 它和缩放滑动条是两套"调版式"的手感，值调到 0 也只是把格子挤在一起）。 */
		constexpr float kThumbnailGap = 32.0f;

		/* 底栏右端那组（统计数 + 缩放滑动条）：滑动条的标称宽度与退让下限 */
		constexpr float kZoomSliderWidth = 96.0f;
		constexpr float kMinZoomSliderWidth = 48.0f;

		/* 底栏留给路径（上一级按钮 + 面包屑）的最小宽度：右端那组退让时按它算 */
		constexpr float kMinCrumbWidth = 96.0f;

		/* 底栏给"选中的项"这类摘要留的最小宽度：低于它就整段不画 ——
		 * 留着只会被裁成两三个字母的碎片，比不显示更像出了 bug。 */
		constexpr float kMinSelectionWidth = 56.0f;

		/* 文件操作弹层（新建 / 重命名 / 删除确认）里的控件尺寸：两个按钮等宽，排一行 */
		constexpr float kPopupButtonSize = 84.0f;
		constexpr float kPopupInputWidth = 240.0f;

		/* 面包屑的分隔符是矢量三角（见 ShowBrowserFooter），这是它占位的方框边长 ——
		 * 与卡片折叠箭头一样按字号取比例，字号变了不会走形。 */
		inline float CrumbSeparatorSize()
		{
			return ImGui::GetFontSize() * 0.55f;
		}

		/* 网格选中框在单元格四周外扩的留白，以及分列时要给相邻两格留的最小间隙。
		 * 框会被列自己的裁剪矩形裁掉（ImGui 的 Columns 默认给每一列套一个裁剪矩形），
		 * 所以列宽必须按「单元格 + 两侧外扩 + 间隙」算，不能只按缩略图算。 */
		constexpr float kGridSelectionPad = 4.0f;
		constexpr float kMinCellGap = 8.0f;

		/* 网格上方要留的余地 = 选中框的外扩 + 描边（半个线宽还得再加抗锯齿）。
		 * 内容栏的绘制裁剪矩形上沿正好压在内容顶上，不留余地的话第一行选中框的上边
		 * 会整条被切掉（框画得出来，只是少一条边）。 */
		constexpr float kGridTopInset = kGridSelectionPad + 4.0f;

		/* 顶栏 / 底栏与主体之间的那条分隔线（1px，自绘） */
		constexpr float kBarDivider = 1.0f;

		/* 底栏（分隔线 + 一行）的总高：主体预留与底栏自己定位都用它 */
		inline float FooterBandHeight()
		{
			return ImGui::GetFrameHeight() + kBarDivider;
		}

		/* 资源根目录的显示名：取最后一级目录名（路径以分隔符结尾时 filename() 为空） */
		std::string AssetsRootName()
		{
			const std::string folder_name = PathToUtf8(g_AssetsPath.filename());
			return folder_name.empty() ? PathToUtf8(g_AssetsPath) : folder_name;
		}

		/* 网格里的文件名：逐行居中，长名字按可用宽度换行。别用 ImGui 的 CalcWordWrapPositionA：
		 * 它只在空白和 .,;!?" 处断行、长段兜底还会失效，一行放不下就被裁剪切尾。这里自己按像素
		 * 贪心断行：取"放得下的最远前缀"再回退到自然断点（_ - . / 空格），每行都放得下、断点也好读。 */
		void DrawCenteredWrappedText(const std::string& text, float width, ImU32 color)
		{
			ImFont* font = ImGui::GetFont();
			const float font_size = ImGui::GetFontSize();
			const float line_height = ImGui::GetTextLineHeightWithSpacing();
			const ImVec2 start = ImGui::GetCursorScreenPos();
			const char* const text_begin = text.c_str();
			const char* const text_end = text_begin + text.size();

			const auto measure = [&](const char* begin, const char* end)
			{
				return font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, begin, end).x;
			};
			const auto char_length = [](unsigned char lead)
			{
				return lead >= 0xF0 ? 4 : (lead >= 0xE0 ? 3 : (lead >= 0xC0 ? 2 : 1));
			};
			/* 可以断在它之后的字符：空白 + 文件名常见分隔符 */
			const auto is_break_char = [](unsigned char c)
			{
				return c < 0x80 && (c == ' ' || c == '\t' || c == '_' || c == '-' || c == '.'
					|| c == ',' || c == ';' || c == '/' || c == '\\' || c == ')');
			};

			std::vector<std::pair<const char*, const char*>> lines;
			const char* line_start = text_begin;
			while (line_start < text_end)
			{
				/* 1) 放得下的最远前缀（逐字符推进，UTF-8 安全） */
				const char* fit = line_start;
				while (fit < text_end)
				{
					const char* next = ImMin(fit + char_length(static_cast<unsigned char>(*fit)), text_end);
					if (measure(line_start, next) > width)
						break;
					fit = next;
				}

				/* 一个字符都放不下：至少吃掉一个，免得死循环 */
				if (fit == line_start)
					fit = ImMin(line_start + char_length(static_cast<unsigned char>(*line_start)), text_end);

				/* 2) 在放得下的前缀里回退到最后一个自然断点；断点太靠前就不要了
				 *    （为了一个老早出现的分隔符而空掉大半行不划算），那样直接按前缀硬断。 */
				const char* line_end = fit;
				if (fit < text_end)
				{
					for (const char* probe = fit; probe > line_start; )
					{
						const char* char_start = probe - 1;
						while (char_start > line_start && (static_cast<unsigned char>(*char_start) & 0xC0) == 0x80)
							--char_start;

						if (is_break_char(static_cast<unsigned char>(*char_start))
							&& measure(line_start, probe) >= width * 0.5f)
						{
							line_end = probe;   /* 断在分隔符之后（`demo_skin_`） */
							break;
						}

						probe = char_start;
					}
				}

				/* 3) 行尾空白不显示（分隔符保留） */
				const char* visible_end = line_end;
				while (visible_end > line_start && (visible_end[-1] == ' ' || visible_end[-1] == '\t'))
					--visible_end;

				lines.emplace_back(line_start, visible_end);
				line_start = line_end;
				while (line_start < text_end && (*line_start == ' ' || *line_start == '\t'))
					++line_start;
			}

			ImDrawList* draw_list = ImGui::GetWindowDrawList();
			draw_list->PushClipRect(start, ImVec2(start.x + width, start.y + lines.size() * line_height), true);
			for (size_t i = 0; i < lines.size(); ++i)
			{
				const auto& [line_begin, line_end] = lines[i];
				const float text_width = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, line_begin, line_end).x;
				draw_list->AddText(font, font_size,
					ImVec2(start.x + ImMax((width - text_width) * 0.5f, 0.0f), start.y + i * line_height),
					color, line_begin, line_end);
			}
			draw_list->PopClipRect();
			ImGui::Dummy(ImVec2(width, ImMax(line_height, lines.size() * line_height)));
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

	/* 文件类型 -> 图标：与层级面板共用同一套矢量图标语言（单色、可着色、任意尺寸不糊） */
	Icons::Id EditorResourceBrowser::ResolveFileIcon(FileType type)
	{
		switch (type)
		{
		case FileType::Folder:   return Icons::Id::Directory;
		case FileType::Image:    return Icons::Id::FileImage;
		case FileType::Scene:    return Icons::Id::FileScene;
		case FileType::MtlGraph: return Icons::Id::FileMtlGraph;
		default:                 return Icons::Id::File;
		}
	}

	const char* EditorResourceBrowser::TypeFilterName(TypeFilter filter)
	{
		switch (filter)
		{
		case TypeFilter::All:      return "All types";
		case TypeFilter::Folder:   return "Folders";
		case TypeFilter::Image:    return "Images";
		case TypeFilter::Scene:    return "Scenes";
		case TypeFilter::MtlGraph: return "Mtl Graphs";
		default:                   return "All types";
		}
	}

	bool EditorResourceBrowser::MatchesTypeFilter(TypeFilter filter, FileType type)
	{
		switch (filter)
		{
		case TypeFilter::All:      return true;
		case TypeFilter::Folder:   return type == FileType::Folder;
		case TypeFilter::Image:    return type == FileType::Image;
		case TypeFilter::Scene:    return type == FileType::Scene;
		case TypeFilter::MtlGraph: return type == FileType::MtlGraph;
		}

		return true;
	}

	bool EditorResourceBrowser::PassesContentFilter(const FileNode& node) const
	{
		return MatchesTypeFilter(m_TypeFilter, node.Type)
			&& ContainsCaseInsensitive(node.FilePath.c_str(), m_FilterNeedle);
	}

	SharedPtr<DeviceTexture> EditorResourceBrowser::ThumbnailOf(FileNode& node)
	{
		if (node.Type != FileType::Image)
			return nullptr;

		/* 一个文件只尝试一次：失败（或贴图没加载出来）就当作没有，
		 * 由调用方回落到"图片文件"图标，免得每帧重试 */
		if (!node.ThumbnailResolved)
		{
			node.ThumbnailResolved = true;
			node.Thumbnail = TextureAssetManager::Instance().GetOrCreateTexture(
				PathToUtf8(g_AssetsPath / PathFromUtf8(node.FilePath)));

			if (node.Thumbnail != nullptr && !node.Thumbnail->IsLoaded())
				node.Thumbnail = nullptr;
		}

		return node.Thumbnail;
	}

	std::string EditorResourceBrowser::DisplayNodeName(const FileNode& node)
	{
		return node.ParentNode.expired() ? AssetsRootName() : node.FileName;
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

			m_RootFileNodeTree = CreateSharedPtr<FileNode>(PathToUtf8(g_AssetsPath), "", FileType::Folder, 0, -1);
			BuildFileNodeTree(m_RootFileNodeTree);
			UpdateDirectoryStamp();

			/* 文件操作（含撤销 / 重做）改过路径：先把要找回的路径对齐到新位置。
			 * 按前缀替换而不是全等 —— 改名的是一个目录时，它下面的路径也一起变了
			 * （当前目录就可能在它里面）。删除的项对齐后找不到，自然被丢掉。 */
			const auto remap = [this](const std::string& path)
			{
				if (m_RemapFrom.empty() || path.rfind(m_RemapFrom, 0) != 0)
					return path;
				return m_RemapTo + path.substr(m_RemapFrom.size());
			};

			const std::string remapped_current = remap(current_path);
			m_CurrentFileNode = remapped_current.empty() ? nullptr : FindNode(m_RootFileNodeTree, remapped_current);
			if (!m_CurrentFileNode)
				m_CurrentFileNode = m_RootFileNodeTree;   /* 未浏览过，或原目录已被删除 */

			/* 选中项同理：重建后节点指针全变，高亮比的是指针 —— 不按路径找回来的话
			 * 高亮会凭空消失，而底栏还显示着旧名字。已经不在树里的（被删掉）直接丢掉。 */
			std::vector<SharedPtr<FileNode>> reselected;
			reselected.reserve(m_Selection.size());
			for (const SharedPtr<FileNode>& selected : m_Selection)
			{
				if (SharedPtr<FileNode> found = FindNode(m_RootFileNodeTree, remap(selected->FilePath)))
					reselected.push_back(found);
			}

			m_Selection = std::move(reselected);
			m_SelectionAnchor = (m_SelectionAnchor != nullptr)
				? FindNode(m_RootFileNodeTree, remap(m_SelectionAnchor->FilePath)) : nullptr;

			/* 刚新建的那一项：建完就选中它，用户不用自己去找（否则"新建成功了但看不出来"） */
			if (!m_PendingSelectPath.empty())
			{
				if (SharedPtr<FileNode> created = FindNode(m_RootFileNodeTree, m_PendingSelectPath))
				{
					m_Selection = { created };
					m_SelectionAnchor = created;
				}
				m_PendingSelectPath.clear();
			}

			m_RemapFrom.clear();
			m_RemapTo.clear();

			/* 目录树按新节点重画，把当前目录重新露出来（展开祖先 + 滚到可见） */
			m_RevealCurrentNode = true;
		}

		/* 过滤结果每帧现算：过滤词与目录树两者都可能变 */
		FillBrowserFilter();

		/* 一个面板 = 顶部一行（图标 + 搜索 / 筛选）+ 左右两栏（目录树 | 内容区）+ 右栏底部的
		 * 路径栏。面板不带上下内边距（顶栏要贴面板上边）；左右内边距保留，只有右栏外沿例外 ——
		 * 越过一档、把滚动条顶到面板右缘（见 ShowBrowserBody）。两栏是子窗口，各自把上下内边距加回去。 */
		const ImVec2 pane_padding = ImGui::GetStyle().WindowPadding;
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pane_padding.x, 0.0f));

		ImGui::Begin(Panel::kResourceBrowser);
		{
			if (!m_CurrentFileNode)
				m_CurrentFileNode = m_RootFileNodeTree; /* 默认为根节点 */

			/* 弹层 ID 在面板根作用域上算一次：右键菜单在内容栏与目录树两个子窗口里，
			 * 各自算 ID 会得到不同的值，弹层就对不上了（见 DrawAssetOperationPopups）。 */
			m_PopupNewFolder = ImGui::GetID("##NewAssetFolder");
			m_PopupRename = ImGui::GetID("##RenameAsset");
			m_PopupDelete = ImGui::GetID("##DeleteAssets");

			/* 底栏（分隔线 + 一行）钉在面板底边上，主体只要让出它的高度 ——
			 * 这个数由我们自己定（栏高 + 1px 分隔线），不用猜 ImGui::Separator() 吃掉多少，
			 * 因此既不会少一像素长出滚动条，也不会多留一条空带。 */
			const float reserved_footer = FooterBandHeight();

			ShowBrowserTopBar();
			ShowBrowserBody(reserved_footer, pane_padding.y);
			ShowBrowserFooter();

			/* 文件操作弹层画在面板根：三件都在右键菜单里点开 */
			DrawAssetOperationPopups();
		}
		ImGui::End();

		ImGui::PopStyleVar();
	}

	/* 主体：左右两栏 —— 左「Folders」目录树、中间可拖的分隔条、右内容区。
	 * 两栏都是子窗口：内容区的缩略图网格用的是 ImGui::Columns，而 Columns 以「当前窗口」
	 * 为界（窗口的 WorkRect / Indent），不套子窗口的话它会横跨整个面板、压到目录树上。 */
	void EditorResourceBrowser::ShowBrowserBody(float reserved_footer, float pane_padding)
	{
		PROFILE_FUNCTION();

		const ImGuiStyle& style = ImGui::GetStyle();

		/* 主体的高度按绝对几何算：从当前光标一直到底栏上边（面板底边 - 底栏高度）。
		 * 不用 GetContentRegionAvail()：顶栏与底栏都是贴边画的，面板的内边距/内容区
		 * 那套账不参与，两头的数用绝对量才对得上（否则主体会短一截，栏底空出一条）。 */
		ImGuiWindow* window = ImGui::GetCurrentWindow();
		const float panel_bottom = window->Pos.y + window->Size.y;
		const ImVec2 body(ImGui::GetContentRegionAvail().x,
			ImMax(panel_bottom - reserved_footer - ImGui::GetCursorScreenPos().y,
				ImGui::GetFrameHeight() * 2.0f));
		const float body_height = body.y;
		const float bar_width = ImMax(style.ItemSpacing.x, 4.0f);

		/* 左栏宽度 = 可用宽度 × 比例，两端都钳住：见 kMin* 常量 */
		const float max_tree = ImMax(body.x - kMinContentWidth - bar_width, kMinTreePaneWidth);
		const float tree_width = ImClamp(m_TreePaneRatio * body.x, kMinTreePaneWidth, max_tree);

		/* ---- 左栏：目录树 ----
		 * 把左子窗口的滚动禁掉，卡身撑满整栏、滚动交给卡身里的子窗口。子窗口带 AlwaysUseWindowPadding，
		 * 横向内边距取 CardPad（卡背景贴栏两边）、上下清零；留白交给卡片自己提供。 */
		const float card_pad = PanelChrome::CardPad();
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(card_pad, 0.0f));
		ImGui::BeginChild("##BrowserTreePane", ImVec2(tree_width, body_height), false,
			ImGuiWindowFlags_AlwaysUseWindowPadding | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
		{
			const PanelChrome::Card card = PanelChrome::BeginCard("Folders", Icons::Id::Directory);
			if (card.Open)
			{
				/* 卡身吃掉卡片下方的全部高度：按 EndCard 的算法反推（卡底 = 内容底 + card.Pad），
				 * 卡身高就取"可用高度 - card.Pad"。 */
				const float tree_height = ImMax(
					ImGui::GetContentRegionAvail().y - card.Pad,
					ImGui::GetFrameHeight());

				ImGui::BeginChild("##FolderTree", ImVec2(0.0f, tree_height));
				DrawFolderNode(m_RootFileNodeTree);
				ImGui::EndChild();

				/* 树画完，本次"把当前目录露出来"的请求已兑现 */
				m_RevealCurrentNode = false;
			}
			PanelChrome::EndCard(card);
		}
		ImGui::EndChild();
		ImGui::PopStyleVar();

		/* ---- 分隔条：拖这里改左右比例 ---- */
		ImGui::SameLine(0.0f, 0.0f);
		PanelChrome::HorizontalSplitter(m_TreePaneRatio, body.x,
			kMinTreePaneRatio, kMaxTreePaneRatio, bar_width, body_height);

		/* ---- 右栏：内容区 ----
		 * 同样要 AlwaysUseWindowPadding：缩略图与表格才不贴着分隔条与面板边线 */
		ImGui::SameLine(0.0f, 0.0f);
		/* 这里要传 ImVec2：函数参数 `pane_padding` 是 float（只表示上下内边距），
		 * 传它会被隐式选到 PushStyleVar 的 float 重载，运行时直接断言挂掉。 */
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, style.WindowPadding);
		ImGui::BeginChild("##BrowserContentPane", ImVec2(ImGui::GetContentRegionAvail().x, body_height), false,
			ImGuiWindowFlags_AlwaysUseWindowPadding);
		ShowBrowserContent();
		ImGui::EndChild();
		ImGui::PopStyleVar();
	}

	void EditorResourceBrowser::DrawFolderNode(const SharedPtr<FileNode>& node)
	{
		for (const auto& child_node : node->ChildNodes)
		{
			if (child_node->Type != FileType::Folder)
				continue;

			if (m_VisibleNodes.count(child_node.get()) == 0)
				continue;

			/* ID 用相对路径而不是节点指针：目录树重建后指针会变，用指针会让展开状态
			 * 在"编辑器外新增一个文件"之后整个塌回去 */
			ImGui::PushID(child_node->FilePath.c_str());

			/* 当前目录的祖先链要展开，否则"跳过去了但看不见"；
			 * 过滤时也全展开 —— 目录名不含过滤词、只是子孙命中的目录同样要能看到。 */
			const bool is_current = (child_node == m_CurrentFileNode);
			if (!m_FilterNeedle.empty() || (m_RevealCurrentNode && IsAncestorOrSelf(child_node, m_CurrentFileNode)))
				ImGui::SetNextItemOpen(true, ImGuiCond_Always);

			ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow
				| ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanFullWidth;
			if (is_current)
				flags |= ImGuiTreeNodeFlags_Selected; /* 选中高亮由 ImGui 画，不必自绘整行 */

			const bool is_opened = ImGui::TreeNodeEx("##node", flags, "%s", "");

			/* 点目录名（不是点展开箭头）= 切换当前目录 */
			if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
				SetCurrentNode(child_node);

			/* 目录树里也能新建 / 改名 / 删除：改名的目标是点到的这一项，
			 * 与内容区的选中项无关（右键不动选中项 —— 只想改个目录名，不该把选择清掉） */
			if (ImGui::BeginPopupContextItem())
			{
				DrawAssetContextMenuItems(child_node->FilePath, true);
				ImGui::EndPopup();
			}

			/* 只在"跳过来"的那一帧滚过去：每帧都滚就再也拖不动滚动条了 */
			if (is_current && m_RevealCurrentNode)
				ImGui::SetScrollHereY(0.5f);

			PanelChrome::DrawTreeRowLabel(ResolveFileIcon(child_node->Type), child_node->FileName);

			if (is_opened)
			{
				DrawFolderNode(child_node);
				ImGui::TreePop();
			}

			ImGui::PopID();
		}
	}

	/* 切换当前目录：置上「下次画目录树时把它露出来」。
	 * 选中项属于上一个目录，一并清掉。 */
	void EditorResourceBrowser::SetCurrentNode(const SharedPtr<FileNode>& node)
	{
		if (node == nullptr || node == m_CurrentFileNode)
			return;

		m_CurrentFileNode = node;
		ClearSelection();
		m_RevealCurrentNode = true;
	}

	/* ---- 选择（内容区，可多选）：全部入口都在这一节，绘制代码只读不写 ---- */

	bool EditorResourceBrowser::IsNodeSelected(const FileNode* node) const
	{
		for (const SharedPtr<FileNode>& selected : m_Selection)
		{
			if (selected.get() == node)
				return true;
		}

		return false;
	}

	void EditorResourceBrowser::SelectSingle(const SharedPtr<FileNode>& node)
	{
		m_Selection.clear();
		if (node != nullptr)
			m_Selection.push_back(node);

		m_SelectionAnchor = node;
	}

	void EditorResourceBrowser::ToggleSelection(const SharedPtr<FileNode>& node)
	{
		if (node == nullptr)
			return;

		for (auto it = m_Selection.begin(); it != m_Selection.end(); ++it)
		{
			if (it->get() == node.get())
			{
				m_Selection.erase(it);
				m_SelectionAnchor = node;   /* 锚点仍是刚点的这一项：接着 Shift 点选从它算 */
				return;
			}
		}

		m_Selection.push_back(node);
		m_SelectionAnchor = node;
	}

	void EditorResourceBrowser::SelectRangeTo(const SharedPtr<FileNode>& node,
	                                          const std::vector<SharedPtr<FileNode>>& visible)
	{
		const auto index_of = [&visible](const FileNode* target)
		{
			for (size_t i = 0; i < visible.size(); ++i)
			{
				if (visible[i].get() == target)
					return static_cast<int>(i);
			}

			return -1;
		};

		const int to = index_of(node.get());
		const int from = (m_SelectionAnchor != nullptr) ? index_of(m_SelectionAnchor.get()) : -1;

		/* 锚点不在当前列表里（刚换过目录 / 被过滤掉了）就当这次是第一次点选 */
		if (to < 0 || from < 0)
		{
			SelectSingle(node);
			return;
		}

		m_Selection.clear();
		for (int i = ImMin(from, to); i <= ImMax(from, to); ++i)
			m_Selection.push_back(visible[static_cast<size_t>(i)]);

		/* 锚点不动：连续 Shift 点选时范围始终从同一个锚点算起 */
	}

	void EditorResourceBrowser::ApplyClickSelection(const SharedPtr<FileNode>& node,
	                                                 const std::vector<SharedPtr<FileNode>>& visible)
	{
		const ImGuiIO& io = ImGui::GetIO();

		if (io.KeyShift)
			SelectRangeTo(node, visible);
		else if (io.KeyCtrl || io.KeySuper)
			ToggleSelection(node);
		else
			SelectSingle(node);
	}

	void EditorResourceBrowser::ClearSelection()
	{
		m_Selection.clear();
		m_SelectionAnchor = nullptr;
	}

	void EditorResourceBrowser::HandleSelectionShortcuts(const std::vector<SharedPtr<FileNode>>& visible)
	{
		const ImGuiIO& io = ImGui::GetIO();

		/* 光看悬停还不够：键盘焦点可能在搜索框里，所以正在输入文字（WantTextInput）时一律不抢键；
		 * 文件操作的弹层开着时更不抢 —— 里面就有输入框与按钮，Delete / Esc 会误伤 */
		if (!ImGui::IsWindowHovered() || io.WantTextInput)
			return;

		/* 弹层由面板根作用域上的 ID 开的，这里也用那套 ID 判"开着没有" */
		if (ImGui::IsPopupOpen(m_PopupNewFolder, ImGuiPopupFlags_None)
			|| ImGui::IsPopupOpen(m_PopupRename, ImGuiPopupFlags_None)
			|| ImGui::IsPopupOpen(m_PopupDelete, ImGuiPopupFlags_None))
			return;

		if ((io.KeyCtrl || io.KeySuper) && ImGui::IsKeyPressed(ImGuiKey_A))
		{
			m_Selection = visible;
			m_SelectionAnchor = visible.empty() ? nullptr : visible.front();
		}

		if (ImGui::IsKeyPressed(ImGuiKey_Escape))
			ClearSelection();

		/* Delete / Backspace = 删除选中（先弹确认）。macOS 上标 delete 的键其实就是 Backspace，
		 * 两个都收。重命名没有快捷键（ImGui 没有 F2 枚举），走右键菜单。 */
		if (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace))
		{
			std::vector<std::string> paths;
			paths.reserve(m_Selection.size());
			for (const SharedPtr<FileNode>& node : m_Selection)
				paths.push_back(node->FilePath);

			if (!paths.empty())
				OpenDeletePopup(std::move(paths));
		}
	}

	/* ---- 文件操作：新建文件夹 / 重命名 / 删除 ----
	 * 三件都做成 ICommand 走编辑历史，于是 Ctrl+Z / Ctrl+Y、菜单与工具栏的撤销 / 重做
	 * 对它们同样有效 —— 这也是"资源管理器支持撤销"的全部意义：不是另开一套历史。 */

	std::filesystem::path EditorResourceBrowser::AbsoluteAssetPath(const std::string& relative_path)
	{
		return g_AssetsPath / PathFromUtf8(relative_path);
	}

	void EditorResourceBrowser::ExecuteCommand(UniquePtr<ICommand> command)
	{
		if (command == nullptr)
			return;

		if (m_CommandSink)
		{
			/* 交给编辑历史：撤销 / 重做（含工具栏按钮与 Ctrl+Z）都归它管 */
			m_CommandSink(std::move(command));
			return;
		}

		/* 没有 sink（面板单独跑 / headless 测试）：直接执行 —— 功能对，只是没有历史 */
		command->Do();
	}

	void EditorResourceBrowser::OnAssetPathChanged(const std::string& from, const std::string& to)
	{
		/* 撤销 / 重做也要立刻看到结果：不等 0.5s 的目录轮询 */
		m_IsDirty = true;

		if (from.empty())
		{
			/* 新建：重建后选中新建的那一项 */
			m_PendingSelectPath = to;
			return;
		}

		if (to.empty())
		{
			/* 删除：重建时按路径找不到就丢掉（选中项与当前目录都走同一条找回逻辑） */
			return;
		}

		m_RemapFrom = from;
		m_RemapTo = to;
	}

	void EditorResourceBrowser::OpenNewFolderPopup(const std::string& parent_path)
	{
		m_PendingParentPath = parent_path;
		m_PendingRenamePath.clear();

		snprintf(m_NameBuffer, sizeof(m_NameBuffer), "%s", "New Folder");

		ImGui::OpenPopup(m_PopupNewFolder);
	}

	void EditorResourceBrowser::OpenRenamePopup(const std::string& path)
	{
		const std::filesystem::path name = PathFromUtf8(path).filename();

		m_PendingRenamePath = path;
		m_PendingParentPath.clear();

		snprintf(m_NameBuffer, sizeof(m_NameBuffer), "%s", PathToUtf8(name).c_str());

		ImGui::OpenPopup(m_PopupRename);
	}

	void EditorResourceBrowser::OpenDeletePopup(std::vector<std::string> paths)
	{
		m_PendingDeletePaths = std::move(paths);

		ImGui::OpenPopup(m_PopupDelete);
	}

	void EditorResourceBrowser::DrawAssetOperationPopups()
	{
		DrawNamePopup(m_PopupNewFolder, false);
		DrawNamePopup(m_PopupRename, true);

		/* ---- 删除确认 ---- */
		if (ImGui::BeginPopupEx(m_PopupDelete,
				ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
		{
			ImGui::TextUnformatted(m_PendingDeletePaths.size() == 1
				? "Delete this item?" : "Delete these items?");

			/* 说清"删到哪儿去了"：撤销能用 Ctrl+Z，但 File 菜单 / 工具栏的 Undo 也一样管用 */
			ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::TextDim);
			ImGui::TextUnformatted("移到项目回收站 .helios-trash（Assets 旁边）");
			ImGui::TextUnformatted("可用 Ctrl+Z 或工具栏的 Undo 还原");
			ImGui::PopStyleColor();

			constexpr size_t kMaxListed = 8;
			for (size_t index = 0; index < m_PendingDeletePaths.size() && index < kMaxListed; ++index)
				ImGui::BulletText("%s", PathToUtf8(PathFromUtf8(m_PendingDeletePaths[index]).filename()).c_str());

			if (m_PendingDeletePaths.size() > kMaxListed)
				ImGui::BulletText("... +%zu more", m_PendingDeletePaths.size() - kMaxListed);

			ImGui::Separator();

			if (ImGui::Button("Delete", ImVec2(kPopupButtonSize, 0.0f)))
			{
				std::vector<std::filesystem::path> paths;
				paths.reserve(m_PendingDeletePaths.size());
				for (const std::string& path : m_PendingDeletePaths)
					paths.push_back(AbsoluteAssetPath(path));

				/* 一整批只留一条历史：撤销一次全部回来 */
				ExecuteCommand(CreateUniquePtr<DeleteAssetsCommand>(this, AssetTrashRoot(), std::move(paths)));
				m_PendingDeletePaths.clear();
				ImGui::CloseCurrentPopup();
			}

			ImGui::SameLine();
			if (ImGui::Button("Cancel", ImVec2(kPopupButtonSize, 0.0f)))
			{
				m_PendingDeletePaths.clear();
				ImGui::CloseCurrentPopup();
			}

			ImGui::EndPopup();
		}
	}

	/* 名字输入弹层：新建文件夹与重命名共用（只有文案与"确认后做什么"不同） */
	void EditorResourceBrowser::DrawNamePopup(ImGuiID popup_id, bool is_rename)
	{
		if (!ImGui::BeginPopupEx(popup_id, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
			return;

		ImGui::TextUnformatted(is_rename ? "Rename" : "New Folder");

		/* 名字非法时把原因写在输入框下面，并禁用确认按钮 —— 别让用户点了没反应 */
		const std::string typed = m_NameBuffer;
		const std::string error = AssetNameError(typed);

		/* 目标路径：重命名 = 同目录下换个名字；新建 = 父目录下新建 */
		const std::filesystem::path pending_source = is_rename
			? AbsoluteAssetPath(m_PendingRenamePath) : std::filesystem::path();
		const std::filesystem::path parent = is_rename
			? pending_source.parent_path() : AbsoluteAssetPath(m_PendingParentPath);
		const std::filesystem::path requested = parent / PathFromUtf8(typed);
		const std::filesystem::path target = MakeUniquePath(parent, typed);

		ImGui::SetNextItemWidth(kPopupInputWidth);

		/* 弹层一出现就选好名字，直接打字即可；只在出现的那一帧抢焦点，
		 * 否则每帧都把焦点抢回输入框，底下的按钮就点不动了。 */
		if (ImGui::IsWindowAppearing())
			ImGui::SetKeyboardFocusHere();

		const bool submitted = ImGui::InputText("##name", m_NameBuffer, sizeof(m_NameBuffer),
			ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_EnterReturnsTrue);

		if (!error.empty())
		{
			ImGui::Text("名字不合法：%s", error.c_str());
		}
		else if (target != requested)
		{
			/* 同名被占了：说清会建成什么名字，免得用户以为没生效 */
			ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::TextDim);
			ImGui::Text("%s 已被占用，将创建为 %s", typed.c_str(), PathToUtf8(target.filename()).c_str());
			ImGui::PopStyleColor();
		}

		ImGui::Separator();

		const bool can_confirm = error.empty();

		ImGui::BeginDisabled(!can_confirm);
		const bool pressed = ImGui::Button(is_rename ? "Rename" : "Create", ImVec2(kPopupButtonSize, 0.0f)) || submitted;
		ImGui::EndDisabled();

		if (pressed && can_confirm)
		{
			if (is_rename)
			{
				ExecuteCommand(CreateUniquePtr<RenameAssetCommand>(this, pending_source, target));
			}
			else
			{
				ExecuteCommand(CreateUniquePtr<CreateAssetFolderCommand>(this, target));
			}

			ImGui::CloseCurrentPopup();
		}

		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(kPopupButtonSize, 0.0f)))
			ImGui::CloseCurrentPopup();

		ImGui::EndPopup();
	}

	/* 右键菜单的条目：结点自己的操作（内容区的网格 / 列表与目录树共用同一份） */
	void EditorResourceBrowser::DrawAssetContextMenuItems(const std::string& path, bool is_folder)
	{
		const std::filesystem::path absolute = AbsoluteAssetPath(path);

		if (ImGui::MenuItem("Show in file explorer"))
		{
			std::filesystem::path shown = absolute;
			shown = std::filesystem::absolute(shown).make_preferred();
			const std::string shown_utf8 = PathToUtf8(shown);
			OpenFileExplorer(shown_utf8.c_str());
		}

		if (is_folder)
		{
			if (ImGui::MenuItem("New Folder"))
				OpenNewFolderPopup(path);
		}

		/* 重命名作用于点到的这一项（右键菜单本来就是"这一项"的菜单）：
		 * 多选时也只改它一个，其余选中项不受影响 —— 不用先缩成单选才好操作 */
		if (ImGui::MenuItem("Rename"))
			OpenRenamePopup(path);

		if (ImGui::MenuItem("Delete", "Del"))
			OpenDeletePopup({ path });
	}

	void EditorResourceBrowser::DrawBackgroundContextMenuItems()
	{
		const std::string current = (m_CurrentFileNode != nullptr) ? m_CurrentFileNode->FilePath : std::string();

		if (ImGui::MenuItem("New Folder"))
			OpenNewFolderPopup(current);

		if (ImGui::MenuItem("Show in file explorer"))
		{
			std::filesystem::path shown = std::filesystem::absolute(AbsoluteAssetPath(current)).make_preferred();
			const std::string shown_utf8 = PathToUtf8(shown);
			OpenFileExplorer(shown_utf8.c_str());
		}
	}

	bool EditorResourceBrowser::IsAncestorOrSelf(const SharedPtr<FileNode>& node, const SharedPtr<FileNode>& target)
	{
		for (SharedPtr<FileNode> current = target; current != nullptr; current = current->ParentNode.lock())
		{
			if (current == node)
				return true;
		}

		return false;
	}

	/* 顶部一行 = 原来的「标题行」+「控制行」合并：左端搜索框，右端四枚图标按钮（回到上次
	 * 路径 / 重进路径 / 类型筛选 / 新建资源）。当前目录名不在这行显示（路径栏说得更清楚）。 */

	/* 顶栏：左端搜索框，右端「后退 / 前进 / 筛选 / 新建」四枚图标按钮（新建贴最右，依次
	 * 向左）。放不下就压窄搜索框（下限 80；固定阈值总会在某个宽度上算错、控件重叠）；右端
	 * 四枚不参与退让。筛选是纯图标按钮，点击弹类型单选菜单，生效时高亮。 */
	void EditorResourceBrowser::ShowBrowserTopBar()
	{
		PROFILE_FUNCTION();

		const ImGuiStyle& style = ImGui::GetStyle();

		/* 顶栏就画在面板的客户端顶边上（面板本身不带上下内边距，见 OnImGuiRenderer），
		 * 这一栏的高因此正好等于里面的控件高；横向仍按内容区左端对齐。 */
		const PanelChrome::HeaderRow row = PanelChrome::BeginHeaderRow(Icons::Id::Directory);
		const float control_height = ImGui::GetFrameHeight();
		const float control_y = row.Min.y + (row.Height - control_height) * 0.5f;
		const float gap = style.ItemInnerSpacing.x;

		/* ---- 尺寸 ----
		 * 类型筛选的宽度按 BeginCombo 自己的账算：文字从「控件左端 + FramePadding.x」起、
		 * 到「控件右端 − 箭头区(GetFrameHeight)」为止，前面还要给前置图标留一档 ——
		 * 少算哪一笔，最长的类型名就会被箭头区裁掉一截（"Mtl Graphs" 是最长的那个）。 */
		const float type_width = ImGui::CalcTextSize("Mtl Graphs").x + control_height * 1.5f;
		/* ---- 左端：搜索框（贴左端）----
		 * 搜索框不撑满：够用就好，宽度多出来的部分留给中间那一段留白，
		 * 也不至于窄到看不清输入的内容；放不下时跟着可用宽度缩，下限 80。 */
		const float search_preferred = 220.0f;
		const float search_min = 80.0f;

		bool show_type = true;
		float search_width = search_preferred;

		for (int step = 0; step < 3; ++step)
		{
			show_type = (step < 2);
			search_width = (step == 0) ? search_preferred : search_min;

			const float needed = (show_type ? type_width + gap : 0.0f) + search_width;
			if (needed <= row.Right - row.TitleX)
				break;
		}

		const float search_x = row.Right - search_width;
		const float type_x = search_x - gap - type_width;

		/* 类型筛选：与文本过滤一起决定内容区显示什么（计数用的是同一条判据）。
		 * 只影响右栏 —— 左栏是导航，把要进去的目录藏掉就没法用了。 */
		if (show_type)
		{
			ImGui::SetCursorScreenPos(ImVec2(type_x, control_y));
			ImGui::SetNextItemWidth(type_width);
			if (ImGui::BeginCombo("##AssetTypeFilter", TypeFilterName(m_TypeFilter)))
			{
				for (int i = 0; i < static_cast<int>(TypeFilter::COUNT); ++i)
				{
					const auto filter = static_cast<TypeFilter>(i);
					const bool selected = (filter == m_TypeFilter);

					if (ImGui::Selectable(TypeFilterName(filter), selected))
						m_TypeFilter = filter;

					if (selected)
						ImGui::SetItemDefaultFocus();
				}

				ImGui::EndCombo();
			}

			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Filter by asset type");
		}

		/* ---- 搜索框：贴最右端，类型筛选紧挨在它左边 ---- */
		ImGui::SetCursorScreenPos(ImVec2(search_x, control_y));
		ImGui::SetNextItemWidth(search_width);
		ImGui::InputTextWithHint("##ResourceFilter", "Search assets...", m_Filter, sizeof(m_Filter));

		PanelChrome::EndHeaderRow(row);
	}

	/* 底部栏：上一级 + 当前路径（面包屑，每级可点，级间用矢量三角）+ 选中的文件，
	 * 右端是统计数与缩放的滑动条（从顶栏搬下来）。
	 * 路径过长时从最前面开始省略：越靠后越接近当前位置，越有用。 */
	void EditorResourceBrowser::ShowBrowserFooter()
	{
		PROFILE_FUNCTION();

		const ImGuiStyle& style = ImGui::GetStyle();
		ImGuiWindow* window = ImGui::GetCurrentWindow();

		/* 定位：贴所在栏（右栏）的下边（分隔线在行的正上方），连下内边距一起用掉 ——
		 * 与顶栏对称，栏的高都等于控件高，不额外留空带。 */
		const float panel_bottom = window->Pos.y + window->Size.y;
		const float footer_top = panel_bottom - ImGui::GetFrameHeight();
		ImGui::SetCursorScreenPos(ImVec2(window->DC.CursorStartPos.x, footer_top));

		/* 上边线自绘而不是用 ImGui::Separator()：Separator 会吃掉「1px + 行距」，
		 * 于是这一栏就比控件高出一截；自绘的线不参与布局，栏高 = 行高 + 1px，说得清也量得准。 */
		ImGui::GetWindowDrawList()->AddLine(
			ImVec2(window->DC.CursorStartPos.x, footer_top - kBarDivider * 0.5f),
			ImVec2(window->DC.CursorStartPos.x + ImGui::GetContentRegionAvail().x,
				footer_top - kBarDivider * 0.5f),
			ImGui::GetColorU32(EditorTheme::Token::Border));

		const PanelChrome::HeaderRow row = PanelChrome::BeginHeaderRow(Icons::Id::None);
		const float control_height = ImGui::GetFrameHeight();
		const float control_y = row.Min.y + (row.Height - control_height) * 0.5f;
		const float gap = style.ItemInnerSpacing.x;

		/* ---- 右端的缩放滑动条 + 统计数 ----
		 * 绝对定位在右端、不参与这一行的排版（面包屑可以省，这俩不行）。滑条是「圆点在细线上」
		 * 那版：没有控件框、当前值就用圆点表示、数值走 tooltip。 */
		char count_text[48] = {};
		{
			int total_count = 0;
			int matched_count = 0;
			for (const auto& child_node : m_CurrentFileNode->ChildNodes)
			{
				++total_count;
				if (PassesContentFilter(*child_node))
					++matched_count;
			}

			if (matched_count == total_count)
				snprintf(count_text, sizeof(count_text), "%d items", total_count);
			else
				snprintf(count_text, sizeof(count_text), "%d / %d", matched_count, total_count);
		}
		const float count_width = ImGui::CalcTextSize(count_text).x;
		const float return_width = control_height;
		/* 路径那一侧至少要装下：上一级按钮 + 一段面包屑 + 一段选中项摘要 */
		const float path_min = return_width + gap + kMinCrumbWidth + gap * 2.0f + kMinSelectionWidth;
		const float group_gap = gap * 2.0f;                          /* 路径与右端那组之间的空隙 */
		const float bar_span = row.Right - row.Min.x;

		const bool show_count = (bar_span - path_min - group_gap - count_width - gap - kZoomSliderWidth >= 0.0f);
		const float zoom_width = ImClamp(
			bar_span - path_min - group_gap - (show_count ? count_width + gap : 0.0f),
			kMinZoomSliderWidth, kZoomSliderWidth);

		const float zoom_x = row.Right - zoom_width;
		const float count_x = zoom_x - gap - count_width;
		const float right_group_x = show_count ? count_x : zoom_x;

		/* 当前目录的祖先链（根 → 当前） */
		std::vector<SharedPtr<FileNode>> chain;
		for (SharedPtr<FileNode> node = m_CurrentFileNode; node != nullptr; node = node->ParentNode.lock())
			chain.push_back(node);

		std::reverse(chain.begin(), chain.end());

		/* 面包屑从最左开始排；右边还要放（可选的）选中项摘要与右端那组。
		 * 选中项可能是一堆：单个直接给文件名，多个给「N selected」（名字列在 tooltip 里）。 */
		std::string selection_text;
		if (m_Selection.size() == 1)
			selection_text = m_Selection.front()->FileName;
		else if (m_Selection.size() > 1)
			selection_text = std::to_string(m_Selection.size()) + " selected";

		const float selected_width = selection_text.empty()
			? 0.0f : ImGui::CalcTextSize(selection_text.c_str()).x + gap * 2.0f;
		const float selection_right = right_group_x - gap;            /* 摘要最多画到这儿 */
		const float crumb_right = selection_right - selected_width;   /* 面包屑再让出它 */

		/* 放不下就从最前面省：先量总宽，再决定从第几级开始画 */
		const float separator_size = CrumbSeparatorSize();

		const auto crumb_width = [&](const SharedPtr<FileNode>& node)
		{
			return ImGui::CalcTextSize(DisplayNodeName(*node).c_str()).x + style.FramePadding.x * 2.0f
				+ separator_size + gap;
		};

		float crumbs_width = 0.0f;
		for (const SharedPtr<FileNode>& node : chain)
			crumbs_width += crumb_width(node);

		size_t first = 0;
		while (first + 1 < chain.size() && crumbs_width > crumb_right - row.Min.x - return_width - gap)
		{
			crumbs_width -= crumb_width(chain[first]);
			++first;
		}

		/* ---- 上一级（贴最左；已在根目录时置灰而不是隐藏，按钮位置才不跳动） ---- */
		const bool at_root = (*m_CurrentFileNode) == (*m_RootFileNodeTree);
		if (at_root)
		{
			ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
			ImGui::PushStyleVar(ImGuiStyleVar_Alpha, style.Alpha * style.DisabledAlpha);
		}

		if (Icons::IconButton(Icons::Id::Return, ImVec2(return_width, return_width), false, "Parent folder"))
			SetCurrentNode(m_CurrentFileNode->ParentNode.lock());

		if (at_root)
		{
			ImGui::PopStyleVar();
			ImGui::PopItemFlag();
		}

		/* ---- 面包屑 ----
		 * 不要压 FramePadding.y：Selectable 会走"基线对齐"分支，整条低半格（实测 5px），底栏就
		 * 比控件高 —— 高度本来就被 size_arg 定死了，压它没意义。 */
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, 0.0f));

		/* 放 Selectable 之前先清掉"基线偏移"：它会把你整块下移 CurrLineTextBaseOffset（前面是
		 * 带边框按钮时 = FramePadding.y），面包屑就比同行按钮低半格。这一行是"横条"，按整行对齐。 */
		const auto continue_row = [&]()
		{
			ImGui::SameLine(0.0f, gap);
			ImGui::GetCurrentWindow()->DC.CurrLineTextBaseOffset = 0.0f;
		};

		/* 级间分隔符：矢量三角（与卡片折叠箭头同一套画法，朝右）。
		 * 原来打的是一个 "/" —— 三角更像个"往下一级"的箭头，也不依赖字体里有没有字形。 */
		const auto draw_separator = [&]()
		{
			continue_row();
			ImGui::Dummy(ImVec2(separator_size, control_height));

			const ImVec2 min = ImGui::GetItemRectMin();
			const ImVec2 max = ImGui::GetItemRectMax();
			PanelChrome::DrawDisclosureArrow(ImGui::GetWindowDrawList(),
				ImVec2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f),
				separator_size, false, ImGui::GetColorU32(EditorTheme::Token::TextDim));
		};

		for (size_t i = first; i < chain.size(); ++i)
		{
			const SharedPtr<FileNode>& node = chain[i];
			const bool is_current = (node == m_CurrentFileNode);

			if (i != first)
			{
				draw_separator();
			}
			else if (first > 0)
			{
				/* 前面被省略了：用省略号说明这段路径不是从根开始的。注意：它是这一行的第一个控件，
				 * 位置得自己钉到行首；少了这一步 "..." 会掉到下一行、这栏变两行高（窄面板下才暴露）。 */
				continue_row();
				ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::TextDim);
				ImGui::TextUnformatted("...");
				ImGui::PopStyleColor();

				draw_separator();
			}

			continue_row();
			ImGui::PushID(node.get());

			/* 选中态（当前目录）用 Selectable 的选中高亮，其余可点跳转 */
			ImGui::PushStyleColor(ImGuiCol_Text,
				is_current ? EditorTheme::Token::Text : EditorTheme::Token::TextDim);

			const std::string name = DisplayNodeName(*node);
			if (ImGui::Selectable(name.c_str(), is_current, ImGuiSelectableFlags_None,
					ImVec2(ImGui::CalcTextSize(name.c_str()).x, control_height)))
			{
				SetCurrentNode(node);
			}

			ImGui::PopStyleColor();

			if (ImGui::IsItemHovered() && !is_current)
			{
				/* 悬停给完整相对路径：面包屑会被省略，tooltip 补全 */
				ImGui::SetTooltip("%s", node->FilePath.empty() ? name.c_str() : node->FilePath.c_str());
			}

			ImGui::PopID();
		}

		ImGui::PopStyleVar();

		/* ---- 选中的项（不属于目录链，单独一段强调色文本） ----
		 * 面包屑已经把它那份宽度预留出来了，正常情况一定放得下；只有"连一段面包屑都
		 * 撑满"的极窄面板才会轮到它被挤掉 —— 那时宁可整段不画，也不留两三个字母的碎片。 */
		if (!selection_text.empty()
			&& selection_right - (ImGui::GetCursorScreenPos().x + gap * 2.0f) >= kMinSelectionWidth)
		{
			ImGui::SameLine(0.0f, gap * 2.0f);
			ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::Accent);

			const float selected_width_clip = ImMax(selection_right - ImGui::GetCursorScreenPos().x, 1.0f);
			ImGui::PushClipRect(ImGui::GetCursorScreenPos(),
				ImVec2(ImGui::GetCursorScreenPos().x + selected_width_clip, row.Min.y + row.Height), true);
			ImGui::TextUnformatted(selection_text.c_str());
			ImGui::PopClipRect();

			/* 多选时 tooltip 把名字列出来（太长就截断）：否则想知道选了谁只能一个个点 */
			if (m_Selection.size() > 1 && ImGui::IsItemHovered())
			{
				constexpr size_t kMaxListed = 16;

				ImGui::BeginTooltip();
				for (size_t i = 0; i < m_Selection.size() && i < kMaxListed; ++i)
					ImGui::TextUnformatted(m_Selection[i]->FileName.c_str());

				if (m_Selection.size() > kMaxListed)
					ImGui::Text("... +%zu more", m_Selection.size() - kMaxListed);

				ImGui::EndTooltip();
			}

			ImGui::PopStyleColor();
		}

		/* ---- 右端的统计数 + 缩放滑动条 ----
		 * 绝对定位在右端（与顶栏里的控件同一套摆法），不参与这一行的排版：
		 * 面包屑是可省的、它俩不是；贴右端也保证拖动时滑动条不跟着计数文字一起挪。 */
		if (show_count)
		{
			ImGui::SetCursorScreenPos(ImVec2(count_x, control_y));
			ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::TextDim);
			ImGui::TextUnformatted(count_text);
			ImGui::PopStyleColor();
		}

		ImGui::SetCursorScreenPos(ImVec2(zoom_x, control_y));
		ImGui::PushID("AssetZoom");
		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);

		ImGui::SetNextItemWidth(zoom_width);
		ImGui::SliderFloat("##ThumbnailSize", &m_ThumbnailSize, kMinThumbnailSize, kMaxThumbnailSize, "%.0f");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Thumbnail size (px) — 拖到 32 以下内容区退回详细列表");

		ImGui::PopStyleVar();
		ImGui::PopID();

		PanelChrome::EndHeaderRow(row, false);
	}

	/* 主体：缩略图够大就用网格，否则退回详细列表 */
	void EditorResourceBrowser::ShowBrowserContent()
	{
		PROFILE_FUNCTION();

		const float panel_width = ImGui::GetContentRegionAvail().x;

		/* 这一帧的可见项（顺序 = 显示顺序）：网格 / 列表照它画，范围选择、全选、计数也用它 ——
		 * 只有一个判据（PassesContentFilter），四处各过滤一遍的话迟早会走样。 */
		std::vector<SharedPtr<FileNode>> visible;
		visible.reserve(m_CurrentFileNode->ChildNodes.size());
		for (const SharedPtr<FileNode>& child : m_CurrentFileNode->ChildNodes)
		{
			if (PassesContentFilter(*child))
				visible.push_back(child);
		}

		HandleSelectionShortcuts(visible);

		/* "点空白处清空选择"必须等条目都提交完再判：遍历前 IsAnyItemHovered() 读到的是上一帧
		 * 的残留，会把 Ctrl 点选当成"新加一项"清掉（多选时好时坏）。做法：遍历中记标记、遍历完再判。 */
		bool clicked_an_item = false;

		const int shown_count = static_cast<int>(visible.size());

		if (m_ThumbnailSize > 32.0f)
		{
			/* 一列要装得下「单元格 + 选中框两侧外扩」，还得和邻格隔开一点 ——
			 * 只按缩略图算列宽的话，最右一列的选中框会从列的裁剪矩形里探出去被切掉一条边。 */
			const float cell_footprint = m_ThumbnailSize
				+ ImMax(kThumbnailGap, kGridSelectionPad * 2.0f + kMinCellGap);
			int column_count = static_cast<int>(panel_width / cell_footprint);
			if (column_count < 1)
				column_count = 1;

			/* 第一行上方让出选中框外扩的那点高度（见 kGridTopInset）：
			 * 留在内容里而不是加在内容栏的内边距上 —— 滚动范围也要跟着算进去，
			 * 否则滚到顶时框的上边还是会被裁。 */
			ImGui::Dummy(ImVec2(0.0f, kGridTopInset));

			ImGui::Columns(column_count, nullptr, false);

			for (const SharedPtr<FileNode>& child_node : visible)
			{
				ImGui::PushID(child_node->FilePath.c_str());
				{
					const float column_start_x = ImGui::GetCursorPosX();
					const float column_width = ImGui::GetColumnWidth();
					const bool is_selected = IsNodeSelected(child_node.get());
					const ImVec2 cell_rect(m_ThumbnailSize, m_ThumbnailSize);
					const SharedPtr<DeviceTexture> thumbnail = ThumbnailOf(*child_node);

					/* 缩略图在列里居中：列宽是「面板宽度 ÷ 列数」，比单元格宽（除不尽时会多出一点），
					 * 而下面的文件名是按列宽居中画的 —— 缩略图若贴着列左端，名字就会整体偏右，
					 * 看着像"名字没对齐在图标下边"。两者共用列的中轴，名字才落在选中框的正下方。 */
					ImGui::SetCursorPosX(column_start_x + (column_width - m_ThumbnailSize) * 0.5f);

					/* 命中区恒为 ThumbnailSize 的正方形，图片和图标共用（选中框从它推出，两种格子才会一样大）。
					 * 注意：图片别用 ImageButton —— 它的控件矩形 = 图片 + 2×FramePadding，命中区大一圈、选中框
					 * 跟着外扩被列裁掉，图片还偏右下 FramePadding、跟文件名错开。 */
					ImGui::InvisibleButton("##cell", cell_rect);

					const ImVec2 cell_min = ImGui::GetItemRectMin();
					const ImVec2 cell_max = ImGui::GetItemRectMax();

					if (thumbnail != nullptr)
					{
						/* 图片：自己往这一格里画（上下翻转，贴图 v 轴朝下） */
						ImGui::GetWindowDrawList()->AddImage((ImTextureID)thumbnail.get(),
							cell_min, cell_max, ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));
					}
					else
					{
						/* 非图片（或缩略图没加载出来）：画类型图标。
						 * 颜色与树行、文件名一致（Token::Text），图标才不显灰。 */
						Icons::Draw(ImGui::GetWindowDrawList(), ResolveFileIcon(child_node->Type),
							ImVec2((cell_min.x + cell_max.x) * 0.5f, (cell_min.y + cell_max.y) * 0.5f),
							m_ThumbnailSize * 0.6f, ImGui::GetColorU32(EditorTheme::Token::Text));
					}

					/* 选中态：给单元格描一圈强调色（底栏会显示它的路径） */
					if (is_selected)
					{
						ImGui::GetWindowDrawList()->AddRect(
							ImVec2(cell_min.x - kGridSelectionPad, cell_min.y - kGridSelectionPad),
							ImVec2(cell_max.x + kGridSelectionPad, cell_max.y + kGridSelectionPad),
							ImGui::GetColorU32(EditorTheme::Token::Accent), ImGui::GetStyle().FrameRounding, 0, 1.5f);
					}

					/* 拖拽 */
					if (ImGui::BeginDragDropSource())
					{
						const char* item_path = child_node->FilePath.c_str();
						ImGui::SetDragDropPayload("RESOURCE_BROWSER_ITEM", item_path, strlen(item_path) + 1);
						ImGui::EndDragDropSource();
					}

					/* 单击 = 选中（Shift 连选 / Ctrl 加选，见 ApplyClickSelection），
					 * 双击文件夹 = 进去（目录树会跟着跳过去） */
					if (ImGui::IsItemClicked())
					{
						clicked_an_item = true;
						ApplyClickSelection(child_node, visible);
					}

						/* 快速双击的第二击：文件夹 = 进去，文件 = 就地改名。注意：带修饰键的双击不算"打开" ——
						 * Ctrl / Shift 连点同一个文件夹是在加选 / 移出，两次落在同一格被判成双击会意外进目录（选择被清空）。 */
					if (child_node->Type == FileType::Folder
						&& !ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyShift && !ImGui::GetIO().KeySuper
						&& ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
						SetCurrentNode(child_node);

					/* 右键菜单：点在没选中的项上先把它选中（已在选中集合里就保持多选不变） */
					if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)
						&& !IsNodeSelected(child_node.get()))
						SelectSingle(child_node);

					if (ImGui::BeginPopupContextItem())
					{
						DrawAssetContextMenuItems(child_node->FilePath, child_node->Type == FileType::Folder);
						ImGui::EndPopup();
					}

					/* 文件名逐行居中，长名称仍按列宽换行；保持列起点以免受按钮提交位置影响。 */
					ImGui::SetCursorPosX(column_start_x);
					DrawCenteredWrappedText(child_node->FileName, column_width,
						ImGui::GetColorU32(is_selected ? EditorTheme::Token::Accent : EditorTheme::Token::Text));

					ImGui::NextColumn();
				}
				ImGui::PopID();
			}

			ImGui::Columns(1);

			/* 目录里明明有东西却什么都没通过筛选：说清楚，而不是留一片空白 */
			if (shown_count == 0 && !m_CurrentFileNode->ChildNodes.empty())
			{
				ImGui::Dummy(ImVec2(0.0f, ImGui::GetFrameHeight()));
				ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::TextDim);
				ImGui::TextUnformatted("No assets match the current filter");
				ImGui::PopStyleColor();
			}
		}
		else
		{
			/* 列表模式：三列（名字 / 类型 / 大小），名字列带文件类型图标 */
			ImGui::BeginTable("Assets List", 3);
			{
				ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_NoHide);
				ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed);
				ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed);
				ImGui::TableHeadersRow();

				clicked_an_item = BuildFileListDetail(visible);
			}
			ImGui::EndTable();
		}

		/* 点空白处 = 清空选择：条目一个都没被点到，而左键确实按在这一栏里
	 * （走完所有条目再判，理由见上面 clicked_an_item 的说明）。 */
		if (!clicked_an_item && ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
			ClearSelection();

		/* 空处右键 = 在当前目录下操作（新建文件夹）；条目自己有一份菜单（NoOpenOverItems 分开） */
		if (ImGui::BeginPopupContextWindow("##AssetBackground",
				ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
		{
			DrawBackgroundContextMenuItems();
			ImGui::EndPopup();
		}
	}

	void EditorResourceBrowser::BuildFileNodeTree(const SharedPtr<FileNode>& parent_node)
	{
		for (auto& directory_entry : std::filesystem::directory_iterator(
			g_AssetsPath / PathFromUtf8(parent_node->FilePath)))
		{
			const auto& path = directory_entry.path();
			auto relative_path = GetRelativePath(g_AssetsPath, path);
			auto filename = relative_path.filename();
			const std::string file_name = PathToUtf8(filename);

			/* 以 '.' 开头的（.DS_Store、.git、项目的回收站……）一律不进资源树：
			 * 它们不是资源，列出来只会碍事（删除资源的回收站就放在 Assets 旁边，
			 * 万一有人把 Assets 指到别处、回收站落进去，也不会被当成资产显示出来）。 */
			if (file_name.empty() || file_name.front() == '.')
				continue;

			/* 创建一个文件节点 */
			auto file_node = CreateSharedPtr<FileNode>();
			file_node->ParentNode = parent_node;
			file_node->FileName = file_name;
			file_node->FilePath = PathToUtf8(relative_path);
			file_node->Depth = parent_node->Depth + 1;

			if (directory_entry.is_directory()) /* 文件夹，递归目录 */
			{
                file_node->Type = FileType::Folder;
				parent_node->ChildNodes.emplace_back(file_node);
				BuildFileNodeTree(file_node);
			}
			else
			{
				auto ext = PathToUtf8(filename.extension());
				transform(ext.begin(), ext.end(), ext.begin(), ::toupper);
				if (ext == ".JPG" || ext == ".PNG" || ext == ".DDS" || ext == ".TGA" || ext == ".BMP")
				{
                    file_node->Type = FileType::Image;
				}
				else if (ext == ".SCN")
				{
                    file_node->Type = FileType::Scene;
				}
				else if (ext == ".MTLGRAPH")
				{
                    file_node->Type = FileType::MtlGraph;
				}
				else
				{
                    file_node->Type = FileType::Default;
				}

				/* 图标不在这里取：非图片是矢量图标，图片缩略图按需加载（见 ThumbnailOf） */
				file_node->FileSize = static_cast<float>(std::filesystem::file_size(path)) / 1024.0f;
				parent_node->ChildNodes.emplace_back(file_node);
			}
		}
		m_IsDirty = false;
	}

	/* 详细列表的行：当前目录的直接子项 —— 层级交给目录树，这里不再套一层树，
	 * 与缩略图网格展示的范围保持一致（可见项由调用方算好传进来）。 */
	bool EditorResourceBrowser::BuildFileListDetail(const std::vector<SharedPtr<FileNode>>& visible)
	{
		bool clicked_an_item = false;

		for (const SharedPtr<FileNode>& child_node : visible)
		{
			const bool is_folder = (child_node->Type == FileType::Folder);
			const bool is_selected = IsNodeSelected(child_node.get());

			ImGui::PushID(child_node->FilePath.c_str());

			ImGui::TableNextRow();
			ImGui::TableNextColumn();

			/* Leaf：列表里不给展开箭头（要看层级去目录树）；选中态用 ImGui 自己的行高亮 */
			ImGui::TreeNodeEx("##node",
				ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen
					| (is_selected ? ImGuiTreeNodeFlags_Selected : ImGuiTreeNodeFlags_None),
				"%s", "");

			/* 单击 = 选中（Shift 连选 / Ctrl 加选），双击文件夹 = 进去 */
			if (ImGui::IsItemClicked())
			{
				clicked_an_item = true;
				ApplyClickSelection(child_node, visible);
			}

			/* 同上：带修饰键的双击是加选 / 移出，不是"打开" */
			if (is_folder && !ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyShift && !ImGui::GetIO().KeySuper
				&& ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
				SetCurrentNode(child_node);

			/* 右键菜单：点在没选中的项上先把它选中 */
			if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)
				&& !is_selected)
				SelectSingle(child_node);

			if (ImGui::BeginPopupContextItem())
			{
				DrawAssetContextMenuItems(child_node->FilePath, is_folder);
				ImGui::EndPopup();
			}

			/* 选中态由 ImGui 的行高亮表达（与网格的"强调色描边"是同一套语义的另一半） */
			PanelChrome::DrawTreeRowLabel(ResolveFileIcon(child_node->Type), child_node->FileName);

			/* 文件类型 */
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(ExtractFileSuffix(child_node->FileName).c_str());

			/* 文件大小（文件夹不计） */
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(is_folder ? "" : (ToString(child_node->FileSize, 2) + "KB").c_str());

			ImGui::PopID();
		}

		/* 目录里明明有东西却什么都没通过筛选：说清楚，而不是留一张空表 */
		if (visible.empty() && !m_CurrentFileNode->ChildNodes.empty())
		{
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::TextDim);
			ImGui::TextUnformatted("No assets match the current filter");
			ImGui::PopStyleColor();
		}

		return clicked_an_item;
	}

	std::filesystem::path EditorResourceBrowser::GetRelativePath(const std::filesystem::path& dir, const std::filesystem::path& path)
	{
		return path.lexically_relative(dir);
	}
}
