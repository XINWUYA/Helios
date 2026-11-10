#pragma once
#include <entt/entt.hpp>
#include <string>
#include <unordered_map>
#include "RenderCommon.h"
#include <glm/glm.hpp>

#include "FrameGraph/FrameGraphResourceHandle.h"
#include "PostProcess/PostProcessEffectRegistry.h"
#include "Helios/Scene/LayerMask.h"
#include "Frustum.h"
#include "Helios/Scene/Sky.h"

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
	struct HELIOS_API VisibleMeshObject
	{
		int ObjectId{ -1 };
		glm::mat4 Local2WorldMat;
		/* 上一帧的世界矩阵（运动矢量用）：新出现的对象等于当前值，速度 0 */
		glm::mat4 PrevLocal2WorldMat{ 1.0f };
		SharedPtr<MeshSegment> MeshSegment;
		/* 收集时按解析链定下的最终材质（实体绑定 ?: 模型默认 ?: 内置白模） */
		SharedPtr<Material> Material;
		/* 投影/接受阴影（来自 ModelComponent）：分别为"是否写入阴影图"与"是否采样阴影" */
		bool CastShadow{ true };
		bool ReceiveShadow{ true };
		/* 蒙皮网格的包围盒是绑定姿态，动起来会出界，不参与视锥剔除 */
		bool Cullable{ true };
		/* 世界空间包围盒：收集时算一次，相机视锥 / 级联 / 点光各面共用，
		 * 剔除时不再每趟把局部盒乘一遍矩阵。没设过局部盒时为 false */
		glm::vec3 WorldCenter{ 0.0f };
		glm::vec3 WorldExtent{ 0.0f };
		bool HasWorldBounds{ false };

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
	class HELIOS_API RenderView
	{
	public:
		RenderView(Camera* owner_camera);
		/* 探针捕获专用：不绑相机，渲染内容由调用方逐面给（见 PrepareCapture） */
		explicit RenderView(std::string debug_name);
		~RenderView();

		/* 获取裁剪相机 */
		[[nodiscard]] const Camera* GetCullingCamera() const { return m_pOwnerCamera; }

		/* 设置启用视锥体剔除 */
		void SetEnableFrustumCulling(bool enable) { m_IsEnableCulling = enable; }
		[[nodiscard]] bool IsEnableFrustumCulling() const { return m_IsEnableCulling; }

		/* 关掉后场景不再收集这个视图去渲染，相机矩阵照常更新（缺省开） */
		void SetEnableRender(bool enable) { m_IsEnableRender = enable; }
		[[nodiscard]] bool IsEnableRender() const { return m_IsEnableRender; }

		/* 前向默认渲染图改画到视图纹理（缺省画到窗口）：编辑器里窗口被界面盖住，要靠纹理才看得到画面 */
		void SetOutputToTexture(bool enable) { m_IsOutputToTexture = enable; }
		[[nodiscard]] bool IsOutputToTexture() const { return m_IsOutputToTexture; }

		/* 本帧由这个视图推进探针烘焙（场景每帧只挑一个视图，见 Scene::OnUpdate） */
		void SetBakeReflectionProbes(bool enable) { m_IsBakeReflectionProbes = enable; }
		[[nodiscard]] bool IsBakeReflectionProbes() const { return m_IsBakeReflectionProbes; }

		/* 渲染管线（前向 / 延迟）：决定本视图的渲染图如何组织 —— 缺省的
		 * UpdateFrameGraph 路径按它分派（自行组织渲染图的相机读它做同样分派）。
		 * 缺省前向；相机上有同名转发（Camera::SetRenderPipeline）。 */
		void SetRenderPipeline(RenderPipeline pipeline) { m_RenderPipeline = pipeline; }
		[[nodiscard]] RenderPipeline GetRenderPipeline() const { return m_RenderPipeline; }

		/* 调试视图模式（Surface / Lighting / Analysis / Animation 分类见 DebugViewMode）：
		 * 与渲染管线同级 —— 每个视图单独一份，缺省 None = 正常着色；
		 * 相机上有同名转发（Camera::SetDebugViewMode）。 */
		void SetDebugViewMode(DebugViewMode mode) { m_DebugViewMode = mode; }
		[[nodiscard]] DebugViewMode GetDebugViewMode() const { return m_DebugViewMode; }

		/* 编辑器视图：目前只控制 G-Buffer 的拾取通道（实体 id 附件）。
		 * 相机视图与探针捕获都不要，缺省关着，省 4 字节/像素 */
		void SetEditorView(bool enable) { m_IsEditorView = enable; }
		[[nodiscard]] bool IsEditorView() const { return m_IsEditorView; }

		/* 可见层掩码：与对象的 LayerMask 相交非空才进可见列表（阴影投射同源）。
		 * 缺省全部层；相机上有同名转发（Camera::SetCullingMask）。 */
		void SetCullingMask(LayerMask mask) { m_CullingMask = mask; }
		[[nodiscard]] LayerMask GetCullingMask() const { return m_CullingMask; }

		/* 选中实体 id（ObjectId 附件里那一套）：>= 0 时视口沿它的轮廓勾一圈描边。
		 * 编辑器每帧按层级选中项写入，缺省 -1 = 不画。 */

		/* 本帧的后处理设置快照：由 Scene 每帧解析（后处理对象 → 逐效果采集）后写入；
		 * 渲染侧（各效果的 Build）只读这份快照，不认识场景实体。缺省空 = 全部效果关闭。 */
		void SetPostProcessState(const PostProcess::PostProcessState& state) { m_PostProcessState = state; }
		[[nodiscard]] const PostProcess::PostProcessState& GetPostProcessState() const { return m_PostProcessState; }

		/* 本帧的天空状态：由 Scene 每帧解析（SkyComponent → 日月方向与大气参数）后写入；
		 * 渲染器填充天空常量时只读这份快照。缺省是一份能用的白天天空 —— 场景里没有天空组件
		 * 也能画出合理的天空，只是不会随时间变。 */
		void SetSkyState(const Sky::SkyState& state) { m_SkyState = state; }
		[[nodiscard]] const Sky::SkyState& GetSkyState() const { return m_SkyState; }

		/* 设置视口区域 */
		void SetViewportRegion(const ViewportRegion& region);
		[[nodiscard]] const ViewportRegion& GetViewportRegion() const { return m_ViewportRegion; }

		/* 设置所属的Scene */
		void SetOwnerScene(const SharedPtr<Scene>& scene)
		{
			/* 换了场景要重新注册阴影光源：上一份注册结果指向旧场景的光源 */
			if (m_pOwnerScene.lock() != scene)
			{
				m_ShadowLightSignatures.clear();
				/* 历史里是另一个场景的画面，不能拿来混合 */
				m_IsHistoryValid = false;
			}
			m_pOwnerScene = scene;
		}
        SharedPtr<Scene> GetOwnerScene() const { return m_pOwnerScene.lock(); }

		/* 优先级 */
		void SetPriority(int32_t priority) { m_Priority = priority; }
		int32_t GetPriority() const { return m_Priority; }

		/* 设置RenderTarget */
		void SetRenderTargetHandle(FrameGraphResourceHandle handle) { m_RenderTargetHandle = handle; }
		[[nodiscard]] SharedPtr<DeviceTexture> GetRenderTarget() const;

		/* 获取FrameGraph */
		[[nodiscard]] const SharedPtr<FrameGraph>& GetFrameGraph() const { return m_pFrameGraph; }

		/* ---- 时域效果的视图状态 ----
		 * 抖动只进渲染用的投影矩阵：剔除 / 阴影 / 拾取 / 探针烘焙一律用相机原矩阵，
		 * 否则阴影边缘和 gizmo 会跟着抖。每帧填充视图常量之前调一次。 */
		void BeginTemporalFrame(bool jitter_enabled);
		[[nodiscard]] const glm::mat4& GetRenderProjectionMatrix() const { return m_RenderProjection; }
		[[nodiscard]] const glm::mat4& GetRenderViewProjectionMatrix() const { return m_RenderViewProjection; }
		/* 运动矢量专用的两帧视图投影：不含抖动。抖动是"同一点换像素"，算进位移里
		 * 会把每帧的亚像素扰动当成物体在动，重投影反而更抖 */
		[[nodiscard]] const glm::mat4& GetVelocityViewProjectionMatrix() const { return m_VelocityViewProjection; }
		[[nodiscard]] const glm::mat4& GetPrevVelocityViewProjectionMatrix() const { return m_PrevVelocityViewProjection; }
		[[nodiscard]] const glm::vec2& GetJitterPixels() const { return m_JitterPixels; }
		/* 本帧的相位序号：时域效果用它做双缓冲的 ping-pong（相邻帧奇偶不同） */
		[[nodiscard]] uint32_t GetJitterIndex() const { return m_JitterIndex; }

		/* 历史还能不能用：首帧、尺寸变化、换场景、相机瞬移、刚开关时都是 false */
		[[nodiscard]] bool IsHistoryValid() const { return m_IsHistoryValid; }
		void InvalidateHistory() { m_IsHistoryValid = false; }
		/* 本帧已经写过历史（resolve 执行完之后调），下一帧才能接着用 */
		void MarkHistoryValid() { m_IsHistoryValid = true; }

		/* 跨帧纹理：按 key 取，尺寸或格式变了就重建并作废历史（时域效果的 ping-pong 用） */
		SharedPtr<DeviceTexture> AcquireHistory(const std::string& key, uint32_t width, uint32_t height,
			TextureFormat format);

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

		/* 探针捕获前的准备：不收集可见对象，只把场景 / 层 / 视口 / 管线与视图级变体备好。
		 * 捕获固定走前向并关掉调试档位 —— 立方图里不该有读数 */
		void PrepareCapture(const SharedPtr<Scene>& scene, LayerMask culling_mask, const ViewportRegion& region);

		/* 视图级变体取值：换视图才变的维度（管线开关这类）。写进来的值会并进
		 * 材质变体请求，逐绘制提交时按来源分发；换视图重建视图对象即可复位 */
		void SetVariantDefine(const std::string& name, int value);
		[[nodiscard]] const ShaderVariantDefines& GetVariantDefines() const { return m_VariantDefines; }

		/* 执行渲染当前View */
		void Execute();

		/* 获取可见MeshSegment（已按相机视锥剔除） */
		[[nodiscard]] const std::vector<VisibleMeshObject>& GetVisibleMeshObjects() const { return m_VisibleMeshObjects; }
		/* 阴影照得到的范围里的对象（不做相机剔除）：画面外的物体也会把影子投进画面，
		 * 阴影拟合与投射用这份 */
		[[nodiscard]] const std::vector<VisibleMeshObject>& GetShadowCasterObjects() const { return m_SceneMeshObjects; }

		/* 按给定可见层掩码收集场景对象（模型段 + 解析后的材质），写进 out（不清空）。
		 * 视图每帧用自身 CullingMask 收集；反射探针烘焙按探针的 CullingMask 调同一份逻辑。 */
		void CollectVisibleMeshObjects(LayerMask culling_mask, std::vector<VisibleMeshObject>& out) const;

		/* 探针捕获用：按探针的层收集，场景只挂了天空组件时补一个内建天空 ——
		 * 与相机渲染同一份收集口径，否则烘焙出来的环境里没有天。 */
		void CollectProbeCaptureObjects(LayerMask culling_mask, std::vector<VisibleMeshObject>& out) const;

		/* 获取光源 */
		[[nodiscard]] const std::vector<SharedPtr<Light>>& GetValidLights() const { return m_ValidLights; }

		/* 本视图是否存在需要投影的光源（级联或点光/聚光任一） */
		[[nodiscard]] bool HasShadowCast() const { return m_IsHasShadowCast; }

	private:
		/* 两个构造函数共用的初始化：建 FrameGraph 与阴影管理器 */
		void Init(const std::string& debug_name);
		/* 视锥体剔除，仅保留对当前RenderView可见的对象 */
		void PrepareVisibleObjects();
		/* 按实体组装可绘制对象（材质解析 + 世界包围盒），索引查询出来的候选走这里 */
		void BuildMeshObjects(const Scene& scene, const std::vector<entt::entity>& entities,
			std::vector<VisibleMeshObject>& out) const;
		/* 准备光源信息。register_shadows = false 时只收光源列表，不注册阴影
		 * （探针捕获用中性阴影图，不需要自己的一套级联） */
		void PrepareLights(bool register_shadows = true);
		/* 给可见对象补上一帧的世界矩阵（运动矢量用） */
		void PreparePreviousTransforms();
		/* 更新FrameGraph */
		void UpdateFrameGraph();


		/* 视口区域 */
		ViewportRegion m_ViewportRegion{};
		/* 是否启用视锥体剔除 */
		bool m_IsEnableCulling{ true };
		/* 是否参与场景渲染 */
		bool m_IsEnableRender{ true };
		/* 前向默认渲染图是否画到视图纹理 */
		bool m_IsOutputToTexture{ false };
		/* 本帧是否推进探针烘焙 */
		bool m_IsBakeReflectionProbes{ false };
		/* 渲染管线（前向 / 延迟） */
		RenderPipeline m_RenderPipeline{ RenderPipeline::Forward };
		/* 调试视图模式（Surface / Lighting / Analysis / Animation 分类） */
		DebugViewMode m_DebugViewMode{ DebugViewMode::None };
		/* 可见层掩码：与对象 LayerMask 相交为空的对象不渲染 */
		LayerMask m_CullingMask{ Layers::All };
		/* 是不是编辑器视图：决定写不写拾取通道 */
		bool m_IsEditorView{ false };
		/* 视图级变体取值，逐绘制提交时并进变体请求 */
		ShaderVariantDefines m_VariantDefines{};
		/* 后处理设置快照（每视图一份；Scene 解析写入，缺省空 = 全部效果关闭） */
		PostProcess::PostProcessState m_PostProcessState{};
		/* 天空状态快照（每视图一份；Scene 解析写入） */
		Sky::SkyState m_SkyState{};
		/* 相机 */
		Camera* m_pOwnerCamera{ nullptr };
		/* 所属的Scene */
		WeakPtr<Scene> m_pOwnerScene;
		/* 优先级 */
		int32_t m_Priority{ 0 };

		/* 按层与显隐收集到的全部对象（不做视锥剔除） */
		std::vector<VisibleMeshObject> m_SceneMeshObjects{};
		/* 视锥体剔除之后，对当前可见MeshSegment */
		std::vector<VisibleMeshObject> m_VisibleMeshObjects{};
		/* 对场景产生影响的光源 */
		std::vector<SharedPtr<Light>> m_ValidLights{};
		/* 上一帧注册进阴影管理器的光源（含阴影配置签名）：同一批光源不必重建，
		 * 重建会把级联矩阵整份作废，等于每帧拿全场景投射物重新拟合一遍 */
		std::vector<SharedPtr<Light>> m_ShadowLights{};
		std::vector<uint64_t> m_ShadowLightSignatures{};

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

		/* 抖动相位：Halton(2,3) 16 帧循环，单位是像素 */
		uint32_t m_JitterIndex{ 0 };
		glm::vec2 m_JitterPixels{ 0.0f };
		/* 渲染用矩阵（投影含抖动）与上一帧的值（重投影基准） */
		glm::mat4 m_RenderProjection{ 1.0f };
		glm::mat4 m_RenderViewProjection{ 1.0f };
		/* 运动矢量的基准：两帧都不带抖动 */
		glm::mat4 m_VelocityViewProjection{ 1.0f };
		glm::mat4 m_PrevVelocityViewProjection{ 1.0f };
		bool m_IsHistoryValid{ false };

		/* 跨帧纹理：key → 纹理与其描述（尺寸 / 格式变了就重建） */
		struct HistoryTexture
		{
			SharedPtr<DeviceTexture> Texture;
			uint32_t Width{ 0 };
			uint32_t Height{ 0 };
			TextureFormat Format{ TextureFormat::RGBA8 };
		};
		std::unordered_map<std::string, HistoryTexture> m_HistoryTextures;

		/* 本帧是否走时域累积（抖动 + 运动矢量），由 BeginTemporalFrame 写入 */
		bool m_IsTemporalEnabled{ false };
		/* 上一帧的对象世界矩阵（ObjectId → 矩阵）与这一帧的暂存：
		 * 只在时域累积开启时维护，关掉就清空，不做无谓的哈希 */
		std::unordered_map<int32_t, glm::mat4> m_PrevWorldTransforms;
		std::unordered_map<int32_t, glm::mat4> m_CurrentWorldTransforms;
		/* 表已经推进到哪一帧：同一帧重复渲染时不能推第二次，否则上一帧就成了这一帧 */
		uint32_t m_PrevTransformFrame{ 0 };
	};
}
