#pragma once
#include <filesystem>
#include <vector>

namespace Helios
{
	/* 资源浏览器：编辑资源 */
	class EditorResourceBrowser
	{
	public:
		EditorResourceBrowser() = default;
		~EditorResourceBrowser() = default;

		/* 渲染UI */
		void OnImGuiRenderer();

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
			SharedPtr<DeviceTexture> Icon;						/* 文件图标 */

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
		/* 构建文件节点UI(详细信息) */
		void BuildFileUIListTreeDetail(const SharedPtr<FileNode>& node);
		/* 构建文件节点UI(简洁) */
		void BuildFileUIListTreeSimple(const SharedPtr<FileNode>& node);

		/* 在树中按相对路径查找节点：重建后据此恢复浏览位置 */
		static SharedPtr<FileNode> FindNode(const SharedPtr<FileNode>& node, const std::string& path);

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

		/* 只有文件夹文件变化时才更新的文件节点树 */
		bool m_IsDirty{ true };

		/* 目录状态指纹：目录数量 + 最新的目录写入时间 */
		size_t m_DirectoryCount{ 0 };
		std::filesystem::file_time_type m_NewestDirectoryWriteTime{};
		/* 距上次检查目录状态的累计时间 */
		float m_RefreshElapsed{ 0.0f };
	};
}
