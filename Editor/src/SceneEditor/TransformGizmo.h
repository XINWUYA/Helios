#pragma once
/* 变换 gizmo（平移 / 旋转 / 缩放）：世界空间的手柄布局、射线命中与拖拽数学。
 * 手柄本身是真正的 3D 网格（几何与提交在 SceneGizmos.cpp），本文件只回答"手柄在哪、
 * 射线命中谁、拖出多少世界增量"。三条约束：
 * 1. 手柄尺寸按屏幕像素折算（WorldPerPixel = 屏幕系数 × 到相机的深度），远近手感一致；
 * 2. 命中一律射线 × 几何（轴 = 线段、环 = 折线段、方块 = OBB、平面 = 四边形、中心 = 球），
 *    不做屏幕空间近似；
 * 3. 拖拽只产出世界增量（ApplyDelta），本地变换分解与命令栈提交留在 SceneEditorLayer。 */

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>	/* glm::rotation */

#include <algorithm>
#include <cmath>
#include <limits>

namespace Helios::TransformGizmo
{
	/* 操作类型：0 / 1 / 2 与工具栏、快捷键的 int 口径一致 */
	enum class Operation : int32_t
	{
		None = -1,
		Translate = 0,
		Rotate = 1,
		Scale = 2
	};

	inline constexpr int32_t kAxisCount = 3;
	/* 缩放 / 平移的轴心手柄（缩放 = 方块、平移 = 球） */
	inline constexpr int32_t kCenterHandle = 3;
	/* 平移的平面手柄：4 / 5 / 6 = 法线为 X / Y / Z 的平面 */
	inline constexpr int32_t kPlaneHandleBase = 4;
	/* 旋转的白色外环：绕视线旋转 */
	inline constexpr int32_t kOuterRingHandle = 7;

	/* 中心的淡圆（trackball）：在圆内任意拖动都绕视点自由旋转 */
	inline constexpr int32_t kTrackballHandle = 8;
	inline float kTrackballRadiusRatio() { return 0.92f; }

	/* ---- 手柄尺寸（逻辑像素）：乘以 WorldPerPixel 得世界长度 ---- */
	inline constexpr float kAxisLengthPx = 80.0f;	/* 轴（含箭头）总长 */
	inline constexpr float kArrowLengthPx = 16.0f;	/* 平移箭头长度（0.2 × 轴长） */
	inline constexpr float kArrowRadiusPx = 4.8f;	/* 平移箭头底面半径（0.06 × 轴长） */
	inline constexpr float kShaftRadiusPx = 1.6f;	/* 轴杆半径（0.02 × 轴长） */
	inline constexpr float kRingRadiusPx = 82.0f;	/* 旋转环大半径 */
	inline constexpr float kRingTubeRatio = 0.02f;	/* 环管半径 / 大半径（与网格几何同源） */
	inline constexpr float kCubeHalfPx = 4.8f;		/* 缩放方块半边长（边长 0.12 × 轴长） */
	inline constexpr float kCenterHalfPx = 6.0f;	/* 缩放轴心方块半边长 */
	inline constexpr float kPickPx = 8.0f;			/* 拾取容差（屏幕像素） */
	inline constexpr int32_t kRingSegments = 64;	/* 环的折线段数（与网格同源，命中共用） */

	/* ---- 手柄相对轴长的比例（对齐 ImGuizmo 的观感）---- */
	inline constexpr float kPlaneSideRatio = 0.24f;			/* 平面手柄边长 */
	inline constexpr float kPlaneOffsetRatio = 0.60f;		/* 平面手柄中心到原点的距离 */
	inline constexpr float kCenterSphereRatio = 0.12f;		/* 平移中心球半径（约 9.6px） */
	inline constexpr float kOuterRingRatio = 1.1f;			/* 白色外环半径 / 彩色环半径 */
	inline constexpr float kOuterRingTubeRatio = 0.008f;	/* 外环管半径 / 外环半径（约 1.5px 粗） */

	/* 轴与视线夹角过小（投影短于它）时拖拽方向不可靠：BeginDrag 换一个求交平面兜底 */
	inline constexpr float kMinAxisFacing = 1.0e-3f;
	/* 环几乎侧对相机时平面求交退化：改用屏幕角度兜底 */
	inline constexpr float kMinRingFacing = 0.12f;
	/* 中心方块的行程基准：鼠标每移这么多像素，缩放倍率 ±1 */
	inline constexpr float kUniformScalePixels = 100.0f;

	/* ---- 一帧的布局：绘制、命中、拖拽共用同一份采样结果 ---- */
	struct Layout
	{
		glm::mat4 ViewProjection{ 1.0f };
		glm::mat4 InverseViewProjection{ 1.0f };
		glm::vec2 ViewportMin{ 0.0f };
		glm::vec2 ViewportSize{ 1.0f };

		glm::vec3 Origin{ 0.0f };			/* gizmo 原点（世界） */
		glm::mat3 Basis{ 1.0f };			/* 三列 = 三根轴的世界方向（已归一化） */
		glm::vec3 CameraPosition{ 0.0f };
		glm::vec3 CameraForward{ 0.0f, 0.0f, -1.0f };
		glm::vec3 CameraRight{ 1.0f, 0.0f, 0.0f };
		glm::vec3 CameraUp{ 0.0f, 1.0f, 0.0f };

		glm::vec2 Center{ 0.0f };			/* 原点投影到屏幕的位置 */
		float WorldPerPixel{ 0.0f };		/* 原点处 1 逻辑像素对应的世界长度 */
		bool Valid{ false };				/* 原点在相机背后 / 视口退化时整帧不画也不拾取 */
	};

	inline glm::vec3 AxisNormal(const Layout& layout, int32_t axis)
	{
		const int32_t index = std::clamp(axis, 0, kAxisCount - 1);
		return glm::normalize(layout.Basis[index]);
	}

	/* 世界点 → 屏幕点。w <= 0 表示在相机背后（NDC 会镜像），返回 false 由调用方丢弃。 */
	inline bool ProjectPoint(const Layout& layout, const glm::vec3& point, glm::vec2& out)
	{
		const glm::vec4 clip = layout.ViewProjection * glm::vec4(point, 1.0f);
		if (clip.w <= 1e-6f)
			return false;

		const glm::vec2 ndc = glm::vec2(clip.x, clip.y) / clip.w;
		out = layout.ViewportMin + glm::vec2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f) * layout.ViewportSize;
		return true;
	}

	/* 世界长度 → 屏幕像素（用于把几何容差换算成拾取阈值） */
	inline float WorldToPixels(const Layout& layout, float world_length)
	{
		return layout.WorldPerPixel > 0.0f ? world_length / layout.WorldPerPixel : 0.0f;
	}

	/* 组装布局：basis 取实体世界变换的三列（去掉缩放），nullptr 语义的退化轴用单位轴兜底。
	 * screen_scale = 每逻辑像素在单位距离上的世界尺寸（见 SceneGizmos::ComputeGizmoScreenScale）；
	 * 相机右 / 上方向用于中心球的屏幕空间自由平移。 */
	inline Layout BuildLayout(const glm::vec3& origin, const glm::mat3& basis,
		const glm::vec3& camera_position, const glm::vec3& camera_forward,
		const glm::vec3& camera_right, const glm::vec3& camera_up,
		float screen_scale,
		const glm::mat4& view_projection, const glm::vec2& viewport_min, const glm::vec2& viewport_size)
	{
		Layout layout;
		layout.ViewProjection = view_projection;
		layout.InverseViewProjection = glm::inverse(view_projection);
		layout.ViewportMin = viewport_min;
		layout.ViewportSize = glm::vec2(glm::max(viewport_size.x, 1.0f), glm::max(viewport_size.y, 1.0f));
		layout.Origin = origin;
		layout.CameraPosition = camera_position;
		layout.CameraForward = glm::normalize(camera_forward);
		layout.CameraRight = glm::normalize(camera_right);
		layout.CameraUp = glm::normalize(camera_up);

		for (glm::length_t axis = 0; axis < 3; ++axis)
		{
			const glm::vec3 column = basis[axis];
			const float length = glm::length(column);
			layout.Basis[axis] = length > 1e-6f ? column / length : glm::vec3(
				axis == 0 ? 1.0f : 0.0f, axis == 1 ? 1.0f : 0.0f, axis == 2 ? 1.0f : 0.0f);
		}

		/* 原点到相机的视线深度决定 1 像素的世界长度：透视下与深度成正比 */
		const float depth = glm::dot(origin - camera_position, layout.CameraForward);
		layout.WorldPerPixel = screen_scale * std::max(depth, 1.0e-4f);
		if (!(layout.WorldPerPixel > 0.0f) || !std::isfinite(layout.WorldPerPixel))
			return layout;

		layout.Valid = ProjectPoint(layout, origin, layout.Center);
		return layout;
	}

	/* 环在环平面内的正交基：u × v = 法线（角度沿 u→v 递增 = 绕法线的右手旋转） */
	inline void RingBasis(const glm::vec3& normal, glm::vec3& u, glm::vec3& v)
	{
		const glm::vec3 helper = std::fabs(normal.y) < 0.9f
			? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
		u = glm::normalize(glm::cross(helper, normal));
		v = glm::cross(normal, u);
	}

	/* ---- 手柄的世界变换：渲染与命中共用同一份尺寸 ----
	 * 网格口径（见 SceneGizmos）：圆柱半径 1 长 1 沿 +Z、圆锥底面半径 1 高 3 沿 +Z、
	 * 圆环大半径 1 管半径 kRingTubeRatio、立方体边长 1 中心在原点。 */
	inline float AxisLength(const Layout& layout)
	{
		return kAxisLengthPx * layout.WorldPerPixel;
	}

	inline float ArrowLength(const Layout& layout)
	{
		return std::min(kArrowLengthPx * layout.WorldPerPixel, AxisLength(layout) * 0.45f);
	}

	inline float RingRadius(const Layout& layout)
	{
		return kRingRadiusPx * layout.WorldPerPixel;
	}

	inline float CubeHalfExtent(const Layout& layout)
	{
		return kCubeHalfPx * layout.WorldPerPixel;
	}

	inline float CenterHalfExtent(const Layout& layout)
	{
		return kCenterHalfPx * layout.WorldPerPixel;
	}

	inline float CenterSphereRadius(const Layout& layout)
	{
		return kCenterSphereRatio * AxisLength(layout);
	}

	inline float OuterRingRadius(const Layout& layout)
	{
		return kOuterRingRatio * RingRadius(layout);
	}

	/* 平面手柄：法线轴 n，跨过另外两根轴；边长与中心距都按轴长折算 */
	inline float PlaneSide(const Layout& layout)
	{
		return kPlaneSideRatio * AxisLength(layout);
	}

	/* 中心沿两根轴方向的角平分线外移 —— 与 ImGuizmo 一样贴在两根轴之间 */
	inline glm::vec3 PlaneCenter(const Layout& layout, int32_t normal_axis)
	{
		const int32_t axis_a = (normal_axis + 1) % 3;
		const int32_t axis_b = (normal_axis + 2) % 3;
		const glm::vec3 bisector = AxisNormal(layout, axis_a) + AxisNormal(layout, axis_b);
		const float length = glm::length(bisector);
		if (length < 1.0e-6f)
			return layout.Origin;

		return layout.Origin + bisector / length * (kPlaneOffsetRatio * AxisLength(layout));
	}

	struct Handles
	{
		glm::mat4 Shaft[kAxisCount]{};
		glm::mat4 Arrow[kAxisCount]{};
		glm::mat4 Cube[kAxisCount]{};
		glm::mat4 Ring[kAxisCount]{};
		glm::mat4 Plane[kAxisCount]{};	/* 索引 = 平面法线轴 */
		glm::mat4 Center{ 1.0f };		/* 缩放轴心方块（边长 1 的立方体） */
		glm::mat4 CenterSphere{ 1.0f };	/* 平移中心球（半径 1 的球） */
		glm::mat4 OuterRing{ 1.0f };	/* 白色外环（朝向相机） */
	};

	/* +Z → 轴方向的旋转；轴与 +Z 反向时 glm 会走 180° 特例分支 */
	inline glm::quat AxisRotation(const glm::vec3& axis)
	{
		return glm::rotation(glm::vec3(0.0f, 0.0f, 1.0f), glm::normalize(axis));
	}

	inline Handles ComputeHandles(const Layout& layout, Operation operation)
	{
		Handles handles;
		const glm::mat4 to_origin = glm::translate(glm::mat4(1.0f), layout.Origin);

		const float axis_length = AxisLength(layout);
		const float arrow_length = ArrowLength(layout);
		const float cube_half = CubeHalfExtent(layout);
		/* 轴杆必须顶到端点图形：平移让出箭头的长度，缩放让出方块的半边长，
		 * 否则两种模式都会在轴杆和端点之间留一截空隙 */
		const float shaft_length = operation == Operation::Scale
			? std::max(axis_length - cube_half, 1.0e-6f)
			: std::max(axis_length - arrow_length, 1.0e-6f);
		const float shaft_radius = kShaftRadiusPx * layout.WorldPerPixel;
		const float arrow_radius = kArrowRadiusPx * layout.WorldPerPixel;
		const float ring_radius = RingRadius(layout);

		for (int32_t axis = 0; axis < kAxisCount; ++axis)
		{
			const glm::vec3 direction = AxisNormal(layout, axis);
			const glm::mat4 orientation = glm::mat4_cast(AxisRotation(direction));

			/* 轴杆：圆柱网格本身从 0 到 +Z，直接缩放到 shaft_length（不要再平移半段） */
			handles.Shaft[axis] = to_origin * orientation
				* glm::scale(glm::mat4(1.0f), glm::vec3(shaft_radius, shaft_radius, shaft_length));

			/* 箭头：圆锥底面落在 shaft 末端，尖端压在轴端（网格高 3 → z 缩放 1/3） */
			handles.Arrow[axis] = to_origin * orientation
				* glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, shaft_length))
				* glm::scale(glm::mat4(1.0f), glm::vec3(arrow_radius, arrow_radius, arrow_length / 3.0f));

			/* 缩放手柄：轴端立方体 */
			handles.Cube[axis] = to_origin
				* glm::translate(glm::mat4(1.0f), direction * axis_length)
				* orientation
				* glm::scale(glm::mat4(1.0f), glm::vec3(cube_half * 2.0f));

			/* 旋转环：网格大半径 1、管半径 kRingTubeRatio，整环等比缩放 */
			handles.Ring[axis] = to_origin * orientation * glm::scale(glm::mat4(1.0f), glm::vec3(ring_radius));
		}

		/* 平移平面手柄：单位四边形（XY 平面、边长 1、中心原点）由两根轴的方向摆位，
		 * 沿两轴角平分线外移固定距离 —— 拾取用的是同一组方向，见 HitTest */
		const float plane_side = PlaneSide(layout);
		for (int32_t normal_axis = 0; normal_axis < kAxisCount; ++normal_axis)
		{
			const glm::vec3 axis_a = AxisNormal(layout, (normal_axis + 1) % 3);
			const glm::vec3 axis_b = AxisNormal(layout, (normal_axis + 2) % 3);
			const glm::vec3 normal = AxisNormal(layout, normal_axis);
			const glm::vec3 center = PlaneCenter(layout, normal_axis);

			handles.Plane[normal_axis] = glm::mat4(
				glm::vec4(axis_a * plane_side, 0.0f),
				glm::vec4(axis_b * plane_side, 0.0f),
				glm::vec4(normal * plane_side, 0.0f),
				glm::vec4(center, 1.0f));
		}

		handles.Center = to_origin * glm::scale(glm::mat4(1.0f), glm::vec3(CenterHalfExtent(layout) * 2.0f));
		handles.CenterSphere = to_origin * glm::scale(glm::mat4(1.0f), glm::vec3(CenterSphereRadius(layout)));

		/* 白色外环：铺在垂直于视线的平面上（法线 = 相机前向），网格同样是 XY 平面上的单位圆环 */
		glm::vec3 ring_u, ring_v;
		RingBasis(layout.CameraForward, ring_u, ring_v);
		const glm::mat3 outer_basis(ring_u, ring_v, layout.CameraForward);
		handles.OuterRing = to_origin * glm::mat4(outer_basis)
			* glm::scale(glm::mat4(1.0f), glm::vec3(OuterRingRadius(layout)));
		return handles;
	}

	/* ---- 射线 ---- */
	struct Ray
	{
		glm::vec3 Origin{ 0.0f };
		glm::vec3 Direction{ 0.0f, 0.0f, -1.0f };
	};

	/* 屏幕点 → 世界射线：反投影 NDC 中面一点（z = 0.5），与相机位置连线。
	 * 取中面而不是近 / 远平面：编辑器相机是反向 Z，近远平面的 z 语义与常规相反，中面绕开这一点。 */
	inline Ray MakeRay(const Layout& layout, const glm::vec2& screen_point)
	{
		const glm::vec2 ndc{
			(screen_point.x - layout.ViewportMin.x) / layout.ViewportSize.x * 2.0f - 1.0f,
			1.0f - (screen_point.y - layout.ViewportMin.y) / layout.ViewportSize.y * 2.0f };

		glm::vec4 point = layout.InverseViewProjection * glm::vec4(ndc.x, ndc.y, 0.5f, 1.0f);
		if (!(std::fabs(point.w) > 1e-6f))
			point.w = 1.0f;
		point /= point.w;

		Ray ray;
		ray.Origin = layout.CameraPosition;
		const glm::vec3 to_point = glm::vec3(point) - ray.Origin;
		const float length = glm::length(to_point);
		ray.Direction = length > 1e-6f ? to_point / length : layout.CameraForward;
		return ray;
	}

	/* 射线 × 平面。射线与平面几乎平行时分母趋零、交点飞到无穷远，判失败。 */
	inline bool IntersectRayPlane(const Ray& ray, const glm::vec3& plane_point,
		const glm::vec3& plane_normal, glm::vec3& hit)
	{
		const float denominator = glm::dot(plane_normal, ray.Direction);
		if (std::fabs(denominator) < 1.0e-5f)
			return false;

		const float distance = glm::dot(plane_point - ray.Origin, plane_normal) / denominator;
		if (!std::isfinite(distance))
			return false;

		hit = ray.Origin + ray.Direction * distance;
		return true;
	}

	/* 射线到线段的最短距离（射线参数 s >= 0、线段参数 t ∈ [0, 1]）。
	 * 轴杆 / 箭头 / 环折线段共用它 —— 命中距离就是两者在世界空间的真实间距。 */
	inline float DistanceRayToSegment(const Ray& ray, const glm::vec3& from, const glm::vec3& to)
	{
		const glm::vec3 u = ray.Direction;
		const glm::vec3 v = to - from;
		const glm::vec3 w = ray.Origin - from;

		const float a = glm::dot(u, u);
		const float b = glm::dot(u, v);
		const float c = glm::dot(v, v);
		const float d = glm::dot(u, w);
		const float e = glm::dot(v, w);

		float s = 0.0f;
		float t = 0.0f;
		const float denominator = a * c - b * b;
		if (std::fabs(denominator) > 1.0e-8f)
		{
			s = (b * e - c * d) / denominator;
			t = (a * e - b * d) / denominator;
		}

		if (s < 0.0f)
		{
			s = 0.0f;
			t = c > 1.0e-8f ? glm::clamp(e / c, 0.0f, 1.0f) : 0.0f;
		}
		else if (t < 0.0f)
		{
			t = 0.0f;
			s = std::max(-d / std::max(a, 1.0e-8f), 0.0f);
		}
		else if (t > 1.0f)
		{
			t = 1.0f;
			s = std::max((b - d) / std::max(a, 1.0e-8f), 0.0f);
		}

		const glm::vec3 point_ray = ray.Origin + u * s;
		const glm::vec3 point_segment = from + v * t;
		return glm::length(point_ray - point_segment);
	}

	/* 射线 × 有向盒（三根正交轴 + 各轴半长）：轴端方块与轴心方块共用 */
	inline bool IntersectRayBox(const Ray& ray, const glm::vec3& center,
		const glm::vec3& axis_x, const glm::vec3& axis_y, const glm::vec3& axis_z,
		const glm::vec3& half_extent)
	{
		const glm::vec3 local_origin = glm::vec3(
			glm::dot(ray.Origin - center, axis_x),
			glm::dot(ray.Origin - center, axis_y),
			glm::dot(ray.Origin - center, axis_z));
		const glm::vec3 local_direction = glm::vec3(
			glm::dot(ray.Direction, axis_x),
			glm::dot(ray.Direction, axis_y),
			glm::dot(ray.Direction, axis_z));

		float t_min = 0.0f;
		float t_max = std::numeric_limits<float>::max();
		for (int32_t axis = 0; axis < 3; ++axis)
		{
			const float origin = local_origin[axis];
			const float direction = local_direction[axis];
			const float extent = half_extent[axis];

			if (std::fabs(direction) < 1.0e-7f)
			{
				if (origin < -extent || origin > extent)
					return false;
				continue;
			}

			float t_near = (-extent - origin) / direction;
			float t_far = (extent - origin) / direction;
			if (t_near > t_far)
				std::swap(t_near, t_far);

			t_min = std::max(t_min, t_near);
			t_max = std::min(t_max, t_far);
			if (t_min > t_max)
				return false;
		}
		return true;
	}

	/* 射线 × 球（中心 + 半径）：平移的中心球 */
	inline bool IntersectRaySphere(const Ray& ray, const glm::vec3& center, float radius)
	{
		const glm::vec3 offset = ray.Origin - center;
		const float b = glm::dot(offset, ray.Direction);
		const float c = glm::dot(offset, offset) - radius * radius;
		const float discriminant = b * b - c;
		if (discriminant < 0.0f)
			return false;

		/* 射线起点在球外：取较小的根（近端）；起点在球内则直接算命中 */
		return -b - std::sqrt(discriminant) >= 0.0f || c <= 0.0f;
	}

	/* 轴端方块 / 轴心方块的正交基：z = 轴方向（轴心方块用 gizmo 自己的基） */
	inline void BoxAxes(const glm::vec3& axis_z, glm::vec3& axis_x, glm::vec3& axis_y)
	{
		const glm::vec3 helper = std::fabs(axis_z.y) < 0.9f
			? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
		axis_x = glm::normalize(glm::cross(helper, axis_z));
		axis_y = glm::cross(axis_z, axis_x);
	}

	/* ---- 命中 ---- */
	struct Hit
	{
		int32_t Handle{ -1 };
		float Distance{ std::numeric_limits<float>::max() };	/* 屏幕像素 */
		float Depth{ std::numeric_limits<float>::max() };		/* 视线深度：距离并列时取更近的 */
	};

	inline void ConsiderHit(Hit& best, int32_t handle, float distance, float depth)
	{
		const bool closer = distance < best.Distance - 1.0f;
		const bool tie_but_nearer = std::fabs(distance - best.Distance) <= 1.0f && depth < best.Depth;
		if (best.Handle < 0 || closer || tie_but_nearer)
		{
			best.Handle = handle;
			best.Distance = distance;
			best.Depth = depth;
		}
	}

	/* 命中：全部在世界空间做射线求交，阈值按拾取像素折算。
	 * 旋转环按绘制用的同一组折线段求距离（侧对相机时也稳，不用平面求交）。 */
	inline int32_t HitTest(const Layout& layout, Operation operation, const glm::vec2& mouse)
	{
		if (!layout.Valid || operation == Operation::None)
			return -1;

		const Ray ray = MakeRay(layout, mouse);
		Hit best;

		if (operation == Operation::Rotate)
		{
			/* 白色外环：与画面平行的圆，按平面求交后比半径（交点必在环平面内，
			 * 命中距离就是交点与圆周的世界间距） */
			const float outer_radius = OuterRingRadius(layout);
			glm::vec3 outer_hit;
			if (IntersectRayPlane(ray, layout.Origin, layout.CameraForward, outer_hit))
			{
				const float radial_error = std::fabs(glm::length(outer_hit - layout.Origin) - outer_radius);
				const float distance = WorldToPixels(layout, radial_error);
				if (distance <= kPickPx)
					ConsiderHit(best, kOuterRingHandle, distance, -1.0f);
			}

			const float ring_radius = RingRadius(layout);
			for (int32_t axis = 0; axis < kAxisCount; ++axis)
			{
				const glm::vec3 normal = AxisNormal(layout, axis);
				glm::vec3 u, v;
				RingBasis(normal, u, v);

				for (int32_t segment = 0; segment < kRingSegments; ++segment)
				{
					const float angle_from = glm::two_pi<float>() * static_cast<float>(segment)
						/ static_cast<float>(kRingSegments);
					const float angle_to = glm::two_pi<float>() * static_cast<float>(segment + 1)
						/ static_cast<float>(kRingSegments);

					const glm::vec3 from = layout.Origin
						+ (u * std::cos(angle_from) + v * std::sin(angle_from)) * ring_radius;
					const glm::vec3 to = layout.Origin
						+ (u * std::cos(angle_to) + v * std::sin(angle_to)) * ring_radius;

					const float distance = WorldToPixels(layout, DistanceRayToSegment(ray, from, to));
					if (distance > kPickPx)
						continue;

					/* 同一根环上离相机最近的那段才是画面里压在上面的 */
					const float depth = std::min(
						glm::dot(from - layout.CameraPosition, layout.CameraForward),
						glm::dot(to - layout.CameraPosition, layout.CameraForward));
					ConsiderHit(best, axis, distance, depth);
				}
			}

			/* 中心淡圆（trackball）：圆盘内任意点都能起拖，优先级低于环 */
			{
				glm::vec3 disc_hit;
				if (IntersectRayPlane(ray, layout.Origin, layout.CameraForward, disc_hit))
				{
					const float disc_radius = RingRadius(layout) * kTrackballRadiusRatio();
					if (glm::length(disc_hit - layout.Origin) <= disc_radius * 0.92f)
						ConsiderHit(best, kTrackballHandle, 1.0f,
							glm::dot(layout.Origin - layout.CameraPosition, layout.CameraForward));
				}
			}

			return best.Handle;
		}

		const float axis_length = AxisLength(layout);
		const float arrow_length = ArrowLength(layout);
		const float shaft_end = std::max(axis_length - arrow_length, 0.0f);

		for (int32_t axis = 0; axis < kAxisCount; ++axis)
		{
			const glm::vec3 direction = AxisNormal(layout, axis);
			const glm::vec3 tip = layout.Origin + direction * axis_length;
			const glm::vec3 shaft_tip = layout.Origin + direction * shaft_end;

			const float shaft_distance = WorldToPixels(layout, DistanceRayToSegment(ray,
				layout.Origin, shaft_tip));
			if (shaft_distance <= kPickPx)
				ConsiderHit(best, axis, shaft_distance, glm::dot(shaft_tip - layout.CameraPosition, layout.CameraForward));

			if (operation == Operation::Translate)
			{
				/* 箭头：底面半径 6.5 像素，按几何半径放宽阈值 —— 箭头的实体部分比轴杆粗 */
				const float arrow_distance = WorldToPixels(layout, DistanceRayToSegment(ray, shaft_tip, tip));
				if (arrow_distance <= kPickPx + kArrowRadiusPx * 0.5f)
					ConsiderHit(best, axis, std::max(arrow_distance - kArrowRadiusPx * 0.5f, 0.0f),
						glm::dot(tip - layout.CameraPosition, layout.CameraForward));
			}
			else if (operation == Operation::Scale)
			{
				/* 轴端方块：盒体按几何尺寸 + 拾取容差外扩 */
				glm::vec3 axis_x, axis_y;
				BoxAxes(direction, axis_x, axis_y);
				const float half = CubeHalfExtent(layout) + kPickPx * layout.WorldPerPixel * 0.5f;
				if (IntersectRayBox(ray, tip, axis_x, axis_y, direction, glm::vec3(half)))
					ConsiderHit(best, axis, 0.0f, glm::dot(tip - layout.CameraPosition, layout.CameraForward));
			}
		}

		if (operation == Operation::Translate)
		{
			/* 平面手柄：与所在平面求交，交点落在四边形内即命中（局部坐标 ±半边长） */
			const float half_side = PlaneSide(layout) * 0.5f;
			for (int32_t normal_axis = 0; normal_axis < kAxisCount; ++normal_axis)
			{
				const glm::vec3 normal = AxisNormal(layout, normal_axis);
				glm::vec3 hit;
				if (!IntersectRayPlane(ray, layout.Origin, normal, hit))
					continue;

				const glm::vec3 center = PlaneCenter(layout, normal_axis);
				const glm::vec3 offset = hit - center;
				const float along_a = glm::dot(offset, AxisNormal(layout, (normal_axis + 1) % 3));
				const float along_b = glm::dot(offset, AxisNormal(layout, (normal_axis + 2) % 3));
				if (std::fabs(along_a) > half_side || std::fabs(along_b) > half_side)
					continue;

				ConsiderHit(best, kPlaneHandleBase + normal_axis, 0.0f,
					glm::dot(center - layout.CameraPosition, layout.CameraForward));
			}

			/* 中心球：屏幕空间自由平移，压在轴杆起点上，优先级最高 */
			if (IntersectRaySphere(ray, layout.Origin, CenterSphereRadius(layout)))
				ConsiderHit(best, kCenterHandle, -2.0f,
					glm::dot(layout.Origin - layout.CameraPosition, layout.CameraForward));
		}

		if (operation == Operation::Scale)
		{
			/* 平面手柄：两轴同步缩放，命中判定与平移的平面手柄完全一致 */
			const float half_side = PlaneSide(layout) * 0.5f;
			for (int32_t normal_axis = 0; normal_axis < kAxisCount; ++normal_axis)
			{
				const glm::vec3 normal = AxisNormal(layout, normal_axis);
				glm::vec3 hit;
				if (!IntersectRayPlane(ray, layout.Origin, normal, hit))
					continue;

				const glm::vec3 center = PlaneCenter(layout, normal_axis);
				const glm::vec3 offset = hit - center;
				const float along_a = glm::dot(offset, AxisNormal(layout, (normal_axis + 1) % 3));
				const float along_b = glm::dot(offset, AxisNormal(layout, (normal_axis + 2) % 3));
				if (std::fabs(along_a) > half_side || std::fabs(along_b) > half_side)
					continue;

				ConsiderHit(best, kPlaneHandleBase + normal_axis, 0.0f,
					glm::dot(center - layout.CameraPosition, layout.CameraForward));
			}

			const glm::vec3 axis_x = AxisNormal(layout, 0);
			const glm::vec3 axis_y = AxisNormal(layout, 1);
			const glm::vec3 axis_z = AxisNormal(layout, 2);
			const float half = CenterHalfExtent(layout) + kPickPx * layout.WorldPerPixel * 0.5f;
			if (IntersectRayBox(ray, layout.Origin, axis_x, axis_y, axis_z, glm::vec3(half)))
				ConsiderHit(best, kCenterHandle, -2.0f, glm::dot(layout.Origin - layout.CameraPosition, layout.CameraForward));
		}

		return best.Handle;
	}

	/* ---- 拖拽状态与数学 ---- */
	struct Drag
	{
		bool Active{ false };
		Operation Kind{ Operation::None };
		int32_t Handle{ -1 };

		glm::vec3 Origin{ 0.0f };
		glm::vec3 Axis{ 0.0f, 1.0f, 0.0f };
		glm::vec3 PlanePoint{ 0.0f };
		glm::vec3 PlaneNormal{ 0.0f, 1.0f, 0.0f };
		glm::vec3 RingU{ 1.0f, 0.0f, 0.0f };
		glm::vec3 RingV{ 0.0f, 1.0f, 0.0f };

		float StartParam{ 0.0f };			/* 平移 / 缩放：按下时在轴上的参数 */
		glm::vec3 StartPlanePoint{ 0.0f };	/* 平面手柄：按下时在平面上的落点 */
		glm::vec3 TrackballStart{ 0.0f };	/* trackball：按下时的球面点（相机空间） */
		glm::vec2 StartMouse{ 0.0f };		/* 中心球：按下时的屏幕位置 */
		glm::vec3 CameraRight{ 1.0f, 0.0f, 0.0f };
		glm::vec3 CameraUp{ 0.0f, 1.0f, 0.0f };
		float WorldPerPixel{ 1.0f };		/* 按下时的像素换算：拖拽中不随原点漂移 */
		float StartAngle{ 0.0f };			/* 旋转：按下时在环平面内的角度 */
		float LastAngle{ 0.0f };			/* 上一帧角度：跨 ±π 时用差分累加解跳变 */
		float AccumulatedAngle{ 0.0f };
		float AxisWorldLength{ 1.0f };		/* 缩放的基准长度 = 手柄的世界长度 */
		float StartScreenDistance{ 1.0f };	/* 中心缩放的基准屏幕距离 */

		bool ScreenAngleFallback{ false };	/* 环侧对相机：改用屏幕角度 */
		float AngleSign{ 1.0f };
	};

	/* 拖拽增量：三者只有一项有效（按 Operation 分派） */
	struct Delta
	{
		glm::vec3 Translation{ 0.0f };
		float Rotation{ 0.0f };		/* 绕 Drag::Axis 的弧度 */
		glm::quat RotationQuat{ 1.0f, 0.0f, 0.0f, 0.0f };	/* trackball：任意轴旋转 */
		glm::vec3 Scale{ 1.0f };	/* 各局部轴的乘数 */

		[[nodiscard]] bool IsIdentity() const
		{
			return Translation.x == 0.0f && Translation.y == 0.0f && Translation.z == 0.0f
				&& Rotation == 0.0f
				&& RotationQuat.w > 0.999999f
				&& Scale.x == 1.0f && Scale.y == 1.0f && Scale.z == 1.0f;
		}
	};

	/* 屏幕绕中心的角度（数学正方向：y 轴向上；屏幕坐标 y 向下，故对 dy 取负） */
	inline float ScreenAngleAt(const glm::vec2& center, const glm::vec2& point)
	{
		return std::atan2(-(point.y - center.y), point.x - center.x);
	}

	/* 屏幕点投影到以 gizmo 中心为圆心的单位球：球内取球面，球外压到赤道 */
	inline glm::vec3 ProjectToTrackball(const Layout& layout, const glm::vec2& screen, float radius)
	{
		/* radius 是世界长度，屏幕点是逻辑像素：先换算成像素再归一化 */
		const float radius_px = glm::max(radius / glm::max(layout.WorldPerPixel, 1.0e-6f), 1.0e-4f);
		const glm::vec2 local{ (screen.x - layout.Center.x) / radius_px,
			-(screen.y - layout.Center.y) / radius_px };
		const float length_sq = glm::dot(local, local);
		if (length_sq <= 1.0f)
			return glm::vec3(local.x, local.y, std::sqrt(1.0f - length_sq));
		return glm::vec3(local / std::sqrt(length_sq), 0.0f);
	}

	inline bool BeginDrag(const Layout& layout, Operation operation, int32_t handle,
		const glm::vec2& mouse, Drag& drag)
	{
		drag = Drag{};
		if (!layout.Valid || operation == Operation::None || handle < 0)
			return false;

		/* 记下操作类型：UI 侧靠它判断「当前在旋转」来写角度弧 */
		drag.Kind = operation;
		drag.Handle = handle;
		drag.Origin = layout.Origin;

		const Ray ray = MakeRay(layout, mouse);

		/* 中心淡圆：记下按下时的球面点 */
		if (operation == Operation::Rotate && handle == kTrackballHandle)
		{
			drag.TrackballStart = ProjectToTrackball(layout, mouse,
				RingRadius(layout) * kTrackballRadiusRatio());
			drag.Active = true;
			return true;
		}

		if (operation == Operation::Rotate)
		{
			if (handle >= kAxisCount && handle != kOuterRingHandle)
				return false;

			/* 白色外环绕视线旋转：轴取相机前向的反向（指向相机），屏幕逆时针就是正角 */
			drag.Axis = handle == kOuterRingHandle
				? -layout.CameraForward : AxisNormal(layout, handle);
			drag.PlanePoint = layout.Origin;
			drag.PlaneNormal = drag.Axis;
			RingBasis(drag.Axis, drag.RingU, drag.RingV);

			/* 相机在轴的正侧时，绕轴的右手旋转在屏幕上表现为逆时针（屏幕角增大） */
			drag.AngleSign = glm::dot(drag.Axis, layout.CameraPosition - drag.Origin) >= 0.0f ? 1.0f : -1.0f;

			glm::vec3 hit;
			const bool plane_usable = std::fabs(glm::dot(drag.PlaneNormal, ray.Direction)) >= kMinRingFacing;
			if (plane_usable && IntersectRayPlane(ray, drag.PlanePoint, drag.PlaneNormal, hit))
			{
				const glm::vec3 offset = hit - drag.Origin;
				drag.StartAngle = std::atan2(glm::dot(offset, drag.RingV), glm::dot(offset, drag.RingU));
			}
			else
			{
				drag.ScreenAngleFallback = true;
				drag.StartAngle = ScreenAngleAt(layout.Center, mouse);
			}

			drag.LastAngle = drag.StartAngle;
			drag.Active = true;
			return true;
		}

		/* 缩放的平面手柄：与平移一样先求出按下点所在的平面落点 */
		if (operation == Operation::Scale && handle >= kPlaneHandleBase)
		{
			const int32_t normal_axis = handle - kPlaneHandleBase;
			if (normal_axis >= kAxisCount)
				return false;

			drag.PlaneNormal = glm::normalize(AxisNormal(layout, normal_axis));
			drag.PlanePoint = layout.Origin;

			glm::vec3 hit;
			if (!IntersectRayPlane(MakeRay(layout, mouse), drag.PlanePoint, drag.PlaneNormal, hit))
				return false;

			drag.StartPlanePoint = hit;
			drag.AxisWorldLength = std::max(AxisLength(layout), 1.0e-4f);
			drag.Active = true;
			return true;
		}

		if (operation == Operation::Scale && handle == kCenterHandle)
		{
			const glm::vec2 offset{ mouse.x - layout.Center.x, mouse.y - layout.Center.y };
			drag.StartScreenDistance = std::max(glm::length(offset), 1.0f);
			drag.AxisWorldLength = std::max(AxisLength(layout), 1.0e-4f);
			drag.Active = true;
			return true;
		}

		/* 平移中心球：屏幕空间自由平移，按下点与像素换算都存下来（拖拽中不重算） */
		if (operation == Operation::Translate && handle == kCenterHandle)
		{
			drag.StartMouse = mouse;
			drag.WorldPerPixel = layout.WorldPerPixel;
			drag.CameraRight = layout.CameraRight;
			drag.CameraUp = layout.CameraUp;
			drag.Active = true;
			return true;
		}

		/* 平移平面手柄：沿平面拖动，落点记在起始世界坐标里 */
		if (operation == Operation::Translate && handle >= kPlaneHandleBase
			&& handle < kPlaneHandleBase + kAxisCount)
		{
			const int32_t normal_axis = handle - kPlaneHandleBase;
			drag.Axis = AxisNormal(layout, normal_axis);
			drag.PlanePoint = layout.Origin;
			drag.PlaneNormal = drag.Axis;

			glm::vec3 hit;
			if (!IntersectRayPlane(ray, drag.PlanePoint, drag.PlaneNormal, hit))
				return false;

			drag.StartPlanePoint = hit;
			drag.Active = true;
			return true;
		}

		if (handle >= kAxisCount)
			return false;

		drag.Axis = AxisNormal(layout, handle);
		drag.AxisWorldLength = std::max(AxisLength(layout), 1.0e-4f);

		/* 求交平面：包含轴、法线尽量正对相机 —— 射线与平面接近垂直时交点最稳 */
		glm::vec3 normal = layout.CameraForward - drag.Axis * glm::dot(layout.CameraForward, drag.Axis);
		if (glm::length(normal) < kMinAxisFacing)
		{
			/* 轴正对视线：相机前向和轴平行，改用一个与视线垂直的方向当平面法线 */
			const glm::vec3 helper = std::fabs(layout.CameraForward.y) < 0.9f
				? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
			normal = glm::cross(drag.Axis, helper);
			if (glm::length(normal) < kMinAxisFacing)
				return false;
		}
		drag.PlaneNormal = glm::normalize(normal);
		drag.PlanePoint = layout.Origin;

		glm::vec3 hit;
		if (!IntersectRayPlane(ray, drag.PlanePoint, drag.PlaneNormal, hit))
			return false;

		drag.StartParam = glm::dot(hit - drag.Origin, drag.Axis);
		drag.Active = true;
		return true;
	}

	inline bool UpdateDrag(const Layout& layout, Drag& drag, const glm::vec2& mouse, bool snap, Delta& delta)
	{
		delta = Delta{};
		if (!drag.Active)
			return false;

		/* 中心淡圆：球面两点之间的旋转，轴和角都由拖动方向决定 */
		if (drag.Kind == Operation::Rotate && drag.Handle == kTrackballHandle)
		{
			const glm::vec3 current = ProjectToTrackball(layout, mouse,
				RingRadius(layout) * kTrackballRadiusRatio());
			glm::vec3 axis = glm::cross(drag.TrackballStart, current);
			const float axis_length = glm::length(axis);
			if (axis_length < 1.0e-5f)
				return false;

			axis /= axis_length;
			const float angle = std::acos(
				glm::clamp(glm::dot(drag.TrackballStart, current), -1.0f, 1.0f));

			/* 球面点在相机空间：x = right，y = up，z 指向观察者 */
			const glm::vec3 world_axis = layout.CameraRight * axis.x
				+ layout.CameraUp * axis.y - layout.CameraForward * axis.z;
			delta.RotationQuat = glm::angleAxis(angle, world_axis);
			return true;
		}

		if (drag.Kind == Operation::Rotate)
		{
			float angle = 0.0f;
			if (drag.ScreenAngleFallback)
			{
				angle = ScreenAngleAt(layout.Center, mouse) * drag.AngleSign;
			}
			else
			{
				glm::vec3 hit;
				if (!IntersectRayPlane(MakeRay(layout, mouse), drag.PlanePoint, drag.PlaneNormal, hit))
					return false;

				const glm::vec3 offset = hit - drag.Origin;
				angle = std::atan2(glm::dot(offset, drag.RingV), glm::dot(offset, drag.RingU));
			}

			/* 按帧差累加而不是直接取绝对角：跨 ±π 时绝对角会跳一整圈，累加差值则连续 */
			float step = angle - drag.LastAngle;
			if (step > glm::pi<float>())
				step -= glm::two_pi<float>();
			else if (step < -glm::pi<float>())
				step += glm::two_pi<float>();
			drag.LastAngle = angle;
			drag.AccumulatedAngle += step;

			delta.Rotation = drag.AccumulatedAngle;
			if (snap)
			{
				const float snap_step = glm::radians(15.0f);
				delta.Rotation = std::round(delta.Rotation / snap_step) * snap_step;
			}
			return true;
		}

		/* 缩放的平面手柄：鼠标在平面内离原点的距离之比，两根轴乘同一个倍数 */
		if (drag.Kind == Operation::Scale && drag.Handle >= kPlaneHandleBase)
		{
			const int32_t normal_axis = drag.Handle - kPlaneHandleBase;
			const int32_t u_axis = (normal_axis + 1) % kAxisCount;
			const int32_t v_axis = (normal_axis + 2) % kAxisCount;

			glm::vec3 hit;
			if (!IntersectRayPlane(MakeRay(layout, mouse), drag.PlanePoint, drag.PlaneNormal, hit))
				return false;

			const float start_radius = glm::max(glm::length(drag.StartPlanePoint - drag.Origin), 1.0e-4f);
			const float now_radius = glm::length(hit - drag.Origin);
			float multiplier = now_radius / start_radius;
			if (snap)
				multiplier = std::round(multiplier / 0.1f) * 0.1f;
			multiplier = glm::max(multiplier, 0.01f);

			glm::vec3 scale(1.0f);
			scale[u_axis] = multiplier;
			scale[v_axis] = multiplier;
			delta.Scale = scale;
			return true;
		}

		if (drag.Kind == Operation::Scale && drag.Handle == kCenterHandle)
		{
			const glm::vec2 offset{ mouse.x - layout.Center.x, mouse.y - layout.Center.y };
			float multiplier = 1.0f
				+ (glm::length(offset) - drag.StartScreenDistance) / kUniformScalePixels;
			if (snap)
				multiplier = std::round(multiplier / 0.1f) * 0.1f;
			multiplier = std::max(multiplier, 0.01f);
			delta.Scale = glm::vec3(multiplier);
			return true;
		}

		/* 平移中心球：鼠标像素位移换成相机右 / 上方向的世界位移，不约束到任何轴 */
		if (drag.Kind == Operation::Translate && drag.Handle == kCenterHandle)
		{
			const glm::vec2 moved{ mouse.x - drag.StartMouse.x, mouse.y - drag.StartMouse.y };
			glm::vec3 translation = drag.CameraRight * (moved.x * drag.WorldPerPixel)
				- drag.CameraUp * (moved.y * drag.WorldPerPixel);

			if (snap)
			{
				/* 绝对位置对齐 0.25 的世界网格（逐世界分量取整） */
				for (glm::length_t axis = 0; axis < 3; ++axis)
				{
					const float absolute = drag.Origin[axis] + translation[axis];
					translation[axis] = std::round(absolute / 0.25f) * 0.25f - drag.Origin[axis];
				}
			}

			delta.Translation = translation;
			return true;
		}

		glm::vec3 hit;
		if (!IntersectRayPlane(MakeRay(layout, mouse), drag.PlanePoint, drag.PlaneNormal, hit))
			return false;

		/* 平移平面手柄：起点到当前落点的世界位移（两端都在平面上，天然贴着平面走） */
		if (drag.Kind == Operation::Translate && drag.Handle >= kPlaneHandleBase)
		{
			glm::vec3 translation = hit - drag.StartPlanePoint;
			if (snap)
			{
				for (glm::length_t axis = 0; axis < 3; ++axis)
				{
					const float absolute = drag.Origin[axis] + translation[axis];
					translation[axis] = std::round(absolute / 0.25f) * 0.25f - drag.Origin[axis];
				}
			}

			delta.Translation = translation;
			return true;
		}

		const float param = glm::dot(hit - drag.Origin, drag.Axis);

		if (drag.Kind == Operation::Translate)
		{
			float offset = param - drag.StartParam;
			if (snap)
			{
				/* 绝对位置对齐 0.25 的世界网格：先加上原点在该轴上的投影再取整 */
				const float absolute = glm::dot(drag.Origin, drag.Axis) + offset;
				const float snapped = std::round(absolute / 0.25f) * 0.25f;
				offset = snapped - glm::dot(drag.Origin, drag.Axis);
			}
			delta.Translation = drag.Axis * offset;
			return true;
		}

		/* 缩放：按"手柄应该有的世界长度"取倍数 —— 鼠标向外拖出一个手柄长度即放大一倍。
		 * 分母用固定基准而不是按下时的轴向参数：抓在轴根部也不会除出天文数字。 */
		float multiplier = 1.0f + (param - drag.StartParam) / drag.AxisWorldLength;
		if (snap)
			multiplier = std::round(multiplier / 0.1f) * 0.1f;
		multiplier = std::max(multiplier, 0.01f);
		delta.Scale[drag.Handle] = multiplier;
		return true;
	}

	/* 把增量套到起始世界矩阵上。平移 / 旋转左乘（旋转绕 gizmo 原点）；
	 * 缩放右乘对角矩阵 —— 只改三个局部轴的模长，不引入切变。 */
	inline glm::mat4 ApplyDelta(const Drag& drag, const Delta& delta, const glm::mat4& start_world)
	{
		switch (drag.Kind)
		{
		case Operation::Translate:
			return glm::translate(glm::mat4(1.0f), delta.Translation) * start_world;
		case Operation::Rotate:
			if (delta.RotationQuat.w < 0.999999f)
			{
				return glm::translate(glm::mat4(1.0f), drag.Origin)
					* glm::mat4_cast(delta.RotationQuat)
					* glm::translate(glm::mat4(1.0f), -drag.Origin)
					* start_world;
			}
			return glm::translate(glm::mat4(1.0f), drag.Origin)
				* glm::rotate(glm::mat4(1.0f), delta.Rotation, drag.Axis)
				* glm::translate(glm::mat4(1.0f), -drag.Origin)
				* start_world;
		case Operation::Scale:
			return start_world * glm::scale(glm::mat4(1.0f), delta.Scale);
		default:
			return start_world;
		}
	}
}
