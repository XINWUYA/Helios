#include "Pch.h"
#include "UICanvasHost.h"

#include <RmlUi/Core.h>

#include "UIFrameRenderer.h"
#include "UIRenderInterface.h"
#include "UIRmlInterfaces.h"
#include "UIRuntime.h"

#include "Helios/Core/Input.h"
#include "Helios/Scene/SceneCommon.h"

namespace Helios
{
	namespace UI
	{
		namespace
		{
			/* 打开 / 布局期间把 RmlUi 的诊断写进本宿主：接口是进程级单例，进来换表、走时还回去 */
			class DiagnosticsScope final
			{
			public:
				explicit DiagnosticsScope(UIDiagnostics* diagnostics)
					: m_FileSink(FileInterface().SetDiagnostics(diagnostics))
					, m_SystemSink(SystemInterface().SetDiagnostics(diagnostics))
				{
				}

				~DiagnosticsScope()
				{
					FileInterface().SetDiagnostics(m_FileSink);
					SystemInterface().SetDiagnostics(m_SystemSink);
				}

				DiagnosticsScope(const DiagnosticsScope&) = delete;
				DiagnosticsScope& operator=(const DiagnosticsScope&) = delete;

			private:
				UIDiagnostics* m_FileSink{ nullptr };
				UIDiagnostics* m_SystemSink{ nullptr };
			};

			size_t CountSubtree(const Rml::Element* element)
			{
				size_t count = 1;
				for (int index = 0; index < element->GetNumChildren(); ++index)
				{
					if (const Rml::Element* child = element->GetChild(index))
						count += CountSubtree(child);
				}
				return count;
			}

			/* 引擎键码 → RmlUi 键标识。没列到的键交给 RmlUi 当未知键，导航与按钮不受影响 */
			Rml::Input::KeyIdentifier ToRmlKeyIdentifier(KeyCode key)
			{
				using namespace Rml::Input;
				using KeyIdentifier = Rml::Input::KeyIdentifier;
				if (key >= Key::A && key <= Key::Z)
					return static_cast<KeyIdentifier>(KI_A + (static_cast<int>(key) - static_cast<int>(Key::A)));
				if (key >= Key::D0 && key <= Key::D9)
					return static_cast<KeyIdentifier>(KI_0 + (static_cast<int>(key) - static_cast<int>(Key::D0)));
				if (key >= Key::F1 && key <= Key::F12)
					return static_cast<KeyIdentifier>(KI_F1 + (static_cast<int>(key) - static_cast<int>(Key::F1)));

				switch (key)
				{
				case Key::Space: return KI_SPACE;
				case Key::Enter: return KI_RETURN;
				case Key::Tab: return KI_TAB;
				case Key::Escape: return KI_ESCAPE;
				case Key::Backspace: return KI_BACK;
				case Key::Insert: return KI_INSERT;
				case Key::Delete: return KI_DELETE;
				case Key::Home: return KI_HOME;
				case Key::End: return KI_END;
				case Key::PageUp: return KI_PRIOR;
				case Key::PageDown: return KI_NEXT;
				case Key::Up: return KI_UP;
				case Key::Down: return KI_DOWN;
				case Key::Left: return KI_LEFT;
				case Key::Right: return KI_RIGHT;
				case Key::LeftShift: return KI_LSHIFT;
				case Key::RightShift: return KI_RSHIFT;
				case Key::LeftControl: return KI_LCONTROL;
				case Key::RightControl: return KI_RCONTROL;
				case Key::LeftAlt: return KI_LMENU;
				case Key::RightAlt: return KI_RMENU;
				case Key::LeftSuper:
				case Key::RightSuper: return KI_LWIN;
				default: return KI_UNKNOWN;
				}
			}

			/* 修饰键状态：RmlUi 用它判组合键与 CapsLock 下的导航 */
			int ReadRmlModifiers()
			{
				using namespace Rml::Input;
				int state = 0;
				if (Input::IsKeyPressedRaw(Key::LeftControl) || Input::IsKeyPressedRaw(Key::RightControl))
					state |= KM_CTRL;
				if (Input::IsKeyPressedRaw(Key::LeftShift) || Input::IsKeyPressedRaw(Key::RightShift))
					state |= KM_SHIFT;
				if (Input::IsKeyPressedRaw(Key::LeftAlt) || Input::IsKeyPressedRaw(Key::RightAlt))
					state |= KM_ALT;
				if (Input::IsKeyPressedRaw(Key::LeftSuper) || Input::IsKeyPressedRaw(Key::RightSuper))
					state |= KM_META;
				if (Input::IsKeyPressedRaw(Key::CapsLock))
					state |= KM_CAPSLOCK;
#if PLATFORM_MACOS
				/* macOS 上文本编辑习惯按 Command：把 ⌘ 一并当成 Ctrl 递进去，
				 * 否则全选、复制、粘贴一个都按不出来 */
				if (Input::IsKeyPressedRaw(Key::LeftSuper) || Input::IsKeyPressedRaw(Key::RightSuper))
					state |= KM_CTRL;
#endif
				return state;
			}

			/* 焦点在文本输入上时，键盘归打字用：摇杆的键盘通道要让位 */
			bool HasTextFocusIn(Rml::Context* context)
			{
				if (context == nullptr)
					return false;

				const Rml::Element* focus = context->GetFocusElement();
				while (focus != nullptr)
				{
					const Rml::String tag = focus->GetTagName();
					if (tag == "textarea")
						return true;
					if (tag == "input")
					{
						const Rml::String type = focus->GetAttribute<Rml::String>("type", "text");
						return type == "text" || type == "password" || type == "search"
							|| type == "email" || type == "url" || type == "tel";
					}
					focus = focus->GetParentNode();
				}
				return false;
			}
		}

		UICanvasHost::UICanvasHost()
			: m_Diagnostics(CreateUniquePtr<UIDiagnostics>())
		{
		}

		UICanvasHost::~UICanvasHost()
		{
			Close();
		}

		bool UICanvasHost::Open(const std::string& document_path, const Options& options)
		{
			Close();
			Initialise();
			if (!IsInitialised())
				return false;

			*m_Diagnostics = UIDiagnostics{};
			m_Options = options;
			m_DocumentPath = document_path;
			m_ContextName = options.ContextPrefix + "_" + std::to_string(NextHostSerial());

			/* 建 Context 时给个能用的尺寸：1x1 会让文档在 Show 那一刻定成 0 尺寸 */
			m_Context = Rml::CreateContext(Rml::String(m_ContextName.c_str()),
				Rml::Vector2i(std::max(options.Width, 1), std::max(options.Height, 1)));
			if (m_Context == nullptr)
			{
				CORE_LOG_ERROR("Failed to create UI context '{}' for '{}'", m_ContextName, document_path);
				return false;
			}

			m_Context->SetDensityIndependentPixelRatio(m_Options.DensityRatio);

			/* 加载期间打开的文件记下来：热重载靠它判断改的是文档还是样式 */
			DiagnosticsScope diagnostics(m_Diagnostics.get());
			FileInterface().BeginCapture();
			m_Document = m_Context->LoadDocument(Rml::String(document_path.c_str()));
			m_DocumentFile = ResolveAssetPath(document_path);
			WatchFiles(FileInterface().TakeCaptured(), m_DocumentFile);
			if (m_Options.WatchSiblingStyles)
				WatchSiblingStyles();

			if (m_Document == nullptr)
			{
				CORE_LOG_ERROR("Failed to load UI document '{}'", document_path);
				Close();
				return false;
			}

			m_Document->Show();
			return true;
		}

		void UICanvasHost::Close()
		{
			if (m_Context != nullptr)
			{
				m_Context->UnloadAllDocuments();
				Rml::RemoveContext(Rml::String(m_ContextName.c_str()));
			}
			m_Context = nullptr;
			m_Document = nullptr;
			m_ContextName.clear();
			m_DocumentFile.clear();
			m_Watched.clear();
		}

		void UICanvasHost::SetDensityRatio(float ratio)
		{
			m_Options.DensityRatio = ratio > 0.0f ? ratio : 1.0f;
			if (m_Context != nullptr)
				m_Context->SetDensityIndependentPixelRatio(m_Options.DensityRatio);
		}

		void UICanvasHost::Update(int width, int height)
		{
			if (m_Context == nullptr)
				return;

			m_Context->SetDimensions(Rml::Vector2i(width, height));
			/* 样式表要等第一次布局才打开：这一段打开的文件补进监听表 */
			DiagnosticsScope diagnostics(m_Diagnostics.get());
			FileInterface().BeginCapture();
			m_Context->Update();
			WatchFiles(FileInterface().TakeCaptured(), m_DocumentFile);
		}

		void UICanvasHost::Render(int width, int height)
		{
			if (m_Context == nullptr)
				return;

			m_Context->SetDimensions(Rml::Vector2i(width, height));
			UIRenderInterface& recorder = RenderInterface();
			recorder.BeginFrame();
			m_Context->Render();
			FrameRenderer().Draw(recorder, static_cast<uint32_t>(width), static_cast<uint32_t>(height));
		}

		UICanvasHost::Change UICanvasHost::ReloadIfChanged()
		{
			if (m_Context == nullptr)
				return Change::None;

			bool document_changed = false;
			bool style_changed = false;
			for (WatchedFile& watched : m_Watched)
			{
				std::error_code error;
				const std::filesystem::file_time_type stamp = std::filesystem::last_write_time(watched.Path, error);
				if (error || stamp == watched.Stamp)
					continue;

				watched.Stamp = stamp;
				if (watched.Document)
					document_changed = true;
				else
					style_changed = true;
			}

			/* 结构变了要重新解析文档，样式变了就地重排就够 */
			if (document_changed)
				return Change::Document;

			if (style_changed && m_Document != nullptr)
			{
				m_Document->ReloadStyleSheet();
				return Change::Style;
			}
			return Change::None;
		}

		bool UICanvasHost::ProcessMouseMove(float x, float y)
		{
			return m_Context != nullptr
				&& !m_Context->ProcessMouseMove(static_cast<int>(x), static_cast<int>(y), 0);
		}

		bool UICanvasHost::ProcessMouseButtonDown(int button)
		{
			return m_Context != nullptr && !m_Context->ProcessMouseButtonDown(button, 0);
		}

		bool UICanvasHost::ProcessMouseButtonUp(int button)
		{
			return m_Context != nullptr && !m_Context->ProcessMouseButtonUp(button, 0);
		}

		bool UICanvasHost::ProcessMouseWheel(float delta)
		{
			return m_Context != nullptr && !m_Context->ProcessMouseWheel(delta, 0);
		}

		void UICanvasHost::ProcessMouseLeave()
		{
			if (m_Context != nullptr)
				m_Context->ProcessMouseLeave();
		}

		bool UICanvasHost::ProcessKeyDown(KeyCode key)
		{
			const Rml::Input::KeyIdentifier rml_key = ToRmlKeyIdentifier(key);
			if (m_Context == nullptr || rml_key == Rml::Input::KI_UNKNOWN)
				return false;
			return !m_Context->ProcessKeyDown(rml_key, ReadRmlModifiers());
		}

		bool UICanvasHost::ProcessKeyUp(KeyCode key)
		{
			const Rml::Input::KeyIdentifier rml_key = ToRmlKeyIdentifier(key);
			if (m_Context == nullptr || rml_key == Rml::Input::KI_UNKNOWN)
				return false;
			return !m_Context->ProcessKeyUp(rml_key, ReadRmlModifiers());
		}

		bool UICanvasHost::ProcessTextInput(uint32_t codepoint)
		{
			return m_Context != nullptr && !m_Context->ProcessTextInput(static_cast<Rml::Character>(codepoint));
		}

		bool UICanvasHost::HasTextFocus() const
		{
			return HasTextFocusIn(m_Context);
		}

		bool UICanvasHost::IsPointerOverControl() const
		{
			if (m_Context == nullptr)
				return false;

			/* 空白处命中的是铺满画面的文档本体，只有文档里的元素才算控件 */
			const Rml::Element* hovered = m_Context->GetHoverElement();
			return hovered != nullptr && hovered != hovered->GetOwnerDocument();
		}

		bool UICanvasHost::IsPointOverControl(float x, float y) const
		{
			if (m_Context == nullptr)
				return false;

			const Rml::Element* hit = m_Context->GetElementAtPoint(Rml::Vector2f(x, y));
			return hit != nullptr && hit != hit->GetOwnerDocument();
		}

		size_t UICanvasHost::CountElements() const
		{
			return m_Document != nullptr ? CountSubtree(m_Document) : 0;
		}

		const std::vector<std::string>& UICanvasHost::GetMissingFiles() const
		{
			return m_Diagnostics->MissingFiles;
		}

		const std::string& UICanvasHost::GetLastMessage() const
		{
			return m_Diagnostics->LastMessage;
		}

		void UICanvasHost::WatchFile(const std::filesystem::path& file, bool document)
		{
			if (file.empty())
				return;

			for (const WatchedFile& watched : m_Watched)
			{
				if (watched.Path == file)
					return;
			}

			WatchedFile entry;
			entry.Path = file;
			entry.Document = document;
			std::error_code error;
			entry.Stamp = std::filesystem::last_write_time(file, error);
			m_Watched.push_back(entry);
		}

		void UICanvasHost::WatchFiles(const std::vector<std::filesystem::path>& files,
			const std::filesystem::path& document_file)
		{
			for (const std::filesystem::path& file : files)
				WatchFile(file, file == document_file);
		}

		void UICanvasHost::WatchSiblingStyles()
		{
			if (m_DocumentFile.empty())
				return;

			std::error_code error;
			for (const auto& entry : std::filesystem::directory_iterator(m_DocumentFile.parent_path(), error))
			{
				if (entry.path().extension() != ".rcss")
					continue;
				WatchFile(entry.path(), false);
			}
		}
	}
}
