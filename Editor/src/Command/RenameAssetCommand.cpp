#include "Pch.h"
#include "Command/RenameAssetCommand.h"

namespace Helios
{
	RenameAssetCommand::RenameAssetCommand(IAssetChangeSink* sink, const std::filesystem::path& from,
		const std::filesystem::path& to)
		: m_Sink(sink), m_From(from), m_To(to)
	{
		/* 标签按"这次操作干了什么"给，撤销菜单里一眼分得开两种来路：同目录下换名字 = Rename
		 * （重命名弹层），换了目录 = Move（目录树拖拽移动 / 带着改名的移动）。标签给的都是
		 * 结果那一侧的名字：撤销后菜单写 "Undo Move hero.png"，看到的是它叫什么。 */
		const bool same_directory = (m_From.parent_path() == m_To.parent_path());
		m_Label = std::string(same_directory ? "Rename " : "Move ") + PathToUtf8(m_To.filename());
	}

	void RenameAssetCommand::Do()
	{
		Rename(m_From, m_To);
	}

	void RenameAssetCommand::Undo()
	{
		Rename(m_To, m_From);
	}

	void RenameAssetCommand::Rename(const std::filesystem::path& from, const std::filesystem::path& to)
	{
		std::error_code error;
		if (!std::filesystem::exists(from, error) || error)
		{
			CORE_LOG_ERROR("重命名失败，源已不存在：{0}", PathToUtf8(from));
			return;
		}

		/* 目标被占了就别覆盖：撤销时原名字可能已经被别人用掉 */
		if (std::filesystem::exists(to, error) && !error)
		{
			CORE_LOG_ERROR("重命名失败，目标已存在：{0}", PathToUtf8(to));
			return;
		}

		std::filesystem::rename(from, to, error);
		if (error)
		{
			CORE_LOG_ERROR("重命名失败：{0} -> {1}（{2}）", PathToUtf8(from), PathToUtf8(to), error.message());
			return;
		}

		if (m_Sink != nullptr)
			m_Sink->OnAssetPathChanged(AssetRelativePath(from), AssetRelativePath(to));
	}
}
