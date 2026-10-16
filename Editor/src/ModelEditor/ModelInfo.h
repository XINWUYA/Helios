#pragma once
#include <string>
#include <utility>
#include <vector>
#include <glm/glm.hpp>
#include "Helios/VirtualDevice/DeviceVertexArray.h"

namespace Helios
{
	/* 模型材质的载体：只留槽名（导出槽表、加载后按名对位用）。
	 * 具体参数不放这里 —— 编辑期改的是 MaterialGroup 里那份活材质（按着色器反射出参数行）。 */
	struct MaterialParams
	{
		std::string Name;
	};

	/* 子模型信息 */
	struct SubModelInfo
	{
		std::string Name;
		SharedPtr<DeviceVertexArray> VertexArray;
		std::vector<float> VertexData; /* 顶点数据的原始信息，保存模型时需要 */
		MaterialParams MaterialParams;
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
