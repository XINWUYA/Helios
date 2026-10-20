#include "Pch.h"
#include "SceneGizmos.h"
#include "EditorBuiltinCamera.h"
#include "TransformGizmo.h"
#include <glm/gtx/quaternion.hpp>
#include <cmath>

#include "Helios/Application/Application.h"
#include "Helios/Core/AssetManager.h"
#include "Helios/Common/Math.h"
#include "Helios/Common/Utils.h"
#include "Helios/Scene/Camera.h"
#include "Helios/Scene/Material.h"
#include "Helios/Scene/Scene.h"
#include "Helios/Scene/SkeletonModel.h"

/* 场景 gizmo 的完整实现（对外接口见 SceneGizmos.h）：
 *   常量 / 顶点辅助 → 网格与坐标轴几何 → 各类型实体图标几何 → 顶点数组缓存 →
 *   材质 → 变换拆解工具 → 各部分的提交函数（网格 / 世界轴 / 每类型实体图标）。 */
namespace Helios
{
	namespace
	{
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
		 * 两种尺寸语义：参数驱动（点光 / 聚光 / 相机 / 反射探针盒 = 真世界尺度，不吃实体缩放）
		 * 和图标驱动（屏上尺寸随距离收缩；平行光不响应缩放、精灵按轴完整响应，
		 * 反射探针的轴心点保持屏幕恒定）。 */
		const glm::vec3 kLightGizmoColor{ 1.00f, 0.84f, 0.42f };	/* 暖黄：光源 */
		const glm::vec3 kCameraGizmoColor{ 0.66f, 0.82f, 1.00f };	/* 淡蓝：相机 */
		const glm::vec3 kProbeGizmoColor{ 0.45f, 0.90f, 0.78f };	/* 青绿：反射探针 */
		const glm::vec3 kProbeInnerGizmoColor{ 0.25f, 0.50f, 0.43f };	/* 过渡带核心区（外框同色暗版） */
		const glm::vec3 kSpriteGizmoColor{ 0.95f, 0.62f, 0.85f };	/* 粉紫：精灵 */

		/* 线框圆段数：真世界尺度的球 / 锥可占屏很大，段数低时折线棱角明显 */
		constexpr int32_t kGizmoCircleSegments = 128;
		constexpr int32_t kGizmoDotSegments = 16;			/* 轴心点小圆段数 */
		constexpr float kGizmoDotRadius = 0.045f;			/* 轴心点半径（各类型共用） */

		/* 平行光（太阳）：圆环 + 8 条长短交替射线 + 方向杆 */
		constexpr float kSunRingRadius = 0.34f;
		constexpr float kSunRayLong = 0.58f;
		constexpr float kSunRayShort = 0.47f;
		constexpr float kSunRodLength = 1.25f;

		/* 点光：单位球线框（半径 1 —— 绘制时 × range 得衰减半径）；
		 * 反射探针：方盒（Unity 式） */
		constexpr float kPointGizmoUnitRadius = 1.0f;
		constexpr float kProbeBoxHalfExtent = 0.5f;

		/* 聚光：单位锥（顶点在原点、轴 = 本地 -Z、长 1、底半径 1）；绘制时按
		 * range 与锥角做各向异性缩放：横向 × range·tan(锥角/2)、轴向 × range */
		constexpr float kSpotGizmoUnitLength = 1.0f;
		/* range 下限保护：防零尺寸缩放矩阵让线框整个消失 */
		constexpr float kGizmoMinRange = 1.0e-3f;
		/* 聚光内锥配色：外锥同色的压暗档（内外双锥同屏时区分） */
		const glm::vec3 kLightGizmoInnerColor = kLightGizmoColor * 0.55f;

		/* 相机：视锥（近 / 远矩形 + 棱 + 上向标记）+ 成像背盒。视锥按规范形状（45° / 16:9）建，
		 * 绘制时乘"真实量 / 规范量"因子张开到真实的 near / far 与 FOV / 宽高比。
		 * 注意：视锥长度 = far，编辑器相机的远裁剪面要给得足够大（见 EditorCamera）。 */
		constexpr float kCameraGizmoNear = 0.5f;	/* 缺省近平面显示位置（无相机组件时兜底） */
		constexpr float kCameraGizmoFar = 1.5f;		/* 规范形状的显示总长（远平面所在的 z；亦作 far 的尺度归一） */
		constexpr float kCameraGizmoMarkerHeight = 0.24f;	/* 远平面上向标记高度 */
		constexpr float kCameraGizmoBodyNear = 0.08f;		/* 机身盒沿 +Z 的范围 */
		constexpr float kCameraGizmoBodyFar = 0.5f;
		constexpr float kCameraGizmoBodyHalfW = 0.2f;
		constexpr float kCameraGizmoBodyHalfH = 0.14f;
		constexpr float kCameraGizmoDefaultTanHalfFov = 0.4142f;	/* tan(45° / 2)：规范形状的张开 */
		constexpr float kCameraGizmoDefaultAspect = 1.778f;		/* 规范形状的宽高比（16:9） */

		/* 精灵：框 + 对角线（图像占位样式） */
		constexpr float kSpriteGizmoHalfExtent = 0.5f;

		/* 不吃网格淡出的 gizmo：范围放到天边（远大于任何真实距离，且两值不等 ——
		 * float 在 1e9 量级 +1 会被舍掉） */
		constexpr float kGizmoNoFadeInner = 1.0e6f;
		constexpr float kGizmoNoFadeOuter = 2.0e6f;

		/* === 骨骼节点预览（Debug View 的 Skeleton 档）===
		 * 关节画成屏幕恒定大小的圆点，父子关节之间拉一条线；关深度测试通看
		 * 网格内部（骨骼本来就埋在模型里）。 */
		const glm::vec3 kBoneLineColor{ 1.00f, 0.66f, 0.30f };	/* 橙：骨骼连线 */
		const glm::vec3 kJointNodeColor{ 1.00f, 0.86f, 0.52f };	/* 亮黄：关节节点 */
		constexpr float kBoneLineWidth = 3.0f;			/* 连线宽度（逻辑像素） */
		constexpr float kJointNodePixelSize = 36.0f;	/* 圆点像素基准（× 轴心点半径 ≈ 1.6 像素） */

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
		 * 几何按“光向 = -Y”的规范朝向建，绘制时旋到实际光向（见 SubmitLightGizmos） */
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

		/* 点光：三正交圆环（单位球线框；半径 1，绘制时 × range 得衰减半径）。
		 * 轴心点不在这一组里 —— 它保持屏幕恒定小尺寸、单独提交（见 SubmitLightGizmos） */
		std::vector<float> BuildPointLightGizmoVertices()
		{
			std::vector<float> vertices;
			PushCircleBand(vertices, glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f),
				kPointGizmoUnitRadius, kGizmoCircleSegments, kLightGizmoColor);
			PushCircleBand(vertices, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f),
				kPointGizmoUnitRadius, kGizmoCircleSegments, kLightGizmoColor);
			PushCircleBand(vertices, glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(1.0f, 0.0f, 0.0f),
				kPointGizmoUnitRadius, kGizmoCircleSegments, kLightGizmoColor);
			return vertices;
		}

		/* 聚光：单位锥线框（顶点在原点、轴 = 本地 -Z、长 1、底半径 1）。内 / 外锥共用同一份几何，
		 * 各用一套各向异性缩放张开到真实锥角（外锥满色 / 内锥压暗）。 */
		std::vector<float> BuildSpotLightConeVertices(const glm::vec3& color)
		{
			std::vector<float> vertices;
			const glm::vec3 axis(0.0f, 0.0f, -1.0f);
			const glm::vec3 axis_u(1.0f, 0.0f, 0.0f);
			const glm::vec3 axis_v(0.0f, 1.0f, 0.0f);
			const glm::vec3 base_center = axis * kSpotGizmoUnitLength;

			PushCircleBand(vertices, base_center, axis_u, axis_v, kSpotGizmoUnitLength,
				kGizmoCircleSegments, color);

			for (int32_t side = 0; side < 4; ++side)
			{
				const float angle = 0.5f * PI * static_cast<float>(side);
				const glm::vec3 rim = base_center
					+ (axis_u * std::cos(angle) + axis_v * std::sin(angle)) * kSpotGizmoUnitLength;
				PushLineBand(vertices, glm::vec3(0.0f), rim, color);
			}
			return vertices;
		}

		/* 光源的位置标记：轴心点（点光 / 聚光）—— 屏幕恒定小尺寸、不随影响范围变形 */
		std::vector<float> BuildLightPivotDotVertices()
		{
			std::vector<float> vertices;
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

		/* 反射探针：影响范围盒（单位盒，提交时按 BoxSize 缩放成真尺度）——
		 * 外框与过渡内框共用几何、两套颜色 */
		std::vector<float> BuildProbeBoxVertices(const glm::vec3& color)
		{
			std::vector<float> vertices;
			PushBoxBand(vertices, glm::vec3(0.0f), glm::vec3(kProbeBoxHalfExtent), color);
			return vertices;
		}

		/* 探针轴心点：位置标记，屏幕恒定小尺寸（不随 BoxSize 变形） */
		std::vector<float> BuildProbePivotDotVertices()
		{
			std::vector<float> vertices;
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

		/* === 变换 gizmo 的实体网格（单位尺寸，绘制时由世界矩阵缩放；见 TransformGizmo.h）===
		 * 顶点色恒为白（配色走材质的统一色），a_Dir / a_Side 填 0（那是轴带的屏幕展开参数）。
		 * 绕序无所谓 —— gizmo 材质关面剔除。 */

		/* 圆锥：底面半径 1、高 3、沿 +Z，尖端在 +Z 末端（平移箭头） */
		std::vector<float> BuildTransformConeVertices()
		{
			constexpr int32_t kSegments = 32;
			constexpr float kHeight = 3.0f;
			const glm::vec3 color(1.0f);
			const glm::vec3 apex(0.0f, 0.0f, kHeight);

			std::vector<float> vertices;
			vertices.reserve(static_cast<size_t>(kSegments) * 6 * 10);
			for (int32_t segment = 0; segment < kSegments; ++segment)
			{
				const float angle_from = 2.0f * PI * static_cast<float>(segment) / static_cast<float>(kSegments);
				const float angle_to = 2.0f * PI * static_cast<float>(segment + 1) / static_cast<float>(kSegments);
				const glm::vec3 from(std::cos(angle_from), std::sin(angle_from), 0.0f);
				const glm::vec3 to(std::cos(angle_to), std::sin(angle_to), 0.0f);

				PushVertex(vertices, apex, color);
				PushVertex(vertices, from, color);
				PushVertex(vertices, to, color);

				/* 底面封口：从背面看箭头也不漏空 */
				PushVertex(vertices, glm::vec3(0.0f), color);
				PushVertex(vertices, to, color);
				PushVertex(vertices, from, color);
			}
			return vertices;
		}

		/* 圆柱：半径 1、长 1、从 0 到 +Z（轴杆与缩放的轴共用） */
		std::vector<float> BuildTransformCylinderVertices()
		{
			constexpr int32_t kSegments = 12;
			const glm::vec3 color(1.0f);
			const glm::vec3 bottom_center(0.0f, 0.0f, 0.0f);
			const glm::vec3 top_center(0.0f, 0.0f, 1.0f);

			std::vector<float> vertices;
			vertices.reserve(static_cast<size_t>(kSegments) * 12 * 10);
			for (int32_t segment = 0; segment < kSegments; ++segment)
			{
				const float angle_from = 2.0f * PI * static_cast<float>(segment) / static_cast<float>(kSegments);
				const float angle_to = 2.0f * PI * static_cast<float>(segment + 1) / static_cast<float>(kSegments);
				const glm::vec3 bottom_from(std::cos(angle_from), std::sin(angle_from), 0.0f);
				const glm::vec3 bottom_to(std::cos(angle_to), std::sin(angle_to), 0.0f);
				const glm::vec3 top_from(bottom_from.x, bottom_from.y, 1.0f);
				const glm::vec3 top_to(bottom_to.x, bottom_to.y, 1.0f);

				PushVertex(vertices, bottom_from, color);
				PushVertex(vertices, top_from, color);
				PushVertex(vertices, bottom_to, color);

				PushVertex(vertices, bottom_to, color);
				PushVertex(vertices, top_from, color);
				PushVertex(vertices, top_to, color);

				PushVertex(vertices, top_center, color);
				PushVertex(vertices, top_to, color);
				PushVertex(vertices, top_from, color);

				PushVertex(vertices, bottom_center, color);
				PushVertex(vertices, bottom_from, color);
				PushVertex(vertices, bottom_to, color);
			}
			return vertices;
		}

		/* 圆环：大半径 1、管半径由调用方给（彩色环 = kRingTubeRatio，白色外环更细），
		 * 铺在 XY 平面（法线 +Z），64 个主段 × 8 个管段（整环等比缩放即可保持管径比例） */
		/* fade_min：背侧的最低透明度（0 = 完全隐藏，1 = 不淡出）；
		 * phase：相机在该环平面内的方位角。逐段按余弦算淡出，写进顶点色的 r 通道。 */
		std::vector<float> BuildTransformRingVertices(float tube_radius, float fade_min, float phase)
		{
			constexpr int32_t kMajorSegments = TransformGizmo::kRingSegments;
			constexpr int32_t kMinorSegments = 8;

			const auto point_at = [tube_radius](float major, float minor)
			{
				const float radial = 1.0f + tube_radius * std::cos(minor);
				return glm::vec3(radial * std::cos(major), radial * std::sin(major),
					tube_radius * std::sin(minor));
			};

			std::vector<float> vertices;
			vertices.reserve(static_cast<size_t>(kMajorSegments) * kMinorSegments * 6 * 10);
			for (int32_t major = 0; major < kMajorSegments; ++major)
			{
				const float major_from = 2.0f * PI * static_cast<float>(major) / static_cast<float>(kMajorSegments);
				const float major_to = 2.0f * PI * static_cast<float>(major + 1) / static_cast<float>(kMajorSegments);

				/* 段中点相对相机的朝向：正对时满色，正背对时降到 fade_min */
				const float near_factor = 0.5f + 0.5f * std::cos((major_from + major_to) * 0.5f - phase);
				const glm::vec3 color(fade_min + (1.0f - fade_min) * near_factor);

				for (int32_t minor = 0; minor < kMinorSegments; ++minor)
				{
					const float minor_from = 2.0f * PI * static_cast<float>(minor) / static_cast<float>(kMinorSegments);
					const float minor_to = 2.0f * PI * static_cast<float>(minor + 1) / static_cast<float>(kMinorSegments);

					const glm::vec3 p00 = point_at(major_from, minor_from);
					const glm::vec3 p01 = point_at(major_from, minor_to);
					const glm::vec3 p10 = point_at(major_to, minor_from);
					const glm::vec3 p11 = point_at(major_to, minor_to);

					/* side 存参数角：片元按它和相机方位之差做背侧淡出 */
					PushVertex(vertices, p00, color, glm::vec3(0.0f), major_from);
					PushVertex(vertices, p10, color, glm::vec3(0.0f), major_from);
					PushVertex(vertices, p11, color, glm::vec3(0.0f), major_from);

					PushVertex(vertices, p00, color, glm::vec3(0.0f), major_from);
					PushVertex(vertices, p11, color, glm::vec3(0.0f), major_from);
					PushVertex(vertices, p01, color, glm::vec3(0.0f), major_from);
				}
			}
			return vertices;
		}

		/* 半环：参数角落在 [half * π, (half + 1) * π) 的管状圆环。
		 * 拆成两个半环后，按相机在环平面内的方位决定哪一半朝前，背侧那一半用淡色。 */

		/* 平面手柄的空心边框：单位正方形（XY 平面、边长 1）上四条宽 0.07 的边。
		 * 内部完全空着，不依赖 alpha 混合也能透出背后的场景。 */
		std::vector<float> BuildTransformPlaneFrameVertices()
		{
			const glm::vec3 color(1.0f);
			std::vector<float> vertices;
			const float h = 0.5f;
			const float w = 0.07f;
			const auto push_quad = [&](const glm::vec3& a, const glm::vec3& b,
				const glm::vec3& c, const glm::vec3& d)
			{
				PushVertex(vertices, a, color);
				PushVertex(vertices, b, color);
				PushVertex(vertices, c, color);
				PushVertex(vertices, a, color);
				PushVertex(vertices, c, color);
				PushVertex(vertices, d, color);
			};

			push_quad({ -h, h - w, 0.0f }, { h, h - w, 0.0f }, { h, h, 0.0f }, { -h, h, 0.0f });
			push_quad({ -h, -h, 0.0f }, { -h, -h + w, 0.0f }, { h, -h + w, 0.0f }, { h, -h, 0.0f });
			push_quad({ -h, -h + w, 0.0f }, { -h + w, -h + w, 0.0f }, { -h + w, h - w, 0.0f },
				{ -h, h - w, 0.0f });
			push_quad({ h - w, -h + w, 0.0f }, { h, -h + w, 0.0f }, { h, h - w, 0.0f },
				{ h - w, h - w, 0.0f });
			return vertices;
		}

		/* 中心淡圆：XY 平面上半径 1 的圆盘（trackball 区域） */
		std::vector<float> BuildTransformDiscVertices()
		{
			constexpr int32_t kSegments = TransformGizmo::kRingSegments;
			const glm::vec3 color(1.0f);

			std::vector<float> vertices;
			vertices.reserve(static_cast<size_t>(kSegments) * 3 * 10);
			for (int32_t segment = 0; segment < kSegments; ++segment)
			{
				const float angle_from = 2.0f * PI * static_cast<float>(segment) / static_cast<float>(kSegments);
				const float angle_to = 2.0f * PI * static_cast<float>(segment + 1) / static_cast<float>(kSegments);

				PushVertex(vertices, glm::vec3(0.0f), color);
				PushVertex(vertices, glm::vec3(std::cos(angle_from), std::sin(angle_from), 0.0f), color);
				PushVertex(vertices, glm::vec3(std::cos(angle_to), std::sin(angle_to), 0.0f), color);
			}
			return vertices;
		}

		/* 立方体：边长 1、中心在原点（缩放手柄） */
		std::vector<float> BuildTransformCubeVertices()
		{
			const glm::vec3 color(1.0f);
			std::vector<float> vertices;
			vertices.reserve(36 * 10);

			/* 六个面：法线轴 + 正负侧，面内两个正交方向 */
			for (int32_t axis = 0; axis < 3; ++axis)
			{
				const int32_t u = (axis + 1) % 3;
				const int32_t v = (axis + 2) % 3;
				for (int32_t side = 0; side < 2; ++side)
				{
					const float offset = side == 0 ? 0.5f : -0.5f;
					const float winding = side == 0 ? 1.0f : -1.0f;

					glm::vec3 corners[4];
					for (int32_t corner = 0; corner < 4; ++corner)
					{
						corners[corner] = glm::vec3(0.0f);
						corners[corner][axis] = offset;
						corners[corner][u] = ((corner == 0 || corner == 3) ? -0.5f : 0.5f) * winding;
						corners[corner][v] = (corner < 2 ? -0.5f : 0.5f) * winding;
					}

					PushVertex(vertices, corners[0], color);
					PushVertex(vertices, corners[1], color);
					PushVertex(vertices, corners[2], color);

					PushVertex(vertices, corners[0], color);
					PushVertex(vertices, corners[2], color);
					PushVertex(vertices, corners[3], color);
				}
			}
			return vertices;
		}

		/* 平行四边形：边长 1、中心在原点、铺在 XY 平面（平移的平面手柄）。
		 * 提交时由"两根轴的方向组成的基"摆位，四条边分别平行于那两根轴 */
		std::vector<float> BuildTransformQuadVertices()
		{
			const glm::vec3 color(1.0f);
			const glm::vec3 corners[4] = {
				{ -0.5f, -0.5f, 0.0f }, { 0.5f, -0.5f, 0.0f },
				{ 0.5f, 0.5f, 0.0f }, { -0.5f, 0.5f, 0.0f } };

			std::vector<float> vertices;
			vertices.reserve(6 * 10);
			for (const int32_t index : { 0, 1, 2, 0, 2, 3 })
				PushVertex(vertices, corners[index], color);
			return vertices;
		}

		/* 球：半径 1、中心在原点（平移的中心球）。经线 × 纬线铺满整球，
		 * 关面剔除所以背面不补 —— 正面看是完整圆盘 */
		std::vector<float> BuildTransformSphereVertices()
		{
			constexpr int32_t kLongitudes = 24;
			constexpr int32_t kLatitudes = 12;
			const glm::vec3 color(1.0f);

			const auto point_at = [](float longitude, float latitude)
			{
				const float ring_radius = std::cos(latitude);
				return glm::vec3(ring_radius * std::cos(longitude), ring_radius * std::sin(longitude),
					std::sin(latitude));
			};

			std::vector<float> vertices;
			vertices.reserve(static_cast<size_t>(kLongitudes) * kLatitudes * 6 * 10);
			for (int32_t latitude = 0; latitude < kLatitudes; ++latitude)
			{
				const float lat_from = -0.5f * PI + PI * static_cast<float>(latitude) / static_cast<float>(kLatitudes);
				const float lat_to = -0.5f * PI + PI * static_cast<float>(latitude + 1) / static_cast<float>(kLatitudes);

				for (int32_t longitude = 0; longitude < kLongitudes; ++longitude)
				{
					const float lon_from = 2.0f * PI * static_cast<float>(longitude) / static_cast<float>(kLongitudes);
					const float lon_to = 2.0f * PI * static_cast<float>(longitude + 1) / static_cast<float>(kLongitudes);

					const glm::vec3 p00 = point_at(lon_from, lat_from);
					const glm::vec3 p01 = point_at(lon_from, lat_to);
					const glm::vec3 p10 = point_at(lon_to, lat_from);
					const glm::vec3 p11 = point_at(lon_to, lat_to);

					PushVertex(vertices, p00, color);
					PushVertex(vertices, p10, color);
					PushVertex(vertices, p11, color);

					PushVertex(vertices, p00, color);
					PushVertex(vertices, p11, color);
					PushVertex(vertices, p01, color);
				}
			}
			return vertices;
		}

		/* 角度弧：环平面内的一段带状圆环（世界坐标直接给出，模型矩阵取单位阵）。
		 * 固定 kRingSegments 段、整段扫过 [start, start + sweep] —— 顶点数恒定，
		 * 每次拖拽逐帧原地覆盖同一份缓冲（与骨骼连线同一套做法） */

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

		SharedPtr<DeviceVertexArray> GetSpotLightOuterConeVertexArray()
		{
			static SharedPtr<DeviceVertexArray> vertex_array;
			if (!vertex_array)
				vertex_array = MakeGizmoVertexArray("EditorSpotLightOuterCone_VertexArray",
					BuildSpotLightConeVertices(kLightGizmoColor));
			return vertex_array;
		}

		SharedPtr<DeviceVertexArray> GetSpotLightInnerConeVertexArray()
		{
			static SharedPtr<DeviceVertexArray> vertex_array;
			if (!vertex_array)
				vertex_array = MakeGizmoVertexArray("EditorSpotLightInnerCone_VertexArray",
					BuildSpotLightConeVertices(kLightGizmoInnerColor));
			return vertex_array;
		}

		SharedPtr<DeviceVertexArray> GetLightPivotDotVertexArray()
		{
			static SharedPtr<DeviceVertexArray> vertex_array;
			if (!vertex_array)
				vertex_array = MakeGizmoVertexArray("EditorLightPivotDot_VertexArray", BuildLightPivotDotVertices());
			return vertex_array;
		}

		/* 视锥几何随近平面比例（near / far）变化（几十个顶点）：参数变化时重建并缓存
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

		SharedPtr<DeviceVertexArray> GetProbeBoxVertexArray()
		{
			static SharedPtr<DeviceVertexArray> vertex_array;
			if (!vertex_array)
				vertex_array = MakeGizmoVertexArray("EditorProbeBox_VertexArray",
					BuildProbeBoxVertices(kProbeGizmoColor));
			return vertex_array;
		}

		SharedPtr<DeviceVertexArray> GetProbeInnerBoxVertexArray()
		{
			static SharedPtr<DeviceVertexArray> vertex_array;
			if (!vertex_array)
				vertex_array = MakeGizmoVertexArray("EditorProbeInnerBox_VertexArray",
					BuildProbeBoxVertices(kProbeInnerGizmoColor));
			return vertex_array;
		}

		SharedPtr<DeviceVertexArray> GetProbePivotDotVertexArray()
		{
			static SharedPtr<DeviceVertexArray> vertex_array;
			if (!vertex_array)
				vertex_array = MakeGizmoVertexArray("EditorProbePivotDot_VertexArray",
					BuildProbePivotDotVertices());
			return vertex_array;
		}

		SharedPtr<DeviceVertexArray> GetSpriteGizmoVertexArray()
		{
			static SharedPtr<DeviceVertexArray> vertex_array;
			if (!vertex_array)
				vertex_array = MakeGizmoVertexArray("EditorSpriteGizmo_VertexArray", BuildSpriteGizmoVertices());
			return vertex_array;
		}

		/* 关节圆点：与实体轴心点同一套几何（三个正交小圆），只是换了颜色 */
		std::vector<float> BuildJointNodeVertices()
		{
			std::vector<float> vertices;
			PushPivotDot(vertices, glm::vec3(0.0f), kJointNodeColor);
			return vertices;
		}

		SharedPtr<DeviceVertexArray> GetJointNodeVertexArray()
		{
			static SharedPtr<DeviceVertexArray> vertex_array;
			if (!vertex_array)
				vertex_array = MakeGizmoVertexArray("EditorSkeletonJoint_VertexArray", BuildJointNodeVertices());
			return vertex_array;
		}

		/* === 变换 gizmo 的顶点数组：每种单位网格各缓存一份 === */

		SharedPtr<DeviceVertexArray> GetTransformConeVertexArray()
		{
			static SharedPtr<DeviceVertexArray> vertex_array;
			if (!vertex_array)
				vertex_array = MakeGizmoVertexArray("EditorTransformGizmoCone_VertexArray", BuildTransformConeVertices());
			return vertex_array;
		}

		SharedPtr<DeviceVertexArray> GetTransformCylinderVertexArray()
		{
			static SharedPtr<DeviceVertexArray> vertex_array;
			if (!vertex_array)
				vertex_array = MakeGizmoVertexArray("EditorTransformGizmoCylinder_VertexArray",
					BuildTransformCylinderVertices());
			return vertex_array;
		}


		/* 白色外环用同一套圆环几何，只是管径更细 */

		SharedPtr<DeviceVertexArray> GetTransformRingDynamicVertexArray(int32_t slot,
			const std::vector<float>& vertices)
		{
			static SharedPtr<DeviceVertexArray> arrays[6];
			static size_t capacities[6] = { 0, 0, 0, 0, 0, 0 };
			const int32_t index = std::clamp(slot, 0, 5);

			if (vertices.empty())
				return nullptr;

			if (!arrays[index] || vertices.size() != capacities[index])
			{
				arrays[index] = MakeGizmoVertexArray(
					"EditorTransformGizmoRing_VertexArray_" + std::to_string(index), vertices);
				capacities[index] = vertices.size();
				return arrays[index];
			}

			arrays[index]->GetVertexBuffers().front()->SetData(vertices.data(),
				static_cast<uint32_t>(vertices.size() * sizeof(float)));
			return arrays[index];
		}

		SharedPtr<DeviceVertexArray> GetTransformPlaneFrameVertexArray()
		{
			static SharedPtr<DeviceVertexArray> vertex_array;
			if (!vertex_array)
				vertex_array = MakeGizmoVertexArray("EditorTransformGizmoPlaneFrame_VertexArray",
					BuildTransformPlaneFrameVertices());
			return vertex_array;
		}

		SharedPtr<DeviceVertexArray> GetTransformDiscVertexArray()
		{
			static SharedPtr<DeviceVertexArray> vertex_array;
			if (!vertex_array)
				vertex_array = MakeGizmoVertexArray("EditorTransformGizmoDisc_VertexArray",
					BuildTransformDiscVertices());
			return vertex_array;
		}

		SharedPtr<DeviceVertexArray> GetTransformOuterRingVertexArray()
		{
			static SharedPtr<DeviceVertexArray> vertex_array;
			if (!vertex_array)
				vertex_array = MakeGizmoVertexArray("EditorTransformGizmoOuterRing_VertexArray",
					BuildTransformRingVertices(TransformGizmo::kOuterRingTubeRatio, 1.0f, 0.0f));
			return vertex_array;
		}

		SharedPtr<DeviceVertexArray> GetTransformCubeVertexArray()
		{
			static SharedPtr<DeviceVertexArray> vertex_array;
			if (!vertex_array)
				vertex_array = MakeGizmoVertexArray("EditorTransformGizmoCube_VertexArray", BuildTransformCubeVertices());
			return vertex_array;
		}

		SharedPtr<DeviceVertexArray> GetTransformQuadVertexArray()
		{
			static SharedPtr<DeviceVertexArray> vertex_array;
			if (!vertex_array)
				vertex_array = MakeGizmoVertexArray("EditorTransformGizmoQuad_VertexArray", BuildTransformQuadVertices());
			return vertex_array;
		}

		SharedPtr<DeviceVertexArray> GetTransformSphereVertexArray()
		{
			static SharedPtr<DeviceVertexArray> vertex_array;
			if (!vertex_array)
				vertex_array = MakeGizmoVertexArray("EditorTransformGizmoSphere_VertexArray",
					BuildTransformSphereVertices());
			return vertex_array;
		}

		/* 角度弧的顶点每帧由当前拖拽角生成：顶点数恒定，只原地覆盖缓冲 */
		/* 旋转角度扇区：圆心 + 弧上采样点组成的三角扇，铺满整块面积 */
		std::vector<float> BuildTransformSectorVertices(const glm::vec3& origin, const glm::vec3& u,
			const glm::vec3& v, float radius, float start, float sweep)
		{
			constexpr int32_t kSegments = TransformGizmo::kRingSegments;
			const glm::vec3 color(1.0f);

			std::vector<float> vertices;
			vertices.reserve(static_cast<size_t>(kSegments) * 3 * 10);
			for (int32_t segment = 0; segment < kSegments; ++segment)
			{
				const float angle_from = start + sweep * static_cast<float>(segment) / static_cast<float>(kSegments);
				const float angle_to = start + sweep * static_cast<float>(segment + 1) / static_cast<float>(kSegments);
				const glm::vec3 point_from = origin
					+ (u * std::cos(angle_from) + v * std::sin(angle_from)) * radius;
				const glm::vec3 point_to = origin
					+ (u * std::cos(angle_to) + v * std::sin(angle_to)) * radius;

				PushVertex(vertices, origin, color);
				PushVertex(vertices, point_from, color);
				PushVertex(vertices, point_to, color);
			}
			return vertices;
		}

		/* 拖拽轨迹虚线：按固定世界长度切段、段间留空 */
		std::vector<float> BuildTransformDashVertices(const glm::vec3& from, const glm::vec3& to,
			float dash_length)
		{
			std::vector<float> vertices;
			const glm::vec3 delta = to - from;
			const float length = glm::length(delta);
			if (length < 1.0e-6f)
				return vertices;

			const glm::vec3 direction = delta / length;
			const float dash = std::max(dash_length, 1.0e-6f);
			const float gap = dash * 0.7f;
			const glm::vec3 color(1.0f);

			float cursor = 0.0f;
			while (cursor < length)
			{
				const float segment_end = std::min(cursor + dash, length);
				PushVertex(vertices, from + direction * cursor, color);
				PushVertex(vertices, from + direction * segment_end, color);
				cursor = segment_end + gap;
			}
			return vertices;
		}

		SharedPtr<DeviceVertexArray> GetTransformArcVertexArray(const std::vector<float>& vertices)
		{
			static SharedPtr<DeviceVertexArray> vertex_array;
			static size_t capacity = 0;

			if (vertices.empty())
				return nullptr;

			if (!vertex_array || vertices.size() != capacity)
			{
				vertex_array = MakeGizmoVertexArray("EditorTransformGizmoArc_VertexArray", vertices);
				capacity = vertices.size();
				return vertex_array;
			}

			vertex_array->GetVertexBuffers().front()->SetData(vertices.data(),
				static_cast<uint32_t>(vertices.size() * sizeof(float)));
			return vertex_array;
		}

		SharedPtr<DeviceVertexArray> GetTransformDashVertexArray(const std::vector<float>& vertices)
		{
			static SharedPtr<DeviceVertexArray> vertex_array;
			static size_t capacity = 0;

			if (vertices.empty())
				return nullptr;

			if (!vertex_array || vertices.size() != capacity)
			{
				vertex_array = MakeGizmoVertexArray("EditorTransformGizmoDash_VertexArray", vertices);
				capacity = vertices.size();
				return vertex_array;
			}

			vertex_array->GetVertexBuffers().front()->SetData(vertices.data(),
				static_cast<uint32_t>(vertices.size() * sizeof(float)));
			return vertex_array;
		}

		/* 骨骼连线的顶点缓冲：位置每帧随姿势变。DrawArrays 按缓冲区容量出顶点数，
		 * 所以容量必须正好等于本帧数据 —— 数量变了重建，没变就原地覆盖。 */
		SharedPtr<DeviceVertexArray> GetBoneLineVertexArray(const std::vector<float>& vertices)
		{
			static SharedPtr<DeviceVertexArray> vertex_array;
			static size_t capacity = 0;

			if (vertices.empty())
				return nullptr;

			if (!vertex_array || vertices.size() != capacity)
			{
				vertex_array = MakeGizmoVertexArray("EditorSkeletonBone_VertexArray", vertices);
				capacity = vertices.size();
				return vertex_array;
			}

			vertex_array->GetVertexBuffers().front()->SetData(vertices.data(),
				static_cast<uint32_t>(vertices.size() * sizeof(float)));
			return vertex_array;
		}

		/* gizmo 材质：纯色、不参与光照、线 / 锥不剔面、默认测深度并写深度（跟场景互相遮挡）。
		 * 混合用 SrcAlpha / OneMinusSrcAlpha（网格远景靠 alpha 渐隐）。
		 * grid_major > 0 表示画的是地面网格；其余 gizmo 要显式写 0。 */
		SharedPtr<Material> CreateGizmoMaterial(float fade_inner, float fade_outer, float grid_major)
		{
			auto material = CreateSharedPtr<Material>();
			material->SetShader(AssetManager::Instance().GetOrLoad<DeviceShader>(ABSOLUTE_PATH("Shaders/Overlay/Axis.glsl")));

			/* 走透明队列：不透明批不参与混合，顶点色里的 alpha 只会让颜色变暗不会变透明 */
			material->SetRenderQueue(RENDER_QUEUE_TRANSPARENT);

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

			/* 网格坐标系（线身份分类用）：缺省 = 场景口径（原点 0 / 缩放 1，逐位等价旧行为）；
			 * 模型预览的网格每次提交前会覆盖这两个值（见 SubmitGridGizmo） */
			material->SetParameters(ParamType::Vec2, "u_GridOrigin", glm::vec2(0.0f));
			material->SetParameters(ParamType::Float, "u_GridScale", 1.0f);

			/* 轴带展开参数：宽度与视口尺寸都在执行期按帧更新（见 SceneGizmos::Submit）；
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

		/* 实体 gizmo 材质：跟坐标轴同一个 shader，只是淡出范围放到天边（淡出是网格收边用的） */
		SharedPtr<Material> GetEntityGizmoMaterial()
		{
			static SharedPtr<Material> material;
			if (!material)
				material = CreateGizmoMaterial(kGizmoNoFadeInner, kGizmoNoFadeOuter, 0.0f);
			return material;
		}

		/* 骨骼节点材质：同样吃坐标轴 shader（屏幕空间等宽线 + 顶点色），不测深度、不写，
		 * 骨骼才透得过网格 */
		SharedPtr<Material> GetSkeletonMaterial()
		{
			static SharedPtr<Material> material;
			if (!material)
			{
				material = CreateGizmoMaterial(kGizmoNoFadeInner, kGizmoNoFadeOuter, 0.0f);
				material->GetRasterState().EnableDepthTest = false;
				material->GetRasterState().EnableDepthWrite = false;
			}
			return material;
		}

		/* 子网格高亮的材质：平铺纯色（复用线框着色器 —— 那个本来就只吃位置和一个裸色 uniform）。
		 * 深度测试跟场景一致（GreaterEqual）= 只染该段看得见的表面，被别的物体挡住的部分不染；
		 * 注意是实心填充不是线框，别把 Wireframe 光栅位打开 */
		SharedPtr<Material> GetHighlightMaterial()
		{
			static SharedPtr<Material> material;
			if (!material)
			{
				material = CreateSharedPtr<Material>();
				material->SetShader(AssetManager::Instance().GetOrLoad<DeviceShader>(
					ABSOLUTE_PATH("Shaders/Overlay/Wireframe.glsl")));
				/* 与编辑器 UI 的强调色同值（#A69EFF 紫）：悬停子模型卡一眼看出对应哪段 */
				material->SetParameters(ParamType::Vec4, "u_WireframeColor",
					glm::vec4(0.651f, 0.620f, 1.0f, 1.0f));

				auto& raster_state = material->GetRasterState();
				raster_state.CullMode = CullMode::Cull_None;
			}
			return material;
		}

		/* === 变换 gizmo（平移 / 旋转 / 缩放）的材质与提交 ===
		 * 手柄是实体网格：纯色、不参与光照、关面剔除，并且关深度测试 —— 永远画在场景前面
		 * （ImGuizmo 的行为；被物体挡住就点不到了）。 */

		/* 三轴配色 + 高亮橙 + 轴心白（轴心不属于任何一根轴） */
		const glm::vec4 kTransformGizmoAxisColors[3] = {
			{ 0.90f, 0.22f, 0.24f, 1.0f }, { 0.42f, 0.78f, 0.20f, 1.0f }, { 0.22f, 0.48f, 0.95f, 1.0f } };
		const glm::vec4 kTransformGizmoHighlightColor{ 1.00f, 0.62f, 0.16f, 1.0f };
		const glm::vec4 kTransformGizmoCenterColor{ 0.82f, 0.84f, 0.88f, 1.0f };
		/* 平面手柄 = 两根轴颜色的各半，半透明贴着场景；白环留一点透明才看得见底下的彩色环 */
		constexpr float kTransformGizmoPlaneAlpha = 0.45f;
		const glm::vec4 kTransformGizmoOuterRingColor{ 1.00f, 1.00f, 1.00f, 0.90f };

		/* 高亮不换色相：把手柄自身颜色往白里提一档，选中后仍能看出是哪根轴 */
		glm::vec4 LiftGizmoColor(const glm::vec4& base, float alpha)
		{
			constexpr float kLift = 0.45f;
			return glm::vec4(base.r + (1.0f - base.r) * kLift,
				base.g + (1.0f - base.g) * kLift,
				base.b + (1.0f - base.b) * kLift, alpha);
		}

		SharedPtr<Material> CreateTransformGizmoMaterial(const glm::vec4& color)
		{
			auto material = CreateSharedPtr<Material>();
			material->SetShader(AssetManager::Instance().GetOrLoad<DeviceShader>(
				ABSOLUTE_PATH("Shaders/Overlay/GizmoSolid.glsl")));
			material->SetParameters(ParamType::Vec4, "u_GizmoColor", color);
			/* 默认不做背侧淡出：只有旋转环的材质会每帧把自己的相机方位写进来 */
			material->SetParameters(ParamType::Float, "u_RingPhase", -1.0f);
			material->SetParameters(ParamType::Float, "u_RingFadeMin", 0.25f);

			auto& raster_state = material->GetRasterState();
			raster_state.CullMode = CullMode::Cull_None;
			/* 不测深度：AxisPass 的深度附件是场景深度、比较方向是 GreaterEqual（更远才通过），
			 * 手柄压到近平面后反而会被全部剔除。手柄之间的压盖靠提交顺序。 */
			raster_state.EnableDepthTest = false;
			raster_state.EnableDepthWrite = false;
			/* 平面手柄与白环带 alpha：混色走标准的 SrcAlpha / OneMinusSrcAlpha。
			 * 不透明的颜色带 a = 1，走同一套混合公式结果不变。 */
			raster_state.EnableBlend = true;
			raster_state.BlendEquationRGB = BlendEquation::Add;
			raster_state.BlendEquationA = BlendEquation::Add;
			raster_state.BlendFuncSrcRGB = BlendFunc::SrcAlpha;
			raster_state.BlendFuncSrcA = BlendFunc::SrcAlpha;
			raster_state.BlendFuncDstRGB = BlendFunc::OneMinusSrcAlpha;
			raster_state.BlendFuncDstA = BlendFunc::OneMinusSrcAlpha;
			return material;
		}

		/* handle = 0 / 1 / 2 三轴，3 = 轴心，4 / 5 / 6 = 平面（索引 = 法线轴），7 = 白色外环 */
		/* 旋转拖拽的角度扇区：颜色跟着被拖的轴走（Blender 的做法），半透明铺满圆心到弧。
		 * axis < 0 时用高亮橙兜底。 */
		/* 旋转环的亮边：轴色往白里提，管径放大一圈垫在底下，深色背景上也能看清 */

		/* 平面手柄的亮边：两轴混色提亮、不透明，画在填充底下一层放大一圈 */
		/* 参数是法线轴序号 0 / 1 / 2（调用方直接传 normal_axis，不再做 handle 换算） */
		SharedPtr<Material> GetTransformPlaneBorderMaterial(int32_t normal_axis, bool highlighted)
		{
			static SharedPtr<Material> materials[3][2];
			const int32_t axis = std::clamp(normal_axis, 0, 2);
			const int32_t slot = highlighted ? 1 : 0;
			if (!materials[axis][slot])
				materials[axis][slot] = CreateTransformGizmoMaterial(
					LiftGizmoColor(kTransformGizmoAxisColors[axis], 1.0f));
			return materials[axis][slot];
		}

		/* 中心淡圆：默认几乎不可见（0.02），鼠标悬浮或拖拽时才明显 */
		SharedPtr<Material> GetTransformTrackballMaterial(bool highlighted)
		{
			static SharedPtr<Material> materials[2];
			const int32_t slot = highlighted ? 1 : 0;
			if (!materials[slot])
				materials[slot] = CreateTransformGizmoMaterial(glm::vec4(0.85f, 0.87f, 0.92f,
					highlighted ? 0.18f : 0.09f));
			return materials[slot];
		}

		SharedPtr<Material> GetTransformSectorMaterial(int32_t axis)
		{
			static SharedPtr<Material> materials[3];
			static SharedPtr<Material> fallback;

			if (axis < 0 || axis >= 3)
			{
				/* 白外环没有轴色：扇形用白色，和环本身一致 */
				if (!fallback)
					fallback = CreateTransformGizmoMaterial(
						glm::vec4(kTransformGizmoOuterRingColor.r, kTransformGizmoOuterRingColor.g,
							kTransformGizmoOuterRingColor.b, 0.30f));
				return fallback;
			}

			if (!materials[axis])
			{
				const glm::vec4& base = kTransformGizmoAxisColors[axis];
				materials[axis] = CreateTransformGizmoMaterial(
					glm::vec4(base.r, base.g, base.b, 0.30f));
			}
			return materials[axis];
		}

		SharedPtr<Material> GetTransformGizmoMaterial(int32_t handle, bool highlighted)
		{
			static SharedPtr<Material> axis_materials[3];
			static SharedPtr<Material> axis_highlight_materials[3];
			static SharedPtr<Material> plane_materials[3];
			static SharedPtr<Material> plane_highlight_materials[3];
			static SharedPtr<Material> center_material;
			static SharedPtr<Material> highlight_material;
			static SharedPtr<Material> outer_ring_material;
			static SharedPtr<Material> outer_ring_highlight_material;

			const bool is_plane = handle >= TransformGizmo::kPlaneHandleBase
				&& handle < TransformGizmo::kPlaneHandleBase + TransformGizmo::kAxisCount;
			const bool is_outer_ring = handle == TransformGizmo::kOuterRingHandle;

			if (highlighted)
			{
				/* 高亮统一走「自身颜色提亮」：轴用轴色、平面用两轴混色、轴心与白环用白 */
				if (is_plane)
				{
					const int32_t normal_axis = handle - TransformGizmo::kPlaneHandleBase;
					if (!plane_highlight_materials[normal_axis])
						plane_highlight_materials[normal_axis] = CreateTransformGizmoMaterial(
							LiftGizmoColor(kTransformGizmoAxisColors[normal_axis], kTransformGizmoPlaneAlpha));
					return plane_highlight_materials[normal_axis];
				}

				if (is_outer_ring)
				{
					if (!outer_ring_highlight_material)
						outer_ring_highlight_material = CreateTransformGizmoMaterial(
							LiftGizmoColor(kTransformGizmoOuterRingColor, kTransformGizmoOuterRingColor.a));
					return outer_ring_highlight_material;
				}

				if (handle >= 0 && handle < 3)
				{
					if (!axis_highlight_materials[handle])
						axis_highlight_materials[handle] = CreateTransformGizmoMaterial(
							LiftGizmoColor(kTransformGizmoAxisColors[handle], 1.0f));
					return axis_highlight_materials[handle];
				}

				if (!highlight_material)
					highlight_material = CreateTransformGizmoMaterial(
						LiftGizmoColor(kTransformGizmoCenterColor, 1.0f));
				return highlight_material;
			}

			if (is_plane)
			{
				const int32_t normal_axis = handle - TransformGizmo::kPlaneHandleBase;
				SharedPtr<Material>& material = plane_materials[normal_axis];
				if (!material)
				{
					/* 颜色取「这根法线轴」的色：XY 面是蓝色、YZ 面是红色、XZ 面是绿色 */
					const glm::vec4& axis_color = kTransformGizmoAxisColors[normal_axis];
					material = CreateTransformGizmoMaterial(
						glm::vec4(axis_color.r, axis_color.g, axis_color.b, kTransformGizmoPlaneAlpha));
				}
				return material;
			}

			if (is_outer_ring)
			{
				if (!outer_ring_material)
					outer_ring_material = CreateTransformGizmoMaterial(kTransformGizmoOuterRingColor);
				return outer_ring_material;
			}

			if (handle >= 0 && handle < 3)
			{
				SharedPtr<Material>& material = axis_materials[handle];
				if (!material)
					material = CreateTransformGizmoMaterial(kTransformGizmoAxisColors[handle]);
				return material;
			}

			if (!center_material)
				center_material = CreateTransformGizmoMaterial(kTransformGizmoCenterColor);
			return center_material;
		}

		/* 背侧半环的材质：轴色降到 42% 不透明度，做出前后层次 */
		/* 旋转环材质：每根轴一份。选中不改色，只是把该环补成完整的圆 */
		SharedPtr<Material> GetTransformRingMaterial(int32_t axis)
		{
			static SharedPtr<Material> materials[3];
			const int32_t index = std::clamp(axis, 0, 2);
			if (!materials[index])
				materials[index] = CreateTransformGizmoMaterial(kTransformGizmoAxisColors[index]);
			return materials[index];
		}

		SharedPtr<Material> GetTransformGizmoFadedMaterial(int32_t axis)
		{
			static SharedPtr<Material> materials[3];
			const int32_t index = std::clamp(axis, 0, 2);
			if (!materials[index])
			{
				const glm::vec4& base = kTransformGizmoAxisColors[index];
				materials[index] = CreateTransformGizmoMaterial(
					glm::vec4(base.r, base.g, base.b, 0.42f));
			}
			return materials[index];
		}


		/* 实体世界变换拆解出 gizmo 需要的分量：平移 / 纯旋转（去缩放）/ 各轴缩放。
		 * "纯旋转"让图标本身不随实体缩放变形 —— 各类型对缩放的响应在响应因子里
		 * 单独表达（见 SubmitEntityGizmos） */
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

		/* 组装 gizmo 的模型矩阵：平移 × 纯旋转 × [额外旋转] × 尺寸基准（随距离收缩）× 各轴缩放响应 */
		glm::mat4 BuildGizmoModel(const GizmoTransformParts& parts, float world_scale,
			const glm::quat& extra_rotation, const glm::vec3& response)
		{
			return glm::translate(glm::mat4(1.0f), parts.Translation)
				* glm::mat4_cast(parts.Orientation * extra_rotation)
				* glm::scale(glm::mat4(1.0f), glm::vec3(world_scale) * response);
		}

		/* 世界尺寸 = 像素尺度 × 标称像素 × 距离比例 × 到相机的距离；距离比例 =
		 * clamp(参考距离/距离, 下限, 上限)：中段世界恒定（屏上按 1/距离 递减），
		 * 近 / 远到钳制值后转为屏幕恒定（放大封顶 / 收缩保底） */
		float GizmoWorldScaleAt(const glm::vec3& camera_position, const glm::vec3& position,
			float nominal_pixel_size, float gizmo_screen_scale)
		{
			const float distance = glm::distance(camera_position, position);
			const float pixel_scale = glm::clamp(kGizmoRefDistance / distance,
				kGizmoMinPixelScale, kGizmoMaxPixelScale);
			return gizmo_screen_scale * nominal_pixel_size * pixel_scale * distance;
		}

		/* ===== 各部分的提交 =====
		 * 网格 / 世界轴各一个函数；实体图标按类型各一个函数 —— 遍历 / 显隐过滤 /
		 * 放置各自完整，新增类型只需加一个函数并在 SubmitEntityGizmos 里挂一行。 */

		/* 地面网格：场景口径 = patch 跟着相机平移、按格对齐（整数平移 → 线就一直落在世界整数
		 * 坐标上），远景淡出按到相机的水平距离在着色器里处理（见 Axis.glsl）。
		 * 相机带"模型预览网格"参数时（模型编辑器）：补丁改成以模型为中心、格距按模型缩放，
		 * 淡出范围也按补丁半径拉开 —— 否则大模型取景距离远，1 格 1 单位的网格根本罩不到模型。 */
		void SubmitGridGizmo(const SharedPtr<Material>& material, const EditorCamera& camera)
		{
			const auto& gizmo_options = GetViewportGizmoOptions();
			if (!gizmo_options.ShowGrid)
				return;	/* 分项关：网格不提交 */

			const glm::vec3 camera_position = camera.GetPosition();
			glm::vec3 grid_origin(std::round(camera_position.x), 0.0f, std::round(camera_position.z));
			float grid_scale = 1.0f;
			float fade_inner = kGridFadeInner;
			float fade_outer = kGridFadeOuter;

			if (camera.IsPreviewGridEnabled())
			{
				grid_origin = camera.GetPreviewGridCenter();
				grid_scale = camera.GetPreviewGridScale();

				/* 淡出跟随补丁：到模型中心（相机距离）之间满强度，到补丁边缘前淡完 */
				const float patch_extent = kGridLineExtent * grid_scale;
				const float center_distance = glm::length(glm::vec2(
					camera_position.x - grid_origin.x, camera_position.z - grid_origin.z));
				fade_inner = center_distance;
				fade_outer = center_distance + patch_extent * 0.9f;
			}

			material->SetParameters(ParamType::Float, "u_FadeInner", fade_inner);
			material->SetParameters(ParamType::Float, "u_FadeOuter", fade_outer);
			material->SetParameters(ParamType::Vec2, "u_GridOrigin", glm::vec2(grid_origin.x, grid_origin.z));
			material->SetParameters(ParamType::Float, "u_GridScale", grid_scale);

			/* 网格平面恒在 y = 0：原点给 y 不给都行，这里显式落平 */
			const glm::mat4 grid_transform = glm::scale(
				glm::translate(glm::mat4(1.0f), glm::vec3(grid_origin.x, 0.0f, grid_origin.z)),
				glm::vec3(grid_scale));
			Renderer::FillObjectUniformBuffer(VisibleMeshObject{ -1, grid_transform, nullptr, nullptr });
			Renderer::Submit(material, MeshPrimitive{ GetGridVertexArray(), PrimitiveType::Lines });
		}

		/* 坐标轴固定在世界原点（单位变换）；轴带是三角形 */
		void SubmitWorldAxisGizmo(const SharedPtr<Material>& material)
		{
			const auto& gizmo_options = GetViewportGizmoOptions();
			if (!gizmo_options.ShowWorldAxis)
				return;	/* 分项关：坐标轴不提交 */

			Renderer::FillObjectUniformBuffer(VisibleMeshObject{ -1, glm::mat4(1.0f), nullptr, nullptr });
			Renderer::Submit(material, MeshPrimitive{ GetAxisVertexArray(), PrimitiveType::Triangles });
		}


		/* 光源（平行 / 点 / 聚光）：平行光 = 屏幕恒定的太阳图标；点光 / 聚光 = 真世界尺度的光照
		 * 范围（球半径 = range、双锥延伸到 range，锥口 = 内 / 外锥角）。位置另外画一个屏幕恒定
		 * 的轴心点 —— 范围线框再大再小，标记也都看得见。 */
		void SubmitLightGizmos(const SharedPtr<Scene>& scene, const SharedPtr<Material>& material,
			const glm::vec3& camera_position, float gizmo_screen_scale)
		{
			const auto& gizmo_options = GetViewportGizmoOptions();
			if (!gizmo_options.ShowLight)
				return;	/* 分项关：光源图标整段不提交 */

			/* 注意要非 const 引用：const registry 的 view 会实例化 const 组件视图，
			 * get<> 取不到池（entt 的编译期约束） */
			auto& registry = scene->GetRegistry();

			const auto light_view = registry.view<TransformComponent, LightComponent>();
			for (const entt::entity entity : light_view)
			{
				const auto& light_component = light_view.get<LightComponent>(entity);
				if (!light_component.m_Light)
					continue;

				const GizmoTransformParts parts = DecomposeGizmoTransform(scene->GetWorldTransform(entity));
				/* 屏幕恒定基准：位置标记（轴心点）用；真尺度影响范围用绘制基准 1 */
				const float world_scale = GizmoWorldScaleAt(camera_position, parts.Translation,
					kLightGizmoPixelSize, gizmo_screen_scale);

				/* 提交一份线框：base_scale = 绘制基准（真尺度形状传 1、屏幕恒定标记
				 * 传世界尺寸基准），response = 基准之上的各轴缩放 */
				const auto submit = [&](const GizmoTransformParts& transform_parts,
					const SharedPtr<DeviceVertexArray>& vertex_array, float base_scale,
					const glm::quat& extra_rotation, const glm::vec3& response)
				{
					Renderer::FillObjectUniformBuffer(VisibleMeshObject{ -1,
						BuildGizmoModel(transform_parts, base_scale, extra_rotation, response), nullptr, nullptr });
					Renderer::Submit(material, MeshPrimitive{ vertex_array, PrimitiveType::Triangles });
				};

				bool submit_pivot_dot = false;
				switch (light_component.m_Light->GetLightType())
				{
				case LightType::Directional:
				{
					/* 太阳图标沿真实光向摆放（几何按"光向 = -Y"建，这里再旋到实际方向）。方向已经跟着实体旋转
					 * 换算过，方向杆就不要再叠实体旋转（R 作用两次会转两圈）；图标屏上尺寸恒定。 */
					const auto directional_light = std::static_pointer_cast<DirectionalLight>(light_component.m_Light);
					glm::quat extra_rotation(1.0f, 0.0f, 0.0f, 0.0f);
					const glm::vec3 direction = directional_light->GetDirection();
					if (glm::length(direction) > 0.0f)
						extra_rotation = glm::rotation(glm::vec3(0.0f, -1.0f, 0.0f), glm::normalize(direction));
					GizmoTransformParts icon_parts = parts;
					icon_parts.Orientation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
					submit(icon_parts, GetDirectionalLightGizmoVertexArray(), world_scale, extra_rotation,
						glm::vec3(1.0f));
					break;
				}
				case LightType::Point:
				{
					/* 单位球 × range：球半径 = 衰减半径（真世界尺度；不吃实体缩放 ——
					 * range 是影响范围的唯一来源） */
					const auto point_light = std::static_pointer_cast<PointLight>(light_component.m_Light);
					const float range = std::max(point_light->GetRange(), kGizmoMinRange);
					submit(parts, GetPointLightGizmoVertexArray(), 1.0f, glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
						glm::vec3(range));
					submit_pivot_dot = true;
					break;
				}
				case LightType::Spot:
				{
					/* 单位锥两套各向异性缩放：横向 = range·tan(锥角/2)、轴向 = range ——
					 * 外锥张开到外锥角、内锥张开到内锥角（同一份几何两次绘制、颜色区分）。
					 * 锥轴 = 实体旋转（与光向同源），不吃实体缩放 */
					const auto spot_light = std::static_pointer_cast<SpotLight>(light_component.m_Light);
					const float range = std::max(spot_light->GetRange(), kGizmoMinRange);
					const auto cone_openness = [](float angle_deg)
					{
						return std::tan(glm::radians(glm::clamp(angle_deg, 1.0f, 170.0f)) * 0.5f);
					};
					const float outer_radius = range * cone_openness(spot_light->GetAngle());
					const float inner_radius = range * cone_openness(spot_light->GetInnerAngle());
					const glm::quat no_extra_rotation(1.0f, 0.0f, 0.0f, 0.0f);
					submit(parts, GetSpotLightOuterConeVertexArray(), 1.0f, no_extra_rotation,
						glm::vec3(outer_radius, outer_radius, range));
					submit(parts, GetSpotLightInnerConeVertexArray(), 1.0f, no_extra_rotation,
						glm::vec3(inner_radius, inner_radius, range));
					submit_pivot_dot = true;
					break;
				}
				case LightType::Area:
				case LightType::Volume:
					break;	/* 暂无面光 / 体积光的图标 */
				}

				/* 位置标记：屏幕恒定小尺寸（不随 range / 实体缩放变形） */
				if (submit_pivot_dot)
				{
					submit(parts, GetLightPivotDotVertexArray(), world_scale,
						glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f));
				}
			}
		}

		/* 相机：视锥按真实世界尺度画 —— near / far 落在真实距离、矩形按真实 FOV / 宽高比张开
		 * （不吃实体缩放）；机身盒是屏幕恒定的位置标记。注意：视锥长度 = far，编辑器相机远裁剪面
		 * 要给得足够大（见 EditorCamera）。 */
		void SubmitCameraGizmos(const SharedPtr<Scene>& scene, const SharedPtr<Material>& material,
			const glm::vec3& camera_position, float gizmo_screen_scale)
		{
			const auto& gizmo_options = GetViewportGizmoOptions();
			if (!gizmo_options.ShowCamera)
				return;	/* 分项关：相机图标整段不提交 */

			auto& registry = scene->GetRegistry();

			const auto camera_view = registry.view<TransformComponent, CameraComponent>();
			for (const entt::entity entity : camera_view)
			{
				const auto& camera_component = camera_view.get<CameraComponent>(entity);
				const GizmoTransformParts parts = DecomposeGizmoTransform(scene->GetWorldTransform(entity));
				const float world_scale = GizmoWorldScaleAt(camera_position, parts.Translation,
					kCameraGizmoPixelSize, gizmo_screen_scale);

				/* 视锥尺度 = "真实量 / 规范量"：长度 far / 1.5、近平面深度 near / far；半高乘
				 * tan(fov/2) / tan(22.5°)、半宽乘 aspect / 16:9。没有相机组件 / 正交相机兜底：规范形状 ×
				 * 屏幕恒定基准。 */
				float near_depth_unit = kCameraGizmoNear / kCameraGizmoFar;
				glm::vec3 frustum_scale(world_scale);
				if (camera_component.m_Camera)
				{
					const auto& camera = camera_component.m_Camera;
					float tan_half_fov = kCameraGizmoDefaultTanHalfFov;
					float aspect = kCameraGizmoDefaultAspect;
					if (camera->GetProjectionType() == CameraProjectionType::Perspective)
					{
						const float fov = glm::clamp(camera->GetFov(), 1.0f, 179.0f);
						tan_half_fov = std::tan(glm::radians(fov) * 0.5f);
						aspect = camera->GetAspectRatio();
					}
					const float near_safe = std::max(camera->GetNearClip(), 1.0e-4f);
					const float far_safe = std::max(camera->GetFarClip(), near_safe * 1.001f);
					near_depth_unit = near_safe / far_safe;
					const float length_scale = far_safe / kCameraGizmoFar;
					const float height_scale = length_scale * (tan_half_fov / kCameraGizmoDefaultTanHalfFov);
					frustum_scale = glm::vec3(height_scale * (aspect / kCameraGizmoDefaultAspect),
						height_scale, length_scale);
				}

				/* 视锥（近 / 远矩形 + 棱 + 上向标记）：真实尺度（基准 1 = 不经屏幕恒定） */
				Renderer::FillObjectUniformBuffer(VisibleMeshObject{ -1,
					BuildGizmoModel(parts, 1.0f, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), frustum_scale), nullptr, nullptr });
				Renderer::Submit(material, MeshPrimitive{ GetCameraFrustumVertexArray(near_depth_unit), PrimitiveType::Triangles });

				/* 机身盒 + 轴心点：屏幕恒定小图标（位置标记） */
				Renderer::FillObjectUniformBuffer(VisibleMeshObject{ -1,
					BuildGizmoModel(parts, world_scale, glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
						glm::vec3(1.0f)), nullptr, nullptr });
				Renderer::Submit(material, MeshPrimitive{ GetCameraBodyVertexArray(), PrimitiveType::Triangles });
			}
		}

		/* 反射探针：盒 = 影响范围盒（BoxSize，中心 = 探针位置、轴对齐）——真世界尺度，
		 * 不吃实体旋转 / 缩放，与 IBL 的盒体权重同一份语义（见 Lighting.glsl 的 ProbeBoxWeight）。
		 * 过渡内框 = 每轴向内缩 BlendDistance（权重拉满的核心区）；轴心点屏幕恒定、单独提交。 */
		void SubmitProbeGizmos(const SharedPtr<Scene>& scene, const SharedPtr<Material>& material,
			const glm::vec3& camera_position, float gizmo_screen_scale)
		{
			const auto& gizmo_options = GetViewportGizmoOptions();
			if (!gizmo_options.ShowReflectionProbe)
				return;	/* 分项关：探针盒整段不提交 */

			auto& registry = scene->GetRegistry();

			const auto probe_view = registry.view<TransformComponent, ReflectionProbeComponent>();
			for (const entt::entity entity : probe_view)
			{
				const auto& probe = probe_view.get<ReflectionProbeComponent>(entity).m_ReflectionProbe;
				if (probe == nullptr)
					continue;

				const GizmoTransformParts parts = DecomposeGizmoTransform(scene->GetWorldTransform(entity));
				const glm::vec3 box_size = probe->GetBoxSize();

				/* 影响范围盒：单位盒按 BoxSize 缩放（平移不变、无旋转 —— 盒体是轴对齐的） */
				Renderer::FillObjectUniformBuffer(VisibleMeshObject{ -1,
					glm::translate(glm::mat4(1.0f), parts.Translation) * glm::scale(glm::mat4(1.0f), box_size),
					nullptr, nullptr });
				Renderer::Submit(material, MeshPrimitive{ GetProbeBoxVertexArray(), PrimitiveType::Triangles });

				/* 过渡内框：内缩到零以下（blend 覆盖整个盒）就不画 */
				const float blend = probe->GetBlendDistance();
				if (blend > 0.0f)
				{
					const glm::vec3 inner_size = box_size - glm::vec3(2.0f * blend);
					if (inner_size.x > 0.0f && inner_size.y > 0.0f && inner_size.z > 0.0f)
					{
						Renderer::FillObjectUniformBuffer(VisibleMeshObject{ -1,
							glm::translate(glm::mat4(1.0f), parts.Translation) * glm::scale(glm::mat4(1.0f), inner_size),
							nullptr, nullptr });
						Renderer::Submit(material, MeshPrimitive{ GetProbeInnerBoxVertexArray(), PrimitiveType::Triangles });
					}
				}

				/* 位置标记：轴心点保持屏幕恒定小尺寸（不随 BoxSize / 实体缩放变形） */
				Renderer::FillObjectUniformBuffer(VisibleMeshObject{ -1,
					BuildGizmoModel(parts, GizmoWorldScaleAt(camera_position, parts.Translation,
						kProbeGizmoPixelSize, gizmo_screen_scale),
						glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f)), nullptr, nullptr });
				Renderer::Submit(material, MeshPrimitive{ GetProbePivotDotVertexArray(), PrimitiveType::Triangles });
			}
		}

		/* 精灵：框 = 显示区域 1×1 的实际占位，完整响应缩放、各轴独立 */
		void SubmitSpriteGizmos(const SharedPtr<Scene>& scene, const SharedPtr<Material>& material,
			const glm::vec3& camera_position, float gizmo_screen_scale)
		{
			const auto& gizmo_options = GetViewportGizmoOptions();
			if (!gizmo_options.ShowSprite)
				return;	/* 分项关：精灵框整段不提交 */

			auto& registry = scene->GetRegistry();

			const auto sprite_view = registry.view<TransformComponent, SpriteComponent>();
			for (const entt::entity entity : sprite_view)
			{
				const GizmoTransformParts parts = DecomposeGizmoTransform(scene->GetWorldTransform(entity));
				Renderer::FillObjectUniformBuffer(VisibleMeshObject{ -1,
					BuildGizmoModel(parts, GizmoWorldScaleAt(camera_position, parts.Translation,
						kSpriteGizmoPixelSize, gizmo_screen_scale),
						glm::quat(1.0f, 0.0f, 0.0f, 0.0f), parts.Scale), nullptr, nullptr });
				Renderer::Submit(material, MeshPrimitive{ GetSpriteGizmoVertexArray(), PrimitiveType::Triangles });
			}
		}

		/* 实体图标总调度：光源（平行 / 点 / 聚光）、相机、反射探针、精灵。图标类的屏上尺寸随
		 * 距离收缩（近端封顶、远端保底，参数见 kGizmo* 常量）；gizmo_screen_scale = 单位距离
		 * 上每逻辑像素对应的世界尺寸，由 Submit 给出。实体数量少，就逐条提交。 */
		void SubmitEntityGizmos(RenderView& render_view, const SharedPtr<Material>& material,
			const glm::vec3& camera_position, float gizmo_screen_scale)
		{
			const auto scene = render_view.GetOwnerScene();
			if (!scene)
				return;

			SubmitLightGizmos(scene, material, camera_position, gizmo_screen_scale);
			SubmitCameraGizmos(scene, material, camera_position, gizmo_screen_scale);
			SubmitProbeGizmos(scene, material, camera_position, gizmo_screen_scale);
			SubmitSpriteGizmos(scene, material, camera_position, gizmo_screen_scale);
		}

		/* 骨骼节点预览（Debug View 的 Skeleton 档）：场景里每个骨骼模型的关节画成圆点，
		 * 父子关节之间拉一条线。关节实体由 SkeletonModel 按名字对上关节表，位置跟着
		 * 动画每帧更新；线先画、点后画，点压在线头上。 */
		void SubmitSkeletonNodes(RenderView& render_view, const glm::vec3& camera_position,
			float gizmo_screen_scale)
		{
			const auto scene = render_view.GetOwnerScene();
			if (!scene)
				return;

			auto& registry = scene->GetRegistry();
			const auto model_view = registry.view<TransformComponent, ModelComponent>();

			std::vector<float> bone_vertices;
			std::vector<glm::vec3> joint_positions;
			for (const entt::entity entity : model_view)
			{
				const auto& model = model_view.get<ModelComponent>(entity).m_Model;
				if (model == nullptr || model->IsStaticModel() || !scene->IsEntityVisible(entity))
					continue;

				const SkeletonModel* skeleton = AsSkeletonModel(model.get());
				const auto& joints = skeleton->GetJointEntities();
				if (joints.empty())
					continue;

				const auto& joint_table = skeleton->GetSkin().GetJoints();
				const auto joint_position = [&](size_t index)
				{
					return glm::vec3(scene->GetWorldTransform(joints[index])[3]);
				};

				for (size_t index = 0; index < joints.size(); ++index)
				{
					if (!scene->IsEntityValid(joints[index]))
						continue;

					/* 根骨骼没有父关节，只画点不拉线 */
					if (index < joint_table.size() && joint_table[index].Parent >= 0)
					{
						const size_t parent = static_cast<size_t>(joint_table[index].Parent);
						if (parent < joints.size() && scene->IsEntityValid(joints[parent]))
							PushLineBand(bone_vertices, joint_position(parent), joint_position(index), kBoneLineColor);
					}

					joint_positions.push_back(joint_position(index));
				}
			}

			if (const auto bone_vertex_array = GetBoneLineVertexArray(bone_vertices))
			{
				Renderer::FillObjectUniformBuffer(VisibleMeshObject{ -1, glm::mat4(1.0f), nullptr, nullptr });
				Renderer::Submit(GetSkeletonMaterial(), MeshPrimitive{ bone_vertex_array, PrimitiveType::Triangles });
			}

			/* 关节圆点：屏幕恒定小尺寸，与光源 / 探针的轴心点同一口径 */
			for (const glm::vec3& position : joint_positions)
			{
				GizmoTransformParts parts;
				parts.Translation = position;
				const float world_scale = GizmoWorldScaleAt(camera_position, position,
					kJointNodePixelSize, gizmo_screen_scale);
				Renderer::FillObjectUniformBuffer(VisibleMeshObject{ -1,
					BuildGizmoModel(parts, world_scale, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f)),
					nullptr, nullptr });
				Renderer::Submit(GetSkeletonMaterial(), MeshPrimitive{ GetJointNodeVertexArray(), PrimitiveType::Triangles });
			}
		}
	}

	/* 场景 gizmo 显隐的单一数据源（声明见 SceneGizmos.h）：绘制侧每帧读、UI 侧改写 */
	ViewportGizmoOptions& GetViewportGizmoOptions()
	{
		static ViewportGizmoOptions options;
		return options;
	}

	/* 操作 gizmo 的帧状态（同上：UI 侧写、绘制侧读）。状态在 ImGui 阶段落下，
	 * 下一帧的渲染图按它画 —— 与实体变换本身的一帧延迟保持一致。 */
	TransformGizmoState& GetTransformGizmoState()
	{
		static TransformGizmoState state;
		return state;
	}

	/* 每逻辑像素在单位距离上的世界尺寸：2·tan(fov / 2)·内容缩放 / 物理视口高。
	 * 内容缩放把"设计像素"折成物理像素（Retina 上视口高是逻辑高的两倍）。
	 * 操作 gizmo 与实体图标的屏幕恒定尺寸共用这一口径。 */
	float ComputeGizmoScreenScale(const EditorCamera& camera)
	{
		const auto& viewport_region = camera.GetViewportRegion();
		if (viewport_region.Height == 0)
			return 0.0f;

		/* 无窗口环境（headless 检查直跑渲染图）拿不到窗口缩放，按 1 算 */
		Application* application = Application::Instance();
		const float content_scale = (application != nullptr) ? application->GetWindow().GetContentScale() : 1.0f;
		return 2.0f * std::tan(glm::radians(camera.GetFov()) * 0.5f) * content_scale
			/ static_cast<float>(viewport_region.Height);
	}

	namespace SceneGizmos
	{
		void Submit(RenderView& render_view, const EditorCamera& camera)
		{
			const auto grid_material = GetGridMaterial();
			const auto axis_material = GetAxisMaterial();
			const auto entity_gizmo_material = GetEntityGizmoMaterial();

			/* 轴带展开参数在执行期给：宽度按内容缩放折算成物理像素（视口 RT 就是物理像素）。视口尺寸
			 * 跟 RT 实时一致；网格 / 坐标轴 / 实体 gizmo 三份材质都要给一遍。
			 * 无窗口环境（headless 检查直跑渲染图）拿不到窗口缩放，按 1 算。 */
			Application* application = Application::Instance();
			const float content_scale = (application != nullptr) ? application->GetWindow().GetContentScale() : 1.0f;
			const auto& viewport_region = camera.GetViewportRegion();
			const glm::vec2 viewport_size(
				static_cast<float>(viewport_region.Width),
				static_cast<float>(viewport_region.Height));
			for (const auto& gizmo_material : { grid_material, axis_material, entity_gizmo_material })
			{
				gizmo_material->SetParameters(ParamType::Float, "u_LineWidth", kAxisLineWidth * content_scale);
				gizmo_material->SetParameters(ParamType::Vec2, "u_ViewportSize", viewport_size);
			}

			const glm::vec3 camera_position = camera.GetPosition();
			SubmitGridGizmo(grid_material, camera);
			SubmitWorldAxisGizmo(axis_material);

			/* 实体 gizmo 的像素→世界系数：每"逻辑像素"在单位距离上对应的世界尺寸
			 * （= 2·tan(fov/2)·内容缩放 / 视口高；配 GizmoWorldScaleAt 的距离比例与距离使用） */
			const float gizmo_screen_scale = ComputeGizmoScreenScale(camera);
			SubmitEntityGizmos(render_view, entity_gizmo_material, camera_position, gizmo_screen_scale);

			/* 操作 gizmo（选中实体的平移 / 旋转 / 缩放手柄）：最后画，压在其他 helper 之上；
			 * 材质关深度测试，所以它也不会被场景物体挡住 */
			const TransformGizmoState& transform_gizmo = GetTransformGizmoState();
			if (transform_gizmo.Visible && transform_gizmo.Camera == &camera)
			{
				SubmitTransformGizmo(render_view, transform_gizmo.World, transform_gizmo.Operation,
					transform_gizmo.Highlight, gizmo_screen_scale);
			}

			/* 骨骼节点预览（Debug View 的 Skeleton 档）：最后画，盖在其他 gizmo 上 */
			if (render_view.GetDebugViewMode() == DebugViewMode::Skeleton)
			{
				const auto skeleton_material = GetSkeletonMaterial();
				skeleton_material->SetParameters(ParamType::Float, "u_LineWidth", kBoneLineWidth * content_scale);
				skeleton_material->SetParameters(ParamType::Vec2, "u_ViewportSize", viewport_size);
				SubmitSkeletonNodes(render_view, camera_position, gizmo_screen_scale);
			}
		}

		/* 子网格高亮（模型预览：悬停子模型卡时用）：指定网格段 + 强调色平铺再画一遍
		 * （实心、深度测试跟场景一致：只染可见表面）。调用方保证 segment_index >= 0 */
		void SubmitModelSegmentHighlight(RenderView& render_view, int segment_index)
		{
			const auto scene = render_view.GetOwnerScene();
			if (!scene || segment_index < 0)
				return;

			const auto highlight_material = GetHighlightMaterial();
			auto& registry = scene->GetRegistry();
			const auto model_view = registry.view<TransformComponent, ModelComponent>();
			for (const entt::entity entity : model_view)
			{
				const auto& model_component = model_view.get<ModelComponent>(entity);
				if (model_component.m_Model == nullptr || !scene->IsEntityVisible(entity))
					continue;

				/* 蒙皮模型取当前姿势的动态段：高亮染的是画面上的那一份（源段会停在绑定姿态） */
				const auto& mesh_segments = model_component.m_EnableSkinning
					? model_component.m_Model->GetRenderSegments()
					: model_component.m_Model->GetMeshSegments();
				if (segment_index >= static_cast<int>(mesh_segments.size()))
					continue;

				/* 天空盒不算模型：球面铺满整个视口，染上去整片画面都变强调色 */
				const auto material = ResolveSlotMaterial(*model_component.m_Model,
					mesh_segments[segment_index]->GetSlotIndex(), &model_component.m_SlotOverrides);
				if (material == nullptr || material->IsSkyBox())
					continue;

				const glm::mat4 world_transform = scene->GetWorldTransform(entity);
				Renderer::FillObjectUniformBuffer(VisibleMeshObject{ -1, world_transform, nullptr, nullptr });
				Renderer::Submit(highlight_material, mesh_segments[segment_index]->GetMeshPrimitive());
			}
		}

		/* 变换 gizmo：按操作类型提交实体网格。手柄尺寸由布局给出（屏幕恒定），
		 * 高亮的那一只换橙色材质。轴基取实体世界变换的三列 —— 与拾取侧（TransformGizmo.h）
		 * 同一份计算，画出来的位置和能点到的位置必然一致。
		 * 提交顺序 = 压盖顺序（全部关深度测试）：轴杆 → 端点图形 → 平面手柄 → 轴心 → 白色外环。 */
		void SubmitTransformGizmo(RenderView& render_view, const glm::mat4& entity_world,
			int32_t operation, int32_t highlight, float gizmo_screen_scale)
		{
			const auto transform_operation = static_cast<TransformGizmo::Operation>(operation);
			if (transform_operation == TransformGizmo::Operation::None)
				return;

			const Camera* camera = render_view.GetCullingCamera();
			if (camera == nullptr)
				return;

			/* 轴基：局部空间取世界变换三列归一化，世界空间直接用单位轴 */
			const bool local_space = GetTransformGizmoState().LocalSpace;
			glm::mat3 basis(1.0f);
			for (glm::length_t axis = 0; axis < 3; ++axis)
			{
				if (!local_space)
					continue;

				const glm::vec3 column(entity_world[axis]);
				const float length = glm::length(column);
				if (length > 1.0e-6f)
					basis[axis] = column / length;
				else
					basis[axis] = glm::vec3(axis == 0 ? 1.0f : 0.0f, axis == 1 ? 1.0f : 0.0f,
						axis == 2 ? 1.0f : 0.0f);
			}

			const auto& viewport_region = render_view.GetViewportRegion();
			const TransformGizmo::Layout layout = TransformGizmo::BuildLayout(
				glm::vec3(entity_world[3]), basis,
				camera->GetPosition(), camera->GetForwardDir(),
				camera->GetRightDir(), camera->GetUpDir(), gizmo_screen_scale,
				camera->GetViewProjectionMatrix(),
				glm::vec2(static_cast<float>(viewport_region.MinX), static_cast<float>(viewport_region.MinY)),
				glm::vec2(static_cast<float>(viewport_region.Width), static_cast<float>(viewport_region.Height)));
			if (!layout.Valid)
				return;

			const TransformGizmo::Handles handles = TransformGizmo::ComputeHandles(layout, transform_operation);

			const auto submit = [&](const glm::mat4& model, int32_t handle,
				const SharedPtr<DeviceVertexArray>& vertex_array, bool faded = false)
			{
				Renderer::FillObjectUniformBuffer(VisibleMeshObject{ -1, model, nullptr, nullptr });
				Renderer::Submit(faded ? GetTransformGizmoFadedMaterial(handle)
					: GetTransformGizmoMaterial(handle, handle == highlight),
					MeshPrimitive{ vertex_array, PrimitiveType::Triangles });
			};

			switch (transform_operation)
			{
			case TransformGizmo::Operation::Translate:
				for (int32_t axis = 0; axis < TransformGizmo::kAxisCount; ++axis)
				{
					submit(handles.Shaft[axis], axis, GetTransformCylinderVertexArray());
					submit(handles.Arrow[axis], axis, GetTransformConeVertexArray());
				}
				/* 平面手柄：只画空心边框，内部完全透出场景 */
				for (int32_t normal_axis = 0; normal_axis < TransformGizmo::kAxisCount; ++normal_axis)
				{
					const int32_t plane_handle = TransformGizmo::kPlaneHandleBase + normal_axis;
					Renderer::FillObjectUniformBuffer(
						VisibleMeshObject{ -1, handles.Plane[normal_axis], nullptr, nullptr });
					Renderer::Submit(
						GetTransformPlaneBorderMaterial(normal_axis, plane_handle == highlight),
						MeshPrimitive{ GetTransformPlaneFrameVertexArray(), PrimitiveType::Triangles });
					submit(handles.Plane[normal_axis], plane_handle, GetTransformQuadVertexArray());
				}
				submit(handles.CenterSphere, TransformGizmo::kCenterHandle, GetTransformSphereVertexArray());

				/* 拖拽轨迹：从起点到当前原点一条虚线，标出这次移动的路径 */
				{
					const TransformGizmoState& gizmo_state = GetTransformGizmoState();
					if (gizmo_state.DragActive)
					{
						const auto dash_vertices = BuildTransformDashVertices(gizmo_state.DragStart,
							gizmo_state.DragCurrent, 6.0f * layout.WorldPerPixel);
						const auto dash_array = GetTransformDashVertexArray(dash_vertices);
						if (dash_array)
						{
							Renderer::FillObjectUniformBuffer(
								VisibleMeshObject{ -1, glm::mat4(1.0f), nullptr, nullptr });
							Renderer::Submit(GetTransformGizmoMaterial(TransformGizmo::kCenterHandle, true),
								MeshPrimitive{ dash_array, PrimitiveType::Lines });
						}
					}
				}
				break;
			case TransformGizmo::Operation::Rotate:
				/* 每根轴算一次相机在环平面内的方位，写进材质做背侧淡出 */
				for (int32_t axis = 0; axis < TransformGizmo::kAxisCount; ++axis)
				{
					/* 选中某根环时只留它自己；选中白外环时三根彩环都不画 */
					if (highlight == TransformGizmo::kOuterRingHandle)
						continue;
					if (highlight >= 0 && highlight < TransformGizmo::kAxisCount && axis != highlight)
						continue;

					/* 相位用环的实际模型矩阵取基，和网格几何严格同源（RingBasis 的相位未必一致） */
					const glm::vec3 ring_u = glm::normalize(glm::vec3(handles.Ring[axis][0]));
					const glm::vec3 ring_v = glm::normalize(glm::vec3(handles.Ring[axis][1]));
					const glm::vec3 to_camera = layout.CameraPosition - layout.Origin;
					float phase = std::atan2(glm::dot(to_camera, ring_v), glm::dot(to_camera, ring_u));
					if (phase < 0.0f)
						phase += glm::two_pi<float>();

					const auto ring_material = GetTransformRingMaterial(axis);
					/* 默认只显示朝向相机的半圈（背侧淡到 0），悬浮或拖拽的环显示整圈 */
					const float ring_fade = axis == highlight ? 1.0f : 0.0f;

					const auto ring_vertices = BuildTransformRingVertices(
						TransformGizmo::kRingTubeRatio, ring_fade, phase);
					Renderer::FillObjectUniformBuffer(
						VisibleMeshObject{ -1, handles.Ring[axis], nullptr, nullptr });
					Renderer::Submit(ring_material,
						MeshPrimitive{ GetTransformRingDynamicVertexArray(axis, ring_vertices),
							PrimitiveType::Triangles });
				}

				/* 角度弧：拖拽中才有几何，顶点在世界坐标里现算（模型矩阵取单位阵）。
				 * 帧状态是 UI → 渲染的唯一通道，弧度取吸附后的值 */
				{
					const TransformGizmoState& gizmo_state = GetTransformGizmoState();
					if (gizmo_state.ArcRadius > 0.0f && gizmo_state.ArcSweep != 0.0f)
					{
						glm::vec3 arc_u, arc_v;
						TransformGizmo::RingBasis(gizmo_state.ArcAxis, arc_u, arc_v);
						const auto arc_vertices = BuildTransformSectorVertices(layout.Origin, arc_u, arc_v,
							gizmo_state.ArcRadius, gizmo_state.ArcStart, gizmo_state.ArcSweep);
						const auto arc_array = GetTransformArcVertexArray(arc_vertices);
						if (arc_array)
						{
							Renderer::FillObjectUniformBuffer(VisibleMeshObject{ -1, glm::mat4(1.0f), nullptr, nullptr });
							Renderer::Submit(GetTransformSectorMaterial(gizmo_state.ArcAxisIndex),
								MeshPrimitive{ arc_array, PrimitiveType::Triangles });
						}
					}
				}

				/* 白色外环最后画：压在三只彩色环之上 */
				/* 中心淡圆：铺在环半径以内、朝相机，提示这个区域可以任意旋转 */
				{
					glm::vec3 disc_u, disc_v;
					TransformGizmo::RingBasis(-layout.CameraForward, disc_u, disc_v);
					const float disc_radius = TransformGizmo::RingRadius(layout)
						* TransformGizmo::kTrackballRadiusRatio();
					const glm::mat4 disc_model(
						glm::vec4(disc_u * disc_radius, 0.0f),
						glm::vec4(disc_v * disc_radius, 0.0f),
						glm::vec4(glm::cross(disc_u, disc_v) * disc_radius, 0.0f),
						glm::vec4(layout.Origin, 1.0f));

					Renderer::FillObjectUniformBuffer(
						VisibleMeshObject{ -1, disc_model, nullptr, nullptr });
					Renderer::Submit(
						GetTransformTrackballMaterial(highlight == TransformGizmo::kTrackballHandle),
						MeshPrimitive{ GetTransformDiscVertexArray(), PrimitiveType::Triangles });
				}

				/* 拖某根彩色环时不画白外环（两层圈叠在一起看不清）；
				 * 反过来拖白外环时彩环已经在上面被过滤掉了 */
				if (highlight < 0 || highlight >= TransformGizmo::kAxisCount)
					submit(handles.OuterRing, TransformGizmo::kOuterRingHandle,
						GetTransformOuterRingVertexArray());
				break;
			case TransformGizmo::Operation::Scale:
				for (int32_t axis = 0; axis < TransformGizmo::kAxisCount; ++axis)
				{
					submit(handles.Shaft[axis], axis, GetTransformCylinderVertexArray());
					submit(handles.Cube[axis], axis, GetTransformCubeVertexArray());
				}
				/* 平面手柄：两轴同步缩放（与平移复用同一份四边形网格与摆位） */
				/* 平面手柄：只画空心边框，内部完全透出场景 */
				for (int32_t normal_axis = 0; normal_axis < TransformGizmo::kAxisCount; ++normal_axis)
				{
					const int32_t plane_handle = TransformGizmo::kPlaneHandleBase + normal_axis;
					Renderer::FillObjectUniformBuffer(
						VisibleMeshObject{ -1, handles.Plane[normal_axis], nullptr, nullptr });
					Renderer::Submit(
						GetTransformPlaneBorderMaterial(normal_axis, plane_handle == highlight),
						MeshPrimitive{ GetTransformPlaneFrameVertexArray(), PrimitiveType::Triangles });
					submit(handles.Plane[normal_axis], plane_handle, GetTransformQuadVertexArray());
				}
				submit(handles.Center, TransformGizmo::kCenterHandle, GetTransformCubeVertexArray());
				break;
			default:
				break;
			}
		}
	}
}
