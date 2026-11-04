#pragma once
#include <utility>
#include "RenderAPI.h"
#include "RenderQuery.h"
#include "Helios/Scene/Sky.h"

namespace Helios
{
	class RenderView;
	class Material;
	class DeviceShader;
	class DeviceTexture;
	class DeviceUniformBuffer;
	class ScopedShadowMapBinding;
	class ShadowMapManager;
	struct VisibleMeshObject;
	struct DrawParams;
	class Light;
	struct MeshPrimitive;

	class HELIOS_API Renderer
	{
	public:
		static void Init();
		static void Update();
		static void Release();

		static void SetViewport(uint32_t x_start, uint32_t y_start, uint32_t width, uint32_t height);
		static void SetClearColor(const glm::vec4& color);
		static void Clear();

		/* 绘制一个视图 */
		static void RenderAView(RenderView* view);

		/* 草地的世界范围（xz 最小角与尺寸）：地面材质据此在远处接管草色。
		 * 尺寸为 0 表示场景里没有草地 */
		static void SetGrassArea(const glm::vec2& area_min, const glm::vec2& area_size);

		/* 指定ViewUniformBuffer */
		static void SetViewUniforms(const glm::mat4& view, const glm::mat4& projection, const glm::vec3& view_pos);

		/* 填充天空常量：日月方向、大气系数与曝光，天空类着色器只读它 */
		static void SetSkyUniforms(const Sky::SkyState& state);

		/* 草地间接绘制：先把参数块重置成「N 个索引 / 0 实例」，再由剔除那一步累加实例数 */
		static bool DispatchGrassArgsReset(const SharedPtr<class DeviceStorageBuffer>& args, uint32_t index_count);
		/* 草地 GPU 剔除 + 实例压实：可见叶片抄进目标缓冲，实例数写进参数块。
		 * chunk_bounds 是「压实块号 → 块包围球」（xyz 中心 + w 半径），贴地时中心带地形高度 */
		static bool DispatchGrassCull(const SharedPtr<class DeviceStorageBuffer>& source,
			const SharedPtr<class DeviceStorageBuffer>& target, const SharedPtr<class DeviceStorageBuffer>& args,
			const SharedPtr<class DeviceStorageBuffer>& chunk_bounds,
			const void* param_data, uint32_t param_size, uint32_t instance_count);

		/* 地形 patch 的 GPU 选择：节点表进、压实的 patch 实例与间接参数出。
		 * 参数块与 TerrainSelect.glsl 的 SelectBlock 对应；运行期不支持计算着色器时返回 false */
		static bool DispatchTerrainSelect(const SharedPtr<class DeviceStorageBuffer>& nodes,
			const SharedPtr<class DeviceStorageBuffer>& patches,
			const SharedPtr<class DeviceStorageBuffer>& args,
			const void* param_data, uint32_t param_size, uint32_t node_count);

		/* 草地 GPU 放置：一次派发把整片草地的叶片实例算出来。运行期不支持计算着色器时返回 false */
		/* 高度、块表、密度遮罩分别绑到 2、3、5 号槽；无遮罩用哑缓冲。 */
		static bool DispatchGrassPlacement(const SharedPtr<class DeviceStorageBuffer>& instances,
			const void* param_data, uint32_t param_size, uint32_t instance_count,
			const SharedPtr<class DeviceStorageBuffer>& terrain_heights,
			const SharedPtr<class DeviceStorageBuffer>& chunk_indices,
			const SharedPtr<class DeviceStorageBuffer>& density_mask);

		/* per_draw：单次绘制的覆盖参数（IBL 这类"每次绘制不同"的绑定）。
	 * 只写当前绑定的 Shader 与纹理、不落材质对象 —— 共享材质因此保持只读。 */
	static void Submit(const SharedPtr<Material>& material, const MeshPrimitive& mesh_primitive,
	                   const DrawParams* per_draw = nullptr, uint32_t index_count = 0);

		static int CurrentAPI() { return RenderAPI::GetAPI(); }

		static const SharedPtr<RenderAPI>& GetRenderAPI() { return m_pRenderAPI; }

		/* 视口 MSAA 全局开关：开启后场景几何通道按多重采样绘制、通道结束再解析，
		 * 物体边缘不再出硬阶梯。改动下一帧生效（渲染目标按采样数重建）。 */
		static void SetMSAAEnabled(bool enabled);
		[[nodiscard]] static bool IsMSAAEnabled();
		/* 当前生效的采样数：关闭时为 1，所有渲染通道与默认目标共用这一个值 */
		[[nodiscard]] static uint8_t GetMSAASampleCount();

		static const ResultGPUTimerNode& GetGPUTimerRoot() { return m_GPUTimerRoot; }

		static SharedPtr<DeviceVertexArray> GetFullScreenVertexArray();

		/* 实例数据布局：属性名与着色器里的 a_InstanceMatrix0..3 / a_InstanceParams / a_InstancePrevMatrix0..3 对上 */
		static const VertexBufferLayout& GetObjectInstanceLayout();
		/* 草叶用的紧凑布局：只有 4 列基向量 + 参数（草不需要上一帧矩阵，
		 * 每株省 64 字节，几十万株就差出十几兆） */
		static const VertexBufferLayout& GetGrassInstanceLayout();

		/* 一段实例数据在本帧实例缓冲里的位置 */
		struct InstanceRange
		{
			SharedPtr<DeviceVertexBuffer> Buffer;
			uint32_t FirstInstance{ 0 };
		};
		/* 把一批实例数据追加进本帧的实例缓冲，各 Pass 共用，不再每次新建缓冲。
		 * 步长由调用方给：地形 patch（32 字节）与场景对象（144 字节）共用同一条环 */
		static InstanceRange UploadObjectInstances(const ObjectInstanceData* instances, uint32_t count);
		static InstanceRange UploadInstances(const void* data, uint32_t count, uint32_t stride);

		/* 间接提交：实例数与索引数从 args 缓冲里读（GPU 剔除的产物），一笔画完 */
		static void SubmitIndirect(const SharedPtr<Material>& material, const MeshPrimitive& mesh_primitive,
			const SharedPtr<DeviceVertexBuffer>& instance_buffer,
			const SharedPtr<class DeviceStorageBuffer>& args_buffer, const DrawParams* per_draw = nullptr,
			const VertexBufferLayout* instance_layout = nullptr);

		/* 实例化提交：同材质同网格的一批对象合成一笔，实例数据从 first_instance 起连续 count 条 */
		static void SubmitInstanced(const SharedPtr<Material>& material, const MeshPrimitive& mesh_primitive,
			const SharedPtr<DeviceVertexBuffer>& instance_buffer, uint32_t first_instance, uint32_t instance_count,
			const DrawParams* per_draw = nullptr, uint32_t index_count = 0,
			const VertexBufferLayout* instance_layout = nullptr);

	private:
		/* 提交的公共实现：实例缓冲为空就是普通绘制 */
		static void SubmitDraw(const SharedPtr<Material>& material, const MeshPrimitive& mesh_primitive,
			const SharedPtr<DeviceVertexBuffer>& instance_buffer, uint32_t first_instance,
			const DrawParams* per_draw, uint32_t index_count, uint32_t instance_count = 1,
			const SharedPtr<class DeviceStorageBuffer>& indirect_args = nullptr,
			const VertexBufferLayout* instance_layout = nullptr);
	public:

		/* 全屏 Pass 的材质：不测深度也不写；additive 打开 One/One 加法混合 */
		static SharedPtr<Material> CreateFullScreenMaterial(const SharedPtr<DeviceShader>& shader, bool additive = false);

		static void FillObjectUniformBuffer(const VisibleMeshObject& mesh_object);

		/* GPU 蒙皮：一次 dispatch 把绑定顶点按调色板写进上屏缓冲。
		 * 后端支持计算着色器（Metal / GL 4.3+）才成立；返回 false 时调用方退回 CPU 蒙皮。 */
		static bool DispatchSkin(const SharedPtr<class DeviceStorageBuffer>& palette,
			const SharedPtr<class DeviceStorageBuffer>& bind_vertices,
			const SharedPtr<class DeviceStorageBuffer>& weights,
			const SharedPtr<class DeviceStorageBuffer>& skinned,
			uint32_t vertex_count);

		/* 填充光照 UniformBuffer：light_vp_mats = 级联的光照视图投影矩阵数组，cascade_splits = 各级联
		 * 的视图空间分割距离（远边界），cascade_depth_spans = 深度跨度（折算阴影偏移用，可空）。
		 * 供阴影 Pass 渲染级联前调用。 */
		static void FillLightUniformBuffer(const SharedPtr<Light>& light, const std::vector<glm::mat4>& light_vp_mats, const glm::vec4& cascade_splits,
			const std::vector<float>* cascade_depth_spans = nullptr);
		/* 光照阶段入口：填充完整数据 —— 光源参数 + 该光源的阴影数据
		 * （方向光取级联数组，点光/聚光取面矩阵与层信息，均来自本视图的阴影管理器）。
		 * shadow_maps 为空表示本光源不参与阴影（字段按"无阴影"填充）。 */
		static void FillLightUniformBuffer(const SharedPtr<Light>& light, const ShadowMapManager* shadow_maps);

		/* 无光源的光照数据：强度 0、无阴影。视图的光源全被层剔除时用它画环境光 / IBL 那一笔，
		 * 物体不会因为没光源而整片消失（见前向 / 延迟的光照循环）。 */
		static void FillEmptyLightUniformBuffer();

	private:
		static SharedPtr<DeviceTexture> ExchangeCurrentShadowMap(SharedPtr<DeviceTexture> shadow_map);
		friend class ScopedShadowMapBinding;
		static SharedPtr<RenderAPI> m_pRenderAPI;
		static ResultGPUTimerNode m_GPUTimerRoot;
		static SharedPtr<DeviceTexture> m_pCurrentShadowMap;
	};

	/* 设置当前 Pass 的阴影纹理，并在作用域结束时恢复前值。 */
	class ScopedShadowMapBinding final
	{
	public:
		explicit ScopedShadowMapBinding(SharedPtr<DeviceTexture> shadow_map)
			: m_Previous(Renderer::ExchangeCurrentShadowMap(std::move(shadow_map)))
		{
		}

		~ScopedShadowMapBinding()
		{
			Renderer::ExchangeCurrentShadowMap(std::move(m_Previous));
		}

		ScopedShadowMapBinding(const ScopedShadowMapBinding&) = delete;
		ScopedShadowMapBinding& operator=(const ScopedShadowMapBinding&) = delete;

	private:
		SharedPtr<DeviceTexture> m_Previous;
	};
}
