#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Helios
{
	/* 「Texture Overview」面板：把工程里的贴图收成一张表 —— 工程相对路径、格式、
	 * 像素尺寸、源文件占用与上传后的显存。每列都能点表头排序，搜索框按路径或格式筛。
	 * 数据在打开面板和点刷新时各读一遍磁盘（每张只读文件头，不解码像素）。 */
	class TextureOverviewPanel
	{
	public:
		void OnImGuiRender();

		/* 一行 = 一张贴图（纯数据，headless 可断言） */
		struct Row
		{
			std::string Path;          /* 相对扫描根的路径 */
			std::string Format;        /* "ASTC 6x6" / "RGBA8" / "RGBA32F" */
			std::string SourceFormat;  /* 源图解码后的像素格式：RGBA8 / RGBA32F */
			uint32_t Width{ 0 };
			uint32_t Height{ 0 };
			uint64_t Bytes{ 0 };       /* 上传到 GPU 后的字节数 */
			uint64_t FileBytes{ 0 };   /* 源文件在磁盘上的字节数 */
			uint8_t Mips{ 1 };
			bool Baked{ false };       /* 显存按同目录 .astc 的块数据算 */
		};

		/* 表列序：SortRows 的 column 参数和表头列一一对应 */
		enum Column : int
		{
			kColumnPath = 0,
			kColumnFormat,
			kColumnSize,
			kColumnDisk,
			kColumnMemory,
		};

		/* 表头合计：命中的张数与它们的显存加总 */
		struct Stats
		{
			size_t Count{ 0 };
			uint64_t Bytes{ 0 };
		};

		/* 命中搜索词的那些行的合计；搜索词须已小写化，空串 = 全部（纯函数，headless 可断言） */
		static Stats Sum(const std::vector<Row>& rows, const std::string& lowercase_needle);

		/* 扫 root 下的贴图（Cache 目录与 .astc 产物跳过），逐张量尺寸与显存。
		 * astc_upload = 后端能不能直传块数据（只有 Metal 能）。
		 * unreadable 返回读不出文件头的张数 —— 它们不进 rows。 */
		static void Scan(const std::filesystem::path& root, bool astc_upload,
			std::vector<Row>& out_rows, size_t& out_unreadable);

		/* 解码路径的 mip 链字节数：每级向下取整、最小 1 像素 */
		static uint64_t ImageBytes(uint32_t width, uint32_t height, uint32_t bytes_per_pixel, bool mips);

		/* 完整 mip 级数（含 1x1），与后端建纹理时的算法一致 */
		static uint8_t MipCount(uint32_t width, uint32_t height);

		/* 按列排序；同值按路径收尾，行序帧间稳定 */
		static void SortRows(std::vector<Row>& rows, int column, bool ascending);

		/* 表里显示的格式串：按块数据算的行在后面补上源图的像素格式，
		 * 如 "ASTC 6x6 (RGBA8)" —— 括号里是同一张图不烘焙时会用的格式 */
		static std::string FormatLabel(const Row& row);

		/* 块尺寸能不能直传：4x4 / 5x5 / 6x6 / 8x8，别的块尺寸后端不认，会退回解码 */
		static bool IsUploadableBlockSize(int block_x, int block_y);

		/* 表里现在有哪些行（自检用只读） */
		[[nodiscard]] const std::vector<Row>& GetRows() const { return m_Rows; }

		/* 写搜索词，等价于在框里输入（自检用） */
		void SetSearch(const std::string& text);

	private:
		/* 顶部一行：搜索框与清除 / 刷新钮 */
		void ShowToolbar();
		/* 命中合计：几张、多少显存 */
		void ShowSummary();
		void ShowTable();

		std::vector<Row> m_Rows;
		size_t m_Unreadable{ 0 };
		bool m_RefreshRequested{ false };
		/* 上一帧面板开没开：重新打开时自动扫一遍 */
		bool m_WasVisible{ false };
		/* 搜索词：原样留在输入框里，小写副本给过滤用 */
		char m_Search[128]{};
		std::string m_Filter;
	};
}
