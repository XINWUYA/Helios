#pragma once
#include <glm/glm.hpp>
#include "Helios/Scene/Camera.h"

namespace Helios
{
	/* 相机预览：属性面板「Camera Preview」卡的画面来源。是编辑器侧的工具预览相机（走外部相机
	 * 登记 + 渲染到视图纹理），不接受输入、不叠 gizmo；每帧从被预览的场景相机同步参数，输出供
	 * 面板采样；来源相机自己的视图不受影响。 */
	class CameraPreview final : public Camera
	{
	public:
		CameraPreview();

		/* 从来源相机同步一帧：world_transform = 来源实体沿父链累积的世界变换（跟 Scene::OnUpdate 用
		 * 同一个矩阵，视图矩阵逐元素一致）；width / height = 预览画面尺寸（物理像素）。 */
		void SyncFrom(const Camera& source, const glm::mat4& world_transform, uint32_t width, uint32_t height);

	private:
		/* 组织渲染图：按管线分派到视图纹理（与编辑器相机同一分派，无叠加层） */
		bool ConstructRenderView(RenderView& render_view) override;
	};
}
