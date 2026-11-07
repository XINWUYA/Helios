#include "Pch.h"
#include "ProjectLauncherLayer.h"

#include "Common/EditorCommon.h"
#include "UI/EditorIcons.h"
#include "Common/FileDialog.h"
#include "UI/PanelChrome.h"

#include "Helios/Common/AssetPath.h"
#include "Helios/Common/PathUtils.h"
#include "Common/ProjectConfig.h"
#include "Helios/Core/AssetManager.h"
#include "Helios/Renderer/Renderer.h"
#include "UI/EditorTheme.h"
#include <cstdio>

namespace Helios
{
	namespace
	{
		constexpr float kContentMaxWidth = 780.0f; /* 设计像素：内容列最宽多少 */
		constexpr float kRowHeight = 56.0f;        /* 一行工程：名字 + 路径两行 */
		constexpr float kBrandMark = 128.0f;      /* 品牌记号边长：纹样细节多，96 太挤 */
		constexpr float kChromeHeight = 250.0f;    /* 牌头到页脚的高度估算（实际按实测值走） */
		constexpr float kMinListHeight = 96.0f;    /* 列表被压到最小时也留这么多，再小没法看 */
		constexpr float kEmptyListHeight = 120.0f;
		constexpr float kLoadingBlockHeight = 132.0f; /* 加载页那块：牌头 + 一行说明 + 进度条 */
		constexpr const char* kVersion = "1.0.0";

		ImU32 ToColor(const ImVec4& color)
		{
			return ImGui::GetColorU32(color);
		}

		ImVec4 WithAlpha(const ImVec4& color, float alpha)
		{
			return ImVec4(color.x, color.y, color.z, alpha);
		}

		/* 单行文字裁到 max_x 之前：路径往往比内容列长，别让它压出去 */
		void DrawClippedText(ImDrawList* draw_list, ImFont* font, float font_size, const ImVec2& pos,
			float max_x, ImU32 color, const char* text)
		{
			draw_list->PushClipRect(pos, ImVec2(max_x, pos.y + font_size * 1.5f), true);
			draw_list->AddText(font, font_size, pos, color, text);
			draw_list->PopClipRect();
		}

		/* 行右端的小箭头：只有鼠标停在这一行才画，说明"点了会进去" */
		void DrawChevron(ImDrawList* draw_list, const ImVec2& center, float size, ImU32 color)
		{
			const float thickness = ImMax(1.4f, size * 0.2f);
			draw_list->AddLine(ImVec2(center.x - size, center.y - size), ImVec2(center.x, center.y), color, thickness);
			draw_list->AddLine(ImVec2(center.x, center.y), ImVec2(center.x - size, center.y + size), color, thickness);
		}

		/* 页面底色：自上而下差一档明度，牌头背后再叠一层极淡的强调色光晕 ——
		 * 光从左上角来，页面才不是一块死板的纯色。光晕用同心圆叠出渐隐，每圈都很淡。 */
		void DrawPageBackground(ImDrawList* draw_list, const ImVec2& min, const ImVec2& max, const ImVec2& glow_center)
		{
			const ImU32 top = ToColor(EditorTheme::Token::Neutral1);
			const ImU32 bottom = ToColor(EditorTheme::Token::Neutral0);
			draw_list->AddRectFilledMultiColor(min, max, top, top, bottom, bottom);

			const ImVec4 accent = EditorTheme::Token::Accent;
			constexpr int kGlowSteps = 18;
			for (int i = 0; i < kGlowSteps; ++i)
			{
				const float t = static_cast<float>(i) / static_cast<float>(kGlowSteps - 1);
				const float radius = ImLerp(Scaled(320.0f), Scaled(40.0f), t);
				const float alpha = 0.0045f * (1.0f - t) + 0.0008f;
				draw_list->AddCircleFilled(glow_center, radius, ToColor(WithAlpha(accent, alpha)), 56);
			}
		}
	}

	ProjectLauncherLayer::ProjectLauncherLayer()
		: ILayer("ProjectLauncherLayer", 100) /* 覆盖层最高：启动页盖住编辑器与 UI */
	{
		m_RecentProjects.Load();
		RefreshEntries();

	}

	void ProjectLauncherLayer::OnImGuiRender()
	{
		const ImGuiViewport* viewport = ImGui::GetMainViewport();

		ImGui::SetNextWindowPos(viewport->WorkPos);
		ImGui::SetNextWindowSize(viewport->WorkSize);
		ImGui::SetNextWindowViewport(viewport->ID);

		/* 整页就是一张画布：无标题栏、无停靠、不留边距 */
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		ImGui::PushStyleColor(ImGuiCol_WindowBg, EditorTheme::Token::Neutral0);

		const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse
			| ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking
			| ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus
			| ImGuiWindowFlags_NoSavedSettings;

		if (ImGui::Begin("###HeliosLauncher", nullptr, flags))
		{
			const ImVec2 avail = ImGui::GetContentRegionAvail();

			/* 内容列：先让出两侧边距，窄到放不下就按窗口收 */
			const float max_width = ImMax(avail.x - Scaled(16.0f), Scaled(200.0f));
			const float width = ImMin(ImClamp(avail.x - Scaled(120.0f), Scaled(320.0f), Scaled(kContentMaxWidth)), max_width);
			const float left = (avail.x - width) * 0.5f;

			if (m_Loading)
			{
				/* 加载中：整页只剩进度，画在前景层上（编辑器各层这时已经在下面建起来了）。
				 * 牌头位置和列表页一致，切过来时视觉上是同一页。 */
				const float block_height = Scaled(kLoadingBlockHeight);
				float top = ImMax((avail.y - block_height) * 0.42f, Scaled(24.0f));
				top = ImMin(top, ImMax(avail.y - block_height - Scaled(8.0f), Scaled(12.0f)));

				DrawLoadingOverlay(viewport->WorkPos, viewport->WorkSize, left, top, width);
			}
			else
			{
				/* 竖直方向：牌头到页脚是固定高度，列表吃剩下的 ——
				 * 条目多、窗口矮的时候压缩列表（列表自己带滚动条），按钮与页脚始终留在窗内。
				 * 固定高度按上一帧实测（字体、主题变了也自动跟上），首帧先用估算值。 */
				const float chrome_height = (m_ChromeHeight > 0.0f) ? m_ChromeHeight : Scaled(kChromeHeight);
				const float list_height = ImMin(RecentListHeight(),
					ImMax(avail.y - Scaled(16.0f) - chrome_height, Scaled(kMinListHeight)));
				const float block_height = chrome_height + list_height;

				/* 居中偏上；放不下时先减留白，实在不够才让整页滚 */
				float top = ImMax((avail.y - block_height) * 0.42f, Scaled(24.0f));
				top = ImMin(top, ImMax(avail.y - block_height - Scaled(8.0f), Scaled(12.0f)));

				const ImVec2 window_pos = ImGui::GetWindowPos();
				const float mark_half = Scaled(kBrandMark) * 0.5f;
				const ImVec2 emblem_center(window_pos.x + left + mark_half, window_pos.y + top + mark_half);

				DrawPageBackground(ImGui::GetWindowDrawList(), window_pos,
					ImVec2(window_pos.x + ImGui::GetWindowWidth(), window_pos.y + ImGui::GetWindowHeight()),
					emblem_center);

				/* 背景水印和光圈同心：一枚渐隐的纹样垫在牌头后面 */
				DrawEmblemWatermark(ImGui::GetWindowDrawList(), emblem_center, Scaled(460.0f));

				/* 居左用 Indent 而不是 SetCursorPos —— 换行时 ImGui 把 x 拉回缩进位，
				 * 只有 Indent 能保证每一行都从同一列开始。 */
				ImGui::SetCursorPosY(top);
				ImGui::Indent(left);

				ShowBrand(width);
				ShowRecentList(width, list_height);
				ShowActions(width);
				ShowFooter(width);
				ShowNewProjectPopup();

				/* 记下牌头到页脚真实占的高度：下帧按它决定列表能有多大（首帧用估算值） */
				const float used_height = ImGui::GetCursorPosY() - top;
				if (used_height > list_height + Scaled(80.0f))
					m_ChromeHeight = used_height - list_height;

				/* 选中一行后回车即打开：鼠标双击之外的第二条路（弹窗开着时让给弹窗） */
				if (m_Selected >= 0 && !ImGui::IsPopupOpen("New Project")
					&& !ImGui::IsAnyItemActive() && KeyPressed(ImGuiKey_Enter))
				{
					const Entry& entry = m_Entries[static_cast<size_t>(m_Selected)];
					if (entry.Exists)
						m_Decision = Decision{ Decision::Kind::Open, entry.Path };
				}

				ImGui::Unindent(left);
			}
		}

		ImGui::End();
		ImGui::PopStyleColor();
		ImGui::PopStyleVar(3);
	}

	ProjectLauncherLayer::Decision ProjectLauncherLayer::TakeDecision()
	{
		const Decision decision = m_Decision;
		m_Decision = Decision{};
		return decision;
	}

	void ProjectLauncherLayer::RequestOpen(const std::filesystem::path& project_path)
	{
		m_Decision = Decision{ Decision::Kind::Open, project_path };
	}

	void ProjectLauncherLayer::BeginLoading(const std::string& project_name)
	{
		m_Loading = true;
		m_LoadingProject = project_name;
		m_LoadingStage.clear();
		m_LoadingTarget = 0.0f;
		m_LoadingDisplay = 0.0f;
		m_Error.clear();
	}

	void ProjectLauncherLayer::SetLoadingStage(const std::string& label, float progress)
	{
		m_LoadingStage = label;
		m_LoadingTarget = ImClamp(progress, 0.0f, 1.0f);
	}

	void ProjectLauncherLayer::EndLoading()
	{
		m_Loading = false;
		m_LoadingDisplay = 1.0f;
	}

	void ProjectLauncherLayer::DrawLoadingOverlay(const ImVec2& viewport_min, const ImVec2& viewport_size,
		float left, float top, float width)
	{
		/* 条子追目标：阶段是跳变的，画出来是一条滑过去的曲线 */
		const float dt = ImGui::GetIO().DeltaTime;
		m_LoadingDisplay += (m_LoadingTarget - m_LoadingDisplay) * ImMin(1.0f, dt * 8.0f);
		if (ImAbs(m_LoadingTarget - m_LoadingDisplay) < 0.002f)
			m_LoadingDisplay = m_LoadingTarget;

		ImDrawList* const draw_list = ImGui::GetForegroundDrawList();

		/* 整屏底色：下面正在建起来的编辑器先盖住；和列表页同一套渐变 + 牌头光晕 */
		const ImVec2 origin(viewport_min.x + left, viewport_min.y + top);
		const float mark_half = Scaled(kBrandMark) * 0.5f;
		const ImVec2 emblem_center(origin.x + mark_half, origin.y + mark_half);

		DrawPageBackground(draw_list, viewport_min,
			ImVec2(viewport_min.x + viewport_size.x, viewport_min.y + viewport_size.y),
			emblem_center);
		DrawEmblemWatermark(draw_list, emblem_center, Scaled(460.0f));

		/* 牌头：和列表页同一套尺寸，切过来时不会跳 */
		const float mark = Scaled(kBrandMark);
		DrawEmblem(draw_list, ImVec2(origin.x + mark * 0.5f, origin.y + mark * 0.5f), mark);

		ImFont* const display = EditorTheme::GetFonts().Display;
		ImFont* const word_font = (display != nullptr) ? display : ImGui::GetFont();
		const float word_size = (display != nullptr) ? display->FontSize * display->Scale : ImGui::GetFontSize();
		draw_list->AddText(word_font, word_size,
			ImVec2(origin.x + mark + Scaled(14.0f), origin.y + (mark - word_size) * 0.5f),
			ToColor(EditorTheme::Token::Text), "Helios");

		ImFont* const font = ImGui::GetFont();
		const float font_size = ImGui::GetFontSize();

		/* Opening <工程名> */
		const float opening_y = origin.y + mark + Scaled(22.0f);
		const char* const opening = "Opening";
		const ImVec2 opening_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, opening);
		draw_list->AddText(font, font_size, ImVec2(origin.x, opening_y),
			ToColor(EditorTheme::Token::TextDim), opening);
		draw_list->AddText(font, font_size, ImVec2(origin.x + opening_size.x + Scaled(6.0f), opening_y),
			ToColor(EditorTheme::Token::Text), m_LoadingProject.c_str());

		/* 进度条：细轨 + 强调色填充 */
		const float bar_height = Scaled(3.0f);
		const ImVec2 bar_min(origin.x, opening_y + font_size + Scaled(16.0f));
		const ImVec2 bar_max(bar_min.x + width, bar_min.y + bar_height);

		draw_list->AddRectFilled(bar_min, bar_max, ToColor(EditorTheme::Token::Neutral4), bar_height * 0.5f);
		if (m_LoadingDisplay > 0.001f)
		{
			draw_list->AddRectFilled(bar_min, ImVec2(bar_min.x + width * m_LoadingDisplay, bar_max.y),
				ToColor(EditorTheme::Token::Accent), bar_height * 0.5f);
		}

		/* 阶段名 + 百分比 */
		const float stage_y = bar_max.y + Scaled(10.0f);
		draw_list->AddText(font, font_size, ImVec2(origin.x, stage_y),
			ToColor(EditorTheme::Token::TextDim), m_LoadingStage.c_str());

		char percent[8];
		std::snprintf(percent, sizeof(percent), "%d%%", static_cast<int>(m_LoadingDisplay * 100.0f + 0.5f));
		const ImVec2 percent_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, percent);
		draw_list->AddText(font, font_size, ImVec2(origin.x + width - percent_size.x, stage_y),
			ToColor(EditorTheme::Token::TextDim), percent);
	}

	float ProjectLauncherLayer::RecentListHeight() const
	{
		if (m_Entries.empty())
			return Scaled(kEmptyListHeight);

		return ImMin(Scaled(kRowHeight) * static_cast<float>(m_Entries.size()), Scaled(360.0f));
	}

	void ProjectLauncherLayer::RefreshEntries()
	{
		m_Entries.clear();

		for (const std::filesystem::path& path : m_RecentProjects.Items())
		{
			std::error_code error;
			Entry entry;
			entry.Path = path;
			entry.Name = LoadProjectConfig(path).Name;
			entry.Exists = std::filesystem::is_regular_file(path / "Project.xml", error);
			m_Entries.push_back(std::move(entry));
		}

		if (m_Selected >= static_cast<int>(m_Entries.size()))
			m_Selected = static_cast<int>(m_Entries.size()) - 1;
	}

	void ProjectLauncherLayer::DrawEmblem(ImDrawList* draw_list, const ImVec2& center, float size)
	{
		if (!m_EmblemRequested)
		{
			m_EmblemRequested = true;

			/* 没有渲染设备（离屏预览）时不碰资产缓存，直接用简版 */
			if (Renderer::GetRenderAPI() != nullptr)
				m_pEmblem = AssetManager::Instance().GetOrLoad<DeviceTexture>(
					PathToUtf8(g_EngineResPath / "EditorRes" / "branding" / "sun-emblem.png"));
		}

		const float half = size * 0.5f;
		const ImU32 accent = ToColor(EditorTheme::Token::Accent);

		/* 贴图是白色线稿，按强调色着色 */
		if (m_pEmblem != nullptr && m_pEmblem->IsLoaded())
		{
			draw_list->AddImage((ImTextureID)m_pEmblem.get(),
				ImVec2(center.x - half, center.y - half), ImVec2(center.x + half, center.y + half),
				ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), accent);
			return;
		}

		/* 兜底：圆盘 + 八道光芒 */
		draw_list->AddCircleFilled(center, size * 0.20f, accent, 28);
		for (int i = 0; i < 8; ++i)
		{
			const float angle = static_cast<float>(i) * 0.785398163f;
			const float dx = std::cos(angle);
			const float dy = std::sin(angle);
			draw_list->AddLine(ImVec2(center.x + dx * size * 0.32f, center.y + dy * size * 0.32f),
				ImVec2(center.x + dx * size * 0.47f, center.y + dy * size * 0.47f), accent,
				ImMax(1.0f, size * 0.075f));
		}
	}

	void ProjectLauncherLayer::DrawEmblemWatermark(ImDrawList* draw_list, const ImVec2& center, float size)
	{
		if (!m_FadeRequested)
		{
			m_FadeRequested = true;
			if (Renderer::GetRenderAPI() != nullptr)
				m_pEmblemFade = AssetManager::Instance().GetOrLoad<DeviceTexture>(
					PathToUtf8(g_EngineResPath / "EditorRes" / "branding" / "sun-emblem-fade.png"));
		}

		if (m_pEmblemFade == nullptr || !m_pEmblemFade->IsLoaded())
			return;

		/* 贴图本身的 alpha 已经按半径衰减过，这里只给一个整体透明度，压到背景那一档 */
		const float half = size * 0.5f;
		const ImVec4 tint = WithAlpha(EditorTheme::Token::Accent, 0.16f);
		draw_list->AddImage((ImTextureID)m_pEmblemFade.get(),
			ImVec2(center.x - half, center.y - half), ImVec2(center.x + half, center.y + half),
			ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), ToColor(tint));
	}

	void ProjectLauncherLayer::ShowBrand(float width)
	{
		ImDrawList* const draw_list = ImGui::GetWindowDrawList();
		const ImVec2 origin = ImGui::GetCursorScreenPos();

		/* 品牌记号：太阳纹样，直接落在页面上（不套底座） */
		const float mark = Scaled(kBrandMark);
		DrawEmblem(draw_list, ImVec2(origin.x + mark * 0.5f, origin.y + mark * 0.5f), mark);

		/* 字样与记号同中线 */
		ImFont* const display = EditorTheme::GetFonts().Display;
		ImFont* const word_font = (display != nullptr) ? display : ImGui::GetFont();
		const float word_size = (display != nullptr) ? display->FontSize * display->Scale : ImGui::GetFontSize();

		draw_list->AddText(word_font, word_size,
			ImVec2(origin.x + mark + Scaled(14.0f), origin.y + (mark - word_size) * 0.5f),
			ToColor(EditorTheme::Token::Text), "Helios");

		/* 版本号：一行小字，不做角标 */
		ImFont* const font = ImGui::GetFont();
		const float version_size = ImGui::GetFontSize() * 0.9f;
		const ImVec2 version_text = font->CalcTextSizeA(version_size, FLT_MAX, 0.0f, kVersion);
		draw_list->AddText(font, version_size,
			ImVec2(origin.x + width - version_text.x, origin.y + (mark - version_text.y) * 0.5f),
			ToColor(EditorTheme::Token::TextDim), kVersion);

		/* 副标题从记号下方重新起一行 */
		ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + mark + Scaled(14.0f)));
		ImGui::TextColored(EditorTheme::Token::TextDim, "Open a project to start editing, or create a new one");

		ImGui::Dummy(ImVec2(0.0f, Scaled(18.0f)));

		/* 小节标题：小一号字样 + 到右缘的细线（列表靠这条线起头，不再套框） */
		const char* const title = "RECENT PROJECTS";
		const ImVec2 min = ImGui::GetCursorScreenPos();
		const float height = ImGui::GetTextLineHeight() + Scaled(6.0f);
		ImGui::Dummy(ImVec2(width, height));

		const float font_size = ImGui::GetFontSize() * 0.9f;
		const ImVec2 text_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, title);

		draw_list->AddText(font, font_size, ImVec2(min.x, min.y + (height - text_size.y) * 0.5f),
			ToColor(EditorTheme::Token::TextDim), title);

		const float line_y = min.y + height * 0.5f;
		draw_list->AddLine(ImVec2(min.x + text_size.x + Scaled(12.0f), line_y), ImVec2(min.x + width, line_y),
			ToColor(EditorTheme::Token::Separator), 1.0f);

		ImGui::Dummy(ImVec2(0.0f, Scaled(2.0f)));
	}

	void ProjectLauncherLayer::ShowRecentList(float width, float height)
	{
		const float row_height = Scaled(kRowHeight);
		const float list_height = height;

		/* 列表不套框：行直接落在页面上，靠行间细线分隔（扁平）。
		 * 子窗口只负责裁剪与滚动，透明无边框、无内边距。 */
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		ImGui::PushStyleColor(ImGuiCol_ChildBg, ToColor(EditorTheme::Token::Clear));
		ImGui::BeginChild("##RecentProjects", ImVec2(width, list_height), false);

		ImDrawList* const draw_list = ImGui::GetWindowDrawList();
		ImFont* const font = ImGui::GetFont();
		const float font_size = ImGui::GetFontSize();

		if (m_Entries.empty())
		{
			/* 空列表也保持扁平：居中的图标 + 两行说明 */
			const ImVec2 min = ImGui::GetCursorScreenPos();
			const float content_width = ImGui::GetContentRegionAvail().x;
			const float center_x = min.x + content_width * 0.5f;
			const float icon_y = min.y + list_height * 0.5f - Scaled(18.0f);

			Icons::Draw(draw_list, Icons::Id::Directory, ImVec2(center_x, icon_y), Scaled(28.0f),
				ToColor(EditorTheme::Token::Neutral7));

			const char* const line = "No projects yet";
			const ImVec2 line_text = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, line);
			draw_list->AddText(font, font_size,
				ImVec2(center_x - line_text.x * 0.5f, icon_y + Scaled(22.0f)),
				ToColor(EditorTheme::Token::TextLabel), line);

			const char* const hint = "Create a new project, or open an existing folder";
			const float hint_size = font_size * 0.9f;
			const ImVec2 hint_text = font->CalcTextSizeA(hint_size, FLT_MAX, 0.0f, hint);
			draw_list->AddText(font, hint_size,
				ImVec2(center_x - hint_text.x * 0.5f, icon_y + Scaled(42.0f)),
				ToColor(EditorTheme::Token::TextDim), hint);
		}

		/* 要划掉的工程先记下来：循环里重建列表会让 entry 引用失效 */
		std::filesystem::path remove_request;
		int remove_index = -1;

		/* 会出滚动条时把右边那条栏让出来，右端的字和细线都不会被压在条上 */
		const float scroll_reserve = (row_height * static_cast<float>(m_Entries.size()) > list_height + 0.5f)
			? ImGui::GetStyle().ScrollbarSize + Scaled(6.0f) : 0.0f;
		const float content_right = ImGui::GetCursorScreenPos().x + width - scroll_reserve;

		/* 行与行之间不留间距：行高自己带留白，高度算式才对得上 */
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
		for (int index = 0; index < static_cast<int>(m_Entries.size()); ++index)
		{
			const Entry& entry = m_Entries[static_cast<size_t>(index)];
			const bool last = (index + 1 == static_cast<int>(m_Entries.size()));
			ImGui::PushID(index);

			const ImVec2 row_min = ImGui::GetCursorScreenPos();
			/* 每行现取可用宽度：滚动条出现时行底不会压到条上 */
			ImGui::InvisibleButton("##entry", ImVec2(content_right - row_min.x, row_height));
			const float row_width = ImGui::GetItemRectSize().x;
			const ImVec2 row_max(row_min.x + row_width, row_min.y + row_height);
			const bool hovered = ImGui::IsItemHovered();
			const bool selected = (m_Selected == index);

			if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
			{
				m_Selected = index;
				m_Error.clear();
				if (entry.Exists && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
					m_Decision = Decision{ Decision::Kind::Open, entry.Path };
			}

			if (!entry.Exists && hovered)
				ImGui::SetTooltip("Project folder not found");

			/* 右键菜单：在系统文件管理器里定位这个工程，或只从列表里划掉。
			 * 不用 BeginPopupContextItem —— 它把弹层背景压到 PopupBg 的 0.6 倍，
			 * 半透明底下透着列表文字；这里自己开弹窗，用实底。 */
			if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right))
				ImGui::OpenPopup("##entry_menu");

			/* 弹层版式和编辑器其它右键菜单共用一套（见 PanelChrome::PushMenuStyle） */
			PanelChrome::PushMenuStyle();
			ImGui::SetNextWindowBgAlpha(1.0f);
			if (ImGui::BeginPopup("##entry_menu"))
			{

				std::error_code folder_error;
				const bool folder_exists = std::filesystem::is_directory(entry.Path, folder_error);

				if (!folder_exists)
					ImGui::BeginDisabled();
				const bool reveal_clicked = PanelChrome::MenuItemWithIcon(Icons::Id::Directory, "Show in file explorer");
				if (!folder_exists)
					ImGui::EndDisabled();

				/* 分隔线比弹层底亮一档：主题里的 Separator 比弹层还暗，直接用会看不见 */
				ImGui::PushStyleColor(ImGuiCol_Separator, EditorTheme::Token::Border);
				ImGui::Separator();
				ImGui::PopStyleColor();

				const bool remove_clicked = PanelChrome::MenuItemWithIcon(Icons::Id::Delete, "Remove from list");

				ImGui::EndPopup();

				if (reveal_clicked && folder_exists)
				{
					const std::filesystem::path shown = std::filesystem::absolute(entry.Path).make_preferred();
					const std::string shown_utf8 = PathToUtf8(shown);
					OpenFileExplorer(shown_utf8.c_str());
				}

				if (remove_clicked)
				{
					remove_request = entry.Path;
					remove_index = index;
				}
			}

			PanelChrome::PopMenuStyle();

			/* ---- 行底：整行铺一层很淡的白，选中再加左缘一道强调色竖条 ---- */
			if (selected)
			{
				draw_list->AddRectFilled(row_min, row_max,
					ToColor(ImVec4(1.0f, 1.0f, 1.0f, 0.070f)), Scaled(6.0f));

				const float bar_width = ImMax(2.0f, Scaled(2.5f));
				draw_list->AddRectFilled(ImVec2(row_min.x, row_min.y + Scaled(11.0f)),
					ImVec2(row_min.x + bar_width, row_max.y - Scaled(11.0f)),
					ToColor(EditorTheme::Token::Accent), bar_width * 0.5f);
			}
			else if (hovered)
			{
				draw_list->AddRectFilled(row_min, row_max,
					ToColor(ImVec4(1.0f, 1.0f, 1.0f, 0.045f)), Scaled(6.0f));
			}

			/* ---- 图标：直接落在页面上，占一个固定列宽 ---- */
			const float icon_center_x = row_min.x + Scaled(24.0f);
			const float icon_center_y = row_min.y + row_height * 0.5f;
			Icons::Draw(draw_list, Icons::Id::Directory, ImVec2(icon_center_x, icon_center_y), Scaled(22.0f),
				ToColor(entry.Exists ? EditorTheme::Token::TextLabel : EditorTheme::Token::TextDim));

			const float text_x = row_min.x + Scaled(48.0f);

			/* ---- 右端：找不到工程给一行红字；能找到就在悬停时露出一枚箭头 ---- */
			float text_right = row_max.x - Scaled(10.0f);

			if (!entry.Exists)
			{
				const char* const tag = "NOT FOUND";
				const float tag_size = ImGui::GetFontSize() * 0.78f;
				const ImVec2 tag_text = font->CalcTextSizeA(tag_size, FLT_MAX, 0.0f, tag);
				draw_list->AddText(font, tag_size,
					ImVec2(row_max.x - tag_text.x, icon_center_y - tag_text.y * 0.5f),
					ToColor(EditorTheme::Token::Danger), tag);

				text_right = row_max.x - tag_text.x - Scaled(12.0f);
			}
			else if (hovered)
			{
				DrawChevron(draw_list, ImVec2(row_max.x - Scaled(14.0f), icon_center_y),
					Scaled(4.0f), ToColor(EditorTheme::Token::TextLabel));
				text_right = row_max.x - Scaled(28.0f);
			}

			/* ---- 名字与路径 ---- */
			DrawClippedText(draw_list, font, font_size, ImVec2(text_x, row_min.y + Scaled(12.0f)), text_right,
				ToColor(entry.Exists ? EditorTheme::Token::Text : EditorTheme::Token::TextDim), entry.Name.c_str());

			const std::string path_text = PathToUtf8(entry.Path);
			const ImVec4& path_color = !entry.Exists ? EditorTheme::Token::Danger
				: (selected ? EditorTheme::Token::TextLabel : EditorTheme::Token::TextDim);
			DrawClippedText(draw_list, font, font_size * 0.9f, ImVec2(text_x, row_min.y + Scaled(32.0f)), text_right,
				ToColor(path_color), path_text.c_str());

			/* ---- 行间细线：从文字列起，末行不画 ---- */
			if (!last)
			{
				draw_list->AddLine(ImVec2(text_x, row_max.y - 0.5f), ImVec2(row_max.x, row_max.y - 0.5f),
					ToColor(WithAlpha(EditorTheme::Token::Border, 0.45f)), 1.0f);
			}

			ImGui::PopID();
		}
		ImGui::PopStyleVar();

		/* 列表底部一条细线收口：不套框也要有边界（画在可见底边，滚动时也贴着边） */
		if (!m_Entries.empty())
		{
			const ImGuiWindow* child = ImGui::GetCurrentWindow();
			const float bottom_y = child->Pos.y + child->Size.y - 0.5f;
			draw_list->AddLine(ImVec2(child->Pos.x, bottom_y),
				ImVec2(child->Pos.x + width - scroll_reserve - 0.5f, bottom_y),
				ToColor(EditorTheme::Token::Separator), 1.0f);
		}

		ImGui::EndChild();
		ImGui::PopStyleColor();
		ImGui::PopStyleVar();

		/* 划掉的工程等列表画完再动：边遍历边删会让行引用失效。
		 * 选中项在它后面时索引跟着前移，删的就是选中项就停在原位（越界由 RefreshEntries 收）。 */
		if (!remove_request.empty())
		{
			m_RecentProjects.Remove(remove_request);
			RefreshEntries();

			if (m_Selected > remove_index)
				--m_Selected;
		}
	}

	void ProjectLauncherLayer::ShowActions(float width)
	{
		ImGui::Dummy(ImVec2(0.0f, Scaled(16.0f)));

		/* 两颗按钮分掉一行：窗口窄的时候一起收窄，别顶出内容列 */
		const float spacing = ImGui::GetStyle().ItemSpacing.x;
		const ImVec2 button_size(ImMin(Scaled(158.0f), (width - spacing) * 0.5f), Scaled(36.0f));

		/* 主操作：整页只有这一颗实心强调色按钮，其余两档退到次要位 */
		ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::Token::Accent);
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorTheme::Token::AccentHover);
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, EditorTheme::Token::AccentDown);
		ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::Neutral0);
		const bool open_clicked = ImGui::Button("Open Project...", button_size);
		ImGui::PopStyleColor(4);

		if (open_clicked)
		{
			m_Error.clear();
			const std::string folder = FileDialog::OpenFolder("Select a Helios project folder");
			std::filesystem::path path;
			if (!folder.empty() && TryPathFromUtf8(folder, path))
			{
				std::error_code error;
				if (std::filesystem::is_regular_file(path / "Project.xml", error))
					m_Decision = Decision{ Decision::Kind::Open, path };
				else
					m_Error = "Not a project folder (Project.xml not found): " + folder;
			}
		}

		ImGui::SameLine();
		if (ImGui::Button("New Project", button_size))
		{
			m_Error.clear();
			m_NewProjectError.clear();
			m_NewProjectName[0] = '\0';
			m_FocusNameField = true;
			/* 新工程默认落到引擎的 Projects/ 下：多个工程并排，和示例工程同一个地方 */
			const std::string default_dir = PathToUtf8(g_EngineResPath.parent_path() / "Projects");
			std::snprintf(m_NewProjectLocation, sizeof(m_NewProjectLocation), "%s", default_dir.c_str());
			ImGui::OpenPopup("New Project");
		}

		if (!m_Error.empty())
			ImGui::TextColored(EditorTheme::Token::Danger, "%s", m_Error.c_str());
	}

	void ProjectLauncherLayer::ShowFooter(float width)
	{
		ImGui::Dummy(ImVec2(0.0f, Scaled(12.0f)));

		/* 提示与退出同一行：退出是"离开"这一档，无底色，只留文字。
		 * 提示文字手绘，好跟右侧按钮对齐到同一条中线。 */
		const float button_height = Scaled(26.0f);
		const float quit_width = Scaled(92.0f);
		const ImVec2 line_min = ImGui::GetCursorScreenPos();
		const float quit_x = line_min.x + width - quit_width;

		const char* const hint = "Double-click a project to open it. Right-click to show its folder, or remove it from the list.";
		const float hint_width = quit_x - Scaled(16.0f) - line_min.x;

		/* 放不下就省略号收尾（别硬裁一半），窄到没地方就整句不画 */
		if (hint_width > Scaled(80.0f))
		{
			const float hint_y = line_min.y + (button_height - ImGui::GetTextLineHeight()) * 0.5f;
			ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::TextDim);
			ImGui::SetCursorScreenPos(ImVec2(line_min.x, hint_y));
			PanelChrome::DrawClippedTextLine(hint, hint_width);
			ImGui::PopStyleColor();
		}

		ImGui::SetCursorScreenPos(ImVec2(quit_x, line_min.y));
		ImGui::PushStyleColor(ImGuiCol_Button, ToColor(EditorTheme::Token::Clear));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorTheme::Token::White06);
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, EditorTheme::Token::White12);
		ImGui::PushStyleColor(ImGuiCol_Border, ToColor(EditorTheme::Token::Clear));
		ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::TextDim);
		const bool quit_clicked = ImGui::Button("Quit", ImVec2(quit_width, button_height));
		ImGui::PopStyleColor(5);

		if (quit_clicked)
			m_Decision = Decision{ Decision::Kind::Quit, {} };

		ImGui::SetCursorScreenPos(ImVec2(line_min.x, line_min.y + button_height + ImGui::GetStyle().ItemSpacing.y));
	}

	bool ProjectLauncherLayer::CreateProjectFromForm()
	{
		const std::string name(m_NewProjectName);
		std::filesystem::path parent;

		if (name.empty())
		{
			m_NewProjectError = "Project name is empty";
			return false;
		}

		if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos)
		{
			m_NewProjectError = "Project name can't contain slashes";
			return false;
		}

		if (!TryPathFromUtf8(m_NewProjectLocation, parent))
		{
			m_NewProjectError = "Location is not a valid path";
			return false;
		}

		const std::filesystem::path target = parent / PathFromUtf8(name);
		std::error_code error;
		if (std::filesystem::exists(target, error))
		{
			m_NewProjectError = "Folder already exists: " + PathToUtf8(target);
			return false;
		}

		if (!CreateProject(target, name))
		{
			m_NewProjectError = "Failed to create project: " + PathToUtf8(target);
			return false;
		}

		m_RecentProjects.Add(target);
		RefreshEntries();
		m_Selected = 0;
		m_Decision = Decision{ Decision::Kind::Open, target };
		return true;
	}

	void ProjectLauncherLayer::ShowNewProjectPopup()
	{
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

		/* 不要标题栏：标题、说明、字段、按钮都在同一块面上，圆角和启动页的卡片一致 */
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Scaled(22.0f), Scaled(20.0f)));
		ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, Scaled(12.0f));
		ImGui::PushStyleColor(ImGuiCol_PopupBg, EditorTheme::Token::Neutral3);

		const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize
			| ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove;

		if (!ImGui::BeginPopupModal("New Project", nullptr, flags))
		{
			ImGui::PopStyleColor();
			ImGui::PopStyleVar(2);
			return;
		}

		/* 字段定宽：弹窗宽度由它撑出来，别用 -1 跟着布局跑 */
		const float field_width = Scaled(400.0f);

		/* 标题 + 一句说明 */
		ImFont* const display = EditorTheme::GetFonts().Display;
		ImFont* const title_font = (display != nullptr) ? display : ImGui::GetFont();
		const float title_size = Scaled(20.0f);
		ImGui::GetWindowDrawList()->AddText(title_font, title_size, ImGui::GetCursorScreenPos(),
			ToColor(EditorTheme::Token::Text), "New Project");
		ImGui::Dummy(ImVec2(0.0f, title_size + Scaled(6.0f)));

		ImGui::TextColored(EditorTheme::Token::TextDim, "Create a project folder and start from an empty scene.");
		ImGui::Dummy(ImVec2(0.0f, Scaled(16.0f)));

		/* 名字 */
		ImGui::TextColored(EditorTheme::Token::TextDim, "Name");
		if (m_FocusNameField)
		{
			ImGui::SetKeyboardFocusHere();
			m_FocusNameField = false;
		}

		ImGui::SetNextItemWidth(field_width);
		const bool name_entered = ImGui::InputText("##name", m_NewProjectName, sizeof(m_NewProjectName),
			ImGuiInputTextFlags_EnterReturnsTrue);

		ImGui::Dummy(ImVec2(0.0f, Scaled(10.0f)));

		/* 位置 */
		ImGui::TextColored(EditorTheme::Token::TextDim, "Location");
		ImGui::SetNextItemWidth(field_width - Scaled(96.0f));
		ImGui::InputText("##location", m_NewProjectLocation, sizeof(m_NewProjectLocation));
		ImGui::SameLine();
		if (ImGui::Button("Browse...", ImVec2(Scaled(88.0f), 0.0f)))
		{
			const std::string folder = FileDialog::OpenFolder("Choose where the project folder goes");
			if (!folder.empty())
				std::snprintf(m_NewProjectLocation, sizeof(m_NewProjectLocation), "%s", folder.c_str());
		}

		/* 一行灰字：落在哪 / 哪里不对。长文本裁到字段宽，不把弹窗撑宽 */
		ImGui::Dummy(ImVec2(0.0f, Scaled(8.0f)));
		if (!m_NewProjectError.empty())
		{
			ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::Danger);
			PanelChrome::DrawClippedTextLine(m_NewProjectError.c_str(), field_width);
			ImGui::PopStyleColor();
		}
		else
		{
			std::string target_text = m_NewProjectLocation;
			target_text += "/";
			target_text += (m_NewProjectName[0] != '\0') ? m_NewProjectName : "<name>";

			ImDrawList* const draw_list = ImGui::GetWindowDrawList();
			ImFont* const font = ImGui::GetFont();
			const float font_size = ImGui::GetFontSize();
			const ImVec2 min = ImGui::GetCursorScreenPos();
			const ImVec2 max(min.x + field_width, min.y + font_size * 1.4f);

			const char* const lead = "Will create";
			const float lead_width = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, lead).x;

			draw_list->PushClipRect(min, max, true);
			draw_list->AddText(font, font_size, min, ToColor(EditorTheme::Token::TextDim), lead);
			draw_list->AddText(font, font_size, ImVec2(min.x + lead_width + Scaled(8.0f), min.y),
				ToColor(EditorTheme::Token::TextLabel), target_text.c_str());
			draw_list->PopClipRect();

			ImGui::Dummy(ImVec2(0.0f, font_size * 1.4f));
		}

		ImGui::Dummy(ImVec2(0.0f, Scaled(16.0f)));

		/* 按钮靠右：Cancel 在前、Create 在最后（默认动作放最右） */
		const float button_width = Scaled(96.0f);
		const float button_height = Scaled(32.0f);
		const float buttons_x = ImGui::GetCursorPosX() + field_width
			- button_width * 2.0f - ImGui::GetStyle().ItemSpacing.x;
		ImGui::SetCursorPosX(ImMax(buttons_x, ImGui::GetCursorPosX()));

		if (ImGui::Button("Cancel", ImVec2(button_width, button_height)) || KeyPressed(ImGuiKey_Escape))
			ImGui::CloseCurrentPopup();

		ImGui::SameLine();
		ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::Token::Accent);
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorTheme::Token::AccentHover);
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, EditorTheme::Token::AccentDown);
		ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::Neutral0);
		const bool create_clicked = ImGui::Button("Create", ImVec2(button_width, button_height));
		ImGui::PopStyleColor(4);

		if (create_clicked || name_entered)
		{
			if (CreateProjectFromForm())
				ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
		ImGui::PopStyleColor();
		ImGui::PopStyleVar(2);
	}
}
