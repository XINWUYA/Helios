#include "Pch.h"
#include "EditorResourceBrowser.h"
#include "EditorCommon.h"
#include "EditorIcons.h"
#include "PanelChrome.h"
#include "PanelRegistry.h"
#include "Command/AssetFileOps.h"
#include "Command/CreateAssetFileCommand.h"
#include "Command/CreateAssetFolderCommand.h"
#include "Command/DeleteAssetsCommand.h"
#include "Command/RenameAssetCommand.h"
#include "Helios/Application/FileDialog.h"
#include "Helios/ImGui/EditorTheme.h"
#include "Helios/ImGui/ImGuiExtensions.h"
#include <utility>
#include <vector>

namespace Helios
{
	namespace
	{
		/* 目录状态检查间隔：兼顾刷新及时性与遍历开销 */
		constexpr float kDirectoryCheckInterval = 0.5f;

		/* 浏览位置的历史最多记几步（超出丢最旧的一条）：它不是工作记忆，
		 * 这里只是防"长时间浏览把它撑成无界"。 */
		constexpr size_t kMaxNavigationHistory = 64;

		/* 目录树 / 内容区的宽度分配：比例的可拖范围，以及两栏各自的最小宽度
		 * （目录树至少要放得下一个目录名，内容区至少要放得下一列缩略图） */
		constexpr float kMinTreePaneRatio = 0.12f;
		constexpr float kMaxTreePaneRatio = 0.70f;
		constexpr float kMinTreePaneWidth = 96.0f;
		constexpr float kMinContentWidth = 160.0f;

		/* 缩略图缩放范围（顶部一行的滑动条） */
		constexpr float kMinThumbnailSize = 16.0f;
		constexpr float kMaxThumbnailSize = 128.0f;

		/* 网格里两格之间的最小横向间距：定死，不做成可调项（原本有个 Spacing 滑动条，
		 * 它和缩放滑动条是两套"调版式"的手感，值调到 0 也只是把格子挤在一起）。
		 * 它只决定"这一栏最多分几列"；实际间距 = 它 + 富余宽度均分到格子之间的那一份。 */
		constexpr float kThumbnailGap = 32.0f;

		/* 缩略图网格在内容栏左右各留的余地。取 5 是因为它正好等于「选中框的外扩 +
		 * 半个描边」（kGridSelectionPad + 0.75）：最外侧那两格的选中框因此刚好落在
		 * 内容栏内沿、不会被它的裁剪矩形切掉一条边（上边那一档是 kGridTopInset）。 */
		constexpr float kGridSideInset = 5.0f;

		/* 路径栏右端那组（缩放的滑动条 + 统计数）：滑动条的标称宽度与退让下限 */
		constexpr float kZoomSliderWidth = 96.0f;
		constexpr float kMinZoomSliderWidth = 48.0f;

		/* 路径栏留给路径（面包屑）的最小宽度。注意：路径栏只占右栏宽度，这一档要抠着给 ——
		 * 只保证"至少放得下一段短目录名"，给宽了右端的统计数会被早早挤掉。 */
		constexpr float kMinCrumbWidth = 72.0f;

		/* 一段面包屑至少要放得下这么宽才画：再窄就整级不画，
		 * 别在那一行里留一个看不见却还能点的"点"。 */
		constexpr float kMinCrumbLabelWidth = 8.0f;

		/* 路径栏给"选中的项"摘要留的最小宽度：比这还窄就整段不画（留着也只会被裁成碎片）。
		 * 它不算进 path_min —— 摘要本来就可省，不用一直预留。 */
		constexpr float kMinSelectionWidth = 56.0f;

		/* 文件操作弹层（新建 / 重命名 / 删除确认）里的控件尺寸：两个按钮等宽，排一行 */
		constexpr float kPopupButtonSize = 84.0f;
		constexpr float kPopupInputWidth = 240.0f;

		/* 资源浏览器拖拽的 payload 名：负载 = 相对 Assets 的路径（UTF-8 + '\0'）。
		 * 内容区拖出与目录树拖出用的是同一种身份（拖给别的面板用时接收方也只认这一种）；
		 * 目录树的行又是它的拖放目标 —— 落点 = "搬进这一行指的目录"。 */
		constexpr const char* kAssetDragPayload = "RESOURCE_BROWSER_ITEM";

		/* 顶栏「新建」菜单里能建的文件类型（文件夹单列，不算文件）。这张表是"能建哪些资源"
		 * 的唯一来源：菜单项、名字预填、后缀约束、初始内容全靠它。只列资源树认得出类型的格式；
		 * 最后一项是兜底的不限后缀（.glsl / .mat 自己写后缀）。 */
		struct CreatableFileType
		{
			const char* MenuLabel;
			const char* Extension;    /* 含点；空 = 不限后缀（用户自己写） */
			const char* DefaultStem;  /* 弹层里预填的名字（不带后缀） */
			Icons::Id   Icon;         /* 「新建」下拉里的条目图标 */
		};

		constexpr CreatableFileType kCreatableFiles[] =
		{
			{ "Scene",          ".scn",      "New Scene",          Icons::Id::FileScene },
			{ "Material Graph", ".mtlgraph", "New Material Graph", Icons::Id::FileMtlGraph },
			{ "File...",        "",          "New File",           Icons::Id::File },
		};

		/* 忽略大小写的"以 suffix 结尾"：新建文件时判断用户有没有自己把后缀写上。
		 * 后缀都短，直接按字符比，省得为它引入一份大小写无关的字符串设施。 */
		bool EndsWithIgnoreCase(const std::string& text, const std::string& suffix)
		{
			if (suffix.empty() || text.size() < suffix.size())
				return false;

			const size_t offset = text.size() - suffix.size();
			for (size_t index = 0; index < suffix.size(); ++index)
			{
				char left = text[offset + index];
				char right = suffix[index];

				if (left >= 'A' && left <= 'Z')
					left = static_cast<char>(left - 'A' + 'a');
				if (right >= 'A' && right <= 'Z')
					right = static_cast<char>(right - 'A' + 'a');

				if (left != right)
					return false;
			}

			return true;
		}

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

		/* 栏与主体之间那条分隔线（1px，自绘） */
		constexpr float kBarDivider = 1.0f;

		/* 路径栏（分隔线 + 一行）的总高：右栏里的内容区按它让出位置，
		 * 路径栏自己也按它定位 —— 一处算，两处用，对得上。 */
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
		/* 不留 default：FileType 加了新档却忘了给图标时，-Wswitch 会直接报出来
		 * （留下 default 的话新类型会静默落到通用文件图标上，很难发现）。 */
		switch (type)
		{
		case FileType::Default:  return Icons::Id::File;
		case FileType::Folder:   return Icons::Id::Directory;
		case FileType::Image:    return Icons::Id::FileImage;
		case FileType::Scene:    return Icons::Id::FileScene;
		case FileType::MtlGraph: return Icons::Id::FileMtlGraph;
		case FileType::Shader:   return Icons::Id::FileShader;
		case FileType::Model:    return Icons::Id::FileModel;
		}

		return Icons::Id::File;
	}

	/* 后缀分出来的大类 -> 面板的 FileType：面板比大类多两档（Folder 来自"是不是目录"、
	 * Default = 认不出的后缀），剩下的一一对上。同上：不留 default，漏了会由 -Wswitch 报出来。 */
	EditorResourceBrowser::FileType EditorResourceBrowser::FileTypeOfKind(AssetFileKind kind)
	{
		switch (kind)
		{
		case AssetFileKind::Other:    return FileType::Default;
		case AssetFileKind::Image:    return FileType::Image;
		case AssetFileKind::Scene:    return FileType::Scene;
		case AssetFileKind::MtlGraph: return FileType::MtlGraph;
		case AssetFileKind::Shader:   return FileType::Shader;
		case AssetFileKind::Model:    return FileType::Model;
		}

		return FileType::Default;
	}

	/* 与 FileTypeOfKind 反方向；不留 default：两边的档位要一起长 */
	AssetFileKind EditorResourceBrowser::AssetKindOfFileType(FileType type)
	{
		switch (type)
		{
		case FileType::Default:  return AssetFileKind::Other;
		case FileType::Image:    return AssetFileKind::Image;
		case FileType::Scene:    return AssetFileKind::Scene;
		case FileType::MtlGraph: return AssetFileKind::MtlGraph;
		case FileType::Shader:   return AssetFileKind::Shader;
		case FileType::Model:    return AssetFileKind::Model;
		case FileType::Folder:   return AssetFileKind::Other;   /* 文件夹不看大类（看 IsFolder） */
		}

		return AssetFileKind::Other;
	}

	const char* EditorResourceBrowser::TypeFilterName(TypeFilter filter)
	{
		/* 不留 default：TypeFilter 加了一项却没给显示名时 -Wswitch 会报出来
		 * （否则那一项会顶着 "All types" 出现在下拉里）。 */
		switch (filter)
		{
		case TypeFilter::All:      return "All types";
		case TypeFilter::Folder:   return "Folders";
		case TypeFilter::Image:    return "Images";
		case TypeFilter::Scene:    return "Scenes";
		case TypeFilter::MtlGraph: return "Mtl Graphs";
		case TypeFilter::Shader:   return "Shaders";
		case TypeFilter::Model:    return "Models";
		}

		return "All types";
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
		case TypeFilter::Shader:   return type == FileType::Shader;
		case TypeFilter::Model:    return type == FileType::Model;
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

		/* 就地改名的"本帧画过没有"每帧从头记：帧末据此收起画不出来的编辑器（见函数尾） */
		m_RenameEditDrawn = false;

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

			/* 浏览历史（回到上次路径 / 重进路径）要跟目录重建对齐：重命名过的项按前缀 remap；
			 * 进不去的项（目录已删）直接摘掉；游标那一项写回当前实际位置（当前目录被改名 / 被删也跟得上）。 */
			{
				const std::string current_now = m_CurrentFileNode->FilePath;
				std::vector<std::string> history;
				history.reserve(m_NavHistory.size());

				size_t cursor = 0;
				bool cursor_kept = false;

				for (size_t i = 0; i < m_NavHistory.size(); ++i)
				{
					const bool is_cursor = (i == m_NavCursor);
					const std::string path = is_cursor ? current_now : remap(m_NavHistory[i]);

					if (!is_cursor && FindNode(m_RootFileNodeTree, path) == nullptr)
						continue;

					if (is_cursor)
					{
						cursor = history.size();
						cursor_kept = true;
					}

					history.push_back(path);
				}

				/* 首次重建时历史还是空的（游标也没有落点）：以当前目录为起点 */
				if (!cursor_kept)
				{
					history.assign(1, current_now);
					cursor = 0;
				}

				m_NavHistory = std::move(history);
				m_NavCursor = cursor;
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
			m_PopupNewMenu = ImGui::GetID("##NewAssetMenu");
			m_PopupNewFolder = ImGui::GetID("##NewAssetFolder");
			m_PopupNewFile = ImGui::GetID("##NewAssetFile");
			m_PopupRename = ImGui::GetID("##RenameAsset");
			m_PopupDelete = ImGui::GetID("##DeleteAssets");

			/* 布局：顶栏（贴着面板上边）+ 左右两栏；路径栏在右栏的下边（见 ShowBrowserBody），
			 * 所以这里不再为它预留面板级的高度。 */
			ShowBrowserTopBar(pane_padding);
			ShowBrowserBody();

			/* 文件操作弹层画在面板根：右键菜单与顶栏的「新建」按钮都在这里开弹层。
			 * 把主题的窗口内边距带进去 —— 面板为了贴上下边把它压成了 0，
			 * 弹层要的是主题那一档（不补的话首末条目紧贴弹层的上下边）。 */
			DrawAssetOperationPopups(pane_padding);
		}
		ImGui::End();

		ImGui::PopStyleVar();

		/* 目录树拖拽的待办在这一刻落盘：面板已经画完（本帧的树不会再被读到），
		 * 命令进同一条编辑历史 —— 撤销即可把东西搬回原处。
		 * 与层级面板"挂接延后到遍历之后"是同一个位置上的同一条理由。 */
		ApplyPendingDropMove();

		/* 就地改名这一帧没能画出来（项被过滤 / 切了目录 / 被删掉）：静默收起，别让状态悬着 */
		if (!m_RenameEditPath.empty() && !m_RenameEditDrawn)
			FinishInlineRename(nullptr, false);

		/* 帧末统一发布选中项：本帧所有交互（点选 / 连选 / Esc / 重建后的路径对齐）都已完成，
		 * 只此一处出口 —— 属性面板看到的选中态与浏览器一致。 */
		PublishAssetSelection();
	}

	/* 主体：左右两栏 —— 左「Folders」目录树、中间可拖的分隔条、右内容区（含底部的路径栏）。
	 * 两栏都是子窗口：内容区的缩略图网格用的是 ImGui::Columns，而 Columns 以「当前窗口」
	 * 为界（窗口的 WorkRect / Indent），不套子窗口的话它会横跨整个面板、压到目录树上。 */
	void EditorResourceBrowser::ShowBrowserBody()
	{
		PROFILE_FUNCTION();

		const ImGuiStyle& style = ImGui::GetStyle();
		const ImVec2 pane_padding = style.WindowPadding;

		/* 主体高度按绝对几何算：从光标到面板底边（不用 GetContentRegionAvail —— 顶栏贴边画、
		 * 内边距这笔账不掺和）。底栏不从这扣：它只占右栏底部，左栏撑满整个主体高度。 */
		ImGuiWindow* window = ImGui::GetCurrentWindow();
		const float panel_bottom = window->Pos.y + window->Size.y;
		const ImVec2 body_start = ImGui::GetCursorScreenPos();
		const ImVec2 body(ImGui::GetContentRegionAvail().x,
			ImMax(panel_bottom - body_start.y, ImGui::GetFrameHeight() * 2.0f));
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

				/* 条目之下的空白区域：拖到这里 = 提升到资源根（与层级"空白 = 提升到根"同款）。
				 * 只覆盖空白、不盖到行上 —— 落在行上的拖拽归那一行管（行自己的目标更小，
				 * 交错时 ImGui 也优先接受最小目标）。 */
				{
					ImGuiWindow* const tree = ImGui::GetCurrentWindow();
					const ImVec2 blank_top = ImGui::GetCursorScreenPos();
					const ImVec2 tree_max(tree->Pos.x + tree->Size.x, tree->Pos.y + tree->Size.y);

					if (tree_max.y > blank_top.y + 1.0f)
					{
						const ImRect blank_area(blank_top, tree_max);
						if (ImGui::BeginDragDropTargetCustom(blank_area, ImGui::GetID("##FolderRootDrop")))
						{
							if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetDragPayload))
								RecordDropMove(payload, std::string());

							ImGui::EndDragDropTarget();
						}
					}
				}

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

		/* ---- 右栏：内容区 + 底部路径栏 ----
		 * 路径栏挂在右栏下边（描述右栏正在浏览的目录）；内容区自己滚、路径栏钉住。注意：宽度再加
		 * 一档（面板横向内边距），让右栏吃满到面板右缘，不然滚动条会停在离边界差两档内边距的地方。 */
		ImGui::SameLine(0.0f, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pane_padding.x, 0.0f));
		ImGui::BeginChild("##BrowserRightPane",
			ImVec2(ImGui::GetContentRegionAvail().x + pane_padding.x, body_height), false,
			ImGuiWindowFlags_AlwaysUseWindowPadding | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
		{
			/* 内容区的高度要把路径栏那一档让出来：底栏高 = 行高 + 1px 分隔线，
			 * 让出的正是这一档，两者之间那条线才不会压到内容上、也不会空出一条。 */
			const float content_height = ImMax(
				ImGui::GetContentRegionAvail().y - FooterBandHeight(), ImGui::GetFrameHeight());

			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, pane_padding.y));

			/* 内容栏同样再吃一档（右栏的内边距）：右内沿贴死面板右缘 —— 网格贴着它铺满、
			 * 滚动条也贴着它画。路径栏不跟着吃：底栏的文字 / 滑动条还要那圈内边距。 */
			const ImVec2 content_pos = ImGui::GetCursorScreenPos();
			const ImVec2 content_size(ImGui::GetContentRegionAvail().x + pane_padding.x, content_height);

			/* 光顶到边还不够：ImGui 的绘制裁剪会向内缩"半格内边距"，滚动条最右几像素会被悄悄裁掉。
			 * 用内容栏自己的矩形顶掉当前裁剪（intersect = false，跟 EndCard 同一手法）；内容仍然画在
			 * 窗口 InnerClipRect 里，不会压到滚动条。 */
			ImGui::PushClipRect(content_pos,
				ImVec2(content_pos.x + content_size.x, content_pos.y + content_size.y), false);
			ImGui::BeginChild("##BrowserContentPane", content_size, false,
				ImGuiWindowFlags_AlwaysUseWindowPadding);
			ShowBrowserContent();
			ImGui::EndChild();
			ImGui::PopClipRect();
			ImGui::PopStyleVar();

			ShowBrowserFooter();
		}
		ImGui::EndChild();
		ImGui::PopStyleVar();

		/* 右栏 / 内容栏是故意越过右沿、顶到面板右缘的 —— 但子窗口矩形会被算进面板的内容范围，
		 * 面板就多出 12px 的横滑区。这里把横向内容范围钳回内容区右内沿（纵向不动）。 */
		window->DC.CursorMaxPos.x = ImMin(window->DC.CursorMaxPos.x, body_start.x + body.x);
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

			/* 悬停选中行时 ImGui 画的也是 HeaderHovered（会盖掉选中色）——
			 * 推一档"更亮的选中色"顶住，见 EditorTheme::RowHoverSelected */
			if (is_current)
				ImGui::PushStyleColor(ImGuiCol_HeaderHovered, EditorTheme::RowHoverSelected);

			const bool is_opened = ImGui::TreeNodeEx("##node", flags, "%s", "");

			if (is_current)
				ImGui::PopStyleColor();

			/* 点目录名（不是点展开箭头）= 切换当前目录 —— 但按下帧只记意图：
			 * 松开那一帧再兑现（拖动起手就不算点击，见下面的松开判定）。 */
			if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
				m_PendingTreeNav = child_node;

			/* 拖拽源：把文件夹拖去别的目录（树里拖到别的行、拖到树下方空白 = 提升到根），
			 * 也可以拖给别的面板用（同一个 payload，内容区拖出的是同一种身份）。
			 * 路径会被 ImGui 拷进 payload 缓冲，重建把节点指针换掉也不影响。 */
			if (ImGui::BeginDragDropSource())
			{
				const char* item_path = child_node->FilePath.c_str();
				ImGui::SetDragDropPayload(kAssetDragPayload, item_path, strlen(item_path) + 1);
				ImGui::TextUnformatted(child_node->FileName.c_str());
				ImGui::EndDragDropSource();
			}

			/* 拖拽目标：别处拖来的项落在这一行上 = 搬进这个目录。
			 * 落到自己身上会被 ImGui 拒（源与目标同为这一行）—— "拖起来又原位放下" = 取消；
			 * 其余非法落点（自己的子孙 / 原目录 / 重名）由落盘前的裁决挡住（见 ApplyPendingDropMove）。 */
			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetDragPayload))
					RecordDropMove(payload, child_node->FilePath);

				ImGui::EndDragDropTarget();
			}

			/* 松开那一帧：按下是"点击意图"、没越过拖拽阈值、指针还在这行上 —— 才真的切目录。
			 * 把文件夹拖去别处的那一下不该顺带"进这个目录"（拖动是拿去用，不是查看）。 */
			if (m_PendingTreeNav == child_node && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
			{
				const bool was_drag = ImGui::IsMouseDragPastThreshold(ImGuiMouseButton_Left);
				const bool on_row = ImGui::IsItemHovered();
				m_PendingTreeNav = nullptr;

				if (!was_drag && on_row)
					SetCurrentNode(child_node);
			}

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

	/* ---- 浏览位置与它的历史 ----
	 * 历史里存的是相对 Assets 的路径：文件树会重建，节点指针不保险
	 * （与文件操作的待办同一套理由）。游标左边是「回到上次路径」能去的，右边是「重进路径」能去的。 */

	/* 用户导航（点目录树 / 点面包屑 / 双击文件夹）：落位 + 记一条历史。
	 * 目录没变（又点了当前目录）不算一次导航 —— 不落位也不记，历史里不出现连续重复项。 */
	void EditorResourceBrowser::SetCurrentNode(const SharedPtr<FileNode>& node)
	{
		if (node == nullptr || node == m_CurrentFileNode)
			return;

		RecordNavigation(node->FilePath);
		ApplyCurrentNode(node);
	}

	/* 定位一个资源：切到它所在目录（算一次用户导航，记历史）再选中它。
	 * 树还没建 / 正在重建 / 目标还不在树里：记为「待选中」，重建后的对齐逻辑会消费它
	 * （与新建项选中同一个通道 —— m_PendingSelectPath）。 */
	void EditorResourceBrowser::RevealAsset(const std::string& relative_path)
	{
		PROFILE_FUNCTION();

		if (relative_path.empty())
			return;

		/* Assets 之外的引用（"../" 开头）没有可定位的树节点，直接忽略 */
		if (relative_path.rfind("..", 0) == 0)
			return;

		if (m_RootFileNodeTree == nullptr || m_IsDirty)
		{
			m_PendingSelectPath = relative_path;
			return;
		}

		const std::filesystem::path parent = PathFromUtf8(relative_path).parent_path();
		const std::string parent_path = PathToUtf8(parent);
		const SharedPtr<FileNode> dir = parent_path.empty()
			? m_RootFileNodeTree
			: FindNode(m_RootFileNodeTree, parent_path);
		const SharedPtr<FileNode> target = FindNode(m_RootFileNodeTree, relative_path);

		if (dir == nullptr || target == nullptr)
		{
			/* 树里还没有（外部新增、目录轮询未到）：记为待选中并触发一次重建 */
			m_PendingSelectPath = relative_path;
			m_IsDirty = true;
			return;
		}

		/* 先切目录（落位会清选择），再选中目标 */
		SetCurrentNode(dir);
		SelectSingle(target);
	}

	/* 落位：切目录 + 清掉选中项 + 记上"下次画目录树时露出来（展开祖先 + 滚到可见）"。
	 * 清选择对属性面板是静默的（清基准线、吞掉这次选择动作）；点选 / Ctrl+A / Esc 照常发布。 */
	void EditorResourceBrowser::ApplyCurrentNode(const SharedPtr<FileNode>& node)
	{
		m_CurrentFileNode = node;

		m_Selection.clear();
		m_SelectionAnchor = nullptr;
		m_SelectionActivated = false;
		m_PublishedSelectionPaths.clear();

		/* 慢双击候选属于上一个目录的项，跟着作废 */
		ClearRenameCandidate();

		m_RevealCurrentNode = true;
	}

	void EditorResourceBrowser::RecordNavigation(const std::string& path)
	{
		/* 从历史中间走新路 = 丢弃「前进」的那一段（与浏览器一致）：历史里只留真的走过的那一条 */
		if (m_NavCursor + 1 < m_NavHistory.size())
			m_NavHistory.resize(m_NavCursor + 1);

		/* 超出上限就丢最旧的一条：接着 push 新的一条、游标落在末尾，两头的关系都不受影响 */
		if (m_NavHistory.size() >= kMaxNavigationHistory)
			m_NavHistory.erase(m_NavHistory.begin());

		m_NavHistory.push_back(path);
		m_NavCursor = m_NavHistory.size() - 1;
	}

	int EditorResourceBrowser::FindHistoryStep(int direction, SharedPtr<FileNode>& node) const
	{
		for (int index = static_cast<int>(m_NavCursor) + direction;
			index >= 0 && index < static_cast<int>(m_NavHistory.size());
			index += direction)
		{
			if (SharedPtr<FileNode> found = FindNode(m_RootFileNodeTree, m_NavHistory[static_cast<size_t>(index)]))
			{
				node = found;
				return index;
			}
		}

		return -1;
	}

	void EditorResourceBrowser::NavigateHistory(int direction)
	{
		SharedPtr<FileNode> target;
		const int step = FindHistoryStep(direction, target);
		if (step < 0)
			return;

		m_NavCursor = static_cast<size_t>(step);
		ApplyCurrentNode(target);
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
		m_SelectionActivated = true;
		m_Selection.clear();
		if (node != nullptr)
			m_Selection.push_back(node);

		m_SelectionAnchor = node;
	}

	void EditorResourceBrowser::ToggleSelection(const SharedPtr<FileNode>& node)
	{
		if (node == nullptr)
			return;

		m_SelectionActivated = true;

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
		m_SelectionActivated = true;

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
		m_SelectionActivated = true;
		m_Selection.clear();
		m_SelectionAnchor = nullptr;
		ClearRenameCandidate();
	}

	/* 面板自己的 FileType → 资源大类：跨面板摘要用 AssetFileKind 讲话
	 * （属性面板据此选图标、给类型名，不必认识浏览器的枚举）。 */

	void EditorResourceBrowser::PublishAssetSelection()
	{
		/* 按住左键（或已在拖动会话）就先不发布：结局还没定 —— 可能是点击（松开才发布），也可能
		 * 是拖动（拿去别处，比如拖贴图到材质卡）。按下那帧就发布会把材质卡的拖放目标当场切掉。
		 * 等松开再分别处置。 */
		const bool dragging = ImGui::GetDragDropPayload() != nullptr;
		if (dragging)
			m_PressWasDrag = true;

		if (dragging || ImGui::IsMouseDown(ImGuiMouseButton_Left))
			return; /* 按住期间攒着：等松开后按「点击 / 拖动」两种结局分别处置 */

		/* 每帧比的是路径列表：文件树重建后节点指针会换，路径才是稳定身份；
		 * 另外"本次按住有选择动作"也算变化 —— 同一项被再次点选要能重新激活属性面板。 */
		bool changed = m_SelectionActivated || m_Selection.size() != m_PublishedSelectionPaths.size();
		for (size_t i = 0; !changed && i < m_Selection.size(); ++i)
			changed = (m_Selection[i]->FilePath != m_PublishedSelectionPaths[i]);

		if (!changed)
		{
			m_PressWasDrag = false;
			return;
		}

		/* 这次按下的结局是拖动：高亮 / 底栏只留给浏览器自己，不通知属性面板。
		 * 同步基准并吞掉"选择动作"—— 拖完也不能补发（否则面板会在拖动结束时切走），
		 * 想看这个资源再点一下即可（点击的结局照常发布）。 */
		if (m_PressWasDrag)
		{
			m_PressWasDrag = false;
			m_SelectionActivated = false;
			m_PublishedSelectionPaths.clear();
			m_PublishedSelectionPaths.reserve(m_Selection.size());
			for (const SharedPtr<FileNode>& node : m_Selection)
			{
				if (node != nullptr)
					m_PublishedSelectionPaths.push_back(node->FilePath);
			}
			return;
		}

		m_SelectionActivated = false;
		m_PublishedSelectionPaths.clear();
		m_PublishedSelectionPaths.reserve(m_Selection.size());

		std::vector<AssetSelectionEntry> entries;
		entries.reserve(m_Selection.size());
		for (const SharedPtr<FileNode>& node : m_Selection)
		{
			if (node == nullptr)
				continue;

			AssetSelectionEntry entry;
			entry.Name = node->FileName;
			entry.Path = node->FilePath;
			entry.IsFolder = (node->Type == FileType::Folder);
			entry.Kind = AssetKindOfFileType(node->Type);
			entry.SizeBytes = static_cast<uintmax_t>(node->FileSize * 1024.0f); /* FileSize 以 KB 计 */
			entry.ChildCount = entry.IsFolder ? node->ChildNodes.size() : 0;

			entries.push_back(std::move(entry));
			m_PublishedSelectionPaths.push_back(node->FilePath);
		}

		if (m_AssetSelectionSink.Changed)
			m_AssetSelectionSink.Changed(entries);
	}

	void EditorResourceBrowser::HandleSelectionShortcuts(const std::vector<SharedPtr<FileNode>>& visible)
	{
		const ImGuiIO& io = ImGui::GetIO();

		/* 光看悬停还不够：键盘焦点可能在搜索框里，所以正在输入文字（WantTextInput）时一律不抢键；
		 * 文件操作的弹层开着时更不抢 —— 里面就有输入框与按钮，Delete / Esc 会误伤 */
		if (!ImGui::IsWindowHovered() || io.WantTextInput)
			return;

		/* 弹层由面板根作用域上的 ID 开的，这里也用那套 ID 判"开着没有" */
		if (ImGui::IsPopupOpen(m_PopupNewMenu, ImGuiPopupFlags_None)
			|| ImGui::IsPopupOpen(m_PopupNewFolder, ImGuiPopupFlags_None)
			|| ImGui::IsPopupOpen(m_PopupNewFile, ImGuiPopupFlags_None)
			|| ImGui::IsPopupOpen(m_PopupRename, ImGuiPopupFlags_None)
			|| ImGui::IsPopupOpen(m_PopupDelete, ImGuiPopupFlags_None))
			return;

		if ((io.KeyCtrl || io.KeySuper) && ImGui::IsKeyPressed(ImGuiKey_A))
		{
			m_Selection = visible;
			m_SelectionAnchor = visible.empty() ? nullptr : visible.front();
			ClearRenameCandidate();
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

	/* ---- 文件操作：新建文件夹 / 新建资源文件 / 重命名 / 删除 ----
	 * 都做成 ICommand 走编辑历史，于是 Ctrl+Z / Ctrl+Y、菜单与工具栏的撤销 / 重做
	 * 对它们同样有效 —— 这也是"资源管理器支持撤销"的全部意义：不是另开一套历史。 */

	std::filesystem::path EditorResourceBrowser::AbsoluteAssetPath(const std::string& relative_path)
	{
		return g_AssetsPath / PathFromUtf8(relative_path);
	}

	void EditorResourceBrowser::ExecuteCommand(UniquePtr<ICommand> command)
	{
		if (command == nullptr)
			return;

		if (m_History.Execute)
		{
			/* 交给编辑历史：撤销 / 重做（菜单、主工具栏按钮与 Ctrl+Z）都归它管 */
			m_History.Execute(std::move(command));
			return;
		}

		/* 没有注入通道（面板单独跑 / headless 测试）：直接执行 —— 功能对，只是没有历史 */
		command->Do();
	}

	/* 拖放目标收到 payload：只登记"把谁搬进哪"（落盘延后，见 ApplyPendingDropMove）。
	 * payload 是相对 Assets 的路径；into_dir 空串 = 资源根。 */
	void EditorResourceBrowser::RecordDropMove(const ImGuiPayload* payload, const std::string& into_dir)
	{
		if (payload == nullptr)
			return;

		std::filesystem::path dropped;
		if (payload->DataSize <= 1 || !TryPathFromUtf8Payload(
			payload->Data, static_cast<size_t>(payload->DataSize), dropped))
			return;

		m_PendingDropFrom = PathToUtf8(dropped);
		m_PendingDropInto = into_dir;
	}

	/* 拖拽移动的待办落盘：裁决通过就发一条重命名命令（改名即移动，撤销即搬回）。
	 * 不合法（自身 / 自己的子孙 / 原目录 / 重名 / 源已不在）时什么都不做 ——
	 * 留一行日志说清原因，免得"拖了没反应"无从排查。 */
	void EditorResourceBrowser::ApplyPendingDropMove()
	{
		if (m_PendingDropFrom.empty())
			return;

		const std::string from = std::move(m_PendingDropFrom);
		const std::string into = std::move(m_PendingDropInto);
		m_PendingDropFrom.clear();
		m_PendingDropInto.clear();

		const std::filesystem::path from_abs = AbsoluteAssetPath(from);
		const std::filesystem::path into_abs = AbsoluteAssetPath(into);

		if (const std::string reason = AssetMoveError(from_abs, into_abs); !reason.empty())
		{
			CORE_LOG_INFO("拖拽移动已忽略：{0}（{1}）", from, reason);
			return;
		}

		ExecuteCommand(CreateUniquePtr<RenameAssetCommand>(this, from_abs, into_abs / from_abs.filename()));
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
		m_PendingNewFileExtension.clear();   /* 清掉上一个弹层的后缀约束：文件夹没有后缀 */

		snprintf(m_NameBuffer, sizeof(m_NameBuffer), "%s", "New Folder");

		ImGui::OpenPopup(m_PopupNewFolder);
	}

	void EditorResourceBrowser::OpenNewFilePopup(const std::string& extension, const std::string& default_stem,
		const std::string& parent_path)
	{
		m_PendingParentPath = parent_path;
		m_PendingRenamePath.clear();
		m_PendingNewFileExtension = extension;

		snprintf(m_NameBuffer, sizeof(m_NameBuffer), "%s", default_stem.c_str());

		ImGui::OpenPopup(m_PopupNewFile);
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

	/* 顶栏「新建」下拉：文件夹 + 表里那几种资源文件；目标目录 = 右栏当前浏览的目录。
	 * 版式跟「添加组件」菜单一样（搜索框 + 列表，打开就清空重聚焦）。
	 * 注意：选中的项要等 EndPopup 之后再执行（名字弹层得开在面板根层）。 */
	void EditorResourceBrowser::DrawNewAssetMenu()
	{
		const std::string current = (m_CurrentFileNode != nullptr) ? m_CurrentFileNode->FilePath : std::string();
		int chosen = -1;   /* -1 = 没选；0 = 文件夹；其余 = kCreatableFiles 的下标 + 1 */

		/* BeginPopupEx 不会自动加 NoTitleBar（BeginPopup 才加）——漏了弹层顶上
		 * 会多出一条空标题栏与折叠钮。 */
		if (ImGui::BeginPopupEx(m_PopupNewMenu,
				ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings
				| ImGuiWindowFlags_NoTitleBar))
		{
			if (ImGui::IsWindowAppearing())
			{
				m_NewAssetFilter[0] = '\0';
				ImGui::SetKeyboardFocusHere();
			}

			ImGui::SetNextItemWidth(220.0f);
			Icons::BeginSearchInput();
			ImGui::InputTextWithHint("##NewAssetSearch", "Search...", m_NewAssetFilter,
				sizeof(m_NewAssetFilter));
			Icons::EndSearchInput();
			ImGui::Separator();

			const std::string needle = ToLowercase(m_NewAssetFilter);
			bool any_match = false;

			if (ContainsCaseInsensitive("Folder", needle))
			{
				any_match = true;

				if (PanelChrome::MenuItemWithIcon(Icons::Id::Directory, "Folder"))
					chosen = 0;
			}

			/* 文件类型们：后缀与初始内容都从表里取（AssetFileTemplate 按后缀给模板） */
			for (int index = 0; index < IM_ARRAYSIZE(kCreatableFiles); ++index)
			{
				if (!ContainsCaseInsensitive(kCreatableFiles[index].MenuLabel, needle))
					continue;

				any_match = true;

				if (PanelChrome::MenuItemWithIcon(kCreatableFiles[index].Icon,
						kCreatableFiles[index].MenuLabel))
					chosen = index + 1;
			}

			if (!any_match)
				ImGui::TextDisabled("No matching asset");

			ImGui::EndPopup();
		}

		if (chosen < 0)
			return;

		if (chosen == 0)
		{
			OpenNewFolderPopup(current);
			return;
		}

		const CreatableFileType& type = kCreatableFiles[chosen - 1];
		OpenNewFilePopup(type.Extension, type.DefaultStem, current);
	}

	void EditorResourceBrowser::DrawAssetOperationPopups(const ImVec2& theme_padding)
	{
		/* 弹层的内边距：面板把 WindowPadding.y 压成了 0（顶栏 / 底栏要贴面板的上下边），
		 * 弹层要的却是主题那一档 —— 不补的话首末条目紧贴弹层的上下边（下拉框那边
		 * 已经踩过一次，见 ShowBrowserTopBar）。四个弹层一起补，它们才是一套。 */
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, theme_padding);

		DrawNewAssetMenu();
		DrawNamePopup(m_PopupNewFolder, NamePopupMode::NewFolder);
		DrawNamePopup(m_PopupNewFile, NamePopupMode::NewFile);
		DrawNamePopup(m_PopupRename, NamePopupMode::Rename);

		/* ---- 删除确认 ---- */
		/* BeginPopupEx 不会自动加 NoTitleBar（BeginPopup 才加）——漏了弹层顶上
		 * 会多出一条空标题栏与折叠钮。 */
		if (ImGui::BeginPopupEx(m_PopupDelete,
				ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings
				| ImGuiWindowFlags_NoTitleBar))
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

		ImGui::PopStyleVar();
	}

	/* 名字输入弹层：新建文件夹 / 新建资源文件 / 重命名共用
	 * （只有标题、后缀约束与"确认后建什么"不同，输入与校验是一套） */
	void EditorResourceBrowser::DrawNamePopup(ImGuiID popup_id, NamePopupMode mode)
	{
		/* BeginPopupEx 不会自动加 NoTitleBar（BeginPopup 才加）——漏了弹层顶上
		 * 会多出一条空标题栏与折叠钮。 */
		if (!ImGui::BeginPopupEx(popup_id,
				ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings
				| ImGuiWindowFlags_NoTitleBar))
			return;

		const bool is_rename = (mode == NamePopupMode::Rename);
		const bool is_file = (mode == NamePopupMode::NewFile);

		ImGui::TextUnformatted(is_rename ? "Rename" : (is_file ? "New File" : "New Folder"));

		/* 名字非法时把原因写在输入框下面，并禁用确认按钮 —— 别让用户点了没反应 */
		const std::string typed = m_NameBuffer;
		const std::string error = AssetNameError(typed);

		/* 目标路径：重命名 = 同目录下换个名字；新建 = 父目录下新建 */
		const std::filesystem::path pending_source = is_rename
			? AbsoluteAssetPath(m_PendingRenamePath) : std::filesystem::path();
		const std::filesystem::path parent = is_rename
			? pending_source.parent_path() : AbsoluteAssetPath(m_PendingParentPath);

		/* 新建资源文件时后缀由菜单项定：用户没写就替他补上（"Scene"这一项已经说明要建什么，
		 * 是补上而不是报错）；写了别的后缀也照样补，最终名字会显示在提示行。 */
		std::string final_name = typed;
		if (is_file && !m_PendingNewFileExtension.empty() && !EndsWithIgnoreCase(typed, m_PendingNewFileExtension))
			final_name += m_PendingNewFileExtension;

		const std::filesystem::path requested = parent / PathFromUtf8(final_name);
		const std::filesystem::path target = MakeUniquePath(parent, final_name);

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
		else if (target != requested || final_name != typed)
		{
			/* 最终名字与输入的不一样：说清会建成什么，免得用户以为没生效
			 * （两种来路 —— 同名被占了要让开，后缀被自动补上）。 */
			ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::TextDim);
			ImGui::Text("将创建为 %s", PathToUtf8(target.filename()).c_str());
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
			else if (is_file)
			{
				/* 初始内容按最终后缀取：补出来的后缀也算（.scn 给一份空场景的骨架） */
				const std::string extension = PathToUtf8(target.extension());
				ExecuteCommand(CreateUniquePtr<CreateAssetFileCommand>(
					this, target, AssetFileTemplate(extension)));
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

	/* ---- 内容区就地改名 ----
	 * 文件：快速双击直接改；文件夹：双击是进目录，改名 = 选中后停一下再点一次（Finder 同款）。
	 * 只把"改名"落盘；撤销 / 重做跟弹层共用 RenameAssetCommand。 */

	void EditorResourceBrowser::ClearRenameCandidate()
	{
		m_RenameCandPath.clear();
		m_RenameCandTime = -1.0;
	}

	void EditorResourceBrowser::BeginInlineRename(const SharedPtr<FileNode>& node)
	{
		if (node == nullptr)
			return;

		m_RenameEditPath = node->FilePath;
		snprintf(m_RenameEditBuffer, sizeof(m_RenameEditBuffer), "%s", node->FileName.c_str());
		m_RenameEditFocus = true;

		ClearRenameCandidate();
	}

	void EditorResourceBrowser::FinishInlineRename(const SharedPtr<FileNode>& node, bool commit)
	{
		const std::string typed = m_RenameEditBuffer;

		m_RenameEditPath.clear();
		m_RenameEditBuffer[0] = '\0';
		m_RenameEditFocus = false;

		/* 取消 / 目标已不在 / 没改过 / 非法名（编辑时已标红）：只收起，不动磁盘 */
		if (!commit || node == nullptr || typed == node->FileName || !AssetNameError(typed).empty())
			return;

		/* 撞名与新建同一条原则：让开而不是覆盖（MakeUniquePath 挑一个不冲突的） */
		const std::filesystem::path source = AbsoluteAssetPath(node->FilePath);
		ExecuteCommand(CreateUniquePtr<RenameAssetCommand>(this, source,
			MakeUniquePath(source.parent_path(), typed)));
	}

	/* 内容项"松开"时的慢双击判定（网格 / 列表共用）：点一下已选中的项先记候选；再点同一项、
	 * 且间隔超过双击窗口 → 就地改名；拖动 / 带修饰键 / 快速双击的第二击都不算，候选作废。 */
	void EditorResourceBrowser::HandleItemRenameRelease(const SharedPtr<FileNode>& node)
	{
		const ImGuiIO& io = ImGui::GetIO();

		const bool skip = m_PressSkipsSlowClick;
		m_PressSkipsSlowClick = false;

		const bool plain_click = !skip
			&& ImGui::IsItemHovered()
			&& !ImGui::IsMouseDragPastThreshold(ImGuiMouseButton_Left)
			&& !io.KeyCtrl && !io.KeyShift && !io.KeySuper;

		if (!plain_click)
		{
			ClearRenameCandidate();
			return;
		}

		if (m_RenameCandPath == node->FilePath
			&& ImGui::GetTime() - m_RenameCandTime >= io.MouseDoubleClickTime)
		{
			/* 停顿后的第二击：就地在原地改名（候选在 Begin 里清掉） */
			BeginInlineRename(node);
			return;
		}

		/* 第一击：只有"它已是唯一选中"才留候选 —— 单击已选中项本身仍是"点选 /
		 * 把属性面板切回来"（同一项再次点选照常重新发布），后一下才改名 */
		if (m_Selection.size() == 1 && IsNodeSelected(node.get()))
		{
			m_RenameCandPath = node->FilePath;
			m_RenameCandTime = ImGui::GetTime();
		}
		else
		{
			ClearRenameCandidate();
		}
	}

	/* 就地改名的输入框：回车提交 / Esc 取消 / 点到别处提交；名字非法时标红、回车不提交。
	 * 调用方负责把光标摆到名字的落点与宽度上；"##InlineRename" 在各自的作用域里唯一。 */
	void EditorResourceBrowser::DrawInlineRenameEditor(const SharedPtr<FileNode>& node)
	{
		m_RenameEditDrawn = true;

		/* 激活后的第一帧把键盘焦点交给输入框（SetKeyboardFocusHere 是"下一次项"
		 * 的聚焦请求，只发一次；之后焦点归 ImGui 与用户管） */
		if (m_RenameEditFocus)
		{
			ImGui::SetKeyboardFocusHere();
			m_RenameEditFocus = false;
		}

		const bool invalid = !AssetNameError(m_RenameEditBuffer).empty();
		if (invalid)
			ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::Danger);

		const bool submitted = ImGui::InputText("##InlineRename", m_RenameEditBuffer,
			sizeof(m_RenameEditBuffer), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);

		if (invalid)
			ImGui::PopStyleColor();

		if (submitted)
		{
			/* 非法名留在编辑态（下一帧重新聚焦）继续改 —— 别"按了没反应" */
			if (invalid)
				m_RenameEditFocus = true;
			else
				FinishInlineRename(node, true);
			return;
		}

		/* 失焦一律走"提交"通道（点到别处 / Tab）。Esc 不用单独判：InputText 在 Esc 时会把输入
		 * 缓冲恢复成初始值，这条通道看到"没改过"就只收不落盘 —— 取消语义由它兜住。 */
		if (ImGui::IsItemDeactivated())
			FinishInlineRename(node, true);
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
	void EditorResourceBrowser::ShowBrowserTopBar(const ImVec2& theme_padding)
	{
		PROFILE_FUNCTION();

		const ImGuiStyle& style = ImGui::GetStyle();

		/* 顶栏就画在面板的客户端顶边上（面板本身不带上下内边距，见 OnImGuiRenderer），
		 * 横向按内容区左端对齐。左上角不再画面板图标（目录名在底栏、面板名在窗口标题里，
		 * 那枚文件夹图标只是重复），改成「新建 + 回到上次路径 / 重进路径」三个按钮。 */
		PanelChrome::HeaderRow row = PanelChrome::BeginHeaderRow(Icons::Id::None);

		/* 控件上下各留 1px：栏高因此比控件高 2px。留白的用处是"贴边画"的两个毛病 ——
		 * 控件顶边压在面板上边上、底边又压在下面那条分隔线上，看着像缺了一条边框。 */
		constexpr float kBarInset = 1.0f;
		row.Height += kBarInset * 2.0f;

		const float control_height = ImGui::GetFrameHeight();
		const float control_y = row.Min.y + kBarInset;
		const float gap = style.ItemInnerSpacing.x;

	/* 顶栏：左端搜索框，右端「后退 / 前进 / 筛选 / 新建」四枚图标按钮（新建贴最右，依次
	 * 向左）。放不下就压窄搜索框（下限 80；固定阈值总会在某个宽度上算错、控件重叠）；右端
	 * 四枚不参与退让。筛选是纯图标按钮，点击弹类型单选菜单，生效时高亮。 */
		const float action_size = control_height;
		const float action_step = action_size + gap;          /* 按钮的步长（含它们之间的间隙） */
		const float left_cluster = action_size * 3.0f + gap * 2.0f;

		/* 新建图标是「新建 / 添加」入口统一的那枚加号（与层级面板、属性面板同一枚）：
		 * 同一个"新建"动作到哪儿都是同一张脸。 */
		ImGui::SetCursorScreenPos(ImVec2(row.Min.x, control_y));
		if (Icons::IconButton(Icons::Id::NewAsset, ImVec2(action_size, action_size), false,
			"New asset  (folder / scene / material graph / file)"))
		{
			ImGui::OpenPopup(m_PopupNewMenu);
		}

		/* 两枚导航按钮各自的目标 = 历史里沿那个方向还进得去的一项（见 FindHistoryStep）：
		 * 按钮的可用态与 tooltip 都看它。tooltip 说清"这一步会去哪儿"：“Back to Scenes/Props”；
		 * 根目录的相对路径为空，用它的目录名 —— 与路径栏里对它的称呼一致。 */
		SharedPtr<FileNode> back_target;
		SharedPtr<FileNode> forward_target;
		const bool can_back = (FindHistoryStep(-1, back_target) >= 0);
		const bool can_forward = (FindHistoryStep(1, forward_target) >= 0);

		const auto navigation_tooltip = [](const char* action, const SharedPtr<FileNode>& target)
		{
			const std::string name = target->FilePath.empty() ? DisplayNodeName(*target) : target->FilePath;
			return std::string(action) + " to " + name;
		};

		const std::string back_tip = can_back
			? navigation_tooltip("Back", back_target) : std::string("Back");
		const std::string forward_tip = can_forward
			? navigation_tooltip("Forward", forward_target) : std::string("Forward");

		ImGui::SetCursorScreenPos(ImVec2(row.Min.x + action_step, control_y));
		ImGui::BeginDisabled(!can_back);
		if (Icons::IconButton(Icons::Id::Back, ImVec2(action_size, action_size), false, back_tip.c_str()))
			NavigateHistory(-1);
		ImGui::EndDisabled();

		ImGui::SetCursorScreenPos(ImVec2(row.Min.x + action_step * 2.0f, control_y));
		ImGui::BeginDisabled(!can_forward);
		if (Icons::IconButton(Icons::Id::Forward, ImVec2(action_size, action_size), false, forward_tip.c_str()))
			NavigateHistory(1);
		ImGui::EndDisabled();

		/* ---- 尺寸 ----
		 * 类型筛选的宽度按 BeginCombo 自己的账算：文字从「控件左端 + FramePadding.x」起、
		 * 到「控件右端 − 箭头区(GetFrameHeight)」为止，前面还要给前置图标留一档 ——
		 * 少算哪一笔，最长的类型名就会被箭头区裁掉一截（"Mtl Graphs" 是最长的那个）。 */
		const float arrow_width = control_height;
		const float type_width = ImGui::CalcTextSize("Mtl Graphs").x + arrow_width
			+ style.FramePadding.x + Icons::LeadingIconSpace() + gap;
		/* ---- 左端：搜索框（贴左端）----
		 * 搜索框不撑满：够用就好，宽度多出来的部分留给中间那一段留白，
		 * 也不至于窄到看不清输入的内容；放不下时跟着可用宽度缩，下限 80。 */
		const float search_preferred = 220.0f;
		const float search_min = 80.0f;

		/* 右端那组能用的宽度：从左边那组之后算起（中间隔一格） */
		const float group_room = row.Right - (row.Min.x + left_cluster + gap);

		bool show_type = true;
		float search_width = search_preferred;

		for (int step = 0; step < 3; ++step)
		{
			show_type = (step < 2);
			search_width = (step == 0) ? search_preferred : search_min;

			const float needed = search_width + (show_type ? gap + type_width : 0.0f);
			if (needed <= group_room)
				break;
		}

		/* 两件都靠右端：类型筛选贴最右，搜索紧挨在它左边 —— 留白全留在中间那一段，
		 * "找东西"这一组因此是贴着右边缘的。筛选被舍掉时（面板太窄）搜索自己贴到右端。 */
		const float type_x = show_type ? (row.Right - type_width) : row.Right;
		const float search_x = (show_type ? type_x - gap : row.Right) - search_width;

		/* 类型筛选：与文本过滤一起决定内容区显示什么（计数用的是同一条判据）。
		 * 只影响右栏 —— 左栏是导航，把要进去的目录藏掉就没法用了。 */
		if (show_type)
		{
			const ImVec2 frame_min(type_x, control_y);
			const ImVec2 frame_max(type_x + type_width, control_y + control_height);

			/* 顶栏所在窗口的绘制列表：现在就取。弹层打开后"当前窗口"会切到弹层，
			 * 那时 GetWindowDrawList() 拿到的是弹层的列表（前缀图标会画进去、被裁掉 = 图标凭空消失），
			 * 所以下面自己画的东西一律用这一份。 */
			ImDrawList* const row_draw = ImGui::GetWindowDrawList();

			ImGui::SetCursorScreenPos(frame_min);
			ImGui::SetNextItemWidth(type_width);

			/* 预览（图标 + 名字 + 下箭头）自己画，所以给 BeginCombo 传空预览、不要它自带的箭头：
			 * 它的预览文字钉在 FramePadding 上，要给前置图标让位就得把 FramePadding 撑大，
			 * 而弹层的横向内边距正是取自 FramePadding（BeginComboPopup 把
			 * WindowPadding.x 取成当时的 FramePadding.x）—— 撑大它会让弹层条目又挤又偏。
			 * 自己画则两件事互不干扰：控件里怎么摆随我们，弹层拿到的还是主题那一档。
			 * PushStyleVar(WindowPadding) 是给弹层补回上下内边距用的（见函数头注释）。 */
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, theme_padding);
			const bool type_open = ImGui::BeginCombo("##AssetTypeFilter", "", ImGuiComboFlags_NoArrowButton);

			if (type_open)
			{
				for (int i = 0; i < static_cast<int>(TypeFilter::COUNT); ++i)
				{
					const auto filter = static_cast<TypeFilter>(i);
					const bool selected = (filter == m_TypeFilter);

					/* 悬停选中条目时 ImGui 画的也是 HeaderHovered —— 推"更亮的选中色"顶住 */
					if (selected)
						ImGui::PushStyleColor(ImGuiCol_HeaderHovered, EditorTheme::RowHoverSelected);

					if (ImGui::Selectable(TypeFilterName(filter), selected))
						m_TypeFilter = filter;

					if (selected)
					{
						ImGui::PopStyleColor();
						ImGui::SetItemDefaultFocus();
					}
				}

				ImGui::EndCombo();
			}

			ImGui::PopStyleVar();

			/* 预览画在弹层关掉之后：这时当前窗口已经回到面板，装饰才落在面板的绘制列表上
			 * （也就压在弹层下面，不会糊到弹层上）。 */
			const float icon_room = style.FramePadding.x + Icons::LeadingIconSpace();
			const float arrow_size = ImGui::GetFontSize() * 0.55f;   /* 与卡片折叠箭头同一档 */
			const ImVec2 arrow_center(frame_max.x - style.FramePadding.x - arrow_size * 0.5f,
				(frame_min.y + frame_max.y) * 0.5f);

			Icons::DrawLeadingIcon(Icons::Id::Filter, frame_min, frame_max);
			PanelChrome::DrawDisclosureArrow(row_draw, arrow_center, arrow_size, true,
				ImGui::GetColorU32(EditorTheme::Token::TextDim));

			/* 名字：与图标同一条基线（行内居中），并裁到箭头区之前 —— 类型名将来变长也不会压到箭头上 */
			row_draw->PushClipRect(ImVec2(frame_min.x + icon_room, frame_min.y),
				ImVec2(arrow_center.x - arrow_size, frame_max.y), true);
			row_draw->AddText(
				ImVec2(frame_min.x + icon_room, (frame_min.y + frame_max.y - ImGui::GetFontSize()) * 0.5f),
				ImGui::GetColorU32(EditorTheme::Token::Text), TypeFilterName(m_TypeFilter));
			row_draw->PopClipRect();

			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Filter by asset type");
		}

		/* ---- 搜索框：紧挨类型筛选的左边（面板太窄、筛选被舍掉时它自己贴右端） ---- */
		ImGui::SetCursorScreenPos(ImVec2(search_x, control_y));
		ImGui::SetNextItemWidth(search_width);
		Icons::BeginSearchInput();
		ImGui::InputTextWithHint("##ResourceFilter", "Search...", m_Filter, sizeof(m_Filter));
		Icons::EndSearchInput();

		PanelChrome::EndHeaderRow(row);
	}

	/* 路径栏：当前路径的面包屑（每级可点，级间是矢量三角）+ 选中的文件，右端是缩放滑条和
	 * 统计数。挂在右栏下边。路径太长就从最前面省（越靠后越有用）。没有"上一级"按钮了 ——
	 * 点面包屑上一段就行（那枚图标白占行首一格）。 */
	void EditorResourceBrowser::ShowBrowserFooter()
	{
		PROFILE_FUNCTION();

		const ImGuiStyle& style = ImGui::GetStyle();
		ImGuiWindow* window = ImGui::GetCurrentWindow();

		/* 定位：贴所在栏（右栏）的下边（分隔线在行的正上方），连下内边距一起用掉 ——
		 * 与顶栏对称，栏的高都等于控件高，不额外留空带。 */
		const float pane_bottom = window->Pos.y + window->Size.y;
		const float footer_top = pane_bottom - ImGui::GetFrameHeight();
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

		/* 这一栏是"横条"：所有文本都在行内垂直居中（ImGui 的 Text 贴着行顶画，看起来会高半个
		 * 内边距，所以自己算居中 y；面包屑的居中量跟 text_y 取同一个值，两种画法落在同一条基线）。 */
		const float text_y = row.Min.y + (row.Height - ImGui::GetFontSize()) * 0.5f;
		/* 把光标放到居中处再画文本；顺带清掉"基线偏移"——同一行前面若是按钮 / Selectable，
		 * 它会为了基线对齐把后面的文字整体下移，居中值就被顶掉了。 */
		const auto place_text = [&](float x)
		{
			ImGui::SetCursorScreenPos(ImVec2(x, text_y));
			window->DC.CurrLineTextBaseOffset = 0.0f;
		};

		/* ---- 右端一组：缩放的滑动条 + 统计数（统计数贴最右）----
		 * 这两件原本在顶栏，移到路径栏的右端：路径栏平时只有一条路径，右端这点空间正好装下。
		 * 放不下时的退让阶梯见下面 show_count 那一段。 */
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
		/* 路径那一侧至少要装下一段面包屑（撤销了"上一级"按钮之后它就从行首开始）
		 * （选中项摘要不进这笔账：它放不下时整段不画，见下面画它的那一段） */
		const float path_min = kMinCrumbWidth;
		const float group_gap = gap * 2.0f;                          /* 路径与右端那组之间的空隙 */
		const float bar_span = row.Right - row.Min.x;

		/* 退让阶梯：① 先把滑动条压窄（放到下限也要保住统计数）→ ② 实在放不下才舍掉统计数
		 * → ③ 滑动条自己退回下限。缩放是回到网格视图的唯一入口，最先挨压的是它的宽度；
		 * 统计数是"这一层里有多少东西"的状态，能留就留。 */
		const bool show_count = (bar_span - path_min - group_gap - count_width - gap - kMinZoomSliderWidth >= 0.0f);
		const float zoom_width = ImClamp(
			bar_span - path_min - group_gap - (show_count ? count_width + gap : 0.0f),
			kMinZoomSliderWidth, kZoomSliderWidth);

		/* 统计数贴最右端、滑动条紧挨在它左边：计数文字宽度随目录内容变化，
		 * 让它贴边，滑动条才不会跟着文字变宽而左右跳。 */
		const float count_x = row.Right - count_width;
		const float zoom_x = (show_count ? count_x - gap : row.Right) - zoom_width;
		const float right_group_x = zoom_x;                          /* 摘要最多画到它左边 */

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

		/* 放不下就从最前面省：先量总宽、再决定从第几级开始画。注意：省掉前几级会多出
		 * 「… + 三角」这一截，也要算进阈值（路径栏只占右栏宽，容易漏算）。 */
		const float separator_size = CrumbSeparatorSize();

		const auto crumb_width = [&](const SharedPtr<FileNode>& node)
		{
			return ImGui::CalcTextSize(DisplayNodeName(*node).c_str()).x + style.FramePadding.x * 2.0f
				+ separator_size + gap;
		};

		const float ellipsis_width = ImGui::CalcTextSize("...").x + gap + separator_size + gap;
		/* 面包屑从行首（row.Min.x）开始排 —— 上一级按钮去掉之后，这一行的左端就是它的 */
		const float crumbs_room = crumb_right - row.Min.x;

		float crumbs_width = 0.0f;
		for (const SharedPtr<FileNode>& node : chain)
			crumbs_width += crumb_width(node);

		size_t first = 0;
		while (first + 1 < chain.size())
		{
			/* 第一级不省略时没有"…"那一截，从第二级起才算它 */
			const float extra = (first > 0) ? ellipsis_width : 0.0f;
			if (crumbs_width + extra <= crumbs_room)
				break;

			crumbs_width -= crumb_width(chain[first]);
			++first;
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

		/* 这一行的第一个控件（省略号或第一级面包屑）的位置自己钉在行首：
		 * SameLine 的参考点是"上一个项的右端"，而这一行还没有上一个项 ——
		 * 上一级按钮去掉之后，行首不再有东西替它把光标定位好。 */
		const auto start_row = [&]()
		{
			ImGui::SetCursorScreenPos(ImVec2(row.Min.x, row.Min.y));
			window->DC.CurrLineTextBaseOffset = 0.0f;
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
				start_row();
				ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::TextDim);
				place_text(row.Min.x);
				ImGui::TextUnformatted("...");
				ImGui::PopStyleColor();

				draw_separator();
			}

			/* 第一级且没被省略：它也是这一行的开头，位置同样自己钉（见 start_row） */
			if (i == first && first == 0)
				start_row();
			else
				continue_row();
			ImGui::PushID(node.get());

			/* 选中态（当前目录）用 Selectable 的选中高亮，其余可点跳转 */
			ImGui::PushStyleColor(ImGuiCol_Text,
				is_current ? EditorTheme::Token::Text : EditorTheme::Token::TextDim);

			const std::string name = DisplayNodeName(*node);
			/* 这一级最多画到右端那组左侧：算得再准，也可能有一级特别长的目录名挤不下 ——
			 * 兜这一刀（Selectable 会把标签裁到给定的宽度，悬停的 tooltip 里仍有全名），
			 * 否则它会画到滑动条底下。连一个字母都放不下就整级不画。 */
			const float crumb_room = ImMax(right_group_x - gap - ImGui::GetCursorScreenPos().x, 0.0f);

			if (crumb_room >= kMinCrumbLabelWidth)
			{
				const float label_width = ImMin(ImGui::CalcTextSize(name.c_str()).x, crumb_room);

				/* 当前级（选中）被悬停时 ImGui 画的也是 HeaderHovered —— 推"更亮的选中色"顶住 */
				if (is_current)
					ImGui::PushStyleColor(ImGuiCol_HeaderHovered, EditorTheme::RowHoverSelected);

				if (ImGui::Selectable(name.c_str(), is_current, ImGuiSelectableFlags_None,
						ImVec2(label_width, control_height)))
				{
					SetCurrentNode(node);
				}

				if (is_current)
					ImGui::PopStyleColor();

				if (ImGui::IsItemHovered() && !is_current)
				{
					/* 悬停给完整相对路径：面包屑会被省略、也可能被截断，tooltip 补全 */
					ImGui::SetTooltip("%s", node->FilePath.empty() ? name.c_str() : node->FilePath.c_str());
				}
			}

			ImGui::PopStyleColor();

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

			/* 文本的落点先量下来再定位：place_text 会把光标挪到行内居中处，
			 * 之后的裁剪矩形要按居中后的位置算（按居中前的光标算会差半个内边距）。 */
			const float selection_x = ImGui::GetCursorScreenPos().x;
			const float selected_width_clip = ImMax(selection_right - selection_x, 1.0f);

			place_text(selection_x);
			ImGui::PushClipRect(ImVec2(selection_x, text_y),
				ImVec2(selection_x + selected_width_clip, text_y + ImGui::GetFontSize()), true);
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

		/* ---- 右端的缩放滑动条 + 统计数 ----
		 * 绝对定位在右端、不参与这一行的排版（面包屑可以省，这俩不行）。滑条是「圆点在细线上」
		 * 那版：没有控件框、当前值就用圆点表示、数值走 tooltip。 */
		ImGui::SetCursorScreenPos(ImVec2(zoom_x, control_y));
		ImGui::PushID("AssetZoom");

		ImGui::SetNextItemWidth(zoom_width);
		ImGuiExt::DrawDotSliderFloat("##ThumbnailSize", m_ThumbnailSize,
			kMinThumbnailSize, kMaxThumbnailSize, "%.0f");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Thumbnail size: %.0f px — 拖到 32 以下内容区退回详细列表", m_ThumbnailSize);

		ImGui::PopID();

		if (show_count)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::TextDim);
			place_text(count_x);
			ImGui::TextUnformatted(count_text);
			ImGui::PopStyleColor();
		}

		PanelChrome::EndHeaderRow(row, false);
	}

	/* 主体：缩略图够大就用网格，否则退回详细列表 */
	void EditorResourceBrowser::ShowBrowserContent()
	{
		PROFILE_FUNCTION();

		const float panel_width = ImGui::GetContentRegionAvail().x;
		/* 内容区左上角（绝对屏幕坐标）：网格的横向起点按它算 ——
		 * 不能按 `GetCursorPosX()`（进了 Columns 之后它带着列的偏移，比内容区左沿多一档）。 */
		const float content_left_x = ImGui::GetCursorScreenPos().x;

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
			/* 两格之间至少要隔开这么远：得装得下「选中框两侧外扩」，并留一点余量 ——
			 * 只按缩略图算的话，相邻两格的选中框会贴在一起、甚至互相压上。 */
			const float min_gap = ImMax(kThumbnailGap, kGridSelectionPad * 2.0f + kMinCellGap);

			/* 网格可用的宽度 = 内容栏去掉左右各 kGridSideInset（选中框就画在内沿里） */
			const float grid_width = ImMax(panel_width - kGridSideInset * 2.0f, m_ThumbnailSize);

			/* 列数 = grid_width 里放得下、且相邻两格仍隔得开 min_gap 的最大值：
			 * n 个格子之间只有 n-1 道缝，所以下限里只算 n-1 个 min_gap（+min_gap 是凑那个数）。 */
			int column_count = static_cast<int>((grid_width + min_gap) / (m_ThumbnailSize + min_gap));
			if (column_count < 1)
				column_count = 1;

			/* 相邻两格中轴的间距：多出来的宽度均分到格子之间，首末两格因此贴着内沿。
			 * 只有一列时没有"格间"可分，格子就摆在左内沿上。 */
			const float cell_step = (column_count > 1)
				? (grid_width - m_ThumbnailSize) / static_cast<float>(column_count - 1) : 0.0f;

			/* 文件名的换行宽度：正好是「格子 + 左右各一档内沿」——
			 * 名字以格子中轴居中，最外侧那两格的名字因此恰好在内容栏内沿收住，
			 * 既不会顶到栏外被裁，也不会和邻格的名字连成一片。 */
			const float name_width = m_ThumbnailSize + kGridSideInset * 2.0f;

			/* 第一行上方让出选中框外扩的那点高度（见 kGridTopInset）：
			 * 留在内容里而不是加在内容栏的内边距上 —— 滚动范围也要跟着算进去，
			 * 否则滚到顶时框的上边还是会被裁。 */
			ImGui::Dummy(ImVec2(0.0f, kGridTopInset));

			/* 进 Columns 之前把内容栏自己的裁剪矩形与内容区范围记下来：Columns 会把
			 * window->ClipRect / WorkRect 换成"当前列"的（NextColumn 还会改 WorkRect.Max.x）。 */
			ImGuiWindow* const content_window = ImGui::GetCurrentWindow();
			const ImVec2 content_clip_min = content_window->ClipRect.Min;
			const ImVec2 content_clip_max = content_window->ClipRect.Max;
			const float content_right_x = content_window->InnerRect.Max.x;

			ImGui::Columns(column_count, nullptr, false);

			/* 网格自己的裁剪矩形：横向 = 内容区、纵向沿用内容栏。ImGui Columns 的列裁剪正好卡在列宽
			 * 上，而格子是"首末贴边"铺开 —— 最外两格的选中框会探出列边界被切。注意：每个格子都要
			 * 自己 push（BeginColumns / NextColumn 会替换裁剪栈顶，在循环外 push 对第二列就失效了）。 */
			ImDrawList* const grid_draw = ImGui::GetWindowDrawList();
			const ImVec2 grid_clip_min(content_left_x, content_clip_min.y);
			const ImVec2 grid_clip_max(content_right_x, content_clip_max.y);

			/* 格子的 x：首格贴内容栏左内沿，往后依次加 cell_step、末格贴右内沿 —— 富余的宽度均分到
			 * 格子之间（以前是把每列居中，两侧留下 15~36px 死区、格子浮在中间）。 */
			const float grid_left = content_left_x + kGridSideInset;
			size_t cell_index = 0;

			for (const SharedPtr<FileNode>& child_node : visible)
			{
				ImGui::PushID(child_node->FilePath.c_str());
				{
					const int column_index = static_cast<int>(cell_index) % column_count;
					const float cell_left = grid_left + cell_step * static_cast<float>(column_index);
					const bool is_selected = IsNodeSelected(child_node.get());
					const ImVec2 cell_rect(m_ThumbnailSize, m_ThumbnailSize);
					const SharedPtr<DeviceTexture> thumbnail = ThumbnailOf(*child_node);

					grid_draw->PushClipRect(grid_clip_min, grid_clip_max, false);

					ImGui::SetCursorScreenPos(ImVec2(cell_left, ImGui::GetCursorScreenPos().y));

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

					/* 正在改名的那一格：交互全让给输入框（点到别处 = 提交），这一格只当垫底 ——
					 * 点击仍要记进 clicked_an_item，才不会被当成"点空白"清掉选择 */
					const bool is_editing_this = (m_RenameEditPath == child_node->FilePath);

					if (is_editing_this)
					{
						if (ImGui::IsItemClicked())
						{
							clicked_an_item = true;
							m_PressSkipsSlowClick = true;   /* 这一按是"收起编辑器"，不作慢双击判定 */
						}
					}
					else
					{
						/* 拖拽（拖给别的面板用，或拖进目录树里换个目录 —— 见目录树行的拖放目标） */
						if (ImGui::BeginDragDropSource())
						{
							const char* item_path = child_node->FilePath.c_str();
							ImGui::SetDragDropPayload(kAssetDragPayload, item_path, strlen(item_path) + 1);
							ImGui::EndDragDropSource();
						}

						/* 单击 = 选中（Shift 连选 / Ctrl 加选，见 ApplyClickSelection）；
						 * 按下帧同时记下"这一按是不是双击的第二击"（松开帧判定慢双击用） */
						if (ImGui::IsItemClicked())
						{
							clicked_an_item = true;
							ApplyClickSelection(child_node, visible);
							m_PressSkipsSlowClick = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
						}

						/* 快速双击的第二击：文件夹 = 进去，文件 = 就地改名。注意：带修饰键的双击不算"打开" ——
						 * Ctrl / Shift 连点同一个文件夹是在加选 / 移出，两次落在同一格被判成双击会意外进目录（选择被清空）。 */
						if (!ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyShift && !ImGui::GetIO().KeySuper
							&& ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
						{
							if (child_node->Type == FileType::Folder)
							{
								ClearRenameCandidate();
								SetCurrentNode(child_node);
							}
							else
							{
								BeginInlineRename(child_node);
							}
						}

						/* 慢双击（停顿后的第二击）= 就地改名（判定见 HandleItemRenameRelease） */
						if (ImGui::IsItemDeactivated())
							HandleItemRenameRelease(child_node);
					}

					/* 右键菜单：点在没选中的项上先把它选中（已在选中集合里就保持多选不变） */
					if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)
						&& !IsNodeSelected(child_node.get()))
						SelectSingle(child_node);

					if (ImGui::BeginPopupContextItem())
					{
						DrawAssetContextMenuItems(child_node->FilePath, child_node->Type == FileType::Folder);
						ImGui::EndPopup();
					}

					/* 文件名以格子中轴居中（和缩略图同轴），长名称按 name_width 换行；
					 * 光标一起摆：绘制区与它后面那个 Dummy 都从这一档算起。
					 * 正在改名的那一项换成输入框 —— 同一落点、同宽，看起来就在原地。 */
					ImGui::SetCursorScreenPos(ImVec2(
						cell_left + (m_ThumbnailSize - name_width) * 0.5f, ImGui::GetCursorScreenPos().y));

					if (m_RenameEditPath == child_node->FilePath)
					{
						ImGui::SetNextItemWidth(name_width);
						DrawInlineRenameEditor(child_node);
					}
					else
					{
						DrawCenteredWrappedText(child_node->FileName, name_width,
							ImGui::GetColorU32(is_selected ? EditorTheme::Token::Accent : EditorTheme::Token::Text));
					}

					grid_draw->PopClipRect();

					++cell_index;
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
			/* 列表模式三列（名字 / 类型 / 大小）；横向的账跟网格一套：表宽 = 内容栏宽 - 两侧内沿，
			 * 名字列 WidthStretch 吃掉剩余，类型 / 大小推到右端；NoPadOuterX 拿掉表格外边距。 */
			const float list_width = ImMax(panel_width - kGridSideInset * 2.0f, 160.0f);
			ImGui::SetCursorScreenPos(ImVec2(content_left_x + kGridSideInset, ImGui::GetCursorScreenPos().y));
			ImGui::BeginTable("Assets List", 3, ImGuiTableFlags_NoPadOuterX, ImVec2(list_width, 0.0f));
			{
				ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_NoHide | ImGuiTableColumnFlags_WidthStretch);
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

		/* 慢双击候选只在"点在同一项上"的连击里成立：这一帧的左键没按在任何条目上
		 * （空白 / 面板别的部件 / 面板之外）→ 候选作废 —— 否则"点资源 → 点实体 →
		 * 再点资源"的最后那一下会被当成慢双击，把改名框带出来。 */
		if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !clicked_an_item)
			ClearRenameCandidate();

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
				/* 分类只此一处：后缀 -> 大类在 AssetFileOps 那张表里（图标与类型筛选都跟着它走），
				 * 这里只把大类翻成面板的 FileType。加一种资源类型 = 那边的表加几行 + 这里的 switch
				 * 加一个 case + FileType / TypeFilter / 图标各加一项。 */
				file_node->Type = FileTypeOfKind(AssetFileKindOf(PathToUtf8(filename.extension())));

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

		/* 单元格里的"类型 / 大小"用绘制列表画、不建条目：整行的命中与悬停都归整行的
		 * Selectable（再建条目会跟它抢悬停）。
		 * align_right：数字右对齐贴本列右端，位数不齐也成一条线。 */
		const auto draw_cell_text = [](const std::string& text, bool align_right)
		{
			if (text.empty())
				return;

			const ImVec2 origin = ImGui::GetCursorScreenPos();
			const float text_width = ImGui::CalcTextSize(text.c_str()).x;
			const float x = align_right
				? origin.x + ImMax(ImGui::GetContentRegionAvail().x - text_width, 0.0f) : origin.x;
			ImGui::GetWindowDrawList()->AddText(ImVec2(x, origin.y),
				ImGui::GetColorU32(EditorTheme::Token::Text), text.c_str());
		};

		for (const SharedPtr<FileNode>& child_node : visible)
		{
			const bool is_folder = (child_node->Type == FileType::Folder);
			const bool is_selected = IsNodeSelected(child_node.get());

			ImGui::PushID(child_node->FilePath.c_str());

			ImGui::TableNextRow();
			ImGui::TableNextColumn();

			/* 名字列内容的落点要在进 Selectable 之前取：这是本列内容的起点（与表头的
			 * Name 同一条竖线）；进 Selectable 之后光标已被推到下一行。 */
			const ImVec2 name_origin = ImGui::GetCursorScreenPos();
			/* 名字列可用宽度（同样在进 Selectable 之前量）：就地改名的输入框按它收宽 */
			const float name_column_width = ImGui::GetContentRegionAvail().x;

			/* 整行一个 Selectable（`SpanAllColumns`）：高亮与命中区都是整行（含"类型 / 大小"两列）。
			 * `AllowItemOverlap`：右侧两列的自绘文字不抢整行的悬停。 */
			/* 选中行被悬停时 ImGui 画的也是 HeaderHovered —— 推"更亮的选中色"顶住 */
			if (is_selected)
				ImGui::PushStyleColor(ImGuiCol_HeaderHovered, EditorTheme::RowHoverSelected);

			ImGui::Selectable("##row", is_selected,
				ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowItemOverlap);

			if (is_selected)
				ImGui::PopStyleColor();

			/* 正在改名的那一行：交互全让给输入框（点到别处 = 提交），这一行只当垫底 ——
			 * 点击仍要记进 clicked_an_item，才不会被当成"点空白"清掉选择 */
			const bool is_editing_this = (m_RenameEditPath == child_node->FilePath);

			if (is_editing_this)
			{
				if (ImGui::IsItemClicked())
				{
					clicked_an_item = true;
					m_PressSkipsSlowClick = true;   /* 这一按是"收起编辑器"，不作慢双击判定 */
				}
			}
			else
			{
				/* 单击 = 选中（Shift 连选 / Ctrl 加选，见 ApplyClickSelection）；
				 * 按下帧同时记下"这一按是不是双击的第二击"（松开帧判定慢双击用） */
				if (ImGui::IsItemClicked())
				{
					clicked_an_item = true;
					ApplyClickSelection(child_node, visible);
					m_PressSkipsSlowClick = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
				}

				/* 快速双击的第二击：文件夹 = 进去、文件 = 就地改名（见网格里那段说明）；
				 * 带修饰键的双击是加选 / 移出，不是"打开" */
				if (!ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyShift && !ImGui::GetIO().KeySuper
					&& ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
				{
					if (is_folder)
					{
						ClearRenameCandidate();
						SetCurrentNode(child_node);
					}
					else
					{
						BeginInlineRename(child_node);
					}
				}

				/* 慢双击（停顿后的第二击）= 就地改名（判定见 HandleItemRenameRelease） */
				if (ImGui::IsItemDeactivated())
					HandleItemRenameRelease(child_node);
			}

			/* 右键菜单：点在没选中的项上先把它选中 */
			if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)
				&& !is_selected)
				SelectSingle(child_node);

			if (ImGui::BeginPopupContextItem())
			{
				DrawAssetContextMenuItems(child_node->FilePath, is_folder);
				ImGui::EndPopup();
			}

			/* 名字列的内容（图标 + 文件名）：自己画在 Selectable 之上 —— `DrawTreeRowLabel` 的
			 * 树形让位（箭头 + 缩进）会让图标落不到列首、与表头的 Name 对不齐。
			 * 正在改名的那一项：图标照旧，名字换成输入框（接在图标后、吃满本列剩余宽度）。 */
			{
				const float icon_size = ImGui::GetFontSize();
				const ImU32 text_color = ImGui::GetColorU32(EditorTheme::Token::Text);
				ImDrawList* const draw_list = ImGui::GetWindowDrawList();

				Icons::Draw(draw_list, ResolveFileIcon(child_node->Type),
					ImVec2(name_origin.x + icon_size * 0.5f, name_origin.y + icon_size * 0.5f), icon_size, text_color);

				if (m_RenameEditPath == child_node->FilePath)
				{
					const float rename_width = ImMax(name_column_width
						- icon_size - ImGui::GetStyle().ItemInnerSpacing.x, 60.0f);
					ImGui::SetCursorScreenPos(ImVec2(
						name_origin.x + icon_size + ImGui::GetStyle().ItemInnerSpacing.x, name_origin.y));
					ImGui::SetNextItemWidth(rename_width);
					DrawInlineRenameEditor(child_node);
				}
				else
				{
					draw_list->AddText(ImVec2(name_origin.x + icon_size + ImGui::GetStyle().ItemInnerSpacing.x,
						name_origin.y), text_color, child_node->FileName.c_str());
				}
			}

			/* 文件类型 */
			ImGui::TableNextColumn();
			draw_cell_text(ExtractFileSuffix(child_node->FileName), false);

			/* 文件大小（文件夹不计） */
			ImGui::TableNextColumn();
			draw_cell_text(is_folder ? std::string() : (ToString(child_node->FileSize, 2) + "KB"), true);

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
