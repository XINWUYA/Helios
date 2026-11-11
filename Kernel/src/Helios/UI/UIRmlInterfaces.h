#pragma once
#include <filesystem>
#include <string>
#include <vector>

#include <RmlUi/Core/FileInterface.h>
#include <RmlUi/Core/SystemInterface.h>

namespace Helios
{
	namespace UI
	{
		/* 一次加载 / 布局期间的诊断：缺失文件与最近一条警告。
		 * RmlUi 的接口是进程级单例，用「当前诊断表」把记录分流到各自的宿主。 */
		struct UIDiagnostics
		{
			std::vector<std::string> MissingFiles{};
			std::string LastMessage{};
		};

		/* RmlUi 的系统接口：日志折到引擎日志，路径拼接交给文件接口，
		 * 剪贴板走系统（文本框的复制粘贴要和别的应用互通），文字翻译与软键盘保持基类空实现。 */
		class UISystemInterface final : public Rml::SystemInterface
		{
		public:
			bool LogMessage(Rml::Log::Type type, const Rml::String& message) override;
			void JoinPath(Rml::String& translated_path, const Rml::String& document_path,
				const Rml::String& path) override;
			void SetClipboardText(const Rml::String& text) override;
			void GetClipboardText(Rml::String& text) override;

			/* 换一份诊断表，返回原来那份。宿主进出时成对调用，互不串台 */
			UIDiagnostics* SetDiagnostics(UIDiagnostics* diagnostics)
			{
				UIDiagnostics* previous = m_Diagnostics;
				m_Diagnostics = diagnostics;
				return previous;
			}

		private:
			UIDiagnostics* m_Diagnostics{ nullptr };
		};

		/* RmlUi 的文件接口：文档里只写相对路径，解析口径跟其他资产一致
		 * （项目根优先，找不到回落引擎根，见 ResolveAssetPath）。 */
		class UIFileInterface final : public Rml::FileInterface
		{
		public:
			/* 记录接下来打开的文件（热重载要知道一份文档依赖哪些文件） */
			void BeginCapture() { m_Capturing = true; m_Captured.clear(); }
			[[nodiscard]] std::vector<std::filesystem::path> TakeCaptured()
			{
				m_Capturing = false;
				return std::move(m_Captured);
			}

			/* 换一份诊断表，返回原来那份 */
			UIDiagnostics* SetDiagnostics(UIDiagnostics* diagnostics)
			{
				UIDiagnostics* previous = m_Diagnostics;
				m_Diagnostics = diagnostics;
				return previous;
			}

			Rml::FileHandle Open(const Rml::String& path) override;
			void Close(Rml::FileHandle file) override;
			size_t Read(void* buffer, size_t size, Rml::FileHandle file) override;
			bool Seek(Rml::FileHandle file, long offset, int origin) override;
			size_t Tell(Rml::FileHandle file) override;

		private:
			bool m_Capturing{ false };
			std::vector<std::filesystem::path> m_Captured{};
			UIDiagnostics* m_Diagnostics{ nullptr };
		};
	}
}
