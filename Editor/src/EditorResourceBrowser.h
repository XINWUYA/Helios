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

	/* 资源浏览器：顶部一行（搜索 + 四枚图标：回退 / 重进 / 筛选 / 新建）+ 左右两栏（目录树 |
	 * 内容区）+ 右栏底部路径栏（面包屑 + 缩放滑条 + 统计数）。文件操作（新建 / 重命名 / 删除）
	 * 做成 ICommand 进编辑历史，所以 Ctrl+Z / Ctrl+Y 对文件操作同样有效。 */
		void ShowBrowserTopBar(const ImVec2& theme_padding);
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
		/* 落位到某个节点：切目录 + 清选择 + 置上「下次画目录树时把它露出来」（见 SetCurrentNode）。
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

		/* 把命令塞进编辑历史（没有注入通道时直接执行） */
		void ExecuteCommand(UniquePtr<ICommand> command);

		/* 相对 Assets 的路径 → 绝对路径 */
		static std::filesystem::path AbsoluteAssetPath(const std::string& relative_path);

		/* 待办：新建时是父目录，重命名时是被改名的目标；两个都空 = 没有待办 */
		std::string m_PendingParentPath;
		std::string m_PendingRenamePath;
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

		/* 文件操作（含撤销 / 重做）改过路径后，下一次重建文件树时按它对齐：
		 * from 为空 = 新建（顺带选中新建的那一项），to 为空 = 删除（找不到就丢掉）。 */
		std::string m_RemapFrom;
		std::string m_RemapTo;
		std::string m_PendingSelectPath;

		/* 跨面板通道：编辑历史（文件操作要进同一条历史 —— 撤销 / 重做由菜单与主工具栏触发）。
		 * 只为这一个用途，由主壳层注入。 */
		HistorySink m_History;

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

		/* 目录树占面板可用宽度的比例（可拖，用户偏好所以留在实例上） */
		float m_TreePaneRatio{ 0.42f };
		/* 待办：把当前目录在目录树里露出来（展开祖先 + 滚到可见），下一次画完树即清 */
		bool m_RevealCurrentNode{ true };
		/* 缩略图尺寸与间距：视图菜单里可调，属于"用户偏好"，所以留在实例上 */
		float m_ThumbnailSize{ 64.0f };

		/* 目录状态指纹：目录数量 + 最新的目录写入时间 */
		size_t m_DirectoryCount{ 0 };
		std::filesystem::file_time_type m_NewestDirectoryWriteTime{};
		/* 距上次检查目录状态的累计时间 */
		float m_RefreshElapsed{ 0.0f };
	};
}
