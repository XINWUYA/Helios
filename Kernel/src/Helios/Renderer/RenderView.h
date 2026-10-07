#pragma once
#include "RenderCommon.h"
#include <glm/glm.hpp>

#include "FrameGraph/FrameGraphResourceHandle.h"

namespace Helios
{
	class Camera;
	class Scene;
	class MeshSegment;
	class Material;
	class DeviceTexture;
	class DeviceFrameBuffer;
	class FrameGraph;
	class ShadowMapManager;
	class Light;

	/* 对当前RenderView可见的模型 */
	struct VisibleMeshObject
	{
		int ObjectId{ -1 };
		glm::mat4 Local2WorldMat;
		SharedPtr<MeshSegment> MeshSegment;
		/* 收集时按解析链定下的最终材质（实体绑定 ?: 模型默认 ?: 内置白模） */
		SharedPtr<Material> Material;
		/* 投影/接受阴影（来自 ModelComponent）：分别为"是否写入阴影图"与"是否采样阴影" */
		bool CastShadow{ true };
		bool ReceiveShadow{ true };

		VisibleMeshObject() = default;
		VisibleMeshObject(int object_id, const glm::mat4& local_2_world,
			const SharedPtr<class MeshSegment>& mesh_segment, const SharedPtr<class Material>& material,
			bool cast_shadow = true, bool receive_shadow = true)
			: ObjectId(object_id), Local2WorldMat(local_2_world), MeshSegment(mesh_segment), Material(material),
			  CastShadow(cast_shadow), ReceiveShadow(receive_shadow)
		{}
	};

	/* 视图类：
	 * 每个相机当对应一个RenderView
	 * 每个RenderView单独对应一个FrameGraph
	 * 每个RenderView单独做裁剪
	 * 每个RenderView单独对应一套后处理
	 */
	class RenderView
	{
	public:
		RenderView(Camera* owner_camera);
		~RenderView();

		/* 获取裁剪相机 */
		[[nodiscard]] const Camera* GetCullingCamera() const { return m_pOwnerCamera; }

		/* 设置启用视锥体剔除 */
		void SetEnableFrustumCulling(bool enable) { m_IsEnableCulling = enable; }
		[[nodiscard]] bool IsEnableFrustumCulling() const { return m_IsEnableCulling; }

		/* 设置视口区域 */
		void SetViewportRegion(const ViewportRegion& region);
		[[nodiscard]] const ViewportRegion& GetViewportRegion() const { return m_ViewportRegion; }

		/* 设置所属的Scene */
		void SetOwnerScene(const SharedPtr<Scene>& scene) { m_pOwnerScene = scene; }
        SharedPtr<Scene> GetOwnerScene() const { return m_pOwnerScene.lock(); }

		/* 优先级 */
		void SetPriority(int32_t priority) { m_Priority = priority; }
		int32_t GetPriority() const { return m_Priority; }

		/* 设置RenderTarget */
		void SetRenderTargetHandle(FrameGraphResourceHandle handle) { m_RenderTargetHandle = handle; }
		[[nodiscard]] SharedPtr<DeviceTexture> GetRenderTarget() const;

		/* 获取FrameGraph */
		[[nodiscard]] const SharedPtr<FrameGraph>& GetFrameGraph() const { return m_pFrameGraph; }

		/* 获取阴影管理器 */
		[[nodiscard]] SharedPtr<ShadowMapManager>& GetShadowMapManager() { return m_pShadowMapManager; }

		/* 存储各Pass的FrameBuffer */
		void EmplacePassFrameBuffer(const std::string& name, const SharedPtr<DeviceFrameBuffer>& frame_buffer);
		[[nodiscard]] const SharedPtr<DeviceFrameBuffer>& GetPassFrameBuffer(const std::string& name) const;

		/* 为渲染图组织阴影所需的 Pass：中性阴影图恒建（任何着色阶段都能采样到合法纹理），
		 * 本视图有投影光源时再建真实阴影 Pass；句柄统一落进 Blackboard 的 "ShadowMapHandle"。
		 * 需要采样阴影的管线（前向 / 延迟）在挂接自己的 Pass 前调用一次。 */
		void AddShadowMapPasses();

		/* 准备一帧的RenderView数据 */
		void Prepare();

		/* 执行渲染当前View */
		void Execute();

		/* 获取可见MeshSegment */
		[[nodiscard]] const std::vector<VisibleMeshObject>& GetVisibleMeshObjects() const { return m_VisibleMeshObjects; }

		/* 获取光源 */
		[[nodiscard]] const std::vector<SharedPtr<Light>>& GetValidLights() const { return m_ValidLights; }

		/* 本视图是否存在需要投影的光源（级联或点光/聚光任一） */
		[[nodiscard]] bool HasShadowCast() const { return m_IsHasShadowCast; }

	private:
		/* 视锥体剔除，仅保留对当前RenderView可见的对象 */
		void PrepareVisibleObjects();
		/* 准备光源信息 */
		void PrepareLights();
		/* 更新FrameGraph */
		void UpdateFrameGraph();


		/* 视口区域 */
		ViewportRegion m_ViewportRegion{};
		/* 是否启用视锥体剔除 */
		bool m_IsEnableCulling{ true };
		/* 相机 */
		Camera* m_pOwnerCamera{ nullptr };
		/* 所属的Scene */
		WeakPtr<Scene> m_pOwnerScene;
		/* 优先级 */
		int32_t m_Priority{ 0 };

		/* 视锥体剔除之后，对当前可见MeshSegment */
		std::vector<VisibleMeshObject> m_VisibleMeshObjects{};
		/* 对场景产生影响的光源 */
		std::vector<SharedPtr<Light>> m_ValidLights{};

		/* 当前View的渲染结果输出到该RenderTarget */
		FrameGraphResourceHandle m_RenderTargetHandle{};
		/* FrameGraph */
		SharedPtr<FrameGraph> m_pFrameGraph{ nullptr };
		/* 是否使用自定义的FrameGraph覆盖 */
		bool m_IsOverrideFrameGraph{ false };
		/* 收集当前RenderView对应的FrameGraph中各Pass阶段的FrameBuffer，用于ReadPixels<PassName, FrameBufferPtr> */
		std::unordered_map<std::string, SharedPtr<DeviceFrameBuffer>> m_PassFrameBuffers{};

		/* 是否开启投影 */
		bool m_IsHasShadowCast{ false };
		/* 阴影管理 */
		SharedPtr<ShadowMapManager> m_pShadowMapManager{ nullptr };
	};
}
