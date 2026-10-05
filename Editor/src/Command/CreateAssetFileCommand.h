#pragma once
#include <filesystem>
#include <string>
#include "Helios/Command/Command.h"
#include "Command/AssetFileOps.h"

namespace Helios
{
	/* 新建资源文件：撤销就是删掉刚建的那个。撤销只删"还是我们刚写的那份"（内容逐字节相同）——
	 * 用户可能已经在编辑器或外部工具里改过，宁可留个多余文件、也不删掉他的改动。 */
	class CreateAssetFileCommand final : public ICommand
	{
	public:
		/* file 是绝对路径（= Assets / 相对路径）；content 是初始内容（允许为空文件）；
		 * sink 可为空（只改磁盘、不通知界面） */
		CreateAssetFileCommand(IAssetChangeSink* sink, const std::filesystem::path& file, std::string content);

		void Do() override;
		void Undo() override;

		[[nodiscard]] const char* GetLabel() const override { return m_Label.c_str(); }
		/* 只动磁盘上的资源，不改场景内容（否则"场景未保存"会被它带出来） */
		[[nodiscard]] bool AffectsSceneDocument() const override { return false; }

		[[nodiscard]] const std::filesystem::path& GetFile() const { return m_File; }
		[[nodiscard]] const std::string& GetContent() const { return m_Content; }

	private:
		void Notify(const std::string& from, const std::string& to);
		/* 写文件（返回失败原因）：父目录不存在会一并建出来 —— 新建的目标目录可能刚被撤销掉 */
		[[nodiscard]] bool WriteFile(std::string& out_error) const;
		/* 读回来比对"内容有没有被改过" */
		[[nodiscard]] static bool ReadFile(const std::filesystem::path& file, std::string& out_content);

		IAssetChangeSink* m_Sink{ nullptr };
		std::filesystem::path m_File;
		std::string m_Content;
		std::string m_Label;
	};
}
