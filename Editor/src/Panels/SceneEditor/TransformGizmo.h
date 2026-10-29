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
	bool ProjectPoint(const Layout& layout, const glm::vec3& point, glm::vec2& out);

	/* 世界长度 → 屏幕像素（用于把几何容差换算成拾取阈值） */
	inline float WorldToPixels(const Layout& layout, float world_length)
	{
		return layout.WorldPerPixel > 0.0f ? world_length / layout.WorldPerPixel : 0.0f;
	}

	/* 组装布局：basis 取实体世界变换的三列（去掉缩放），nullptr 语义的退化轴用单位轴兜底。
	 * screen_scale = 每逻辑像素在单位距离上的世界尺寸（见 SceneGizmos::ComputeGizmoScreenScale）；
	 * 相机右 / 上方向用于中心球的屏幕空间自由平移。 */
	Layout BuildLayout(const glm::vec3& origin, const glm::mat3& basis,
		const glm::vec3& camera_position, const glm::vec3& camera_forward,
		const glm::vec3& camera_right, const glm::vec3& camera_up,
		float screen_scale,
		const glm::mat4& view_projection, const glm::vec2& viewport_min, const glm::vec2& viewport_size);

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
	glm::vec3 PlaneCenter(const Layout& layout, int32_t normal_axis);

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

	Handles ComputeHandles(const Layout& layout, Operation operation);

	/* ---- 射线 ---- */
	struct Ray
	{
		glm::vec3 Origin{ 0.0f };
		glm::vec3 Direction{ 0.0f, 0.0f, -1.0f };
	};

	/* 屏幕点 → 世界射线：反投影 NDC 中面一点（z = 0.5），与相机位置连线。
	 * 取中面而不是近 / 远平面：编辑器相机是反向 Z，近远平面的 z 语义与常规相反，中面绕开这一点。 */
	Ray MakeRay(const Layout& layout, const glm::vec2& screen_point);

	/* 射线 × 平面。射线与平面几乎平行时分母趋零、交点飞到无穷远，判失败。 */
	bool IntersectRayPlane(const Ray& ray, const glm::vec3& plane_point,
		const glm::vec3& plane_normal, glm::vec3& hit);

	/* 射线到线段的最短距离（射线参数 s >= 0、线段参数 t ∈ [0, 1]）。
	 * 轴杆 / 箭头 / 环折线段共用它 —— 命中距离就是两者在世界空间的真实间距。 */
	float DistanceRayToSegment(const Ray& ray, const glm::vec3& from, const glm::vec3& to);

	/* 射线 × 有向盒（三根正交轴 + 各轴半长）：轴端方块与轴心方块共用 */
	bool IntersectRayBox(const Ray& ray, const glm::vec3& center,
		const glm::vec3& axis_x, const glm::vec3& axis_y, const glm::vec3& axis_z,
		const glm::vec3& half_extent);

	/* 射线 × 球（中心 + 半径）：平移的中心球 */
	bool IntersectRaySphere(const Ray& ray, const glm::vec3& center, float radius);

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

	void ConsiderHit(Hit& best, int32_t handle, float distance, float depth);

	/* 命中：全部在世界空间做射线求交，阈值按拾取像素折算。
	 * 旋转环按绘制用的同一组折线段求距离（侧对相机时也稳，不用平面求交）。 */
	int32_t HitTest(const Layout& layout, Operation operation, const glm::vec2& mouse);

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
	glm::vec3 ProjectToTrackball(const Layout& layout, const glm::vec2& screen, float radius);

	bool BeginDrag(const Layout& layout, Operation operation, int32_t handle,
		const glm::vec2& mouse, Drag& drag);

	bool UpdateDrag(const Layout& layout, Drag& drag, const glm::vec2& mouse, bool snap, Delta& delta);

	/* 把增量套到起始世界矩阵上。平移 / 旋转左乘（旋转绕 gizmo 原点）；
	 * 缩放右乘对角矩阵 —— 只改三个局部轴的模长，不引入切变。 */
	glm::mat4 ApplyDelta(const Drag& drag, const Delta& delta, const glm::mat4& start_world);
}
