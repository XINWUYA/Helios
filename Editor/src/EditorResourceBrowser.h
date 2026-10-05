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

		/* 文件操作的落地口：把命令交给编辑历史（主壳层注入 m_Context 转发）。
		 * 由外壳注入而不是面板自己去查 Layer —— 面板因此不认识 Layer/CommandStack，
		 * 单独跑（headless 测试）时也不为空：没有 sink 就直接执行，功能一样，只是没有历史。 */
		void SetCommandSink(std::function<void(UniquePtr<ICommand>)> sink) { m_CommandSink = std::move(sink); }

		/* 资源改动的观察者（IAssetChangeSink）：文件命令做完 —— 含撤销与重做 —— 通知它，
		 * 界面据此同步当前目录与选中项，并立刻重建文件树（不用等目录轮询）。 */
		void OnAssetPathChanged(const std::string& from, const std::string& to) override;

	private:
		/* 文件类型，确定文件后缀是否正确 */
		enum class FileType : uint8_t
		{
			Default,
			Folder,
			Image,
			Scene,
			MtlGraph,
		};

		/* 内容筛选里的「按类型」（工具行下拉）：All = 不按类型筛 */
		enum class TypeFilter : uint8_t
		{
			All = 0,
			Folder,
			Image,
			Scene,
			MtlGraph,
			COUNT
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
		void ShowBrowserBody(float reserved_footer, float pane_padding);
		/* 目录树（左栏）：只列文件夹（文件是右栏的事） */
		void DrawFolderNode(const SharedPtr<FileNode>& node);

		/* 顶部一行：左 = 类型筛选 + 搜索（找东西），右 = 计数 + 缩放 + 视图菜单（看状态 / 调视图）。
		 * 当前目录名不在这里显示 —— 底部路径栏说得更清楚，首行不必重复。
		 * 原来是「标题行」与「控制行」两行，各自只用掉一半宽度 —— 合成一行省出一行高给内容区。 */
		void ShowBrowserTopBar();
		/* 底部栏：上一级 + 当前路径面包屑（每级可点）+ 选中的项 */
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

		/* 切换当前目录：置上「下次画目录树时把它露出来」（展开祖先 + 滚到可见） */
		void SetCurrentNode(const SharedPtr<FileNode>& node);

		/* 文件类型 -> 图标：与层级面板共用同一套矢量图标语言 */
		static Icons::Id ResolveFileIcon(FileType type);

		/* 节点的显示名：根节点的 FileName 存的是整条绝对路径，用最后一级目录名代替 */
		static std::string DisplayNodeName(const FileNode& node);

		/* 在树中按相对路径查找节点：重建后据此恢复浏览位置 */
		static SharedPtr<FileNode> FindNode(const SharedPtr<FileNode>& node, const std::string& path);
		/* node 是否等于 target 或它的祖先（用于判断要不要把某个目录展开） */
		static bool IsAncestorOrSelf(const SharedPtr<FileNode>& node, const SharedPtr<FileNode>& target);

		/* ---- 文件操作（新建文件夹 / 重命名 / 删除）----
		 * 三件都在右键菜单里（内容区与目录树都有），执行前要弹一个输入 / 确认层。
		 * 待办目标一律按相对 Assets 的路径记：文件树会重建，节点指针不保险。 */

		/* 右键菜单的条目（在已经开好的 popup 里画） */
		void DrawAssetContextMenuItems(const std::string& path, bool is_folder);
		/* 空处右键：在当前目录下新建 */
		void DrawBackgroundContextMenuItems();
		/* 三个弹层（新建 / 重命名 / 删除确认）：画在面板根作用域，
		 * 因为右键菜单在内容栏与目录树两个子窗口里，各自算 ID 会得到不同的值。 */
		void DrawAssetOperationPopups();

		/* 打开弹层：路径都是相对 Assets 的 */
		void OpenNewFolderPopup(const std::string& parent_path);
		void OpenRenamePopup(const std::string& path);
		void OpenDeletePopup(std::vector<std::string> paths);

		/* 名字输入弹层（新建与重命名共用：m_PendingIsRename 决定标题与确认按钮的文案） */
		void DrawNamePopup(ImGuiID popup_id, bool is_rename);

		/* 把命令塞进编辑历史（m_Context 为空时直接执行） */
		void ExecuteCommand(UniquePtr<ICommand> command);

		/* 相对 Assets 的路径 → 绝对路径 */
		static std::filesystem::path AbsoluteAssetPath(const std::string& relative_path);

		/* 待办：新建时是父目录，重命名时是被改名的目标；两个都空 = 没有待办 */
		std::string m_PendingParentPath;
		std::string m_PendingRenamePath;
		/* 删除确认里的目标 */
		std::vector<std::string> m_PendingDeletePaths;
		/* 名字输入框的内容（新建 / 重命名共用） */
		char m_NameBuffer[128]{};
		/* 面板根作用域上算好的弹层 ID（见 OnImGuiRenderer） */
		ImGuiID m_PopupNewFolder{ 0 };
		ImGuiID m_PopupRename{ 0 };
		ImGuiID m_PopupDelete{ 0 };

		/* 文件操作（含撤销 / 重做）改过路径后，下一次重建文件树时按它对齐：
		 * from 为空 = 新建（顺带选中新建的那一项），to 为空 = 删除（找不到就丢掉）。 */
		std::string m_RemapFrom;
		std::string m_RemapTo;
		std::string m_PendingSelectPath;

		/* 跨面板通道：只为"把文件操作塞进编辑历史"这一个用途（主壳层注入） */
		std::function<void(UniquePtr<ICommand>)> m_CommandSink;

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
