#pragma once
#include <cstdint>
#include <string>
#include "Helios/Scene/Components.h"

namespace Helios::MaterialSlotRow
{
	/* 材质槽的一行：槽名 | 实例标记 | 来源文件名（拖入挂载）| Reset | 清除。
	 * 只管画与收集意图，改数据由调用方按各自的槽语义处理（模型的槽有"回模型默认"，
	 * 草地只有一个槽，回的是内建外观）。模型卡与草地卡共用这一份版式。 */
	struct State
	{
		const char* Label{ "Material" };
		std::string Path;        /* 来源材质文件（绝对或相对都行，显示时只取文件名） */
		std::string EmptyText;   /* 没有来源文件时框里显示的字（如"内置默认"），空则留白 */
		std::string HoverPath;   /* 悬停提示里显示的路径（通常是相对路径），空则不显示这行 */
		bool IsInstance{ false };
		bool CanClear{ false };
		const char* EmptyHint{ "Drag a .mtl material asset here" };
		const char* ClearHint{ "Clear the binding" };
		const char* ResetHint{ "Re-clone from the source asset" };
	};

	enum class Action : uint8_t
	{
		None,
		Assign, /* 拖进来一个材质 */
		Clear,  /* 点 X */
		Reset,  /* 点 Reset：回到来源资产 */
	};

	struct Result
	{
		Action Act{ Action::None };
		SharedPtr<Material> Picked; /* Assign：拖入的材质 */
		std::string SourcePath;     /* Assign：它的绝对路径（落盘要用） */
	};

	/* 画一行并返回用户意图。IsInstance 且有来源路径时才出现 Reset */
	Result Draw(const State& state);
}
