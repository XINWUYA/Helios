#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>
#include <imgui.h>
#include "EditorIcons.h"
#include "Command/AssetFileOps.h"
#include "Helios/Command/Command.h"
#include "Helios/Common/Common.h"

namespace Helios
{
	/* 资源浏览器：顶部一行（搜索 + 四枚图标：回退 / 重进 / 筛选 / 新建）+ 左右两栏（目录树 |
	 * 内容区）+ 右栏底部路径栏（面包屑 + 缩放滑条 + 统计数）。文件操作（新建 / 重命名 / 删除）
	 * 做成 ICommand 进编辑历史，所以 Ctrl+Z / Ctrl+Y 对文件操作同样有效。 */
	class EditorResourceBrowser final : public IAssetChangeSink
	{
	public:
		EditorResourceBrowser() = default;
		~EditorResourceBrowser() override = default;

		/* 渲染UI */
		void OnImGuiRenderer();

		/* 编辑历史通道（跨面板能力，由主壳层注入）：面板不认识 Layer / CommandStack，只把命令交
		 * 出去。没注入时就直接执行命令（headless 测试能用，只是没有历史）。 */
		struct HistorySink
		{
			/* 把命令交给编辑历史（撤销 / 重做从此也管着它） */
			std::function<void(UniquePtr<ICommand>)> Execute;
		};

		void SetHistorySink(HistorySink sink) { m_History = std::move(sink); }

		/* 选中项 → 属性面板的通道（跨面板能力，由主壳层注入）：
		 * 面板之间互不认识，这里只把"选了什么"交给外壳转达。
		 * 选中项变化过（含同一项被再次点选、按路径对齐后的改名）才会通知一次。 */
		struct AssetSelectionSink
		{
			std::function<void(const std::vector<AssetSelectionEntry>&)> Changed;
		};

		void SetAssetSelectionSink(AssetSelectionSink sink) { m_AssetSelectionSink = std::move(sink); }

		/* 在浏览器里定位一个资源（跨面板能力：属性面板点贴图 → 跳到这里）：
		 * 切到它所在目录并选中它。路径按相对 Assets 的口径给；
		 * 找不到（外部新增、轮询未到）就记为待选中并触发一次重建。 */
		void RevealAsset(const std::string& relative_path);

		/* 资源改动的观察者（IAssetChangeSink）：文件命令做完 —— 含撤销与重做 —— 通知它，
		 * 界面据此同步当前目录与选中项，并立刻重建文件树（不用等目录轮询）。 */
		void OnAssetPathChanged(const std::string& from, const std::string& to) override;

	private:
		/* 文件类型。除 Folder（是不是目录）与 Default（认不出的后缀）外，
		 * 其余几档与 AssetFileKind 一一对应 —— 后缀归属哪一类在 AssetFileOps 的那张表里。 */
		enum class FileType : uint8_t
		{
			Default,
			Folder,
			Image,
			Scene,
			MtlGraph,
			Shader,
			Model,
			Material,
			Probe,
		};

		/* 内容筛选里的「按类型」（工具行下拉）：All = 不按类型筛。
		 * 除 All 外与 FileType 一一对应（名字、判据、顺序都跟着走）——
		 * 加一种资源类型时，这两处要一起加（还有分类那一处，见 BuildFileNodeTree）。 */
		enum class TypeFilter : uint8_t
		{
			All = 0,
			Folder,
			Image,
			Scene,
			MtlGraph,
			Shader,
			Model,
			Material,
			Probe,
			COUNT
		};

		/* 名字输入弹层的三种来路。只有标题、后缀约束与"确认后建什么"不同 ——
		 * 输入框、校验、唯一取名、按钮全都是同一套，所以不拆成三个函数。 */
		enum class NamePopupMode : uint8_t
		{
			NewFolder = 0,
			NewFile,       /* 新建资源文件：后缀由菜单里的那一项定（m_PendingNewFileExtension） */
			Rename,
		};

		/* 资源目录下的文件节点，包含文件夹和文件 */
		struct FileNode
		{
			std::string FileName;							/* 文件名 */
			std::string FilePath;							/* 记录当前文件路径 */
			FileType	Type;								/* 文件类型 */
			float		FileSize{ 0 };						/* 文件大小 */
			int			Depth{ -1 };						/* 文件夹距离目标目录的层数 */
			std::vector<SharedPtr<FileNode>> ChildNodes;	/* 子节点(文件夹目录下可能有多个) */
			WeakPtr<FileNode> ParentNode;					/* 父节点，这里使用WeakPtr, 不然会产生智能指针循环引用，导致内存泄漏 */
			/* 图片缩略图：只有图片有，且按需加载（见 ThumbnailOf） */
			SharedPtr<DeviceTexture> Thumbnail;
			bool		ThumbnailResolved{ false };			/* 已尝试加载过（避免失败后每帧重试） */

			FileNode() = default;
            FileNode(std::string name, std::string path, FileType type, float size, int depth)
				: FileName(std::move(name)), FilePath(std::move(path)), Type(type), FileSize(size), Depth(depth)
			{}
			~FileNode()
			{
				ChildNodes.clear();
			}

			bool operator==(const FileNode& other) const
			{
				return	this->FileName == other.FileName &&
					this->FilePath == other.FilePath &&
					this->Type == other.Type &&
					this->Depth == other.Depth &&
					this->ParentNode.lock() == other.ParentNode.lock();
			}
		};

		/* 递归构建文件节点树 */
		void BuildFileNodeTree(const SharedPtr<FileNode>& parent_node);

		/* 主体：左「Folders」目录树卡 + 可拖分隔条 + 右栏（上面内容区 / 下面钉住的路径栏）。两栏
		 * 子窗口各自把上下内边距加回来（面板自己不带，顶栏要贴面板上边）。 */
		void ShowBrowserBody();
		/* 目录树（左栏）：只列文件夹（文件是右栏的事） */
		void DrawFolderNode(const SharedPtr<FileNode>& node);

		/* 顶部一行：左端搜索框；右端四枚图标按钮（自右向左：新建、筛选、前进、后退）。当前目录名
		 * 不在这显示（路径栏说得更清楚）。筛选是纯图标按钮（跟 Debug View / Gizmos 同款），
		 * 点击弹类型单选菜单，生效时高亮。 */
		void ShowBrowserTopBar();
		/* 底部栏：当前路径面包屑（每级可点）+ 选中的项（右端是滑动条与统计数） */
		void ShowBrowserFooter();
		/* 内容区（右栏）：缩略图网格 / 详细列表两种模式。
		 * 先算出这一帧的可见项（显示顺序），网格、列表、范围选择、全选、计数都用它，
		 * 免得四处各过滤一遍而慢慢走样。 */
		void ShowBrowserContent();
		/* 详细列表模式的行；返回"这一帧有没有点到某一行"（调用方据此判断是不是点的空白处） */
		[[nodiscard]] bool BuildFileListDetail(const std::vector<SharedPtr<FileNode>>& visible);

		/* ---- 选择（内容区，可多选）----
		 * 选择只存"当前目录里的项"：切目录就清空、重建后按相对路径找回来。都要走下面几个入口，
		 * 别在绘制代码里直接改 m_Selection。 */
		[[nodiscard]] bool IsNodeSelected(const FileNode* node) const;
		/* 单选（清掉其它），并把 Shift 锚点挪到它 */
		void SelectSingle(const SharedPtr<FileNode>& node);
		/* Ctrl/Cmd 点选：在选中集合里增删（也挪锚点） */
		void ToggleSelection(const SharedPtr<FileNode>& node);
		/* Shift 点选：把锚点到它的这一段（按显示顺序）全部选中；锚点不动 */
		void SelectRangeTo(const SharedPtr<FileNode>& node, const std::vector<SharedPtr<FileNode>>& visible);
		/* 点选分发：Shift 连选 / Ctrl 加选 / 其它单选 */
		void ApplyClickSelection(const SharedPtr<FileNode>& node, const std::vector<SharedPtr<FileNode>>& visible);
		/* 清空选择 */
		void ClearSelection();
		/* 内容区的快捷键：Ctrl/Cmd+A 全选、Esc 清空（正在输入文字时一律不抢键） */
		void HandleSelectionShortcuts(const std::vector<SharedPtr<FileNode>>& visible);

		/* 按类型筛选下拉的显示名 */
		static const char* TypeFilterName(TypeFilter filter);
		/* 类型筛选菜单行的图标：与内容区各类型的图标同一套（All = 漏斗本体） */
		static Icons::Id FilterIconOf(TypeFilter filter);
		/* 该项的类型是否通过筛选 */
		static bool MatchesTypeFilter(TypeFilter filter, FileType type);
		/* 内容区里这一项要不要显示：类型筛选 + 文本过滤。
		 * 网格、详细列表与头部计数都走它，避免三处各判一遍而慢慢走样。 */
		[[nodiscard]] bool PassesContentFilter(const FileNode& node) const;
		/* 图片缩略图按需加载：只有真的要画的时候才建贴图（建树时全加载会拖慢启动、白占显存） */
		static SharedPtr<DeviceTexture> ThumbnailOf(FileNode& node);

		/* ---- 浏览位置（右栏当前目录）与它的历史 ----
		 * 顶栏的「回到上次路径 / 重进路径」走这里：历史按相对 Assets 的路径记
		 * （文件树会重建、节点指针不保险 —— 与文件操作的待办同一套理由）。 */

		/* 切换当前目录（用户导航：目录树点击 / 面包屑 / 双击文件夹）：记一条历史 */
		void SetCurrentNode(const SharedPtr<FileNode>& node);
		/* 落位到某个节点：切目录 + 清选择（静默 —— 浏览位置变化不通知属性面板，
		 * 见 .cpp 同名函数）+ 置上「下次画目录树时把它露出来」（见 SetCurrentNode）。
		 * 从历史里跳转时不重复记历史，所以"记历史"与"落位"拆开。 */
		void ApplyCurrentNode(const SharedPtr<FileNode>& node);
		/* 记一条浏览历史：从历史中间走新路时丢掉"前进"的那一段（与浏览器一致） */
		void RecordNavigation(const std::string& path);
		/* 沿 direction（-1 = 回到上次路径、+1 = 重进路径）找历史里最近的一条还进得去的项：
		 * 找到时返回下标、并把节点写进 node；没有这样的项就返回 -1（按钮据此置灰）。 */
		int FindHistoryStep(int direction, SharedPtr<FileNode>& node) const;
		/* 走一步历史（点按钮时用）：游标落到目标项，不产生新的历史 */
		void NavigateHistory(int direction);

		/* 文件类型 -> 图标：与层级面板共用同一套矢量图标语言 */
		static Icons::Id ResolveFileIcon(FileType type);

		/* 后缀分出来的大类（AssetFileOps）-> 面板自己的 FileType。
		 * 面板比大类多两档：Folder（来自"这是不是目录"，与后缀无关）、
		 * Default（= 大类里的 Other，认不出的后缀）。 */
		static FileType FileTypeOfKind(AssetFileKind kind);

		/* 面板自己的 FileType -> 资源大类（跨面板摘要用 AssetFileKind 讲话） */
		static AssetFileKind AssetKindOfFileType(FileType type);

		/* 节点的显示名：根节点的 FileName 存的是整条绝对路径，用最后一级目录名代替 */
		static std::string DisplayNodeName(const FileNode& node);

		/* 在树中按相对路径查找节点：重建后据此恢复浏览位置 */
		static SharedPtr<FileNode> FindNode(const SharedPtr<FileNode>& node, const std::string& path);
		/* node 是否等于 target 或它的祖先（用于判断要不要把某个目录展开） */
		static bool IsAncestorOrSelf(const SharedPtr<FileNode>& node, const SharedPtr<FileNode>& target);

		/* ---- 文件操作（新建 / 重命名 / 删除）----
		 * 除了新建资源走顶栏「新建」按钮，其余都在右键菜单里（内容区和目录树都有），执行前弹输入 /
		 * 确认层。待办目标一律按相对 Assets 的路径记（文件树会重建，节点指针不保险）。 */

		/* 右键菜单的条目（在已经开好的 popup 里画） */
		void DrawAssetContextMenuItems(const std::string& path, bool is_folder);
		/* 空处右键：在当前目录下新建 */
		void DrawBackgroundContextMenuItems();
		/* 各弹层（新建资源 / 新建文件夹 / 新建文件 / 重命名 / 删除确认）都画在面板根作用域（右键
		 * 菜单在子窗口里，各自算出来的 ID 不同）。theme_padding 是主题的窗口内边距（面板压成了 0），
		 * 弹层用它补回上下内边距。 */
		void DrawAssetOperationPopups(const ImVec2& theme_padding);
		/* 顶栏「新建」按钮的下拉菜单：文件夹 + 几种可建的资源文件（见 .cpp 里的表） */
		void DrawNewAssetMenu();
		/* 顶栏「筛选」按钮的下拉菜单（画在面板根作用域）：类型单选列表，
		 * 行点击不收起弹层（连点切换档位，与工具栏的菜单同款） */
		void DrawTypeFilterMenu();

		/* 打开弹层：路径都是相对 Assets 的 */
		void OpenNewFolderPopup(const std::string& parent_path);
		/* 新建资源文件：extension 形如 ".scn"（空 = 不限后缀，用户自己写）；
		 * default_stem 是预填的名字（不带后缀），parent_path 是新建在哪个目录下 */
		void OpenNewFilePopup(const std::string& extension, const std::string& default_stem,
			const std::string& parent_path);
		void OpenRenamePopup(const std::string& path);
		void OpenDeletePopup(std::vector<std::string> paths);

		/* 名字输入弹层（新建与重命名共用：mode 决定标题、后缀约束与确认后建什么） */
		void DrawNamePopup(ImGuiID popup_id, NamePopupMode mode);

		/* ---- 内容区就地改名 ----
		 * 文件快速双击直接改；文件夹双击是进目录，改名 = 选中后停一下再点（慢双击）。回车提交
		 * （RenameAssetCommand 进历史）、Esc 取消、点到别处也提交。 */

		/* 开始就地改名：名字装进输入框，焦点交给它 */
		void BeginInlineRename(const SharedPtr<FileNode>& node);
		/* 收起编辑器：commit 且名字确实改过（合法）才落盘；node 为空 = 目标已不在（只清状态） */
		void FinishInlineRename(const SharedPtr<FileNode>& node, bool commit);
		/* 慢双击（停顿后的第二击）判定：内容项"松开"时网格 / 列表共用 */
		void HandleItemRenameRelease(const SharedPtr<FileNode>& node);
		/* 就地改名的输入框（共用：调用方把光标摆到名字的落点与宽度上） */
		void DrawInlineRenameEditor(const SharedPtr<FileNode>& node);
		/* 作废慢双击候选（拖动 / 修饰键点击 / 点到别处 / 选择被清 / 切目录） */
		void ClearRenameCandidate();

		/* 把命令塞进编辑历史（没有注入通道时直接执行） */
		void ExecuteCommand(UniquePtr<ICommand> command);

		/* ---- 目录树拖拽移动 = 复用重命名命令（改名即移动）----
		 * 落盘延后到面板画完（ApplyPendingDropMove）：这一帧树还在按旧节点遍历，挪走目录会让递归中
		 * 的父子路径失真 —— 跟层级面板"挂接延后"同一个理由。 */

		/* 拖放目标收到 payload：登记待办（into_dir 是相对 Assets 的口径，空串 = 资源根） */
		void RecordDropMove(const ImGuiPayload* payload, const std::string& into_dir);
		/* 待办落盘：裁决（AssetMoveError）通过就发一条 RenameAssetCommand 进编辑历史 */
		void ApplyPendingDropMove();

		/* 相对 Assets 的路径 → 绝对路径 */
		static std::filesystem::path AbsoluteAssetPath(const std::string& relative_path);

		/* 待办：新建时是父目录，重命名时是被改名的目标；两个都空 = 没有待办 */
		std::string m_PendingParentPath;
		std::string m_PendingRenamePath;
		/* 目录树拖拽的待办落点（画完后统一落盘，见 ApplyPendingDropMove）：
		 * 两个路径都是相对 Assets 的口径；m_PendingDropFrom 为空 = 没有待办，
		 * m_PendingDropInto 为空串 = 资源根（拖到树下方的空白）。 */
		std::string m_PendingDropFrom;
		std::string m_PendingDropInto;
		/* 新建资源文件要求的后缀（含点，如 ".scn"；空 = 不限后缀），只在 NamePopupMode::NewFile 用 */
		std::string m_PendingNewFileExtension;
		/* 删除确认里的目标 */
		std::vector<std::string> m_PendingDeletePaths;
		/* 名字输入框的内容（新建 / 重命名共用） */
		char m_NameBuffer[128]{};
		/* 「新建」菜单的过滤词（与层级、属性面板的菜单同款：打开即清空并聚焦） */
		char m_NewAssetFilter[64]{};
		/* 面板根作用域上算好的弹层 ID（见 OnImGuiRenderer） */
		ImGuiID m_PopupNewMenu{ 0 };
		ImGuiID m_PopupNewFolder{ 0 };
		ImGuiID m_PopupNewFile{ 0 };
		ImGuiID m_PopupRename{ 0 };
		ImGuiID m_PopupDelete{ 0 };
		ImGuiID m_PopupTypeFilter{ 0 };
		/* 「筛选」菜单的弹层锚点（按钮右下角，弹层锚在它正下方、右缘对齐——
		 * 与工具栏的 Debug View / Gizmos 菜单同款锚法） */
		ImVec2 m_FilterMenuAnchor{ 0.0f, 0.0f };

		/* ---- 内容区就地改名的编辑态与慢双击候选 ---- */

		/* 正在就地改名的那一项（相对 Assets 的路径；空 = 没有） */
		std::string m_RenameEditPath;
		/* 输入框内容（提交 / 取消后清掉） */
		char m_RenameEditBuffer[128]{};
		/* 激活后的第一帧把键盘焦点交给输入框（SetKeyboardFocusHere 只发一次请求） */
		bool m_RenameEditFocus{ false };
		/* 本帧编辑器画没画：没画（被过滤 / 切了目录 / 被删掉）就在帧末静默收起 */
		bool m_RenameEditDrawn{ false };
		/* 慢双击候选：上一次"落在唯一选中项上"的点击（路径 + 松开时刻）。
		 * 下一击落在同一项、且隔开了双击窗口，才算"停顿后的第二击" → 就地改名。 */
		std::string m_RenameCandPath;
		double m_RenameCandTime{ -1.0 };
		/* 按下帧记下"这一按在松开时跳过慢双击判定"（按下帧写、松开帧消费）：
		 * 快速双击的第二击（打开 / 进目录 / 文件的直接改名），以及点在正在改名的那一项上
		 * （那是"收起编辑器"，不是慢双击的前一下）。 */
		bool m_PressSkipsSlowClick{ false };

		/* 文件操作（含撤销 / 重做）改过路径后，下一次重建文件树时按它对齐：
		 * from 为空 = 新建（顺带选中新建的那一项），to 为空 = 删除（找不到就丢掉）。 */
		std::string m_RemapFrom;
		std::string m_RemapTo;
		std::string m_PendingSelectPath;

		/* 跨面板通道：编辑历史（文件操作要进同一条历史 —— 撤销 / 重做由菜单与主工具栏触发）。
		 * 只为这一个用途，由主壳层注入。 */
		HistorySink m_History;

		/* 选中项 → 属性面板（见 AssetSelectionSink，由主壳层注入） */
		AssetSelectionSink m_AssetSelectionSink;
		/* 本帧有"选择动作"（点选 / 清空）—— 同一项被再次点选也要重新通知一次，
		 * 否则"点资源 → 点实体 → 再点同一资源"的第三次点击不会把属性面板切回来。
		 * 切目录的静默清空把它也一并吞掉（那次清空不通知面板，见 ApplyCurrentNode）。 */
		bool m_SelectionActivated{ false };
		/* 这次按住左键的结局是不是拖动：是的话，按住期间的选择变化就不通知属性面板（拖动是
		 * "拿去别处用"）—— 否则拖动一起手就发布，属性面板切到资源详情、材质卡的拖放目标当场消失。 */
		bool m_PressWasDrag{ false };
		/* 上次发布过的选中路径：文件树重建后节点指针会换，路径才是稳定身份。
		 * 切目录时随选择一起静默清掉（浏览位置变化不发布，见 ApplyCurrentNode）。 */
		std::vector<std::string> m_PublishedSelectionPaths;
		/* 选中项变了就通知属性面板（每帧末尾调用一次） */
		void PublishAssetSelection();

		/* 资源目录是否被改动（新增 / 删除资源，或编辑器之外的操作） */
		[[nodiscard]] bool HasDirectoryChanged() const;
		/* 记录当前目录状态，供下次比较 */
		void UpdateDirectoryStamp();

		/* 获取文件相对路径 */
		static std::filesystem::path GetRelativePath(const std::filesystem::path& dir, const std::filesystem::path& path);

		/* 资源目录的文件节点树 */
		SharedPtr<FileNode> m_RootFileNodeTree{};
		/* 当前选中的文件节点 */
		SharedPtr<FileNode> m_CurrentFileNode{};

		/* 浏览位置的历史与游标：游标左边是「回到上次路径」能去的，右边是「重进路径」能去的。
		 * 首次重建时补上根目录一项 —— 第一次导航之后就有"上次"可回。 */
		std::vector<std::string> m_NavHistory;
		size_t m_NavCursor{ 0 };
		/* 右栏里选中的项（可多选）：网格 / 列表据此高亮，底栏显示它们。
		 * 顺序 = 点选顺序（Shift 范围按显示顺序）；切目录时清掉 —— 它们属于上一个目录。
		 * `m_SelectionAnchor` 是 Shift 范围选择的锚点（最近一次"不带 Shift 的点选"）。 */
		std::vector<SharedPtr<FileNode>> m_Selection;
		SharedPtr<FileNode> m_SelectionAnchor;

		/* 只有文件夹文件变化时才更新的文件节点树 */
		bool m_IsDirty{ true };

		/* 过滤词（空 = 全部显示） */
		char m_Filter[64]{};
		/* 按类型筛选（工具行下拉） */
		TypeFilter m_TypeFilter{ TypeFilter::All };
		/* 本帧的过滤结果：小写化的过滤词 + 通过过滤的节点（命中项 + 它们的全部祖先）。
		 * 每帧现算一次，不留第二份"是否显示"的缓存。 */
		std::string m_FilterNeedle;
		std::unordered_set<const FileNode*> m_VisibleNodes;
		void FillBrowserFilter();

		/* 目录树占面板可用宽度的比例（可拖，用户偏好所以留在实例上）。
		 * 目录名都不长，默认收窄到三成二：左栏够放下嵌套几级的目录名即可，
		 * 余下的宽度留给内容区（网格能多铺一列）。 */
		float m_TreePaneRatio{ 0.32f };
		/* 待办：把当前目录在目录树里露出来（展开祖先 + 滚到可见），下一次画完树即清 */
		bool m_RevealCurrentNode{ true };
		/* 目录树行"按下时是点击意图"的待办：松开那一帧才决定要不要真的切目录 ——
		 * 中途变成拖拽（把文件夹拖去别处）就不算点击，与"拖动不是查看"同一套语义
		 * （见 DrawFolderNode 的松开判定）。 */
		SharedPtr<FileNode> m_PendingTreeNav;
		/* 缩略图尺寸与间距：视图菜单里可调，属于"用户偏好"，所以留在实例上 */
		float m_ThumbnailSize{ 64.0f };

		/* 目录状态指纹：目录数量 + 最新的目录写入时间 */
		size_t m_DirectoryCount{ 0 };
		std::filesystem::file_time_type m_NewestDirectoryWriteTime{};
		/* 距上次检查目录状态的累计时间 */
		float m_RefreshElapsed{ 0.0f };
	};
}
