#include "Pch.h"
#include "Command/CreateAssetFileCommand.h"

namespace Helios
{
	CreateAssetFileCommand::CreateAssetFileCommand(IAssetChangeSink* sink, const std::filesystem::path& file,
		std::string content)
		: m_Sink(sink), m_File(file), m_Content(std::move(content))
	{
		m_Label = std::string("Create ") + PathToUtf8(m_File.filename());
	}

	void CreateAssetFileCommand::Do()
	{
		std::error_code error;
		if (std::filesystem::exists(m_File, error) && !error)
		{
			/* 已经有一个同名文件了（界面那边用 MakeUniquePath 避让过，这里是兜底）：
			 * 不覆盖别人的东西 —— 与"重命名到已存在的名字就拒绝"是同一条原则。 */
			CORE_LOG_WARN("新建资源文件时目标已存在，保持原样：{0}", PathToUtf8(m_File));
			return;
		}

		std::string write_error;
		if (!WriteFile(write_error))
		{
			CORE_LOG_ERROR("新建资源文件失败：{0}（{1}）", PathToUtf8(m_File), write_error);
			return;
		}

		Notify(std::string(), AssetRelativePath(m_File));
	}

	void CreateAssetFileCommand::Undo()
	{
		std::error_code error;
		if (!std::filesystem::is_regular_file(m_File, error) || error)
			return;

		/* 只删"还是我们刚写的那一份"：用户可能已经编辑过它（或外部工具改过），
		 * 那时宁可留着一个多余的文件，也不能把他的改动一起删掉 ——
		 * 与"撤销新建目录只删空目录"是同一条原则。 */
		std::string current;
		if (!ReadFile(m_File, current))
		{
			CORE_LOG_WARN("撤销新建资源文件时读不回来，保持原样：{0}", PathToUtf8(m_File));
			return;
		}

		if (current != m_Content)
		{
			CORE_LOG_WARN("撤销新建资源文件时内容已被改动，保持原样：{0}", PathToUtf8(m_File));
			return;
		}

		std::filesystem::remove(m_File, error);
		if (error)
		{
			CORE_LOG_ERROR("撤销新建资源文件失败：{0}（{1}）", PathToUtf8(m_File), error.message());
			return;
		}

		Notify(AssetRelativePath(m_File), std::string());
	}

	bool CreateAssetFileCommand::WriteFile(std::string& out_error) const
	{
		std::error_code error;

		/* 父目录可能在这一步之前被撤销掉了（先撤销"新建文件夹"、再撤销里面的新建文件），
		 * 所以这里自己把目录补出来，而不是假定它还在。 */
		const std::filesystem::path parent = m_File.parent_path();
		if (!parent.empty())
		{
			std::filesystem::create_directories(parent, error);
			if (error)
			{
				out_error = "建不了目录 " + PathToUtf8(parent) + "：" + error.message();
				return false;
			}
		}

		/* 用 OpenUtf8File 而不是 std::ofstream：路径在 Windows 上要按 UTF-8 → 宽字符走
		 * （资源目录里带中文是常事），与场景 / 材质的写文件路径保持同一套。 */
		std::unique_ptr<FILE, decltype(&std::fclose)> handle(OpenUtf8File(m_File, "wb"), &std::fclose);
		if (!handle)
		{
			out_error = "打不开文件：";
			out_error += std::strerror(errno);
			return false;
		}

		if (!m_Content.empty() && std::fwrite(m_Content.data(), 1, m_Content.size(), handle.get()) != m_Content.size())
		{
			out_error = "写入不完整：";
			out_error += std::strerror(errno);
			return false;
		}

		return true;
	}

	bool CreateAssetFileCommand::ReadFile(const std::filesystem::path& file, std::string& out_content)
	{
		std::unique_ptr<FILE, decltype(&std::fclose)> handle(OpenUtf8File(file, "rb"), &std::fclose);
		if (!handle)
			return false;

		out_content.clear();

		char buffer[4096];
		size_t read = 0;
		while ((read = std::fread(buffer, 1, sizeof(buffer), handle.get())) > 0)
			out_content.append(buffer, read);

		return std::ferror(handle.get()) == 0;
	}

	void CreateAssetFileCommand::Notify(const std::string& from, const std::string& to)
	{
		if (m_Sink != nullptr)
			m_Sink->OnAssetPathChanged(from, to);
	}
}
