#pragma once
#include "Helios/Scene/Components.h"
#include "Helios/Scene/GrassField.h"
#include "Helios/Scene/Material.h"

namespace Helios::GrassDraw
{
	/* 草地提交的逐绘制参数：风场（每片各不相同）、脚印图与覆盖范围。
	 * 可见着色与阴影投影共用这一份，两边吃到同一阵风、同一个脚印，
	 * 影子才跟得上草的动作。 */
	inline DrawParams MakeDrawParams(const GrassComponent& grass)
	{
		DrawParams per_draw;
		per_draw.FillMissing = MaterialFill::Values | MaterialFill::Textures;
		/* 风相位由场景逐片累加，速度的影响已经积进去了，这里只传相位 */
		per_draw.Overrides.emplace_back(ParamType::Float, "u_WindPhase", grass.m_WindPhase);
		per_draw.Overrides.emplace_back(ParamType::Vec2, "u_WindDirection", grass.m_WindDirection);
		per_draw.Overrides.emplace_back(ParamType::Float, "u_WindStrength", grass.m_WindStrength);
		per_draw.Overrides.emplace_back(ParamType::Float, "u_WindSpatialScale", grass.m_WindSpatialScale);
		/* 画质档的全局密度系数：可见着色与草影共用一份，两边稀稠一致 */
		per_draw.Overrides.emplace_back(ParamType::Float, "u_BladeDensityScale",
			GrassField::GetGlobalDensityScale());

		if (grass.m_Field != nullptr && grass.m_Field->HasTrample())
		{
			per_draw.Overrides.push_back(Material::MakeTextureParam("u_TrampleMap",
				grass.m_Field->GetTrampleTexture()));
			per_draw.Overrides.emplace_back(ParamType::Vec4, "u_TrampleRect",
				grass.m_Field->GetTrampleRect());
		}
		return per_draw;
	}
}
