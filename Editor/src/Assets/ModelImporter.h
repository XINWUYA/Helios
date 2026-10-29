#pragma once
#include "Helios/Common/Export.h"
#include <string>

namespace Helios
{
	/* 导入模型：按后缀分派到对应解析器 ——
	 *   obj                走内置解析（tinyobjloader）
	 *   fbx / gltf / glb / dae / stl   走 assimp
	 * 统一把模型与它引用的贴图收进目标目录（已经在该目录里就只做转换），再写出
	 * .mesh 与 .mtl（格式与 Model::Create 的读取一一对应；.mtl 是槽表，没有材质就不写）。
	 * 返回生成的 .mesh 文件路径；失败返回空串。 */
	HELIOS_API std::string ImportModel(const std::string& filepath, const std::string& dest_dir);

	/* 导入 Obj 模型（ImportModel 的 obj 分支，也供检查直接调用），说明同 ImportModel。 */
	std::string ImportObj(const std::string& filepath, const std::string& dest_dir);
}
