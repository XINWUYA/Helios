#pragma once
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>
#include <spdlog/common.h>
#include "LogBuffer.h"

namespace Helios
{
	/* 「Log」面板：引擎和编辑器两个 logger 的日志窗口。数据源 = LogBuffer（捕获 sink 填的环形
	 * 缓冲），每帧增量取新：工具行（级别过滤 / 搜索 / 跟随 / 计数）+ 虚拟化列表（悬停提示、
	 * 双击复制、单击 / Shift / Ctrl 多选、点空白清空）。选择与资源浏览器同一套语言：
	 * Ctrl/Cmd+A 全选、Ctrl/Cmd+C 复制选中、Esc 清空；跟随钉在尾部、上滚就暂停。
	 * 窗口隐藏时不提交 Begin。 */
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
		/* 级别条数（搜索过滤后、级别显隐前；Critical 并入 Error 槽）——工具行芯片右侧数字 */
		[[nodiscard]] uint32_t GetLevelCount(spdlog::level::level_enum level) const;
		/* 选择：单击行 = 单选、Shift = 范围选、Ctrl/Cmd = 加选、点空白 = 清空；配合快捷键
		 * Ctrl/Cmd+A 全选、Ctrl/Cmd+C 复制选中、Esc 清空（仅列表悬停且非输入态）；
		 * 以下入口与鼠标 / 快捷键走同一路径（自检 / 自动化验证用） */
		void SelectAllVisible();
		void SelectSingle(uint64_t seq);
		void ToggleSelection(uint64_t seq);
		/* 从锚点到 seq 的整段可见行（锚点不在当前列表里时退化为单选） */
		void SelectRangeTo(uint64_t seq);
		void ClearSelection();
		/* 选中且当前可见的行数 */
		[[nodiscard]] uint32_t GetSelectedCount() const;
		/* 选中的可见行按显示顺序拼成多行文本（= 复制到剪贴板的产物） */
		[[nodiscard]] std::string BuildSelectedText() const;
		/* 跟随状态：开 = 新条目到来时钉在尾部；手动上滚自动暂停、滚回底部恢复 */
		[[nodiscard]] bool IsFollowEnabled() const { return m_Follow; }
		/* 上一帧的条目数（过滤后 / 缓存总数） */
		[[nodiscard]] uint32_t GetVisibleCount() const { return static_cast<uint32_t>(m_VisibleEntries.size()); }
		[[nodiscard]] uint32_t GetEntryCount() const { return static_cast<uint32_t>(m_Entries.size()); }

	private:
		/* 从 LogBuffer 增量取新（环形覆盖 / 清空时整体重建缓存） */
		void SyncEntries();
		/* 按搜索词 + 级别显隐重建过滤结果（行索引表），并统计级别条数 */
		void RebuildVisibleEntries();
		/* 工具行：级别过滤（带条数）+ 搜索 + 跟随 + 计数 */
		void ShowToolbar();
		/* 条目列表（ListClipper 虚拟化；悬停提示 + 双击复制 + 跟随滚动） */
		void ShowEntries();
		/* 列表快捷键：Ctrl/Cmd+A 全选、Ctrl/Cmd+C 复制、Esc 清空（hover + 非输入态才响应） */
		void HandleListShortcuts();
		/* 行点击的三分支：Shift 范围 / Ctrl 加选 / 否则单选 */
		void ApplyClickSelection(uint64_t seq);
		/* 行内容：时间 / 级别 / 来源 / 正文；返回本行是否被点击 */
		bool DrawEntryRow(const LogBuffer::Record& record, float row_height);

		bool PassesSearchFilter(const LogBuffer::Record& record) const;

		/* 显示缓存（按序号增量追加；随环形容量截断） */
		std::vector<LogBuffer::Record> m_Entries;
		uint64_t m_LastSeq{ 0 };
		/* 级别显隐（索引 = spdlog::level::level_enum） */
		bool m_LevelVisible[6] = { true, true, true, true, true, true };
		/* 级别条数（槽 0..4 = trace..err；critical 并进 err；每帧随过滤重建） */
		uint32_t m_LevelCounts[5] = { 0, 0, 0, 0, 0 };
		/* 搜索：驻留缓冲（输入框直接编辑）+ 小写化过滤词 */
		char m_SearchBuffer[128] = {};
		std::string m_SearchFilter;
		/* 跟随尾部（新条目到来时钉在底部） */
		bool m_Follow{ true };
		/* 过滤结果：m_Entries 的行索引（每帧重建） */
		std::vector<uint32_t> m_VisibleEntries;
		/* 选中集合（按 Seq 记：行索引随过滤 / 环形滚动不稳）；入口 = 行点击（单选 / 加选 /
		 * 范围）与全选 */
		std::unordered_set<uint64_t> m_SelectedSeqs;
		/* 选择锚点（Shift 范围的起点）；Has 位区分空——Seq 从 0 起，别拿 0 当"无" */
		uint64_t m_SelectionAnchor{ 0 };
		bool m_HasSelectionAnchor{ false };
		/* 行内单行化文本的复用缓冲（每帧只给可见行用，免去逐行分配） */
		std::string m_RowText;
	};
}
