#include "Pch.h"
#include "TextureOverviewPanel.h"

#include <algorithm>

#include "Assets/AstcBaker.h"
#include "Command/AssetFileOps.h"
#include "Common/EditorCommon.h"
#include "Common/TextMatch.h"
#include "EditorUI/EditorIcons.h"
#include "EditorUI/EditorTheme.h"
#include "EditorUI/ImGuiExtensions.h"
#include "EditorUI/PanelChrome.h"
#include "EditorUI/PanelRegistry.h"
#include "Helios/Common/AssetPath.h"
#include "Helios/Common/PathUtils.h"
#include "Helios/Common/Utils.h"
#include "Helios/Core/AstcFile.h"
#include "Helios/Core/TextureSettings.h"
#include "Helios/Renderer/RenderAPI.h"
#include "Panels/PanelWindow.h"

namespace Helios
{
	namespace
	{
		/* 数字列定宽：按最长的一种写法留够，再加表头排序箭头那一档，
		 * 数值变化时列不跟着抖，表头也不会被箭头挤成省略号 */
		float NumberColumnWidth(const char* widest_value)
		{
			return ImGui::CalcTextSize(widest_value).x + ImGui::GetStyle().CellPadding.x * 2.0f
				+ ImGui::GetFontSize();
		}

		/* 尺寸列装的是「宽 x 高」加后面一档 mip 级数 */
		float SizeColumnWidth()
		{
			return NumberColumnWidth("00000 x 00000") + ImGui::GetStyle().ItemInnerSpacing.x
				+ ImGui::CalcTextSize("00 mips").x;
		}

		/* 搜索词命中路径或格式：搜 "astc" 能把块压缩的那些筛出来 */
		bool Matches(const TextureOverviewPanel::Row& row, const std::string& lowercase_needle)
		{
			return ContainsCaseInsensitive(row.Path, lowercase_needle)
				|| ContainsCaseInsensitive(row.Format, lowercase_needle);
		}
	}

	uint8_t TextureOverviewPanel::MipCount(uint32_t width, uint32_t height)
	{
		uint32_t longest = std::max(width, height);
		uint8_t count = 1;
		while (longest > 1)
		{
			longest >>= 1;
			++count;
		}
		return count;
	}

	uint64_t TextureOverviewPanel::ImageBytes(uint32_t width, uint32_t height, uint32_t bytes_per_pixel, bool mips)
	{
		if (width == 0 || height == 0)
			return 0;

		const uint8_t levels = mips ? MipCount(width, height) : 1;
		uint64_t bytes = 0;
		for (uint8_t level = 0; level < levels; ++level)
		{
			const uint64_t level_width = std::max(1u, width >> level);
			const uint64_t level_height = std::max(1u, height >> level);
			bytes += level_width * level_height * bytes_per_pixel;
		}
		return bytes;
	}

	void TextureOverviewPanel::Scan(const std::filesystem::path& root, bool astc_upload,
		std::vector<Row>& out_rows, size_t& out_unreadable)
	{
		out_rows.clear();
		out_unreadable = 0;

		std::error_code error;
		for (std::filesystem::recursive_directory_iterator iter(root,
				std::filesystem::directory_options::skip_permission_denied, error), end;
			iter != end; iter.increment(error))
		{
			if (error)
				break;

			const std::filesystem::directory_entry& item = *iter;
			if (item.is_directory())
			{
				/* Cache 里全是重建得出来的产物，不算工程资产 */
				if (item.path().filename() == "Cache")
					iter.disable_recursion_pending();
				continue;
			}

			if (!item.is_regular_file())
				continue;

			const std::string extension = LowercaseExtension(item.path());
			if (AssetFileKindOf(extension) != AssetFileKind::Image)
				continue;

			/* .astc 是源图旁边的烘焙产物，引擎按源图路径加载：列两行等于把同一份显存算两遍 */
			if (extension == ".astc")
				continue;

			const std::string absolute = PathToUtf8(item.path());

			Row row;
			row.Path = PathToUtf8(std::filesystem::relative(item.path(), root, error));
			if (error || row.Path.empty())
			{
				error.clear();
				row.Path = PathToUtf8(item.path().filename());
			}
			row.FileBytes = static_cast<uint64_t>(item.file_size(error));
			error.clear();

			/* 源图自己的像素格式：HDR 解成 float 按 RGBA32F 建纹理，其余都是 RGBA8。
			 * 烘过的图也照算一份，括号里显示"这张图不烘焙会是什么格式" */
			const TextureLoadConfig config = TextureSettingsStore::Instance().Resolve(absolute);
			row.SourceFormat = config.IsHdr ? "RGBA32F" : "RGBA8";

			/* 合法的同名 .astc 就是上传给 GPU 的那份字节，只有 Metal 直传块数据 */
			const AstcBake::BakedInfo baked = AstcBake::InspectBaked(absolute);
			const bool use_astc = astc_upload && baked.Exists
				&& IsUploadableBlockSize(baked.BlockX, baked.BlockY)
				&& baked.Bytes == AstcBake::EstimateBytes(baked.Width, baked.Height, baked.BlockX, baked.BlockY);

			if (use_astc)
			{
				row.Width = static_cast<uint32_t>(baked.Width);
				row.Height = static_cast<uint32_t>(baked.Height);
				row.Mips = static_cast<uint8_t>(baked.Levels);
				row.Bytes = static_cast<uint64_t>(baked.Bytes) - AstcFile::kHeaderBytes;
				row.Baked = true;
				row.Format = "ASTC " + std::to_string(baked.BlockX) + "x" + std::to_string(baked.BlockY);
			}
			else
			{
				int width = 0;
				int height = 0;
				int channels = 0;
				if (!QueryImageInfo(absolute, width, height, channels) || width <= 0 || height <= 0)
				{
					++out_unreadable;
					continue;
				}

				const uint32_t bytes_per_pixel = config.IsHdr ? 16u : 4u;

				row.Width = static_cast<uint32_t>(width);
				row.Height = static_cast<uint32_t>(height);
				row.Mips = config.IsGenMips ? MipCount(row.Width, row.Height) : 1;
				row.Bytes = ImageBytes(row.Width, row.Height, bytes_per_pixel, config.IsGenMips);
				row.Format = row.SourceFormat;
			}

			out_rows.push_back(std::move(row));
		}
	}

	TextureOverviewPanel::Stats TextureOverviewPanel::Sum(const std::vector<Row>& rows,
		const std::string& lowercase_needle)
	{
		Stats stats;
		for (const Row& row : rows)
		{
			if (!Matches(row, lowercase_needle))
				continue;

			++stats.Count;
			stats.Bytes += row.Bytes;
		}
		return stats;
	}

	void TextureOverviewPanel::SortRows(std::vector<Row>& rows, int column, bool ascending)
	{
		/* 先比数值，同值一律按路径正序收尾：换个方向不该把同价的几行也翻过来 */
		const auto compare_key = [column](const Row& left, const Row& right) -> int
		{
			if (column == kColumnFormat)
			{
				if (left.Format != right.Format)
					return left.Format < right.Format ? -1 : 1;
			}
			else if (column == kColumnSize)
			{
				const uint64_t left_pixels = static_cast<uint64_t>(left.Width) * left.Height;
				const uint64_t right_pixels = static_cast<uint64_t>(right.Width) * right.Height;
				if (left_pixels != right_pixels)
					return left_pixels < right_pixels ? -1 : 1;
			}
			else if (column == kColumnDisk || column == kColumnMemory)
			{
				const uint64_t left_bytes = (column == kColumnDisk) ? left.FileBytes : left.Bytes;
				const uint64_t right_bytes = (column == kColumnDisk) ? right.FileBytes : right.Bytes;
				if (left_bytes != right_bytes)
					return left_bytes < right_bytes ? -1 : 1;
			}

			return 0;
		};

		std::sort(rows.begin(), rows.end(), [&compare_key, ascending](const Row& left, const Row& right)
		{
			const int order = compare_key(left, right);
			if (order == 0)
				return left.Path < right.Path;

			return ascending ? order < 0 : order > 0;
		});
	}

	bool TextureOverviewPanel::IsUploadableBlockSize(int block_x, int block_y)
	{
		/* 与 MetalTexture::ToAstcPixelFormat 认的四种块一致，别的块上传会失败退回解码 */
		const int key = block_x * 100 + block_y;
		return key == 404 || key == 505 || key == 606 || key == 808;
	}

	std::string TextureOverviewPanel::FormatLabel(const Row& row)
	{
		if (!row.Baked || row.SourceFormat.empty())
			return row.Format;

		return row.Format + " (" + row.SourceFormat + ")";
	}

	void TextureOverviewPanel::SetSearch(const std::string& text)
	{
		const size_t length = std::min(text.size(), sizeof(m_Search) - 1);
		text.copy(m_Search, length);
		m_Search[length] = '\0';
		m_Filter = ToLowercase(std::string(m_Search, length));
	}

	/* 顶部一行：搜索框吃左端余量，行尾清除与刷新两枚图标钮 */
	void TextureOverviewPanel::ShowToolbar()
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		const float row_width = ImGui::GetContentRegionAvail().x;
		const float button = ImGui::GetFrameHeight();
		const bool searching = m_Search[0] != '\0';

		const float refresh_x = ImMax(row_width - button, 0.0f);
		const float clear_x = refresh_x - style.ItemSpacing.x - button;
		const float search_right = searching ? clear_x : refresh_x;
		const float search_width = ImMax(search_right - style.ItemSpacing.x, 80.0f);

		Icons::BeginSearchInput();
		ImGui::SetNextItemWidth(search_width);
		if (ImGui::InputTextWithHint("##TextureSearch", "Search path or format...", m_Search,
			sizeof(m_Search)))
		{
			m_Filter = ToLowercase(m_Search);
		}
		Icons::EndSearchInput();

		if (searching)
		{
			ImGuiExt::SameLineAt(clear_x);
			if (Icons::IconButton(Icons::Id::Remove, ImVec2(button, button), false, "Clear search"))
			{
				m_Search[0] = '\0';
				m_Filter.clear();
			}
		}

		ImGuiExt::SameLineAt(refresh_x);
		if (Icons::IconButton(Icons::Id::Reset, ImVec2(button, button), false, "Scan the project again"))
			m_RefreshRequested = true;
	}

	void TextureOverviewPanel::ShowSummary()
	{
		const Stats stats = Sum(m_Rows, m_Filter);

		std::string summary;
		if (stats.Count == m_Rows.size())
		{
			summary = std::to_string(stats.Count) + " textures, " + FormatFileSize(stats.Bytes) + " total";
		}
		else
		{
			summary = std::to_string(stats.Count) + " of " + std::to_string(m_Rows.size())
				+ " textures, " + FormatFileSize(stats.Bytes) + " total";
		}

		if (m_Unreadable > 0)
			summary += ", " + std::to_string(m_Unreadable) + " unreadable";

		ImGui::TextDisabled("%s", summary.c_str());
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("Memory is what each file costs on the GPU:\n"
				"baked ASTC block data when the backend can take it as-is, decoded RGBA otherwise.\n"
				"Disk is the source file on disk");
		}

		ImGui::Separator();
	}

	void TextureOverviewPanel::ShowTable()
	{
		const ImGuiTableFlags flags = ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg
			| ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY;

		if (!ImGui::BeginTable("##Textures", 5, flags))
			return;

		ImGui::TableSetupColumn("Path",
			ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_PreferSortAscending);
		/* 格式列按最长的写法（"ASTC 00x00 (RGBA32F)"）定宽 */
		ImGui::TableSetupColumn("Format",
			ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortAscending,
			NumberColumnWidth("ASTC 00x00 (RGBA32F)"));
		ImGui::TableSetupColumn("Size",
			ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending,
			SizeColumnWidth());
		ImGui::TableSetupColumn("Disk",
			ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending,
			NumberColumnWidth("0000.0 MB"));
		/* 默认按显存倒序：打开就想先看到最贵的几张 */
		ImGui::TableSetupColumn("Memory",
			ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort
				| ImGuiTableColumnFlags_PreferSortDescending, NumberColumnWidth("0000.0 MB"));
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableHeadersRow();

		if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs())
		{
			if (specs->SpecsDirty && specs->SpecsCount > 0)
			{
				const ImGuiTableColumnSortSpecs& spec = specs->Specs[0];
				SortRows(m_Rows, spec.ColumnIndex, spec.SortDirection == ImGuiSortDirection_Ascending);
				specs->SpecsDirty = false;
			}
		}

		for (const Row& row : m_Rows)
		{
			/* 搜索词没命中就跳过：行序仍是整表排好的那份，筛出来还是有序的 */
			if (!Matches(row, m_Filter))
				continue;

			ImGui::TableNextRow();

			ImGui::TableNextColumn();
			const float path_width = ImGui::GetContentRegionAvail().x;
			const bool clipped = ImGui::CalcTextSize(row.Path.c_str()).x > path_width;
			PanelChrome::DrawClippedTextLine(row.Path.c_str(), path_width);
			if (clipped && ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", row.Path.c_str());

			ImGui::TableNextColumn();
			ImGui::TextUnformatted(FormatLabel(row).c_str());

			/* 尺寸后面跟一档 mip 级数：同一行说的是"这张图有多少像素、铺了多少级" */
			ImGui::TableNextColumn();
			ImGui::Text("%u x %u", row.Width, row.Height);
			ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
			ImGui::TextDisabled("%u mip%s", row.Mips, row.Mips > 1 ? "s" : "");

			ImGui::TableNextColumn();
			ImGui::TextUnformatted(FormatFileSize(row.FileBytes).c_str());

			ImGui::TableNextColumn();
			ImGui::TextUnformatted(FormatFileSize(row.Bytes).c_str());
		}

		ImGui::EndTable();
	}

	void TextureOverviewPanel::OnImGuiRender()
	{
		bool& visible = Panel::VisibleFlag(Panel::kTextureOverview);
		/* 重新打开时重扫一遍：烘焙、导入这类改动都落在磁盘上，摆着旧数字不如不摆 */
		const bool reopened = visible && !m_WasVisible;
		m_WasVisible = visible;

		/* SetNextWindowSize 只能在确定会提交时给，关着的窗口先退出去 */
		if (!visible)
			return;

		/* 首次出现给一个放得下五列的尺寸（拖过之后以 ini 记录为准） */
		ImGui::SetNextWindowSize(ImVec2(Scaled(880.0f), Scaled(420.0f)), ImGuiCond_FirstUseEver);

		PanelWindow window(Panel::kTextureOverview, &visible);
		if (!window)
			return;

		if (reopened || m_RefreshRequested)
		{
			m_RefreshRequested = false;
			Scan(g_ProjectPath, RenderAPI::GetAPI() == RenderAPI::Metal, m_Rows, m_Unreadable);
		}

		ShowToolbar();
		ShowSummary();

		if (m_Rows.empty())
		{
			ImGui::TextDisabled("No textures in this project");
			return;
		}

		if (Sum(m_Rows, m_Filter).Count == 0)
		{
			ImGui::TextDisabled("Nothing matches the current search");
			return;
		}

		ShowTable();
	}
}
