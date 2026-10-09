#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <spdlog/common.h>
#include "LogBuffer.h"

namespace Helios
{
	/* 「Log」面板：引擎和编辑器两个 logger 的日志窗口。数据源 = LogBuffer（捕获 sink 填的环形
	 * 缓冲），每帧增量取新：工具行（级别过滤 / 搜索 / 跟随 / 计数）+ 虚拟化列表（悬停提示、双击
	 * 复制）；跟随钉在尾部、上滚就暂停。窗口隐藏时不提交 Begin。 */
	class LogPanel final
	{
	public:
		/* 画整个面板（含停靠窗口的 Begin/End）；visible 与窗口右上角关闭按钮同源 */
		void OnImGuiRender(bool& visible);

		/* ---- 程序化入口（自检 / 自动化验证用；与工具行控件同一状态） ---- */
		void SetSearchFilter(const std::string& filter);
		[[nodiscard]] const std::string& GetSearchFilter() const { return m_SearchFilter; }
		void SetLevelVisible(spdlog::level::level_enum level, bool visible);
		[[nodiscard]] bool IsLevelVisible(spdlog::level::level_enum level) const;
		/* 跟随状态：开 = 新条目到来时钉在尾部；手动上滚自动暂停、滚回底部恢复 */
		[[nodiscard]] bool IsFollowEnabled() const { return m_Follow; }
		/* 上一帧的条目数（过滤后 / 缓存总数） */
		[[nodiscard]] uint32_t GetVisibleCount() const { return static_cast<uint32_t>(m_VisibleEntries.size()); }
		[[nodiscard]] uint32_t GetEntryCount() const { return static_cast<uint32_t>(m_Entries.size()); }

	private:
		/* 从 LogBuffer 增量取新（环形覆盖 / 清空时整体重建缓存） */
		void SyncEntries();
		/* 按级别显隐 + 搜索词重建过滤结果（行索引表） */
		void RebuildVisibleEntries();
		/* 工具行：级别过滤 + 搜索 + 跟随 + 计数 */
		void ShowToolbar();
		/* 条目列表（ListClipper 虚拟化；悬停提示 + 双击复制 + 跟随滚动） */
		void ShowEntries();
		/* 行内容：时间 / 级别 / 来源 / 正文（正文超宽省略号收尾） */
		void DrawEntryRow(const LogBuffer::Record& record, float row_height);

		bool PassesFilter(const LogBuffer::Record& record) const;

		/* 显示缓存（按序号增量追加；随环形容量截断） */
		std::vector<LogBuffer::Record> m_Entries;
		uint64_t m_LastSeq{ 0 };
		/* 级别显隐（索引 = spdlog::level::level_enum） */
		bool m_LevelVisible[6] = { true, true, true, true, true, true };
		/* 搜索：驻留缓冲（输入框直接编辑）+ 小写化过滤词 */
		char m_SearchBuffer[128] = {};
		std::string m_SearchFilter;
		/* 跟随尾部（新条目到来时钉在底部） */
		bool m_Follow{ true };
		/* 过滤结果：m_Entries 的行索引（每帧重建） */
		std::vector<uint32_t> m_VisibleEntries;
		/* 行内单行化文本的复用缓冲（每帧只给可见行用，免去逐行分配） */
		std::string m_RowText;
	};
}
