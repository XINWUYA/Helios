#pragma once
#include <string>
#include <utility>
#include <vector>
#include <glm/glm.hpp>
#include "Helios/VirtualDevice/DeviceVertexArray.h"

namespace Helios
{
	/* 子模型信息：卡片的显示名 / 规模与悬停高亮用；材质走模型的槽表 */
	struct SubModelInfo
	{
		std::string Name;
		SharedPtr<DeviceVertexArray> VertexArray;
		std::pair<glm::vec3, glm::vec3> AABB;
	};

	/* 模型编辑器当前打开的模型信息（.mesh 是唯一入口形态：交换格式先经导入链转换） */
	class ModelInfo final
	{
	public:
		~ModelInfo() = default;

		/* 重置数据 */
		void Reset()
		{
			m_SubModelInfos.clear();
			m_Path.clear();
		}

	private:
		/* 所在路径 */
		std::string m_Path{};
		/* 模型包含的子模型数据 */
		std::vector<SharedPtr<SubModelInfo>> m_SubModelInfos;
		/* 整体的AABB */
		std::pair<glm::vec3, glm::vec3> m_AABB;

		friend class ModelEditorLayer;
	};
}
