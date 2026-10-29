#pragma once
#include <filesystem>
#include <string>
#include <vector>
#include <imgui.h>

#include "Helios/Core/Layer.h"
#include "Panels/ProjectLauncher/RecentProjects.h"

namespace Helios
{
	/* 启动页：先选工程，再进编辑器。
	 * 只负责选 —— 换项目根、建编辑器各层由装配层（EditorApp）在帧末落地。 */
	class ProjectLauncherLayer final : public ILayer
	{
	public:
		struct Decision
		{
			enum class Kind
			{
				None,
				Open,
				Quit
			};

			Kind Kind{ Kind::None };
			std::filesystem::path ProjectPath;
		};

		ProjectLauncherLayer();

		void OnImGuiRender() override;

		/* 本帧的选择结果：取走后清空，同一帧只处理一次 */
		[[nodiscard]] Decision TakeDecision();

		/* 指定一个工程立刻打开（命令行给了 --project 时用，不必等用户点） */
		void RequestOpen(const std::filesystem::path& project_path);

		/* 打开失败时把原因摊在页面上（仍然留在启动页） */
		void ShowError(const std::string& message) { m_Error = message; }

		/* ---- 加载进度：编辑器各层一帧建一层，这期间页面只显示进度 ---- */
		void BeginLoading(const std::string& project_name);
		/* label = 正在做的事，progress = 0..1 的目标值（条子会追过去） */
		void SetLoadingStage(const std::string& label, float progress);
		void EndLoading();

	private:
		struct Entry
		{
			std::filesystem::path Path;
			std::string Name;
			bool Exists{ false };
		};

		/* 列表按磁盘现状重读：工程名取 Project.xml，目录没了标出来 */
		void RefreshEntries();

		/* 品牌记号：烘好的太阳纹样贴图，未加载好先画简版 */
		void DrawEmblem(ImDrawList* draw_list, const ImVec2& center, float size);
		/* 背景水印：同一枚纹样，边缘按半径渐隐，和牌头的光圈同心 */
		void DrawEmblemWatermark(ImDrawList* draw_list, const ImVec2& center, float size);

		/* 牌头：记号 + 引擎名 + 版本号 */
		void ShowBrand(float width);
		/* 最近工程卡"按条数"想要的高度（不含窗口矮时的压缩） */
		float RecentListHeight() const;
		/* height 由页面按窗口剩下的空间定：条目多、窗口矮时卡片自己滚 */
		void ShowRecentList(float width, float height);
		void ShowActions(float width);
		/* 页脚：左边操作提示，右边退出 */
		void ShowFooter(float width);
		void ShowNewProjectPopup();
		/* 加载页画在前景层上：编辑器各层那时已经在下面建起来了，普通窗口会被它们盖住 */
		void DrawLoadingOverlay(const ImVec2& viewport_min, const ImVec2& viewport_size,
			float left, float top, float width);
		/* 按表单内容建工程：成功就直接进这个工程，失败把原因留在表单上 */
		bool CreateProjectFromForm();

		RecentProjects m_RecentProjects;
		std::vector<Entry> m_Entries;
		int m_Selected{ -1 };
		Decision m_Decision;
		std::string m_Error;
		/* 牌头到页脚实际占的高度（第一帧按估算，之后用上一帧量到的值） */
		float m_ChromeHeight{ 0.0f };

		/* 品牌记号与水印贴图（首次绘制时取；没设备时用简版） */
		SharedPtr<DeviceTexture> m_pEmblem;
		SharedPtr<DeviceTexture> m_pEmblemFade;
		bool m_EmblemRequested{ false };
		bool m_FadeRequested{ false };

		/* 加载中：页面只剩"正在打开"的进度，不再响应列表操作 */
		bool m_Loading{ false };
		std::string m_LoadingProject;
		std::string m_LoadingStage;
		float m_LoadingTarget{ 0.0f };  /* 当前阶段折算出来的目标进度 */
		float m_LoadingDisplay{ 0.0f }; /* 画出来的进度：追着目标走，阶段之间是滑过去的 */

		/* 新建工程表单（InputText 要的是裸缓冲区，留够路径长度） */
		char m_NewProjectName[128]{};
		char m_NewProjectLocation[1024]{};
		std::string m_NewProjectError;
		/* 弹窗刚打开时把焦点放进名字输入框 */
		bool m_FocusNameField{ false };
	};
}
