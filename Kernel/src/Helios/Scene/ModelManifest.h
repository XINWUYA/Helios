#pragma once
#include <string>
#include <vector>
#include "Material.h"

namespace Helios
{
	/* .model 清单的读写：几何 payload 路径 + 材质槽表。
	 * 运行时加载（Model::Create）、导入链（ModelImporter）、模型编辑器导出三处共用
	 * 这一份格式定义，改结构只动这里。 */
	namespace ModelManifest
	{
		/* 一条槽：要么引用一份 .mtl（可叠参数覆盖），要么内嵌完整定义 */
		struct Slot
		{
			std::string Name;
			std::string AssetPath;           /* 引用的材质文件（空 = 内嵌定义） */
			SharedPtr<Material> pEmbedded;   /* 内嵌定义（AssetPath 为空时有效） */
			SharedPtr<Material> pOverride;   /* 引用之上的参数覆盖（空 = 纯引用） */
			bool HasRasterOverride{ false }; /* 覆盖里是否带 RasterState */
		};

		/* 读清单：几何路径归一成绝对路径，槽表保持文件里的顺序。
		 * 文件打不开 / 结构不对返回 false。 */
		bool Read(const std::string& path, std::string& out_geometry, std::vector<Slot>& out_slots);

		/* 写清单：几何路径按资产根写成相对形式，目录不存在时自动创建。 */
		bool Write(const std::string& path, const std::string& geometry_path, const std::vector<Slot>& slots);
	}
}
