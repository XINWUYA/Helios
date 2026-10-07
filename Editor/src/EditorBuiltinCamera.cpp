#include "Pch.h"
#include "EditorBuiltinCamera.h"
#include <glm/gtx/quaternion.hpp>
#include <cmath>

#include "Helios/Application/Application.h"
#include "Helios/Common/Math.h"
#include "Helios/ImGui/ImGuiLayer.h"
#include "Helios/Scene/Material.h"
#include "Helios/Scene/ShadowMap.h"

namespace Helios
{
	namespace
	{
		SharedPtr<Material> CreateGBufferMaterial(
			const SharedPtr<Material>& source, const SharedPtr<DeviceShader>& gbuffer_shader)
		{
			auto material = Material::Create(gbuffer_shader);
			for (const auto& [_, parameter] : source->GetAllParameters())
			{
				if (parameter.Type == ParamType::Texture)
				{
					if (gbuffer_shader->GetUniformBinding(parameter.Name) < 0)
						continue;
					const auto texture = std::any_cast<std::pair<SharedPtr<DeviceTexture>, uint32_t>>(parameter.Value);
					material->SetTexture(parameter.Name, texture.first);
				}
				else
				{
					material->SetParameters(parameter.Type, parameter.Name, parameter.Value);
				}
			}
			material->SetRasterState(source->GetRasterState());
			return material;
		}

		/* === 编辑器场景 gizmo：世界空间的地面网格 + 坐标轴 ===
		 * 网格 1 格 = 1 个世界单位刻度，每 10 格一条主线；patch 半径与着色器的
		 * 远景淡出终点一致 —— 淡到 0 恰好落在 patch 边缘，不出现硬切边界。 */
		constexpr float kGridCellSize = 1.0f;
		constexpr int32_t kGridHalfCells = 40;		/* patch 半径（格数） */
		constexpr int32_t kGridMajorEvery = 10;		/* 每多少格一条主线 */
		constexpr float kGridLineExtent =
			static_cast<float>(kGridHalfCells + kGridMajorEvery) * kGridCellSize;	/* 每条线的半长 */
		constexpr float kGridFadeInner = 15.0f;		/* 淡出起点（到相机的水平距离） */
		constexpr float kGridFadeOuter = static_cast<float>(kGridHalfCells) * kGridCellSize;

		/* 坐标轴：半长 1 个世界单位，负端压暗、正端满色（正方向靠亮度区分；
		 * 不放箭头 —— 会和操作的拖拽 gizmo 混淆） */
		constexpr float kAxisHalfLength = 1.0f;
		constexpr float kAxisDimScale = 0.4f;
		/* 轴带宽度（逻辑像素）：执行期按内容缩放折为物理像素，交给着色器做屏幕空间展开 */
		constexpr float kAxisLineWidth = 2.0f;

		/* 颜色：RGBA8 直写（编辑器管线不做 gamma 变换）。网格线的配色不在这里 —— 线身份
		 * 由 Axis.glsl 按世界坐标逐片元分类；这里的顶点色只是兜底。 */
		const glm::vec3 kGridVertexFallbackColor{ 0.16f, 0.16f, 0.17f };
		const glm::vec3 kAxisColors[3] = {
			{ 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } };

		/* === 实体类型 gizmo（光源 / 相机 / 反射探针 / 精灵）===
		 * 两种尺寸语义：参数驱动（点光 / 聚光 / 相机 = 真世界尺度，不吃实体缩放）和
		 * 图标驱动（屏上尺寸随距离收缩；平行光不响应缩放、探针和精灵按轴完整响应）。 */
		const glm::vec3 kLightGizmoColor{ 1.00f, 0.84f, 0.42f };	/* 暖黄：光源 */
		const glm::vec3 kCameraGizmoColor{ 0.66f, 0.82f, 1.00f };	/* 淡蓝：相机 */
		const glm::vec3 kProbeGizmoColor{ 0.45f, 0.90f, 0.78f };	/* 青绿：反射探针 */
		const glm::vec3 kSpriteGizmoColor{ 0.95f, 0.62f, 0.85f };	/* 粉紫：精灵 */

		constexpr int32_t kGizmoCircleSegments = 16;		/* 线框圆段数 */
		constexpr int32_t kGizmoDotSegments = 8;			/* 轴心点小圆段数 */
		constexpr float kGizmoDotRadius = 0.045f;			/* 轴心点半径（各类型共用） */

		/* 平行光（太阳）：圆环 + 8 条长短交替射线 + 方向杆 */
		constexpr float kSunRingRadius = 0.34f;
		constexpr float kSunRayLong = 0.58f;
		constexpr float kSunRayShort = 0.47f;
		constexpr float kSunRodLength = 1.25f;

		/* 点光：球体线框；反射探针：方盒（Unity 式） */
		constexpr float kPointGizmoRadius = 0.5f;
		constexpr float kProbeBoxHalfExtent = 0.5f;

		/* 聚光：锥体（轴心点 + 中段圆 + 底圆 + 四条母线） */
		constexpr float kSpotGizmoLength = 1.25f;
		/* 规范锥角 30°（弧度）：几何一次构建，实际锥角由绘制时的"形状缩放"张开
		 * 到光源的真实外锥角（与相机视锥的 FOV 处理同范式） */
		constexpr float kSpotGizmoAngle = 0.5236f;

		/* 相机：视锥（近 / 远矩形 + 棱 + 上向标记）+ 成像背盒。视锥按规范形状（45° / 16:9）建，
		 * 绘制时乘"真实量 / 规范量"因子张开到真实的 near / far 与 FOV / 宽高比。
		 * 注意：视锥长度 = far，编辑器相机的远裁剪面要给得足够大（见 EditorCamera）。 */
		constexpr float kCameraGizmoNear = 0.5f;	/* 缺省近平面显示位置（无相机组件时兜底） */
		constexpr float kCameraGizmoFar = 1.5f;		/* 规范形状的显示总长（远平面所在的 z） */
		constexpr float kCameraGizmoMarkerHeight = 0.24f;	/* 远平面上向标记高度 */
		constexpr float kCameraGizmoBodyNear = 0.08f;		/* 机身盒沿 +Z 的范围 */
		constexpr float kCameraGizmoBodyFar = 0.5f;
		constexpr float kCameraGizmoBodyHalfW = 0.2f;
		constexpr float kCameraGizmoBodyHalfH = 0.14f;
		constexpr float kCameraGizmoDefaultTanHalfFov = 0.4142f;	/* tan(45° / 2)：规范形状的张开 */
		constexpr float kCameraGizmoDefaultAspect = 1.778f;		/* 规范形状的宽高比（16:9） */
		/* 显示长度的对数基准：远平面 = 该值时显示长度 = 1 倍基准（取引擎默认 Far） */
		constexpr float kCameraGizmoFarRef = 1000.0f;

		/* 精灵：框 + 对角线（图像占位样式） */
		constexpr float kSpriteGizmoHalfExtent = 0.5f;

		/* 每类的标称屏幕尺寸（逻辑像素；绘制时 × 内容缩放折成物理像素） */
		constexpr float kLightGizmoPixelSize = 56.0f;
		constexpr float kProbeGizmoPixelSize = 60.0f;
		constexpr float kCameraGizmoPixelSize = 96.0f;
		constexpr float kSpriteGizmoPixelSize = 68.0f;

		/* 实体 gizmo 随距离收缩的三档：参考距离（默认视距，此处与标称同大小）与
		 * 屏上比例的下限 / 上限（拉远收缩保底、拉近放大封顶） */
		constexpr float kGizmoRefDistance = 10.0f;
		constexpr float kGizmoMinPixelScale = 0.2f;
		constexpr float kGizmoMaxPixelScale = 1.6f;

		/* direction / side 只对轴带有意义（网格用缺省 0：不展开、不羽化） */
		void PushVertex(std::vector<float>& vertices, const glm::vec3& position, const glm::vec3& color,
			const glm::vec3& direction = glm::vec3(0.0f), float side = 0.0f)
		{
			vertices.insert(vertices.end(),
				{ position.x, position.y, position.z, color.r, color.g, color.b,
					direction.x, direction.y, direction.z, side });
		}

		void PushLine(std::vector<float>& vertices, const glm::vec3& from, const glm::vec3& to,
			const glm::vec3& color, const glm::vec3& direction = glm::vec3(0.0f))
		{
			PushVertex(vertices, from, color, direction);
			PushVertex(vertices, to, color, direction);
		}

		/* 线段带：与坐标轴同一套“屏幕空间等宽”构造（方向 + ±side 六个顶点，两个三角形），
		 * 实体 gizmo 的线框全部由它拼出 —— 线宽恒定、边缘羽化 */
		void PushLineBand(std::vector<float>& vertices, const glm::vec3& from, const glm::vec3& to,
			const glm::vec3& color)
		{
			const glm::vec3 delta = to - from;
			const float length = glm::length(delta);
			if (length <= 0.0f)
				return;
			const glm::vec3 direction = delta / length;

			PushVertex(vertices, from, color, direction, -1.0f);
			PushVertex(vertices, from, color, direction, 1.0f);
			PushVertex(vertices, to, color, direction, -1.0f);

			PushVertex(vertices, to, color, direction, -1.0f);
			PushVertex(vertices, from, color, direction, 1.0f);
			PushVertex(vertices, to, color, direction, 1.0f);
		}

		/* 圆环带：在 axis_u / axis_v 张成的平面内，用线段带逐段拼圆 */
		void PushCircleBand(std::vector<float>& vertices, const glm::vec3& center,
			const glm::vec3& axis_u, const glm::vec3& axis_v, float radius, int32_t segments,
			const glm::vec3& color)
		{
			for (int32_t segment = 0; segment < segments; ++segment)
			{
				const float angle_from = 2.0f * PI * static_cast<float>(segment) / static_cast<float>(segments);
				const float angle_to = 2.0f * PI * static_cast<float>(segment + 1) / static_cast<float>(segments);
				PushLineBand(vertices,
					center + (axis_u * std::cos(angle_from) + axis_v * std::sin(angle_from)) * radius,
					center + (axis_u * std::cos(angle_to) + axis_v * std::sin(angle_to)) * radius,
					color);
			}
		}

		/* 方盒线框（12 棱）：相机机身 / 反射探针共用 */
		void PushBoxBand(std::vector<float>& vertices, const glm::vec3& center,
			const glm::vec3& half_extent, const glm::vec3& color)
		{
			glm::vec3 corners[8];
			for (int32_t corner = 0; corner < 8; ++corner)
			{
				corners[corner] = center + glm::vec3(
					(corner & 1) ? half_extent.x : -half_extent.x,
					(corner & 2) ? half_extent.y : -half_extent.y,
					(corner & 4) ? half_extent.z : -half_extent.z);
			}

			for (int32_t corner = 0; corner < 8; ++corner)
			{
				for (int32_t axis = 0; axis < 3; ++axis)
				{
					const int32_t other = corner ^ (1 << axis);
					if (other > corner)	/* 每条棱只画一次 */
						PushLineBand(vertices, corners[corner], corners[other], color);
				}
			}
		}

		/* 轴心点：各类型共用的中心小圆（标记实体原点；三个正交小圆，任意角度看都是圆点） */
		void PushPivotDot(std::vector<float>& vertices, const glm::vec3& center, const glm::vec3& color)
		{
			PushCircleBand(vertices, center, glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f),
				kGizmoDotRadius, kGizmoDotSegments, color);
			PushCircleBand(vertices, center, glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f),
				kGizmoDotRadius, kGizmoDotSegments, color);
			PushCircleBand(vertices, center, glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(1.0f, 0.0f, 0.0f),
				kGizmoDotRadius, kGizmoDotSegments, color);
		}

		/* 地面网格：patch 覆盖 [-R, R]² 的整数线，每条线带线方向（a_Dir）—— 线身份（细线 /
		 * 主线 / 轴中线）由 Axis.glsl 按世界坐标逐片元分类。别按 patch 局部下标判身份：
		 * patch 中心 = round(相机.xz)，按下标判会让亮线跟着视角跳格。 */
		std::vector<float> BuildGridVertices()
		{
			std::vector<float> vertices;
			vertices.reserve(static_cast<size_t>(2 * (2 * kGridHalfCells + 1) * 2 * 10));

			const float extent = kGridLineExtent;
			const glm::vec3 dir_x(1.0f, 0.0f, 0.0f);
			const glm::vec3 dir_z(0.0f, 0.0f, 1.0f);

			for (int32_t index = -kGridHalfCells; index <= kGridHalfCells; ++index)
			{
				const float coord = static_cast<float>(index) * kGridCellSize;

				/* 沿 X 的线（z = coord）与沿 Z 的线（x = coord） */
				PushLine(vertices, { -extent, 0.0f, coord }, { extent, 0.0f, coord },
					kGridVertexFallbackColor, dir_x);
				PushLine(vertices, { coord, 0.0f, -extent }, { coord, 0.0f, extent },
					kGridVertexFallbackColor, dir_z);
			}
			return vertices;
		}

		/* 坐标轴：屏幕空间等宽的"轴带"—— 三维里零宽（两端只差 ±side），宽度由着色器按像素展开、
		 * 边缘 1px 羽化；负端压暗靠顶点色插值。 */
		std::vector<float> BuildAxisVertices()
		{
			std::vector<float> vertices;
			vertices.reserve(3 * 6 * 10);

			for (int32_t axis = 0; axis < 3; ++axis)
			{
				const glm::vec3 direction = (axis == 0) ? glm::vec3(1.0f, 0.0f, 0.0f)
					: (axis == 1) ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(0.0f, 0.0f, 1.0f);
				const glm::vec3 negative_end = -direction * kAxisHalfLength;
				const glm::vec3 positive_end = direction * kAxisHalfLength;
				const glm::vec3 dim_color = kAxisColors[axis] * kAxisDimScale;
				const glm::vec3 full_color = kAxisColors[axis];

				/* 两个三角形：(负-1, 负+1, 正-1) 与 (正-1, 负+1, 正+1)；面剔除已关，绕序无所谓 */
				PushVertex(vertices, negative_end, dim_color, direction, -1.0f);
				PushVertex(vertices, negative_end, dim_color, direction, 1.0f);
				PushVertex(vertices, positive_end, full_color, direction, -1.0f);

				PushVertex(vertices, positive_end, full_color, direction, -1.0f);
				PushVertex(vertices, negative_end, dim_color, direction, 1.0f);
				PushVertex(vertices, positive_end, full_color, direction, 1.0f);
			}
			return vertices;
		}

		/* === 实体类型 gizmo 的几何（本地空间，约 1 单位大小；绘制时乘随距离收缩的尺寸矩阵）=== */

		/* 平行光：太阳式图标（圆环 + 8 条长短交替射线 + 方向杆 + 轴心点）。
		 * 几何按“光向 = -Y”的规范朝向建，绘制时旋到实际光向（见 DrawEntityGizmos） */
		std::vector<float> BuildDirectionalLightGizmoVertices()
		{
			std::vector<float> vertices;
			const glm::vec3 axis_u(1.0f, 0.0f, 0.0f);
			const glm::vec3 axis_v(0.0f, 0.0f, 1.0f);	/* 与 -Y 正交的平面基 */

			PushCircleBand(vertices, glm::vec3(0.0f), axis_u, axis_v, kSunRingRadius, kGizmoCircleSegments,
				kLightGizmoColor);

			/* 射线：正十字（0° / 90° / 180° / 270°）长、斜向短 */
			constexpr int32_t kRayCount = 8;
			for (int32_t ray = 0; ray < kRayCount; ++ray)
			{
				const float angle = 2.0f * PI * static_cast<float>(ray) / static_cast<float>(kRayCount);
				const float outer = (ray % 2 == 0) ? kSunRayLong : kSunRayShort;
				const glm::vec3 direction = axis_u * std::cos(angle) + axis_v * std::sin(angle);
				PushLineBand(vertices, direction * kSunRingRadius, direction * outer, kLightGizmoColor);
			}

			/* 方向杆：沿光向（-Y）—— 表示光传播方向 */
			PushLineBand(vertices, glm::vec3(0.0f),
				glm::vec3(0.0f, -kSunRodLength, 0.0f), kLightGizmoColor);
			PushPivotDot(vertices, glm::vec3(0.0f), kLightGizmoColor);
			return vertices;
		}

		/* 点光：三正交圆环（球体线框）+ 轴心点 */
		std::vector<float> BuildPointLightGizmoVertices()
		{
			std::vector<float> vertices;
			PushCircleBand(vertices, glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f),
				kPointGizmoRadius, kGizmoCircleSegments, kLightGizmoColor);
			PushCircleBand(vertices, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f),
				kPointGizmoRadius, kGizmoCircleSegments, kLightGizmoColor);
			PushCircleBand(vertices, glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(1.0f, 0.0f, 0.0f),
				kPointGizmoRadius, kGizmoCircleSegments, kLightGizmoColor);
			PushPivotDot(vertices, glm::vec3(0.0f), kLightGizmoColor);
			return vertices;
		}

		/* 聚光：锥体线框（轴心点 + 中段圆 + 底圆 + 四条母线；轴向本地 -Z =
		 * 与光传播方向同源；规范锥角 30°，真实锥角经绘制时的形状缩放张开） */
		std::vector<float> BuildSpotLightGizmoVertices()
		{
			std::vector<float> vertices;
			const glm::vec3 axis(0.0f, 0.0f, -1.0f);
			const glm::vec3 axis_u(1.0f, 0.0f, 0.0f);
			const glm::vec3 axis_v(0.0f, 1.0f, 0.0f);
			const glm::vec3 base_center = axis * kSpotGizmoLength;
			const float base_radius = kSpotGizmoLength * std::tan(kSpotGizmoAngle * 0.5f);

			PushCircleBand(vertices, base_center, axis_u, axis_v, base_radius, kGizmoCircleSegments, kLightGizmoColor);
			PushCircleBand(vertices, axis * (kSpotGizmoLength * 0.5f), axis_u, axis_v, base_radius * 0.5f,
				kGizmoCircleSegments, kLightGizmoColor);

			for (int32_t side = 0; side < 4; ++side)
			{
				const float angle = 0.5f * PI * static_cast<float>(side);
				const glm::vec3 rim = base_center
					+ (axis_u * std::cos(angle) + axis_v * std::sin(angle)) * base_radius;
				PushLineBand(vertices, glm::vec3(0.0f), rim, kLightGizmoColor);
			}
			PushPivotDot(vertices, glm::vec3(0.0f), kLightGizmoColor);
			return vertices;
		}

		/* 相机视锥的规范形状（45° / 16:9）：近 / 远矩形 + 四条棱 + 远平面向上标记。
		 * near_depth_unit = 近平面深度（真实比例 = near / far）；真实 FOV / 宽高比与距离
		 * 由绘制时的尺度因子补上。 */
		std::vector<float> BuildCameraFrustumVertices(float near_depth_unit)
		{
			std::vector<float> vertices;
			const float near_distance = kCameraGizmoFar * near_depth_unit;	/* 规范几何里的近平面深度 */
			const float near_half_h = near_distance * kCameraGizmoDefaultTanHalfFov;
			const float near_half_w = near_half_h * kCameraGizmoDefaultAspect;
			const float far_half_h = kCameraGizmoFar * kCameraGizmoDefaultTanHalfFov;
			const float far_half_w = far_half_h * kCameraGizmoDefaultAspect;

			/* 每个面的四个角：左下、右下、右上、左上（面向 -Z 看） */
			const glm::vec3 near_corners[4] = {
				glm::vec3(-near_half_w, -near_half_h, -near_distance),
				glm::vec3(near_half_w, -near_half_h, -near_distance),
				glm::vec3(near_half_w, near_half_h, -near_distance),
				glm::vec3(-near_half_w, near_half_h, -near_distance) };
			const glm::vec3 far_corners[4] = {
				glm::vec3(-far_half_w, -far_half_h, -kCameraGizmoFar),
				glm::vec3(far_half_w, -far_half_h, -kCameraGizmoFar),
				glm::vec3(far_half_w, far_half_h, -kCameraGizmoFar),
				glm::vec3(-far_half_w, far_half_h, -kCameraGizmoFar) };

			for (int32_t corner = 0; corner < 4; ++corner)
			{
				const int32_t next = (corner + 1) % 4;
				PushLineBand(vertices, near_corners[corner], near_corners[next], kCameraGizmoColor);
				PushLineBand(vertices, far_corners[corner], far_corners[next], kCameraGizmoColor);
				PushLineBand(vertices, near_corners[corner], far_corners[corner], kCameraGizmoColor);
			}

			/* 上向标记：远平面顶边中点抬高，两根线成 ^ */
			const glm::vec3 top_left = far_corners[3];
			const glm::vec3 top_right = far_corners[2];
			const glm::vec3 top_middle = (top_left + top_right) * 0.5f
				+ glm::vec3(0.0f, kCameraGizmoMarkerHeight, 0.0f);
			PushLineBand(vertices, top_left, top_middle, kCameraGizmoColor);
			PushLineBand(vertices, top_middle, top_right, kCameraGizmoColor);
			return vertices;
		}

		/* 相机机身盒 + 轴心点：不随视锥张开（FOV / 宽高比）变形 */
		std::vector<float> BuildCameraBodyVertices()
		{
			std::vector<float> vertices;
			const float body_half_depth = (kCameraGizmoBodyFar - kCameraGizmoBodyNear) * 0.5f;
			PushBoxBand(vertices,
				glm::vec3(0.0f, 0.0f, kCameraGizmoBodyNear + body_half_depth),
				glm::vec3(kCameraGizmoBodyHalfW, kCameraGizmoBodyHalfH, body_half_depth),
				kCameraGizmoColor);

			PushPivotDot(vertices, glm::vec3(0.0f), kCameraGizmoColor);
			return vertices;
		}

		/* 反射探针：方盒线框（Unity 式）+ 轴心点 */
		std::vector<float> BuildProbeGizmoVertices()
		{
			std::vector<float> vertices;
			PushBoxBand(vertices, glm::vec3(0.0f), glm::vec3(kProbeBoxHalfExtent), kProbeGizmoColor);
			PushPivotDot(vertices, glm::vec3(0.0f), kProbeGizmoColor);
			return vertices;
		}

		/* 精灵：框 + 对角线（图像占位样式）+ 轴心点 */
		std::vector<float> BuildSpriteGizmoVertices()
		{
			std::vector<float> vertices;
			const float half_extent = kSpriteGizmoHalfExtent;
			const glm::vec3 corners[4] = {
				glm::vec3(-half_extent, -half_extent, 0.0f), glm::vec3(half_extent, -half_extent, 0.0f),
				glm::vec3(half_extent, half_extent, 0.0f), glm::vec3(-half_extent, half_extent, 0.0f) };

			for (int32_t corner = 0; corner < 4; ++corner)
				PushLineBand(vertices, corners[corner], corners[(corner + 1) % 4], kSpriteGizmoColor);

			PushLineBand(vertices, corners[0], corners[2], kSpriteGizmoColor);
			PushLineBand(vertices, corners[1], corners[3], kSpriteGizmoColor);
			PushPivotDot(vertices, glm::vec3(0.0f), kSpriteGizmoColor);
			return vertices;
		}

		SharedPtr<DeviceVertexArray> MakeGizmoVertexArray(const std::string& name, const std::vector<float>& vertices)
		{
			const auto vertex_array = DeviceVertexArray::Create(name);
			vertex_array->Bind();
			const auto vertex_buffer = DeviceVertexBuffer::Create(name + "_Buffer",
				vertices.data(), vertices.size() * sizeof(float));
			vertex_buffer->SetLayout({
				{ "a_Position", BufferDataType::Float3 },
				{ "a_Color", BufferDataType::Float3 },
				{ "a_Dir", BufferDataType::Float3 },	/* 轴带方向；网格填 0 */
				{ "a_Side", BufferDataType::Float },	/* 轴带侧向 [-1, +1]；网格填 0 */
			});
			vertex_array->AddVertexBuffer(vertex_buffer);
			return vertex_array;
		}

		SharedPtr<DeviceVertexArray> GetGridVertexArray()
		{
			static SharedPtr<DeviceVertexArray> vertex_array;
			if (!vertex_array)
				vertex_array = MakeGizmoVertexArray("EditorGrid_VertexArray", BuildGridVertices());
			return vertex_array;
		}

		SharedPtr<DeviceVertexArray> GetAxisVertexArray()
		{
			static SharedPtr<DeviceVertexArray> vertex_array;
			if (!vertex_array)
				vertex_array = MakeGizmoVertexArray("EditorAxis_VertexArray", BuildAxisVertices());
			return vertex_array;
		}

		/* === 实体类型 gizmo 的顶点数组：每种类型一份，惰性创建一次 === */

		SharedPtr<DeviceVertexArray> GetDirectionalLightGizmoVertexArray()
		{
			static SharedPtr<DeviceVertexArray> vertex_array;
			if (!vertex_array)
				vertex_array = MakeGizmoVertexArray("EditorDirectionalLightGizmo_VertexArray", BuildDirectionalLightGizmoVertices());
			return vertex_array;
		}

		SharedPtr<DeviceVertexArray> GetPointLightGizmoVertexArray()
		{
			static SharedPtr<DeviceVertexArray> vertex_array;
			if (!vertex_array)
				vertex_array = MakeGizmoVertexArray("EditorPointLightGizmo_VertexArray", BuildPointLightGizmoVertices());
			return vertex_array;
		}

		SharedPtr<DeviceVertexArray> GetSpotLightGizmoVertexArray()
		{
			static SharedPtr<DeviceVertexArray> vertex_array;
			if (!vertex_array)
				vertex_array = MakeGizmoVertexArray("EditorSpotLightGizmo_VertexArray", BuildSpotLightGizmoVertices());
			return vertex_array;
		}

		/* 相机视锥的规范形状（45° / 16:9）：近 / 远矩形 + 四条棱 + 远平面向上标记。
		 * near_depth_unit = 近平面深度（真实比例 = near / far）；真实 FOV / 宽高比与距离
		 * 由绘制时的尺度因子补上。 */
		float CameraGizmoNearDepth(float near_clip, float far_clip)
		{
			const float near_safe = std::max(near_clip, 1.0e-4f);
			const float far_safe = std::max(far_clip, near_safe * 1.001f);
			return glm::clamp(std::pow(near_safe / far_safe, 0.25f), 0.05f, 0.7f);
		}

		/* 视锥显示长度随 far 对数增长（far = kCameraGizmoFarRef 时 = 1 倍基准）——
		 * 保留"far 越远、视锥越长"的直觉，同时不被超大 far 拉爆屏幕 */
		float CameraGizmoFarLengthScale(float far_clip)
		{
			const float far_safe = std::max(far_clip, 1.0e-4f);
			return std::log(1.0f + far_safe) / std::log(1.0f + kCameraGizmoFarRef);
		}

		/* 视锥几何随 near / far 的显示布局变化（几十个顶点）：参数变化时重建并缓存
		 * 最近一组 —— 参数稳定时零重建，多相机不同参数时逐相机重建（成本可忽略） */
		SharedPtr<DeviceVertexArray> GetCameraFrustumVertexArray(float near_depth_unit)
		{
			static SharedPtr<DeviceVertexArray> vertex_array;
			static float cached_near_depth = -1.0f;
			if (!vertex_array || cached_near_depth != near_depth_unit)
			{
				vertex_array = MakeGizmoVertexArray("EditorCameraFrustum_VertexArray",
					BuildCameraFrustumVertices(near_depth_unit));
				cached_near_depth = near_depth_unit;
			}
			return vertex_array;
		}

		SharedPtr<DeviceVertexArray> GetCameraBodyVertexArray()
		{
			static SharedPtr<DeviceVertexArray> vertex_array;
			if (!vertex_array)
				vertex_array = MakeGizmoVertexArray("EditorCameraBody_VertexArray", BuildCameraBodyVertices());
			return vertex_array;
		}

		SharedPtr<DeviceVertexArray> GetProbeGizmoVertexArray()
		{
			static SharedPtr<DeviceVertexArray> vertex_array;
			if (!vertex_array)
				vertex_array = MakeGizmoVertexArray("EditorProbeGizmo_VertexArray", BuildProbeGizmoVertices());
			return vertex_array;
		}

		SharedPtr<DeviceVertexArray> GetSpriteGizmoVertexArray()
		{
			static SharedPtr<DeviceVertexArray> vertex_array;
			if (!vertex_array)
				vertex_array = MakeGizmoVertexArray("EditorSpriteGizmo_VertexArray", BuildSpriteGizmoVertices());
			return vertex_array;
		}

		/* gizmo 材质：纯色、不参与光照、线 / 锥不剔面、深度写入开启（引擎约定"关闭写入 =
		 * 连深度测试一起关"）。混合用 SrcAlpha / OneMinusSrcAlpha（网格远景靠 alpha 渐隐）。
		 * grid_major > 0 表示画的是地面网格；其余 gizmo 要显式写 0。 */
		SharedPtr<Material> CreateGizmoMaterial(float fade_inner, float fade_outer, float grid_major)
		{
			auto material = CreateSharedPtr<Material>();
			material->SetShader(ShaderAssetManager::Instance().GetOrLoad(ABSOLUTE_PATH("Shaders/Axis.glsl")));

			auto& raster_state = material->GetRasterState();
			raster_state.CullMode = CullMode::Cull_None;
			raster_state.BlendFuncSrcRGB = BlendFunc::SrcAlpha;
			raster_state.BlendFuncSrcA = BlendFunc::SrcAlpha;
			raster_state.BlendFuncDstRGB = BlendFunc::OneMinusSrcAlpha;
			raster_state.BlendFuncDstA = BlendFunc::OneMinusSrcAlpha;

			/* 淡出范围交给着色器（网格那份与网格常量同源） */
			material->SetParameters(ParamType::Float, "u_FadeInner", fade_inner);
			material->SetParameters(ParamType::Float, "u_FadeOuter", fade_outer);
			material->SetParameters(ParamType::Float, "u_GridMajor", grid_major);

			/* 轴带展开参数：宽度与视口尺寸都在执行期按帧更新（见 AxisPass）；
			 * 这里给初值兜底 —— u_ViewportSize 为 0 时展开会除以 0 尺寸视口 */
			material->SetParameters(ParamType::Float, "u_LineWidth", kAxisLineWidth);
			material->SetParameters(ParamType::Vec2, "u_ViewportSize", glm::vec2(1.0f, 1.0f));
			return material;
		}

		/* 地面网格的材质：格距 = kGridMajorEvery，片元据此给每条线分类着色 */
		SharedPtr<Material> GetGridMaterial()
		{
			static SharedPtr<Material> material;
			if (!material)
				material = CreateGizmoMaterial(kGridFadeInner, kGridFadeOuter,
					static_cast<float>(kGridMajorEvery));
			return material;
		}

		/* 坐标轴（±1 的轴带）材质：不参与网格分类 */
		SharedPtr<Material> GetAxisMaterial()
		{
			static SharedPtr<Material> material;
			if (!material)
				material = CreateGizmoMaterial(kGridFadeInner, kGridFadeOuter, 0.0f);
			return material;
		}

		/* 实体 gizmo 材质：跟坐标轴同一个 shader，只是淡出范围放到天边（淡出是网格收边用的）。
		 * 两个值要远大于任何真实距离、还得保持不等（float 在 1e9 量级 +1 会被舍掉）。 */
		SharedPtr<Material> GetEntityGizmoMaterial()
		{
			static SharedPtr<Material> material;
			if (!material)
				material = CreateGizmoMaterial(1.0e6f, 2.0e6f, 0.0f);
			return material;
		}

		/* 实体世界变换拆解出 gizmo 需要的分量：平移 / 纯旋转（去缩放）/ 各轴缩放。
		 * "纯旋转"让图标本身不随实体缩放变形 —— 各类型对缩放的响应在响应因子里
		 * 单独表达（见 DrawEntityGizmos） */
		struct GizmoTransformParts
		{
			glm::vec3 Translation{ 0.0f };
			glm::quat Orientation{ 1.0f, 0.0f, 0.0f, 0.0f };
			glm::vec3 Scale{ 1.0f };	/* 各轴长度，恒为正 */
		};

		GizmoTransformParts DecomposeGizmoTransform(const glm::mat4& world_transform)
		{
			GizmoTransformParts parts;
			parts.Translation = glm::vec3(world_transform[3]);

			const glm::vec3 col_0(world_transform[0]);
			const glm::vec3 col_1(world_transform[1]);
			const glm::vec3 col_2(world_transform[2]);
			parts.Scale = glm::vec3(glm::length(col_0), glm::length(col_1), glm::length(col_2));

			glm::mat4 orientation(1.0f);
			orientation[0] = glm::vec4(parts.Scale.x > 0.0f ? col_0 / parts.Scale.x : glm::vec3(1.0f, 0.0f, 0.0f), 0.0f);
			orientation[1] = glm::vec4(parts.Scale.y > 0.0f ? col_1 / parts.Scale.y : glm::vec3(0.0f, 1.0f, 0.0f), 0.0f);
			orientation[2] = glm::vec4(parts.Scale.z > 0.0f ? col_2 / parts.Scale.z : glm::vec3(0.0f, 0.0f, 1.0f), 0.0f);
			parts.Orientation = glm::quat_cast(orientation);
			return parts;
		}

		/* 缩放合并到标量时取最大轴：各向同性 / 旋转对称形状（球、正圆锥）没有表达
		 * 各轴差异的自由度，取最大维度保证图标不被某个收缩轴带崩（可读性优先） */
		float MaxAxisOf(const glm::vec3& scale)
		{
			return std::max(std::max(scale.x, scale.y), scale.z);
		}

		/* 组装 gizmo 的模型矩阵：平移 × 纯旋转 × [额外旋转] × 尺寸基准（随距离收缩）× 各轴缩放响应 */
		glm::mat4 BuildGizmoModel(const GizmoTransformParts& parts, float world_scale,
			const glm::quat& extra_rotation, const glm::vec3& response)
		{
			return glm::translate(glm::mat4(1.0f), parts.Translation)
				* glm::mat4_cast(parts.Orientation * extra_rotation)
				* glm::scale(glm::mat4(1.0f), glm::vec3(world_scale) * response);
		}

		/* 实体图标总调度：光源（平行 / 点 / 聚光）、相机、反射探针、精灵。图标类的屏上尺寸随
		 * 距离收缩（近端封顶、远端保底，参数见 kGizmo* 常量）；gizmo_screen_scale = 单位距离
		 * 上每逻辑像素对应的世界尺寸，由 Submit 给出。实体数量少，就逐条提交。 */
		void DrawEntityGizmos(RenderView& render_view, const SharedPtr<Material>& material,
			const glm::vec3& camera_position, float gizmo_screen_scale)
		{
			const auto scene = render_view.GetOwnerScene();
			if (!scene)
				return;

			/* 注意要非 const 引用：const registry 的 view 会实例化 const 组件视图，
			 * get<> 取不到池（entt 的编译期约束） */
			auto& registry = scene->GetRegistry();

			/* 全局显隐（工具栏最右 Gizmos 菜单控制的单一数据源）：
			 * 关掉的类型在其循环内全部跳过、不产生任何提交 */
			const auto& gizmo_options = GetViewportGizmoOptions();
			if (!gizmo_options.MasterEnabled)
				return;	/* 总开关关：实体图标全部不提交 */

			/* 世界尺寸 = 像素尺度 × 标称像素 × 距离比例 × 到相机的距离；距离比例 =
			 * clamp(参考距离/距离, 下限, 上限)：中段世界恒定（屏上按 1/距离 递减），
			 * 近 / 远到钳制值后转为屏幕恒定（放大封顶 / 收缩保底） */
			const auto world_scale_at = [&](const glm::vec3& position, float nominal_pixel_size)
				{
					const float distance = glm::distance(camera_position, position);
					const float pixel_scale = glm::clamp(kGizmoRefDistance / distance,
						kGizmoMinPixelScale, kGizmoMaxPixelScale);
					return gizmo_screen_scale * nominal_pixel_size * pixel_scale * distance;
				};

			/* 光源：按 LightComponent 的类型选几何 */
			const auto light_view = registry.view<TransformComponent, LightComponent>();
			for (const entt::entity entity : light_view)
			{
				const auto& light_component = light_view.get<LightComponent>(entity);
				if (!gizmo_options.ShowLight || !light_component.m_Light)
					continue;

				const GizmoTransformParts parts = DecomposeGizmoTransform(scene->GetWorldTransform(entity));
				const float world_scale = world_scale_at(parts.Translation, kLightGizmoPixelSize);

				/* 图标朝向的实体旋转分量：定向类图标（太阳）把"规范朝向"直接旋到实际光向，
				 * 方向本身已随实体旋转推导（见 DirectionalLight::SetTransform），
				 * 再叠一次实体旋转会转两圈（R 作用两次）；点光 / 聚光的图标吃实体旋转 */
				GizmoTransformParts icon_parts = parts;

				SharedPtr<DeviceVertexArray> vertex_array;
				glm::quat extra_rotation(1.0f, 0.0f, 0.0f, 0.0f);
				glm::vec3 response(1.0f);
				switch (light_component.m_Light->GetLightType())
				{
				case LightType::Directional:
				{
					vertex_array = GetDirectionalLightGizmoVertexArray();
					/* 太阳图标沿真实光向摆放：方向读自光照用的同一份数据
					 * （几何按“光向 = -Y”的规范朝向建，这里旋到实际方向） */
					const auto directional_light = std::static_pointer_cast<DirectionalLight>(light_component.m_Light);
					const glm::vec3 direction = directional_light->GetDirection();
					if (glm::length(direction) > 0.0f)
						extra_rotation = glm::rotation(glm::vec3(0.0f, -1.0f, 0.0f), glm::normalize(direction));
					icon_parts.Orientation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
					/* response 保持 1：概念图标无尺寸语义，不响应缩放（Unity 灯光图标惯例） */
					break;
				}
				case LightType::Point:
					vertex_array = GetPointLightGizmoVertexArray();
					/* 球各向同性：合并到最大轴，贴合实体最大维度 */
					response = glm::vec3(MaxAxisOf(parts.Scale));
					break;
				case LightType::Spot:
				{
					vertex_array = GetSpotLightGizmoVertexArray();
					/* 旋转对称锥：Z 调射程、横向调口径；横向合并到最大轴并保持正圆。
					 * 锥角吃"形状缩放"：按规范角 30° 建的几何张开到光源的真实外锥角
					 * （角度读自光照用的同一份数据），锥轴 = 实体旋转（与光向同源）。 */
					const auto spot_light = std::static_pointer_cast<SpotLight>(light_component.m_Light);
					const float angle = glm::clamp(spot_light->GetAngle(), 1.0f, 170.0f);
					const float angle_shape = std::tan(glm::radians(angle) * 0.5f) / std::tan(kSpotGizmoAngle * 0.5f);
					const float radial_scale = std::max(parts.Scale.x, parts.Scale.y) * angle_shape;
					response = glm::vec3(radial_scale, radial_scale, parts.Scale.z);
					break;
				}
				case LightType::Area:
				case LightType::Volume:
					break;	/* 暂无面光 / 体积光的图标 */
				}

				if (vertex_array)
				{
					Renderer::FillObjectUniformBuffer(VisibleMeshObject{
						-1, BuildGizmoModel(icon_parts, world_scale, extra_rotation, response), nullptr, nullptr });
					Renderer::Submit(material, MeshPrimitive{ vertex_array, PrimitiveType::Triangles });
				}
			}

			/* 相机：视锥形状由相机参数（FOV / 宽高比）决定、深度布局随 near / far ——
			 * 视锥组吃"形状缩放"与布局参数，机身盒与轴心点不变形；
			 * 显示尺寸响应实体缩放（Blender 相机缩放放大显示） */
			const auto camera_view = registry.view<TransformComponent, CameraComponent>();
			for (const entt::entity entity : camera_view)
			{
				if (!gizmo_options.ShowCamera)
					continue;	/* 全局隐藏：相机图标整段不提交 */

				const auto& camera_component = camera_view.get<CameraComponent>(entity);
				const GizmoTransformParts parts = DecomposeGizmoTransform(scene->GetWorldTransform(entity));
				const float world_scale = world_scale_at(parts.Translation, kCameraGizmoPixelSize);
				const float entity_scale = MaxAxisOf(parts.Scale);

				/* 视锥尺度 = "真实量 / 规范量"：长度 far / 1.5、近平面深度 near / far；半高乘
				 * tan(fov/2) / tan(22.5°)、半宽乘 aspect / 16:9。没有相机组件 / 正交相机兜底：规范形状 ×
				 * 屏幕恒定基准。 */
				float shape_x = 1.0f;
				float shape_y = 1.0f;
				float near_depth_unit = kCameraGizmoNear / kCameraGizmoFar;
				float far_length_scale = 1.0f;
				if (camera_component.m_Camera)
				{
					const auto& camera = camera_component.m_Camera;
					if (camera->GetProjectionType() == CameraProjectionType::Perspective)
					{
						const float fov = glm::clamp(camera->GetFov(), 1.0f, 179.0f);
						const float tan_half_fov = std::tan(glm::radians(fov) * 0.5f);
						shape_y = tan_half_fov / kCameraGizmoDefaultTanHalfFov;
						shape_x = shape_y * (camera->GetAspectRatio() / kCameraGizmoDefaultAspect);
					}
					near_depth_unit = CameraGizmoNearDepth(camera->GetNearClip(), camera->GetFarClip());
					far_length_scale = CameraGizmoFarLengthScale(camera->GetFarClip());
				}

				/* 视锥（近 / 远矩形 + 棱 + 上向标记）：形状缩放 × 实体缩放，
				 * 显示长度随 far 对数增长（near / far 的显示映射） */
				Renderer::FillObjectUniformBuffer(VisibleMeshObject{ -1,
					BuildGizmoModel(parts, world_scale * far_length_scale, glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
						glm::vec3(shape_x * entity_scale, shape_y * entity_scale, entity_scale)), nullptr, nullptr });
				Renderer::Submit(material, MeshPrimitive{ GetCameraFrustumVertexArray(near_depth_unit), PrimitiveType::Triangles });

				/* 机身盒 + 轴心点：只随尺寸基准与实体缩放 */
				Renderer::FillObjectUniformBuffer(VisibleMeshObject{ -1,
					BuildGizmoModel(parts, world_scale, glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
						glm::vec3(entity_scale)), nullptr, nullptr });
				Renderer::Submit(material, MeshPrimitive{ GetCameraBodyVertexArray(), PrimitiveType::Triangles });
			}

			/* 反射探针：盒 = 体积边界（Godot / Unity 的探针盒语义），完整响应缩放、各轴独立 */
			const auto probe_view = registry.view<TransformComponent, ReflectionProbeComponent>();
			for (const entt::entity entity : probe_view)
			{
				if (!gizmo_options.ShowReflectionProbe)
					continue;	/* 全局隐藏：探针盒整段不提交 */

				const GizmoTransformParts parts = DecomposeGizmoTransform(scene->GetWorldTransform(entity));
				Renderer::FillObjectUniformBuffer(VisibleMeshObject{ -1,
					BuildGizmoModel(parts, world_scale_at(parts.Translation, kProbeGizmoPixelSize),
						glm::quat(1.0f, 0.0f, 0.0f, 0.0f), parts.Scale), nullptr, nullptr });
				Renderer::Submit(material, MeshPrimitive{ GetProbeGizmoVertexArray(), PrimitiveType::Triangles });
			}

			/* 精灵：框 = 显示区域 1×1 的实际占位，完整响应缩放、各轴独立 */
			const auto sprite_view = registry.view<TransformComponent, SpriteComponent>();
			for (const entt::entity entity : sprite_view)
			{
				if (!gizmo_options.ShowSprite)
					continue;	/* 全局隐藏：精灵框整段不提交 */

				const GizmoTransformParts parts = DecomposeGizmoTransform(scene->GetWorldTransform(entity));
				Renderer::FillObjectUniformBuffer(VisibleMeshObject{ -1,
					BuildGizmoModel(parts, world_scale_at(parts.Translation, kSpriteGizmoPixelSize),
						glm::quat(1.0f, 0.0f, 0.0f, 0.0f), parts.Scale), nullptr, nullptr });
				Renderer::Submit(material, MeshPrimitive{ GetSpriteGizmoVertexArray(), PrimitiveType::Triangles });
			}
		}

		/* === 视口导航参数 === */
		constexpr float kMouseSensitivity = 0.003f;	/* 像素 → 拖拽量的统一系数（沿用原手感） */
		constexpr float kMinDistance = 0.5f;		/* 轨道距离上下限 */
		constexpr float kMaxDistance = 1000.0f;
		constexpr float kMaxPitch = 1.553f;			/* 俯仰限位 ±89°：翻越天顶后视线会翻转 */
		constexpr float kWheelZoomStep = 0.9f;		/* 滚轮每格缩放比（乘性） */
		constexpr float kWheelSpeedStep = 1.15f;	/* 飞行速度每格调整比（乘性） */
		constexpr float kMinFlySpeed = 0.5f;
		constexpr float kMaxFlySpeed = 100.0f;
	}
	/* 视口 gizmo 显隐的单一数据源（声明见头文件）：绘制侧每帧读、UI 侧改写 */
	ViewportGizmoOptions& GetViewportGizmoOptions()
	{
		static ViewportGizmoOptions options;
		return options;
	}

	EditorCamera::EditorCamera(float fov, float aspect_ratio, float near_clip, float far_clip)
		: Camera(CameraProjectionType::Perspective, fov, aspect_ratio, near_clip, far_clip)
	{
		PROFILE_FUNCTION();

		UpdateCameraDirections();

		UpdateProjectionMatrix();
		UpdateViewMatrix();
	}

	EditorCamera::~EditorCamera()
	{
	}

	void EditorCamera::OnUpdate(float delta_time)
	{
		PROFILE_FUNCTION();

		Camera::OnUpdate(delta_time);

		UpdateNavigation(delta_time, m_IsNavigationAllowed);

		UpdateViewMatrix();
	}

	/* 视口导航状态机：手势从"指针在视口上时按下按键"起手（allow_new_gesture 由场景层每帧
	 * 授权）；起手后就锁定到那次按键松开 —— 拖拽中指针移出视口、松开修饰键，都不中断。 */
	void EditorCamera::UpdateNavigation(float delta_time, bool allow_new_gesture)
	{
		PROFILE_FUNCTION();

		const bool is_alt_pressed = Input::IsKeyPressed(Key::LeftAlt) || Input::IsKeyPressed(Key::RightAlt);
		const bool is_lmb_pressed = Input::IsMouseButtonPressed(Mouse::ButtonLeft);
		const bool is_mmb_pressed = Input::IsMouseButtonPressed(Mouse::ButtonMiddle);
		const bool is_rmb_pressed = Input::IsMouseButtonPressed(Mouse::ButtonRight);

		/* 手势保持条件：其按键仍按住（Orbit / ZoomDrag 还要求 Alt —— 飞行中按下 Alt 会顺势切换为推拉） */
		bool is_gesture_held = false;
		switch (m_ActiveGesture)
		{
		case NavGesture::Orbit:		is_gesture_held = is_alt_pressed && is_lmb_pressed; break;
		case NavGesture::Pan:		is_gesture_held = is_mmb_pressed; break;
		case NavGesture::ZoomDrag:	is_gesture_held = is_alt_pressed && is_rmb_pressed; break;
		case NavGesture::Fly:		is_gesture_held = is_rmb_pressed && !is_alt_pressed; break;
		default:					break;
		}

		if (m_ActiveGesture != NavGesture::None && !is_gesture_held)
			m_ActiveGesture = NavGesture::None;

		if (m_ActiveGesture == NavGesture::None && allow_new_gesture)
		{
			/* 起手要求按键“按下沿”：按键从别的窗口拖进视口、或按住不放滑入视口，
			 * 都不算起手 —— 否则在视口外拖拽时划过视口会突然开始转视角 */
			const bool is_alt_edge = is_alt_pressed && !m_WasAltPressed;
			const bool is_lmb_edge = is_lmb_pressed && !m_WasLmbPressed;
			const bool is_mmb_edge = is_mmb_pressed && !m_WasMmbPressed;
			const bool is_rmb_edge = is_rmb_pressed && !m_WasRmbPressed;

			if (is_alt_pressed)
			{
				if (is_lmb_pressed && is_lmb_edge)
					m_ActiveGesture = NavGesture::Orbit;
				else if (is_mmb_pressed && is_mmb_edge)
					m_ActiveGesture = NavGesture::Pan;
				else if (is_rmb_pressed && (is_rmb_edge || is_alt_edge))
					m_ActiveGesture = NavGesture::ZoomDrag; /* 飞行中按下 Alt：顺势切换为推拉 */
			}
			else if (is_rmb_pressed && is_rmb_edge)
			{
				m_ActiveGesture = NavGesture::Fly;
			}
			else if (is_mmb_pressed && is_mmb_edge)
			{
				m_ActiveGesture = NavGesture::Pan;
			}
		}

		/* 鼠标增量逐帧维护（闲置时也更新）：起手帧的增量只是正常的单帧位移，不会跳视角 */
		const glm::vec2 mouse_pos = Input::GetMousePos();
		const glm::vec2 mouse_delta = (mouse_pos - m_LastMousePosition) * kMouseSensitivity;
		m_LastMousePosition = mouse_pos;

		switch (m_ActiveGesture)
		{
		case NavGesture::Orbit:		OnMouseRotate(mouse_delta); break;
		case NavGesture::Pan:		OnMousePan(mouse_delta); break;
		case NavGesture::ZoomDrag:	OnMouseZoom(mouse_delta.y); break;
		case NavGesture::Fly:		UpdateFly(mouse_delta, delta_time); break;
		default:					break;
		}

		/* 帧末记录按键状态（下一帧的“按下沿”判据） */
		m_WasAltPressed = is_alt_pressed;
		m_WasLmbPressed = is_lmb_pressed;
		m_WasMmbPressed = is_mmb_pressed;
		m_WasRmbPressed = is_rmb_pressed;
	}

	/* 飞行（RMB 按住）：鼠标原地转向；WASD 沿视线、E/Q 沿世界竖直方向平移。
	 * 转向时枢轴重新落在视线正前方同距离处 —— 眼位保持不动，退出飞行后
	 * 轨道中心就是当时的视线落点，无需交接。 */
	void EditorCamera::UpdateFly(const glm::vec2& mouse_delta, float delta_time)
	{
		PROFILE_FUNCTION();

		if (mouse_delta.x != 0.0f || mouse_delta.y != 0.0f)
		{
			/* 转向前的眼位（位置是派生量，这里直接用轨道参数求出） */
			const glm::vec3 eye_position = m_FocalPoint - m_ForwardDirection * m_Distance;

			m_Yaw += mouse_delta.x * RotateSpeed();
			m_Pitch = glm::clamp(m_Pitch + mouse_delta.y * RotateSpeed(), -kMaxPitch, kMaxPitch);
			UpdateCameraDirections();
			m_FocalPoint = eye_position + m_ForwardDirection * m_Distance;

			m_IsDirty = true;
		}

		glm::vec3 move_direction(0.0f);
		if (Input::IsKeyPressed(Key::W))
			move_direction += m_ForwardDirection;
		if (Input::IsKeyPressed(Key::S))
			move_direction -= m_ForwardDirection;
		if (Input::IsKeyPressed(Key::D))
			move_direction += m_RightDirection;
		if (Input::IsKeyPressed(Key::A))
			move_direction -= m_RightDirection;
		/* 竖直用世界轴：俯仰朝下时 Q/E 不会变成前冲 */
		if (Input::IsKeyPressed(Key::E))
			move_direction += glm::vec3(0.0f, 1.0f, 0.0f);
		if (Input::IsKeyPressed(Key::Q))
			move_direction -= glm::vec3(0.0f, 1.0f, 0.0f);

		if (glm::dot(move_direction, move_direction) > 0.0f)
		{
			/* 眼位与枢轴同步平移（斜向先归一化，避免比单键更快） */
			m_FocalPoint += glm::normalize(move_direction) * (m_MoveSpeed * delta_time);
			m_IsDirty = true;
		}
	}

	/* 设置视口区域 */
	void EditorCamera::SetViewportRegion(const ViewportRegion& region)
	{
		PROFILE_FUNCTION();

		m_ViewportRegion = region;
		m_pRenderView->SetViewportRegion(region);

		m_AspectRatio = static_cast<float>(m_ViewportRegion.Width) / static_cast<float>(m_ViewportRegion.Height);
		UpdateProjectionMatrix();
	}

	/* 构建内置的FrameGraph（延迟渲染：GBuffer + Lighting）
	 * 由 RenderView 在执行时回调，故渲染图每帧按当前视口尺寸重建。 */
	bool EditorCamera::ConstructRenderView(RenderView& render_view)
	{
		PROFILE_FUNCTION();

		/* 视口尺寸无效时不组织渲染图：这里必须返回 true（表示"本相机负责组织"），
		 * 否则会回落到默认前向图并沿用 0 尺寸的渲染目标。渲染图为空时输出为空。 */
		if (m_ViewportRegion.Width <= 0 || m_ViewportRegion.Height <= 0)
			return true;

		auto& frame_graph = render_view.GetFrameGraph();
		frame_graph->Reset();

		FrameGraphTexture::Descriptor color_target_desc;
		color_target_desc.Width = m_ViewportRegion.Width;
		color_target_desc.Height = m_ViewportRegion.Height;
		color_target_desc.TextureFormat = TextureFormat::RGBA8;

		/* 世界位置通道用浮点格式：RGBA8 会把世界坐标钳制到 [0,1]（负值归零、
		 * 大于 1 截平），光照阶段拿它做级联阴影变换时采样点全部落在错误位置 ——
		 * 阴影与场景对不上（偏移、截断，甚至全域误判为受光/被遮）。 */
		FrameGraphTexture::Descriptor world_pos_target_desc = color_target_desc;
		world_pos_target_desc.TextureFormat = TextureFormat::RGBA32F;

		FrameGraphTexture::Descriptor depth_target_desc;
		depth_target_desc.Width = m_ViewportRegion.Width;
		depth_target_desc.Height = m_ViewportRegion.Height;
		depth_target_desc.TextureFormat = TextureFormat::Depth32;

		FrameGraphTexture::Descriptor object_id_desc;
		object_id_desc.Width = m_ViewportRegion.Width;
		object_id_desc.Height = m_ViewportRegion.Height;
		object_id_desc.TextureFormat = TextureFormat::R32I;

		/* GBuffer Pass 使用 default.glsl；普通材质在提交前转换为该 shader 对应的临时材质。 */
		const auto gbuffer_shader = ShaderAssetManager::Instance().GetOrLoad(
			ABSOLUTE_PATH("Shaders/default.glsl"));

		/* 阴影：先生成中性深度数组（保证光照着色器总能采样到合法纹理），
		 * 本视图有投影光源（级联 / 点光 / 聚光）时再生成真实阴影图；
		 * 句柄都落进 Blackboard 的 "ShadowMapHandle"，由光照 Pass 绑定。 */
		{
			auto shadow_map_manager = render_view.GetShadowMapManager();
			shadow_map_manager->AddNoShadowMapPass(*frame_graph);
			if (render_view.HasShadowCast())
			{
				shadow_map_manager->AddShadowPass(*frame_graph, render_view.GetOwnerScene(), &render_view);
			}
			else
			{
				const auto fallback_handle = frame_graph->GetBlackboard()
					.GetResourceHandle<FrameGraphTexture>("NoShadowMapHandle");
				frame_graph->GetBlackboard()["ShadowMapHandle"] = fallback_handle;
			}
		}

		/* GBuffer Pass */
		struct GBufferPassData
		{
			FrameGraphResourceHandleTyped<FrameGraphTexture> GBufferTexture0;
			FrameGraphResourceHandleTyped<FrameGraphTexture> GBufferTexture1;
			FrameGraphResourceHandleTyped<FrameGraphTexture> GBufferTexture2;
			FrameGraphResourceHandleTyped<FrameGraphTexture> GBufferTexture3;
			FrameGraphResourceHandleTyped<FrameGraphTexture> GBufferTexture4;
			FrameGraphResourceHandleTyped<FrameGraphTexture> GBufferTexture5;
			FrameGraphResourceHandleTyped<FrameGraphTexture> ObjectId;
			FrameGraphResourceHandleTyped<FrameGraphTexture> Depth;
		};

		auto gbuffer_pass = frame_graph->AddPass<GBufferPassData>("GBufferPass",
			[&, gbuffer_shader](FrameGraphBuilder& builder, GBufferPassData& data)
			{
				data.GBufferTexture0 = builder.CreateTexture("GBufferTexture0", color_target_desc);
				data.GBufferTexture1 = builder.CreateTexture("GBufferTexture1", color_target_desc);
				data.GBufferTexture2 = builder.CreateTexture("GBufferTexture2", color_target_desc);
				data.GBufferTexture3 = builder.CreateTexture("GBufferTexture3", color_target_desc);
				data.GBufferTexture4 = builder.CreateTexture("GBufferTexture4", color_target_desc);
				data.GBufferTexture5 = builder.CreateTexture("GBufferTexture5", world_pos_target_desc);
				data.ObjectId = builder.CreateTexture("ObjectId", object_id_desc);
				data.Depth = builder.CreateTexture("GBufferDepth", depth_target_desc);

				builder.BindOutputResource(data.GBufferTexture0, FrameGraphTexture::Usage::ColorAttachment);
				builder.BindOutputResource(data.GBufferTexture1, FrameGraphTexture::Usage::ColorAttachment);
				builder.BindOutputResource(data.GBufferTexture2, FrameGraphTexture::Usage::ColorAttachment);
				builder.BindOutputResource(data.GBufferTexture3, FrameGraphTexture::Usage::ColorAttachment);
				builder.BindOutputResource(data.GBufferTexture4, FrameGraphTexture::Usage::ColorAttachment);
				builder.BindOutputResource(data.GBufferTexture5, FrameGraphTexture::Usage::ColorAttachment);
				builder.BindOutputResource(data.ObjectId, FrameGraphTexture::Usage::ColorAttachment | FrameGraphTexture::Usage::Sampleable);
				builder.BindOutputResource(data.Depth, FrameGraphTexture::Usage::DepthAttachment);

				FrameGraphPassInfo::Descriptor pass_desc;
				pass_desc.Attachments.ColorAttachments(0) = data.GBufferTexture0;
				pass_desc.Attachments.ColorAttachments(1) = data.GBufferTexture1;
				pass_desc.Attachments.ColorAttachments(2) = data.GBufferTexture2;
				pass_desc.Attachments.ColorAttachments(3) = data.GBufferTexture3;
				pass_desc.Attachments.ColorAttachments(4) = data.GBufferTexture4;
				pass_desc.Attachments.ColorAttachments(5) = data.GBufferTexture5;
				pass_desc.Attachments.ColorAttachments(6) = data.ObjectId;
				pass_desc.Attachments.DepthAttachment() = data.Depth;
				pass_desc.ColorClearValues.resize(7);
				pass_desc.ColorClearValues[6] = glm::vec4(-1.0f, 0.0f, 0.0f, 0.0f);
				pass_desc.ViewportRegion = m_ViewportRegion;
				builder.CreateRenderPass("GBufferPassRenderTarget", pass_desc);
			},
			[&, gbuffer_shader](const FrameGraphResources& resources, const GBufferPassData& data)
			{
				std::unordered_map<const Material*, SharedPtr<Material>> gbuffer_materials;
				const auto render_pass_info = resources.GetPassRenderTarget();
				render_view.EmplacePassFrameBuffer("GBufferPass", render_pass_info);

				render_pass_info->Bind();
				{
					Renderer::GetRenderAPI()->Clear();

					for (const auto& mesh_object : render_view.GetVisibleMeshObjects())
					{
						/* Fill object uniform buffer */
						Renderer::FillObjectUniformBuffer(mesh_object);

						const auto& material = mesh_object.Material;
						if (material == nullptr)
							continue;

						/* 天空盒不进 G-Buffer：它输出的是最终颜色、顶点着色器把网格
						 * 折叠成全屏天空，转成 G-Buffer 材质只会得到一颗实心球。
						 * 由 SkyPass 在光照之后专绘背景。 */
						if (material->IsSkyBox())
							continue;

						auto gbuffer_material = material;
						if (material->GetShader() != gbuffer_shader)
						{
							auto [cached, inserted] = gbuffer_materials.try_emplace(material.get());
							if (inserted)
								cached->second = CreateGBufferMaterial(material, gbuffer_shader);
							gbuffer_material = cached->second;
						}
						Renderer::Submit(gbuffer_material, mesh_object.MeshSegment->GetMeshPrimitive());
					}
				}
				render_pass_info->Unbind();
				Renderer::GetRenderAPI()->Flush();
			}
			);

		frame_graph->GetBlackboard()["GBufferTexture0"] = gbuffer_pass->GetData().GBufferTexture0;
		frame_graph->GetBlackboard()["GBufferTexture1"] = gbuffer_pass->GetData().GBufferTexture1;
		frame_graph->GetBlackboard()["GBufferTexture2"] = gbuffer_pass->GetData().GBufferTexture2;
		frame_graph->GetBlackboard()["GBufferTexture3"] = gbuffer_pass->GetData().GBufferTexture3;
		frame_graph->GetBlackboard()["GBufferTexture4"] = gbuffer_pass->GetData().GBufferTexture4;
		frame_graph->GetBlackboard()["GBufferTexture5"] = gbuffer_pass->GetData().GBufferTexture5;
		frame_graph->GetBlackboard()["GBufferDepth"] = gbuffer_pass->GetData().Depth;

		/* Lighting Pass */
		struct LightingPassData
		{
			FrameGraphResourceHandleTyped<FrameGraphTexture> GBufferTexture0;
			FrameGraphResourceHandleTyped<FrameGraphTexture> GBufferTexture1;
			FrameGraphResourceHandleTyped<FrameGraphTexture> GBufferTexture2;
			FrameGraphResourceHandleTyped<FrameGraphTexture> GBufferTexture3;
			FrameGraphResourceHandleTyped<FrameGraphTexture> GBufferTexture4;
			FrameGraphResourceHandleTyped<FrameGraphTexture> GBufferTexture5;
			FrameGraphResourceHandleTyped<FrameGraphTexture> ShadowMapHandle;
			FrameGraphResourceHandleTyped<FrameGraphTexture> LightingResult;
		};

		auto lighting_pass = frame_graph->AddPass<LightingPassData>("LightingPass",
			[&](FrameGraphBuilder& builder, LightingPassData& data)
			{
				data.GBufferTexture0 = frame_graph->GetBlackboard().GetResourceHandle<FrameGraphTexture>("GBufferTexture0");
				data.GBufferTexture1 = frame_graph->GetBlackboard().GetResourceHandle<FrameGraphTexture>("GBufferTexture1");
				data.GBufferTexture2 = frame_graph->GetBlackboard().GetResourceHandle<FrameGraphTexture>("GBufferTexture2");
				data.GBufferTexture3 = frame_graph->GetBlackboard().GetResourceHandle<FrameGraphTexture>("GBufferTexture3");
				data.GBufferTexture4 = frame_graph->GetBlackboard().GetResourceHandle<FrameGraphTexture>("GBufferTexture4");
				data.GBufferTexture5 = frame_graph->GetBlackboard().GetResourceHandle<FrameGraphTexture>("GBufferTexture5");
				data.ShadowMapHandle = frame_graph->GetBlackboard().GetResourceHandle<FrameGraphTexture>("ShadowMapHandle");

				data.LightingResult = builder.CreateTexture("LightingResult", color_target_desc);

				builder.BindInputResource(data.GBufferTexture0, FrameGraphTexture::Usage::Sampleable);
				builder.BindInputResource(data.GBufferTexture1, FrameGraphTexture::Usage::Sampleable);
				builder.BindInputResource(data.GBufferTexture2, FrameGraphTexture::Usage::Sampleable);
				builder.BindInputResource(data.GBufferTexture3, FrameGraphTexture::Usage::Sampleable);
				builder.BindInputResource(data.GBufferTexture4, FrameGraphTexture::Usage::Sampleable);
				builder.BindInputResource(data.GBufferTexture5, FrameGraphTexture::Usage::Sampleable);
				builder.BindInputResource(data.ShadowMapHandle, FrameGraphTexture::Usage::Sampleable);
				builder.BindOutputResource(data.LightingResult, FrameGraphTexture::Usage::ColorAttachment | FrameGraphTexture::Usage::Sampleable);

				FrameGraphPassInfo::Descriptor pass_desc;
				pass_desc.Attachments.ColorAttachments(0) = data.LightingResult;
				pass_desc.ViewportRegion = m_ViewportRegion;
				builder.CreateRenderPass("LightingPassRenderTarget", pass_desc);

				builder.AsSideEffect();
			},
			[&](const FrameGraphResources& resources, const LightingPassData& data)
			{
				const auto render_pass_info = resources.GetPassRenderTarget();
				render_view.EmplacePassFrameBuffer("LightingPass", render_pass_info);

				render_pass_info->Bind();
				{
					Renderer::GetRenderAPI()->Clear();

					/* 阴影纹理随 Submit 直绑（按 Shader 反射出的 "u_ShadowMap" 绑定点） */
					ScopedShadowMapBinding shadow_map_binding(resources.Get(data.ShadowMapHandle).Texture);

					const auto& lights = render_view.GetValidLights();
					for (size_t light_index = 0; light_index < lights.size(); ++light_index)
					{
						const auto& light = lights[light_index];

						/* 完整光照数据：光源参数 + 该光源的阴影（级联 / 点光 / 聚光，来自本视图阴影管理器） */
						Renderer::FillLightUniformBuffer(light, render_view.GetShadowMapManager().get());

						SharedPtr<Material> material = CreateSharedPtr<Material>();
						auto& raster_state = material->GetRasterState();
						raster_state.EnableDepthWrite = false;
						raster_state.EnableBlend = true;
						raster_state.BlendEquationRGB = BlendEquation::Add;
						raster_state.BlendEquationA = BlendEquation::Add;
						raster_state.BlendFuncSrcRGB = BlendFunc::One;
						raster_state.BlendFuncSrcA = BlendFunc::One;
						raster_state.BlendFuncDstRGB = BlendFunc::One;
						raster_state.BlendFuncDstA = BlendFunc::One;
						const auto shader = ShaderAssetManager::Instance().GetOrLoad(ABSOLUTE_PATH("Shaders/lighting.glsl"));
						material->SetShader(shader);
						material->SetTexture("u_GBufferTexture0", resources.Get(data.GBufferTexture0).Texture);
						material->SetTexture("u_GBufferTexture1", resources.Get(data.GBufferTexture1).Texture);
						material->SetTexture("u_GBufferTexture2", resources.Get(data.GBufferTexture2).Texture);
						material->SetTexture("u_GBufferTexture3", resources.Get(data.GBufferTexture3).Texture);
						material->SetTexture("u_GBufferTexture4", resources.Get(data.GBufferTexture4).Texture);
						material->SetTexture("u_GBufferTexture5", resources.Get(data.GBufferTexture5).Texture);
						/* 环境光 / 自发光与光源无关，只由第一笔光照合成（多光源逐笔加法叠加） */
						material->SetParameters(ParamType::Int, "u_ComposeAmbientEmission",
							light_index == 0 ? 1 : 0);
						Renderer::Submit(material, Renderer::GetFullScreenVertexArray());
					}
				}
				render_pass_info->Unbind();
			}
			);
		frame_graph->GetBlackboard()["LightingPassOutput"] = lighting_pass->GetData().LightingResult;

			/* 天空背景 Pass：天空盒材质按原样画进光照结果。顶点着色器折叠成全屏天空、深度恒为远平面
			 * （Reversed-Z），跟 GBuffer 深度做 GreaterEqual 比较（只在背景像素落笔）。要在光照之后、
			 * 叠加层之前。 */
		struct SkyPassData
		{
			FrameGraphResourceHandleTyped<FrameGraphTexture> Output;
			FrameGraphResourceHandleTyped<FrameGraphTexture> Depth;
		};

		frame_graph->AddPass<SkyPassData>("SkyPass",
			[&](FrameGraphBuilder& builder, SkyPassData& data)
			{
				data.Output = frame_graph->GetBlackboard().GetResourceHandle<FrameGraphTexture>("LightingPassOutput");
				data.Depth = frame_graph->GetBlackboard().GetResourceHandle<FrameGraphTexture>("GBufferDepth");
				builder.BindInputResource(data.Output, FrameGraphTexture::Usage::ColorAttachment);
				builder.BindInputResource(data.Depth, FrameGraphTexture::Usage::DepthAttachment);

				FrameGraphPassInfo::Descriptor pass_desc;
				pass_desc.Attachments.ColorAttachments(0) = data.Output;
				pass_desc.Attachments.DepthAttachment() = data.Depth;
				/* 保留光照结果与场景深度（天空只在背景像素落笔） */
				pass_desc.PreserveContent = true;
				pass_desc.ViewportRegion = m_ViewportRegion;
				builder.CreateRenderPass("SkyPassRenderTarget", pass_desc);

				/* 与 AxisPass 同理：叠加型 Pass 的产物是"写进已有资源"，
				 * 不声明为目标会被图裁剪剔除 */
				builder.AsSideEffect();
			},
			[&](const FrameGraphResources& resources, const SkyPassData&)
			{
				const auto render_pass_info = resources.GetPassRenderTarget();
				render_view.EmplacePassFrameBuffer("SkyPass", render_pass_info);

				render_pass_info->Bind();
				{
					/* 天空盒材质按原样提交（不做 G-Buffer 转换）：u_SkyTex 采样、
					 * 远平面 z、面剔除姿态都由材质自身的光栅状态决定 */
					for (const auto& mesh_object : render_view.GetVisibleMeshObjects())
					{
						const auto& material = mesh_object.Material;
						if (material == nullptr || !material->IsSkyBox())
							continue;

						Renderer::FillObjectUniformBuffer(mesh_object);
						Renderer::Submit(material, mesh_object.MeshSegment->GetMeshPrimitive());
					}
				}
				render_pass_info->Unbind();
			}
			);

		/* 场景 gizmo Pass（编辑器视口叠加层：地面网格 + 坐标轴 + 实体图标，见 SceneGizmos）。
		 * 直接复用管线输出和场景深度：PreserveContent 不清除附件，gizmo 画进输出、与场景深度
		 * 做 GreaterEqual 比较（会被物体挡住）；它是最后一个写输出纹理的 Pass。 */
		struct AxisPassData
		{
			FrameGraphResourceHandleTyped<FrameGraphTexture> Output;
			FrameGraphResourceHandleTyped<FrameGraphTexture> Depth;
		};

		frame_graph->AddPass<AxisPassData>("AxisPass",
			[&](FrameGraphBuilder& builder, AxisPassData& data)
			{
				data.Output = frame_graph->GetBlackboard().GetResourceHandle<FrameGraphTexture>("LightingPassOutput");
				data.Depth = frame_graph->GetBlackboard().GetResourceHandle<FrameGraphTexture>("GBufferDepth");
				builder.BindInputResource(data.Output, FrameGraphTexture::Usage::ColorAttachment);
				builder.BindInputResource(data.Depth, FrameGraphTexture::Usage::DepthAttachment);

				FrameGraphPassInfo::Descriptor pass_desc;
				pass_desc.Attachments.ColorAttachments(0) = data.Output;
				pass_desc.Attachments.DepthAttachment() = data.Depth;
				/* 叠加层：保留颜色（光照结果）与深度（场景深度） */
				pass_desc.PreserveContent = true;
				pass_desc.ViewportRegion = m_ViewportRegion;
				builder.CreateRenderPass("AxisPassRenderTarget", pass_desc);

				/* 输出纹理由本 Pass 画完并留给 ImGui 采样：
				 * 既不能因为"没有被下游消费"被剔除，也不能在末尾随资源生命周期销毁。 */
				builder.AsSideEffect();
			},
			[&](const FrameGraphResources& resources, const AxisPassData&)
			{
				const auto render_pass_info = resources.GetPassRenderTarget();
				render_view.EmplacePassFrameBuffer("AxisPass", render_pass_info);
				const auto grid_material = GetGridMaterial();
				const auto axis_material = GetAxisMaterial();
				const auto entity_gizmo_material = GetEntityGizmoMaterial();

			/* 轴带展开参数在执行期给：宽度按内容缩放折算成物理像素（视口 RT 就是物理像素）。视口尺寸
			 * 跟 RT 实时一致；网格 / 坐标轴 / 实体 gizmo 三份材质都要给一遍。 */
				const float content_scale = Application::Instance()->GetWindow().GetContentScale();
				const glm::vec2 viewport_size(
					static_cast<float>(m_ViewportRegion.Width),
					static_cast<float>(m_ViewportRegion.Height));
				for (const auto& gizmo_material : { grid_material, axis_material, entity_gizmo_material })
				{
					gizmo_material->SetParameters(ParamType::Float, "u_LineWidth", kAxisLineWidth * content_scale);
					gizmo_material->SetParameters(ParamType::Vec2, "u_ViewportSize", viewport_size);
				}

				/* 实体 gizmo 的像素→世界系数：每"逻辑像素"在单位距离上对应的世界尺寸
				 * （= 2·tan(fov/2)·内容缩放 / 视口高；配 world_scale_at 的距离比例与距离使用） */
				const float gizmo_screen_scale =
					2.0f * std::tan(glm::radians(m_Fov) * 0.5f) * content_scale
					/ static_cast<float>(m_ViewportRegion.Height);

				render_pass_info->Bind();
				{
					/* 视口 gizmo 的全局显隐（工具栏最右 Gizmos 菜单控制的单一数据源）：
					 * 总开关与分项各管一层，总开关关则一律不提交 */
					const auto& gizmo_options = GetViewportGizmoOptions();

		/* 地面网格：patch 跟着相机平移、按格对齐（整数平移 → 线就一直落在世界整数坐标上）；
		 * 远景淡出按到相机的水平距离在着色器里处理（见 Axis.glsl）。 */
					if (gizmo_options.MasterEnabled && gizmo_options.ShowGrid)
					{
						const glm::vec3 camera_pos = GetPosition();
						const glm::vec3 grid_origin(std::round(camera_pos.x), 0.0f, std::round(camera_pos.z));
						Renderer::FillObjectUniformBuffer(VisibleMeshObject{
							-1, glm::translate(glm::mat4(1.0f), grid_origin), nullptr, nullptr });
						Renderer::Submit(grid_material, MeshPrimitive{ GetGridVertexArray(), PrimitiveType::Lines });
					}

					/* 坐标轴固定在世界原点（单位变换）；轴带是三角形 */
					if (gizmo_options.MasterEnabled && gizmo_options.ShowWorldAxis)
					{
						Renderer::FillObjectUniformBuffer(VisibleMeshObject{ -1, glm::mat4(1.0f), nullptr, nullptr });
						Renderer::Submit(axis_material, MeshPrimitive{ GetAxisVertexArray(), PrimitiveType::Triangles });
					}

					/* 各类型实体的类型 gizmo（光源 / 相机 / 反射探针 / 精灵） */
					DrawEntityGizmos(render_view, entity_gizmo_material, GetPosition(), gizmo_screen_scale);
				}
				render_pass_info->Unbind();
			}
			);

		// frame_graph->ExportGraphviz("framegraph.txt");
		render_view.Prepare();

		/* 输出 */
		render_view.SetRenderTargetHandle(lighting_pass->GetData().LightingResult);

		return true;
	}

	int32_t EditorCamera::PickingEntityByPixelPos(uint32_t x, uint32_t y) const
	{
		int32_t entity_id = -1;
		if (const auto& gbuffer_framebuffer = m_pRenderView->GetPassFrameBuffer("GBufferPass"))
		{
			gbuffer_framebuffer->ReadPixel(6, x, y, { PixelFormat::R_Integer, PixelType::Int }, &entity_id);
		}
		return entity_id;
	}



	glm::quat EditorCamera::GetOrientation() const
	{
		return glm::quat(glm::vec3(-m_Pitch, -m_Yaw, 0.0f));
	}

	void EditorCamera::SetViewMatrix(const glm::mat4& view_mat)
	{
		PROFILE_FUNCTION();

		if (view_mat != m_ViewMatrix)
		{
			m_ViewMatrix = view_mat;

			/* 根据ViewMatrix恢复Pitch和Yaw */
			/* todo： 数据未恢复全，仍存在问题，需要重新维护这部分 */
			const auto orientation = glm::toQuat(view_mat);
			const glm::vec3 euler_angles = glm::eulerAngles(orientation);
			m_Pitch = -euler_angles.x;
			m_Yaw = -euler_angles.y;
			m_UpDirection = glm::rotate(orientation, glm::vec3(0.0f, 1.0f, 0.0f));
			m_RightDirection = glm::rotate(orientation, glm::vec3(1.0f, 0.0f, 0.0f));
			m_ForwardDirection = glm::rotate(orientation, glm::vec3(0.0f, 0.0f, -1.0f));
		}
	}

	/* 视图指示器：直接设置轨道朝向（俯仰按导航限位夹取），聚焦点与距离不变 ——
	 * 位置是派生量，下一帧自动落到"枢轴 - 前向 × 距离"的新视点 */
	void EditorCamera::SetOrbitAngles(float yaw, float pitch)
	{
		PROFILE_FUNCTION();

		m_Yaw = yaw;
		m_Pitch = glm::clamp(pitch, -kMaxPitch, kMaxPitch);
		UpdateCameraDirections();

		m_IsDirty = true;
	}

	/* 视图指示器拖拽：像素增量走与鼠标轨道相同的灵敏度与速度系数（手感一致） */
	void EditorCamera::OrbitByPixelDelta(const glm::vec2& pixel_delta)
	{
		PROFILE_FUNCTION();

		OnMouseRotate(pixel_delta * kMouseSensitivity);
	}

	void EditorCamera::UpdateProjectionMatrix()
	{
		PROFILE_FUNCTION();

		m_ProjectionMatrix = MakeReversedZProjection(glm::perspective(glm::radians(m_Fov), m_AspectRatio, m_NearClip, m_FarClip));
	}

	void EditorCamera::UpdateViewMatrix()
	{
		PROFILE_FUNCTION();

		if (m_IsDirty)
		{
			/* 位置恒为派生量：聚焦点 - 前向 * 距离。飞行中的平移与转向也维护这组关系
			 * （见 UpdateFly），因此两种导航状态共享同一套参数，进出飞行无需交接。 */
			m_Position = m_FocalPoint - m_ForwardDirection * m_Distance;
			m_ViewMatrix = glm::inverse(glm::translate(glm::mat4(1.0f), m_Position) * glm::toMat4(GetOrientation()));

			m_IsDirty = false;
		}
	}

	void EditorCamera::UpdateCameraDirections()
	{
		PROFILE_FUNCTION();

		const auto orientation = GetOrientation();
		m_UpDirection = glm::rotate(orientation, glm::vec3(0.0f, 1.0f, 0.0f));
		m_RightDirection = glm::rotate(orientation, glm::vec3(1.0f, 0.0f, 0.0f));
		m_ForwardDirection = glm::rotate(orientation, glm::vec3(0.0f, 0.0f, -1.0f));
	}

	void EditorCamera::OnMousePan(const glm::vec2& delta)
	{
		PROFILE_FUNCTION();

		const auto speed = PanSpeed();
		m_FocalPoint += -GetRightDir() * delta.x * speed.x * m_Distance;
		m_FocalPoint += GetUpDir() * delta.y * speed.y * m_Distance;

		m_IsDirty = true;
	}

	void EditorCamera::OnMouseRotate(const glm::vec2& delta)
	{
		PROFILE_FUNCTION();

		const float yaw_sign = GetUpDir().y < 0 ? -1.0f : 1.0f;
		m_Yaw += yaw_sign * delta.x * RotateSpeed();
		m_Pitch = glm::clamp(m_Pitch + delta.y * RotateSpeed(), -kMaxPitch, kMaxPitch);

		UpdateCameraDirections();

		m_IsDirty = true;
	}

	void EditorCamera::OnMouseZoom(float delta)
	{
		PROFILE_FUNCTION();

		m_Distance = glm::clamp(m_Distance - delta * ZoomSpeed(), kMinDistance, kMaxDistance);

		m_IsDirty = true;
	}

	/* 滚轮缩放：乘性步进（滚一格约 ±10%），触控板的小增量也能平滑生效 */
	void EditorCamera::OnMouseWheelZoom(float wheel)
	{
		PROFILE_FUNCTION();

		m_Distance = glm::clamp(m_Distance * std::pow(kWheelZoomStep, wheel), kMinDistance, kMaxDistance);

		m_IsDirty = true;
	}

	/* 滚轮调飞行速度（飞行中）：乘性步进 + 上下限 */
	void EditorCamera::AdjustMoveSpeed(float wheel)
	{
		PROFILE_FUNCTION();

		m_MoveSpeed = glm::clamp(m_MoveSpeed * std::pow(kWheelSpeedStep, wheel), kMinFlySpeed, kMaxFlySpeed);
	}

	/* 聚焦：轨道中心移到目标点，距离按包围球半径取景（水平 / 垂直视场角取紧的一侧，含边距） */
	void EditorCamera::FocusOn(const glm::vec3& target, float radius)
	{
		PROFILE_FUNCTION();

		m_FocalPoint = target;

		const float half_fov_v = glm::radians(m_Fov) * 0.5f;
		const float half_fov_h = std::atan(std::tan(half_fov_v) * glm::max(m_AspectRatio, 0.01f));
		const float half_fov = glm::max(glm::min(half_fov_v, half_fov_h), 0.01f);
		m_Distance = glm::clamp(radius / std::tan(half_fov) * 1.2f, kMinDistance, kMaxDistance);

		m_IsDirty = true;
	}

	glm::vec2 EditorCamera::PanSpeed() const
	{
		PROFILE_FUNCTION();

		const float x = std::min(m_ViewportRegion.Width / 1000.0f, 2.4f); // max = 2.4f
		const float speed_x = 0.0366f * (x * x) - 0.1778f * x + 0.3021f;

		const float y = std::min(m_ViewportRegion.Height / 1000.0f, 2.4f); // max = 2.4f
		const float speed_y = 0.0366f * (y * y) - 0.1778f * y + 0.3021f;

		return glm::vec2(speed_x, speed_y);
	}

	float EditorCamera::RotateSpeed() const
	{
		return 0.8f;
	}

	float EditorCamera::ZoomSpeed() const
	{
		PROFILE_FUNCTION();

		float distance = m_Distance * 0.2f;
		distance = std::max(distance, 0.0f);
		float speed = distance * distance;
		speed = std::min(speed, 100.0f); // max speed = 100
		return speed;
	}
}
