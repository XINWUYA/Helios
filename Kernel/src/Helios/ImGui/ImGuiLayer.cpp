#include "Pch.h"
#include "ImGuiLayer.h"
#include "ImGuiRenderer.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <backends/imgui_impl_glfw.h>
#include <GLFW/glfw3.h>
#include <filesystem>
#include "Helios/Application/Application.h"
#include "Helios/VirtualDevice/DeviceWindow.h"
#include "Helios/Renderer/Renderer.h"
#include "Helios/Renderer/RenderAPI.h"
#include "Helios/Scene/SceneCommon.h"
#include "Helios/ImGui/EditorTheme.h"

namespace Helios
{
	/* ImGuiPass Payload：此Pass不依赖任何FrameGraph资源 */
	struct ImGuiPassData
	{};

	/* 应用名可能含空格等字符，折算为安全的文件名 */
	static std::string SanitizeFileName(const std::string& name)
	{
		std::string result;
		result.reserve(name.size());
		for (const char c : name)
		{
			const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
				|| (c >= '0' && c <= '9') || c == '_' || c == '-';
			result.push_back(safe ? c : '_');
		}
		return result.empty() ? std::string("App") : result;
	}

	/* 布局 ini 路径：<工程根>/imgui/<应用名>.ini
	 * ASSETS_PATH 对所有 target 相同，若各应用共用同一个 ini 会互相覆盖布局，故按应用名分文件。
	 * 建目录失败时退回工程根，保证布局仍可持久化。 */
	static std::string BuildLayoutIniPath()
	{
		const Application* app = Application::Instance();
		const std::filesystem::path file_name = SanitizeFileName(app != nullptr ? app->GetName() : std::string()) + ".ini";

		const std::filesystem::path root = std::filesystem::path(ASSETS_PATH).parent_path();
		const std::filesystem::path dir = root / "imgui";

		std::error_code error;
		std::filesystem::create_directories(dir, error);
		return (error ? root / file_name : dir / file_name).string();
	}

	ImGuiLayer::ImGuiLayer()
		: ILayer("ImGuiLayer")
	{
	}

	ImGuiLayer::~ImGuiLayer()
	{
	}

	void ImGuiLayer::OnAttached()
	{
		PROFILE_FUNCTION();

		IMGUI_CHECKVERSION();

		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO(); (void)io;
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;       // Enable Keyboard Controls
		io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;			// Enable Docking
		io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;			// Enable Multi-Viewport / Platform Windows
		//io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls

		/* 每个应用各用一份布局文件，避免编辑器与各 Sample 互相覆盖（见 BuildLayoutIniPath） */
		m_IniPath = BuildLayoutIniPath();
		SetLayoutSavingEnabled(true);

		/* 窗口需在字体构建前取得：字形按屏幕 content scale 光栅化，Retina 下文字才锐利 */
		auto* window = static_cast<GLFWwindow*>(Application::Instance()->GetWindow().GetNativeWindow());

		float dpi_scale = 1.0f;
		glfwGetWindowContentScale(window, &dpi_scale, nullptr);	/* Retina = 2.0 */
		const auto editor_fonts = EditorTheme::SetupFonts(io, ABSOLUTE_PATH("EditorRes/fonts"), dpi_scale);
		if (editor_fonts.Regular == nullptr)
		{
			CORE_LOG_ERROR("Failed to load editor fonts: EditorRes/fonts");
		}

		/* 编辑器主题：统一颜色体系 + 形状语言（见 Helios/ImGui/EditorTheme.h） */
		EditorTheme::ApplyDark();

		// Setup Platform bindings (GLFW only, no renderer backend)
		ImGui_ImplGlfw_InitForOpenGL(window, true);

		// Create our custom renderer
		m_Renderer = std::make_unique<ImGuiRenderer>();
		m_Renderer->Init();

		CORE_LOG_INFO("ImGuiLayer initialized with custom ImGuiRenderer");
	}

	void ImGuiLayer::OnDetached()
	{
		PROFILE_FUNCTION();

		m_Renderer->Cleanup();
		m_Renderer.reset();

		ImGui_ImplGlfw_Shutdown();
		ImGui::DestroyContext();
	}

	/* 布局持久化开关：ImGui 在 IniFilename == nullptr 时既不读也不写 ini。
	 * 用于最小化/尺寸退化期间冻结保存，避免钳制后的 DockNode 比例落盘。 */
	void ImGuiLayer::SetLayoutSavingEnabled(bool enabled)
	{
		ImGui::GetIO().IniFilename = enabled ? m_IniPath.c_str() : nullptr;
	}

	void ImGuiLayer::OnEvent(IEvent* event)
	{
		if (m_IsBlockEvents)
		{
			ImGuiIO& io = ImGui::GetIO();
			event->Handled |= event->IsInCategory(EventCategoryMouse) & io.WantCaptureMouse;
			event->Handled |= event->IsInCategory(EventCategoryKeyboard) & io.WantCaptureKeyboard;
		}
	}

	void ImGuiLayer::OnImGuiRender()
	{
		// Show Demo
		//bool show = true;
		//ImGui::ShowDemoWindow(&show);
	}

	void ImGuiLayer::Begin()
	{
		PROFILE_FUNCTION();

		// Platform new frame
		m_Renderer->NewFrame();
		ImGui_ImplGlfw_NewFrame();
		ImGui::NewFrame();
	}

	void ImGuiLayer::End()
	{
		PROFILE_FUNCTION();

		Renderer::GetRenderAPI()->PushDebugGroup("ImGuiPass");
		RenderQueryProfiler::Instance().BeginGPUScope("ImGuiPass");

		/* 生成DrawData,并提交渲染 */
		ImGui::Render();
		RenderPlatformWindows();

		RenderQueryProfiler::Instance().EndGPUScope();
		Renderer::GetRenderAPI()->PopDebugGroup();
	}

	void ImGuiLayer::RenderPlatformWindows()
	{
		PROFILE_FUNCTION();

		auto render_api = Renderer::GetRenderAPI();
		if (!render_api)
			return;

		/* 统一入口：在默认渲染目标上开一个"保留已有内容"的通道。各后端自处（Metal 复用当前
		 * CommandBuffer、drawable 以 LoadActionLoad 挂颜色附件；OpenGL 直接画默认帧缓冲）。 */
		render_api->BeginDefaultRenderPass(/*preserve_content=*/true);
		m_Renderer->RenderDrawData();
		render_api->EndDefaultRenderPass();

		/* io.DisplaySize / DisplayFramebufferScale 由 ImGui_ImplGlfw_NewFrame 每帧从 GLFW 读
		 * （逻辑点 / 像素比），渲染端乘上 scale 得到像素视口。别用引擎窗口尺寸去覆盖 —— GetWidth() 是
		 * 逻辑点，写进 DisplaySize 会让 UI 坐标系跟鼠标坐标错位。 */
		ImGuiIO& io = ImGui::GetIO();
		if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
		{
			GLFWwindow* backup_current_context = glfwGetCurrentContext();
			ImGui::UpdatePlatformWindows();
			ImGui::RenderPlatformWindowsDefault();
			glfwMakeContextCurrent(backup_current_context);
		}
	}

}
