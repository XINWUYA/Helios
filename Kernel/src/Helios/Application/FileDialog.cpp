#include "Pch.h"
#include "FileDialog.h"
#include "Application.h"
#include "GLFW/glfw3.h"

#ifdef PLATFORM_WINDOWS
#include "commdlg.h"
#include "shellapi.h"
#include "Helios/Common/PathUtils.h"
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#elif defined(PLATFORM_MACOS)
#import <Cocoa/Cocoa.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>
#endif

namespace Helios
{
#ifdef PLATFORM_WINDOWS
	namespace
	{
		std::wstring MakeWideFilter(const char* filter)
		{
			static constexpr char default_filter[] = "*.*\0*.*\0";
			const char* source = filter != nullptr ? filter : default_filter;
			std::wstring result;
			for (size_t index = 0;; ++index)
			{
				const auto byte = static_cast<unsigned char>(source[index]);
				if (byte > 0x7f)
					return {};
				result.push_back(static_cast<wchar_t>(byte));
				if (byte == 0 && source[index + 1] == '\0')
				{
					result.push_back(L'\0');
					break;
				}
			}
			return result;
		}
	}
#endif

	/* 通过文件对话框选取指定的文件路径 */
	std::string FileDialog::OpenFile(const char* filter)	{
#ifdef PLATFORM_WINDOWS
		std::wstring wide_filter = MakeWideFilter(filter);
		if (wide_filter.empty())
			return {};

		std::vector<wchar_t> file_path(32768, L'\0');
		std::vector<wchar_t> current_dir(32768, L'\0');
		if (GetCurrentDirectoryW(static_cast<DWORD>(current_dir.size()), current_dir.data()) == 0)
			current_dir.clear();

		OPENFILENAMEW ofn{};
		ofn.lStructSize = sizeof(ofn);
		ofn.hwndOwner = glfwGetWin32Window(
			static_cast<GLFWwindow*>(Application::Instance()->GetWindow().GetNativeWindow()));
		ofn.lpstrFile = file_path.data();
		ofn.nMaxFile = static_cast<DWORD>(file_path.size());
		ofn.lpstrInitialDir = current_dir.empty() ? nullptr : current_dir.data();
		ofn.lpstrFilter = wide_filter.data();
		ofn.nFilterIndex = 1;
		ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

		if (GetOpenFileNameW(&ofn) == TRUE)
			return PathToUtf8(std::filesystem::path(file_path.data()));

		return {};
#elif defined(PLATFORM_MACOS)
		@autoreleasepool {
			NSOpenPanel* openPanel = [NSOpenPanel openPanel];
			[openPanel setAllowsMultipleSelection:NO];
			[openPanel setCanChooseDirectories:NO];
			[openPanel setCanChooseFiles:YES];
			
			// Set parent window
			NSWindow* nsWindow = glfwGetCocoaWindow((GLFWwindow*)Application::Instance()->GetWindow().GetNativeWindow());
			[openPanel beginSheetModalForWindow:nsWindow completionHandler:nil];
			
			if ([openPanel runModal] == NSModalResponseOK) {
				NSURL* url = [[openPanel URLs] objectAtIndex:0];
				NSString* path = [url path];
				return std::string([path UTF8String]);
			}
		}
		return std::string();
#else
		return std::string();
#endif
	}

	/* 保存文件到指定路径 */
	std::string FileDialog::SaveFile(const char* filter)	{
#ifdef PLATFORM_WINDOWS
		std::wstring wide_filter = MakeWideFilter(filter);
		if (wide_filter.empty())
			return {};

		std::vector<wchar_t> file_path(32768, L'\0');
		std::vector<wchar_t> current_dir(32768, L'\0');
		if (GetCurrentDirectoryW(static_cast<DWORD>(current_dir.size()), current_dir.data()) == 0)
			current_dir.clear();

		OPENFILENAMEW ofn{};
		ofn.lStructSize = sizeof(ofn);
		ofn.hwndOwner = glfwGetWin32Window(
			static_cast<GLFWwindow*>(Application::Instance()->GetWindow().GetNativeWindow()));
		ofn.lpstrFile = file_path.data();
		ofn.nMaxFile = static_cast<DWORD>(file_path.size());
		ofn.lpstrInitialDir = current_dir.empty() ? nullptr : current_dir.data();
		ofn.lpstrFilter = wide_filter.data();
		ofn.nFilterIndex = 1;
		ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;

		std::wstring default_extension;
		if (filter != nullptr)
		{
			const char* pattern = std::strchr(filter, '\0');
			if (pattern != nullptr)
			{
				++pattern;
				if (pattern[0] == '*' && pattern[1] == '.')
				{
					for (pattern += 2; *pattern != '\0' && *pattern != ';'; ++pattern)
						default_extension.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*pattern)));
				}
			}
		}
		ofn.lpstrDefExt = default_extension.empty() ? nullptr : default_extension.c_str();

		if (GetSaveFileNameW(&ofn) == TRUE)
			return PathToUtf8(std::filesystem::path(file_path.data()));

		return {};
#elif defined(PLATFORM_MACOS)
		@autoreleasepool {
			NSSavePanel* savePanel = [NSSavePanel savePanel];
			[savePanel setCanCreateDirectories:YES];
			
			// Set parent window
			NSWindow* nsWindow = glfwGetCocoaWindow((GLFWwindow*)Application::Instance()->GetWindow().GetNativeWindow());
			[savePanel beginSheetModalForWindow:nsWindow completionHandler:nil];
			
			if ([savePanel runModal] == NSModalResponseOK) {
				NSURL* url = [savePanel URL];
				NSString* path = [url path];
				return std::string([path UTF8String]);
			}
		}
		return std::string();
#else
		return std::string();
#endif
	}

	/* 打开到指定的文件目录 */
	bool OpenFileExplorer(const char* path)
	{
		if (path == nullptr || *path == '\0')
			return false;

#ifdef PLATFORM_WINDOWS
		std::filesystem::path file_path;
		if (!TryPathFromUtf8(path, file_path))
			return false;

		const std::wstring select_params = L"/select, \"" + file_path.wstring() + L"\"";
		SHELLEXECUTEINFOW shex{};
		shex.cbSize = sizeof(shex);
		shex.fMask = SEE_MASK_FLAG_NO_UI;
		shex.lpFile = L"explorer.exe";
		shex.lpParameters = select_params.c_str();
		shex.lpVerb = L"open";
		shex.nShow = SW_SHOWDEFAULT;
		return ShellExecuteExW(&shex) == TRUE;
#elif defined(PLATFORM_MACOS)
		@autoreleasepool {
			NSString* nsPath = [NSString stringWithUTF8String:path];
			NSURL* url = [NSURL fileURLWithPath:nsPath];
			
			// Open Finder and select the file
			NSArray* fileURLs = [NSArray arrayWithObject:url];
			[[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:fileURLs];
		}
		return true;
#else
		return false;
#endif
	}
}