#include "Pch.h"
#include "UIRmlInterfaces.h"

#include "Helios/Application/Application.h"

#include "Helios/Common/AssetPath.h"
#include "Helios/Common/PathUtils.h"

namespace Helios
{
	namespace UI
	{
		namespace
		{
			std::string ToUtf8(const Rml::String& text)
			{
				return std::string(text.c_str(), text.size());
			}

			/* 没有窗口（headless 检查）时返回空，调用方回落到基类的进程内剪贴板 */
			DeviceWindow* CurrentWindow()
			{
				Application* application = Application::Instance();
				return (application != nullptr) ? &application->GetWindow() : nullptr;
			}

			/* 缺失清单只留最近的几条：href 写错这类问题要能一眼看到，别攒成日志 */
			constexpr size_t kMaxMissingFiles = 8;
		}

		void UISystemInterface::SetClipboardText(const Rml::String& text)
		{
			DeviceWindow* window = CurrentWindow();
			if (window == nullptr || !window->SetClipboardText(ToUtf8(text)))
				Rml::SystemInterface::SetClipboardText(text);
		}

		void UISystemInterface::GetClipboardText(Rml::String& text)
		{
			std::string clipboard;
			DeviceWindow* window = CurrentWindow();
			if (window != nullptr && window->GetClipboardText(clipboard))
			{
				text = clipboard;
				return;
			}

			Rml::SystemInterface::GetClipboardText(text);
		}

		bool UISystemInterface::LogMessage(Rml::Log::Type type, const Rml::String& message)
		{
			if (type >= Rml::Log::LT_WARNING && m_Diagnostics != nullptr)
				m_Diagnostics->LastMessage = message.c_str();

			switch (type)
			{
			case Rml::Log::LT_ERROR:
			case Rml::Log::LT_ASSERT:
				CORE_LOG_ERROR("[UI] {}", ToUtf8(message));
				break;
			case Rml::Log::LT_WARNING:
				CORE_LOG_WARN("[UI] {}", ToUtf8(message));
				break;
			default:
				CORE_LOG_INFO("[UI] {}", ToUtf8(message));
				break;
			}
			return true;
		}

		void UISystemInterface::JoinPath(Rml::String& translated_path, const Rml::String& document_path,
			const Rml::String& path)
		{
			/* 文档路径是相对 Assets 的写法，RCSS / 图片保持在同一个目录体系里拼 */
			std::string joined = ToUtf8(document_path);
			const size_t slash = joined.find_last_of('/');
			joined = slash == std::string::npos ? std::string() : joined.substr(0, slash + 1);

			const std::string relative = ToUtf8(path);
			/* 以 / 开头的当项目根下的绝对路径（RmlUi 约定的根） */
			joined = (!relative.empty() && relative.front() == '/') ? relative.substr(1) : joined + relative;

			translated_path = Rml::String(joined.c_str());
		}

		Rml::FileHandle UIFileInterface::Open(const Rml::String& path)
		{
			const std::filesystem::path resolved = ResolveAssetPath(ToUtf8(path));
			if (m_Capturing)
				m_Captured.push_back(resolved);
			auto* stream = new std::ifstream(resolved, std::ios::binary);
			if (!stream->is_open())
			{
				delete stream;
				CORE_LOG_WARN("UI file not found: {}", resolved.string());
				if (m_Diagnostics != nullptr && m_Diagnostics->MissingFiles.size() < kMaxMissingFiles)
				{
					const std::string missing = resolved.string();
					std::vector<std::string>& files = m_Diagnostics->MissingFiles;
					if (std::find(files.begin(), files.end(), missing) == files.end())
						files.push_back(missing);
				}
				return 0;
			}
			return reinterpret_cast<Rml::FileHandle>(stream);
		}

		void UIFileInterface::Close(Rml::FileHandle file)
		{
			auto* stream = reinterpret_cast<std::ifstream*>(file);
			if (stream == nullptr)
				return;
			stream->close();
			delete stream;
		}

		size_t UIFileInterface::Read(void* buffer, size_t size, Rml::FileHandle file)
		{
			auto* stream = reinterpret_cast<std::ifstream*>(file);
			if (stream == nullptr)
				return 0;
			stream->read(static_cast<char*>(buffer), static_cast<std::streamsize>(size));
			return static_cast<size_t>(stream->gcount());
		}

		bool UIFileInterface::Seek(Rml::FileHandle file, long offset, int origin)
		{
			auto* stream = reinterpret_cast<std::ifstream*>(file);
			if (stream == nullptr)
				return false;

			std::ios_base::seekdir direction = std::ios_base::beg;
			if (origin == SEEK_END)
				direction = std::ios_base::end;
			else if (origin == SEEK_CUR)
				direction = std::ios_base::cur;

			stream->clear();
			stream->seekg(offset, direction);
			return stream->good();
		}

		size_t UIFileInterface::Tell(Rml::FileHandle file)
		{
			auto* stream = reinterpret_cast<std::ifstream*>(file);
			if (stream == nullptr)
				return 0;
			return static_cast<size_t>(stream->tellg());
		}
	}
}
