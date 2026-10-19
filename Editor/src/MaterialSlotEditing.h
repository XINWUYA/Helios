#pragma once

#include "Helios/Scene/Model.h"
#include "Helios/Scene/Components.h"
#include <string>
#include <unordered_map>

namespace Helios
{
	/* 槽材质的编辑语义（属性面板与模型编辑器共用）：
	 * 有效材质 = 实体覆盖 ?: 模型清单默认 ?: 白模；
	 * 改参数自动实例化（克隆来源），不会写到共享 .mtl 上。 */
	namespace MaterialSlotEditing
	{
		[[nodiscard]] SharedPtr<Material> Resolve(const Model& model,
			const std::unordered_map<std::string, MaterialSlotOverride>& overrides,
			const std::string& slot_name);

		/* 有效材质的来源文件（覆盖来源 ?: 模型默认），空 = 内嵌 / 白模 */
		[[nodiscard]] std::string SourcePath(const Model& model,
			const std::unordered_map<std::string, MaterialSlotOverride>& overrides,
			const std::string& slot_name);

		/* 取可编辑的实例：已经是实例就原样返回，否则从当前有效材质克隆 */
		[[nodiscard]] SharedPtr<Material> MakeInstance(const Model& model,
			std::unordered_map<std::string, MaterialSlotOverride>& overrides,
			const std::string& slot_name, bool& changed);

		void WriteParam(const Model& model,
			std::unordered_map<std::string, MaterialSlotOverride>& overrides,
			const std::string& slot_name, ParamType type, const std::string& name,
			const std::any& value, bool& changed);

		void WriteRasterState(const Model& model,
			std::unordered_map<std::string, MaterialSlotOverride>& overrides,
			const std::string& slot_name, const RenderRasterState& state, bool& changed);
	}
}
