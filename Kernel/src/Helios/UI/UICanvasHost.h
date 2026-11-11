#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "Helios/Common/Common.h"
#include "Helios/Common/Export.h"
#include "Helios/Core/KeyCodes.h"

namespace Rml
{
	class Context;
	class ElementDocument;
}

namespace Helios
{
	namespace UI
	{
		struct UIDiagnostics;

		/* 一份 RmlUi 文档的宿主：Context、加载、布局、输入转发与热重载都在这里。
		 * 场景画布与编辑器预览各持一份，差别只在谁来渲染、谁来取控件值。 */
		class HELIOS_API UICanvasHost final
		{
		public:
			struct Options
			{
				/* Context 名字前缀：同进程里可能有多个宿主，名字不能撞 */
				std::string ContextPrefix{ "ui" };
				int Width{ 1280 };
				int Height{ 720 };
				/* dp 与物理像素的比例：Retina 上给 2，元素视觉大小不变，只多出分辨率 */
				float DensityRatio{ 1.0f };
				/* 连带盯文档同目录的 .rcss：<link> 要等首次布局才打开，光靠打开记录会漏 */
				bool WatchSiblingStyles{ false };
			};

			/* 磁盘上的文件改了什么：文档改了要重载结构，样式改了就地重排就够 */
			enum class Change { None, Style, Document };

			UICanvasHost();
			~UICanvasHost();
			UICanvasHost(const UICanvasHost&) = delete;
			UICanvasHost& operator=(const UICanvasHost&) = delete;

			bool Open(const std::string& document_path, const Options& options);
			void Close();
			[[nodiscard]] bool IsOpen() const { return m_Context != nullptr; }

			/* 布局一帧。尺寸取上一帧的绘制尺寸，命中测试也用这一份 */
			void Update(int width, int height);
			/* 录一帧并回放。多份画布按调用顺序叠放，后画的压在上面 */
			void Render(int width, int height);

			void SetDensityRatio(float ratio);
			[[nodiscard]] Change ReloadIfChanged();

			/* ---- 输入（物理像素，原点在左上角）：返回 UI 是否消费了这次事件 ---- */
			bool ProcessMouseMove(float x, float y);
			bool ProcessMouseButtonDown(int button);
			bool ProcessMouseButtonUp(int button);
			bool ProcessMouseWheel(float delta);
			void ProcessMouseLeave();
			bool ProcessKeyDown(KeyCode key);
			bool ProcessKeyUp(KeyCode key);
			bool ProcessTextInput(uint32_t codepoint);

			/* 焦点在文本框里：摇杆之类走键盘通道的控件要让位 */
			[[nodiscard]] bool HasTextFocus() const;

			/* 指针是否落在文档里的元素上：空白处命中的是文档本体，不算控件 */
			[[nodiscard]] bool IsPointerOverControl() const;

			[[nodiscard]] Rml::Context* GetContext() const { return m_Context; }
			[[nodiscard]] Rml::ElementDocument* GetDocument() const { return m_Document; }
			[[nodiscard]] const std::string& GetDocumentPath() const { return m_DocumentPath; }

			/* 文档里的元素数（打开时数一次），预览状态行显示用 */
			[[nodiscard]] size_t CountElements() const;

			/* 本宿主加载期间打不开的文件，以及最近一条 RmlUi 警告 */
			[[nodiscard]] const std::vector<std::string>& GetMissingFiles() const;
			[[nodiscard]] const std::string& GetLastMessage() const;

		private:
			/* 一个被监听的文件：文档本身改了要重建，样式改了就地重排 */
			struct WatchedFile
			{
				std::filesystem::path Path{};
				std::filesystem::file_time_type Stamp{};
				bool Document{ false };
			};

			void WatchFile(const std::filesystem::path& file, bool document);
			void WatchFiles(const std::vector<std::filesystem::path>& files, const std::filesystem::path& document_file);
			/* 同目录的样式表一起盯：<link> 由 RmlUi 在首次布局时才打开 */
			void WatchSiblingStyles();

			Options m_Options{};
			std::string m_ContextName{};
			std::string m_DocumentPath{};
			Rml::Context* m_Context{ nullptr };
			Rml::ElementDocument* m_Document{ nullptr };

			std::filesystem::path m_DocumentFile{};
			std::vector<WatchedFile> m_Watched{};
			/* 诊断只记本宿主这一次加载 / 布局：别的画布出错不该串到这边 */
			UniquePtr<UIDiagnostics> m_Diagnostics{};
		};
	}
}
