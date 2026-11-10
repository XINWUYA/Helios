#include "Pch.h"
#include "Helios/Core/AssetManager.h"
#include "Helios/Profiler/CPUTimeScopeTree.h"
#include "Helios/Scene/SceneCommon.h"
#include "Helios/VirtualDevice/DeviceBuffer.h"
#include "Helios/VirtualDevice/DeviceShader.h"
#include "Renderer.h"
#include "ShaderVariantStreamer.h"
#include "Helios/Profiler/RenderStatistics.h"
#include "RenderView.h"
#include <Helios/VirtualDevice/DeviceFrameBuffer.h>
#include <Helios/VirtualDevice/DeviceTexture.h>
#include <Helios/VirtualDevice/DeviceShader.h>
#include <Helios/VirtualDevice/DeviceUniformBuffer.h>
#include <Helios/Scene/Camera.h>
#include <Helios/Scene/Scene.h>
#include <Helios/Scene/Material.h>
#include <Helios/Scene/Mesh.h>
#include <Helios/Scene/Light.h>
#include <Helios/Scene/ShadowMap.h>
#include <Helios/Renderer/PostProcess/PostProcessPasses.h>
#include <cmath>

namespace Helios
{
	
	/* GlobalUniformBuffer的绑定位置 */
	namespace UniformBufferBindingPoint
	{
		constexpr uint8_t View			= 0;
		constexpr uint8_t Object		= 1;
		constexpr uint8_t Material		= 2;
		constexpr uint8_t Light			= 3;
		/* 草地范围：只有地面类材质声明这个块，用来在远处接管草色 */
		constexpr uint8_t Grass			= 4;
		/* 天空：大气散射与日月星只在天空类着色器里用，参数按视图每帧填一份 */
		constexpr uint8_t Sky			= 5;
	}

	/* Uniform data 跟着色器的 uniform block 逐字节对应（成员顺序、类型一致）。alignas(16) 是必须的：
	 * MSL 的 float4x4/float4 struct 和 GLSL std140 都要求 16 对齐。 */

	/* Per view uniform data */
	struct alignas(16) ViewUniformData
	{
		glm::mat4 ViewMatrix{ 1 };
		glm::mat4 ProjectionMatrix{ 1 };
		glm::mat4 ViewProjectionMatrix{ 1 };
		/* 深度反投影用：从 NDC 与深度还原世界坐标，省掉 G-Buffer 的世界位置附件 */
		glm::mat4 InvViewProjectionMatrix{ 1 };
		/* 运动矢量的基准：两帧都不带抖动，抖动位移不该被当成物体在动 */
		glm::mat4 VelocityViewProjectionMatrix{ 1 };
		glm::mat4 PrevVelocityViewProjectionMatrix{ 1 };
		glm::vec3 ViewPos{ 0.0f };
		uint32_t FrameCounter{ 0 }; /* 当前帧数计数 */
	};

	/* Per object uniform data */
	struct alignas(16) ObjectUniformData
	{
		glm::mat4 LocalToWorldMat{ 1 };	/* 模型矩阵 */
		glm::mat4 PrevLocalToWorldMat{ 1 };	/* 上一帧的模型矩阵：运动矢量用 */
		uint32_t ObjectId{ 0 };				/* 模型ID，用于Picking */
		float ReceiveShadow{ 1.0f };		/* 是否接受阴影（来自 ModelComponent，着色阶段乘进阴影因子） */
	};

	/* 草地范围：xy 是最小角的世界 xz，尺寸在 Size.xy。
	 * 地面材质按「在不在这个矩形里」和「离相机多远」把自己的颜色换成草色。 */
	struct alignas(16) GrassAreaData
	{
		glm::vec4 AreaMin{ 0.0f };    /* xy = 最小角 xz，zw 保留 */
		glm::vec4 AreaSize{ 0.0f };   /* xy = 尺寸 xz（0 = 场景里没有草地） */
	};

	/* 天空常量：大气散射、日月盘与星空都在片元里现算，这里只装一帧的参数。
	 * 与 SkyUniforms.glsl 逐成员对应（顺序、类型、16 字节对齐一致）。 */
	struct alignas(16) SkyUniformData
	{
		/* xyz 指向太阳，w 太阳角半径（弧度） */
		glm::vec4 SunDirection{ 0.0f, 1.0f, 0.0f, 0.008f };
		/* rgb 大气层外的阳光辐照度（已换算到引擎亮度量级），a 盘面竖直压扁因子 */
		glm::vec4 SunRadiance{ 1.0f };
		/* xyz 指向月亮，w 月亮角半径 */
		glm::vec4 MoonDirection{ 0.0f, -1.0f, 0.0f, 0.012f };
		/* rgb 月亮色（按月相与高度衰减过），a 盘面竖直压扁因子 */
		glm::vec4 MoonRadiance{ 1.0f };
		/* xyz 月亮看到的太阳方向（月面着色用），w 保留 */
		glm::vec4 MoonLightDirection{ 0.0f, 1.0f, 0.0f, 0.0f };
		/* xyz 瑞利散射系数（1/m），w 瑞利标高（m） */
		glm::vec4 Rayleigh{ 5.8e-6f, 13.5e-6f, 33.1e-6f, 8000.0f };
		/* x 米氏散射系数，y 米氏标高，z 各向异性 g，w 浑浊度缩放 */
		glm::vec4 Mie{ 12.0e-6f, 1200.0f, 0.80f, 1.0f };
		/* x 行星半径，y 大气半径，z 星空亮度，w 曝光 */
		glm::vec4 Planet{ 6360.0e3f, 6420.0e3f, 1.0f, 1.5f };
		/* xyz 天球极轴，w 星空旋转角（弧度） */
		glm::vec4 Stars{ 0.0f, 0.0f, 1.0f, 0.0f };
	};

	/* Per light uniform data */	constexpr uint32_t MAX_LIGHT_VIEW_PROJ = 4; /* 与 Uniforms.glsl 中的数组长度保持一致 */
	/* 点光立方体阴影的面数上限（与 Uniforms.glsl 中的数组长度保持一致） */
	constexpr uint32_t MAX_PUNCTUAL_SHADOW_FACE = 6;
	struct alignas(16) LightUniformData
	{
		/* 级联阴影的光照视图投影矩阵数组（最多 MAX_LIGHT_VIEW_PROJ 个） */
		glm::mat4 LightViewProjectionMat[MAX_LIGHT_VIEW_PROJ]{ glm::mat4(1) };
		glm::vec4 ColorIntensity{ 1.0f, 1.0f, 1.0f, 1.0f }; /* rgb: color, a: intensity */
		glm::vec3 LightDir{ 0.0f };
		uint32_t LightType{ 0 };
		glm::vec3 LightPos{ 0.0f };
		uint32_t CascadeCount{ 0 };
		glm::vec4 CascadeSplits{ 0.0f };
		/* 阴影深度偏移（来自 ShadowMapInfo::ConstantBias，世界单位），缓解阴影失真（peter-panning / acne）。
		 * 置于 Light UBO 中，避免作为独立 uniform 被遗漏赋值。 */
		float ShadowBias{ 0.0f };
		/* 级联过渡带带宽，取该级联视距跨度的比例（0 = 不混合）；用来抹平相邻级联之间的硬边 */
		float CascadeBlend{ 0.0f };
		/* 法线偏移，单位是该级联单个纹素的世界尺寸（0 = 关闭） */
		float NormalBias{ 0.0f };
		/* std140 对齐补齐：下面的 vec4 / mat4 数组须落在 16 字节边界（GLSL 侧自动对齐，
		 * C++ 侧无对齐要求，不补会错位 12 字节）。顺带承载逐级联的 z 空间常数偏移：
		 * x..w = CascadeShadowBias（见 FillLightUniformData）。 */
		alignas(16) glm::vec4 CascadeShadowBias{ 0.0f };

		/* ---- 点光/聚光阴影（光照阶段逐光源填充） ---- */
		/* 各阴影面的视图投影矩阵（点光 6 面 / 聚光只用 [0]），与采样端的面索引约定一致 */
		glm::mat4 PunctualShadowMat[MAX_PUNCTUAL_SHADOW_FACE]{ glm::mat4(1) };
		/* x: 阴影面在数组中的起始层；y: 是否投影（0/1）；z: 阴影远平面（= 光源范围）；w: 保留 */
		glm::vec4 PunctualShadowParams{ 0.0f };
		/* x: 光照范围；y: cos(内锥半角)；z: cos(外锥半角)；w: 保留 */
		glm::vec4 PunctualLightParams{ 0.0f };
	};

	/* 每帧的实例缓冲：帧数和后端的在途帧数一致（MetalConfig::MaxFramesInFlight），
	 * 轮到某一块时 GPU 早读完了，直接覆盖写 */
	constexpr uint32_t kInstanceRingFrames = 3;
	constexpr uint32_t kInstanceRingInitialBytes = 1u << 20;

	struct InstanceRing
	{
		SharedPtr<DeviceVertexBuffer> Buffers[kInstanceRingFrames];
		uint32_t Capacity[kInstanceRingFrames]{};
		uint32_t Frame{ UINT32_MAX };
		uint32_t Cursor{ 0 };
	};

	struct RenderData
	{
		/* 全局UniformBuffers */
		ViewUniformData ViewUniformData;
		ObjectUniformData ObjectUniformData;
		SharedPtr<DeviceUniformBuffer> pViewUniformBuffer;
		SharedPtr<DeviceUniformBuffer> pObjectUniformBuffer;
		SharedPtr<DeviceUniformBuffer> pLightUniformBuffer;
		SharedPtr<DeviceUniformBuffer> pGrassAreaBuffer;
		SharedPtr<DeviceUniformBuffer> pSkyUniformBuffer;
		InstanceRing Instances;
	};

	static RenderData s_RenderData;
	/* 视图级变体取值：同一时刻只有一张视图在画，渲染期间存一份拷贝，视图销毁也不会悬空 */
	static ShaderVariantDefines s_ViewVariantDefines;

	SharedPtr<RenderAPI> Renderer::m_pRenderAPI = nullptr;
	SharedPtr<DeviceTexture> Renderer::m_pCurrentShadowMap{};

	namespace
	{
		/* 视口 MSAA 的单一数据源：开关与采样数。采样数固定 4x，再往上收益很小。
		 * 默认关闭，需要时在视图菜单里打开。 */
		constexpr uint8_t MSAA_SAMPLE_COUNT = 4;
		bool s_MSAAEnabled = false;
	}

	void Renderer::SetMSAAEnabled(bool enabled)
	{
		s_MSAAEnabled = enabled;
	}

	bool Renderer::IsMSAAEnabled()
	{
		return s_MSAAEnabled;
	}

	uint8_t Renderer::GetMSAASampleCount()
	{
		return s_MSAAEnabled ? MSAA_SAMPLE_COUNT : 1;
	}

	void Renderer::Init()
	{
		PROFILE_CPU_FUNCTION_CH(ProfileChannel::Render);

		m_pRenderAPI = RenderAPI::Create();
		m_pRenderAPI->Init();

		s_RenderData.pViewUniformBuffer = DeviceUniformBuffer::Create(sizeof(ViewUniformData), UniformBufferBindingPoint::View);
		s_RenderData.pObjectUniformBuffer = DeviceUniformBuffer::Create(sizeof(ObjectUniformData), UniformBufferBindingPoint::Object);
		s_RenderData.pLightUniformBuffer = DeviceUniformBuffer::Create(sizeof(LightUniformData), UniformBufferBindingPoint::Light);
		s_RenderData.pGrassAreaBuffer = DeviceUniformBuffer::Create(sizeof(GrassAreaData), UniformBufferBindingPoint::Grass);
		s_RenderData.pSkyUniformBuffer = DeviceUniformBuffer::Create(sizeof(SkyUniformData), UniformBufferBindingPoint::Sky);
	}

	void Renderer::Update()
	{
		PROFILE_CPU_FUNCTION_CH(ProfileChannel::Render);

		/* 收掉上一帧的 GPU 查询、解析结果，并把同一帧的 CPU 数据一起发布；
		 * 同时结算几何统计。帧号在门面内部与 CPU 采集对齐。 */
		Profiler::Instance().EndGPUFrame();

		/* 逐帧递增，与 CPU 采集的帧号同步：面板按帧号对齐 CPU / GPU 两条录制 */
		s_RenderData.ViewUniformData.FrameCounter++;
		Profiler::Instance().BeginGPUFrame(s_RenderData.ViewUniformData.FrameCounter);

		/* 预热队列按帧预算补编，避免一次性编译把一帧拖死 */
		ShaderVariantStreamer::Instance().Tick();
	}

	void Renderer::Release()
	{
		PROFILE_CPU_FUNCTION_CH(ProfileChannel::Render);
		GPUTimeScopeTree::Instance().Release();
		RenderStatistics::Instance().Reset();
		s_RenderData.Instances = InstanceRing{};
	}

	void Renderer::SetViewport(uint32_t x_start, uint32_t y_start, uint32_t width, uint32_t height)
	{
		PROFILE_CPU_FUNCTION_CH(ProfileChannel::Render);

		m_pRenderAPI->SetViewport(x_start, y_start, width, height);
	}

	void Renderer::SetClearColor(const glm::vec4& color)
	{
		PROFILE_CPU_FUNCTION_CH(ProfileChannel::Render);

		m_pRenderAPI->SetClearColor(color);
	}

	void Renderer::Clear()
	{
		PROFILE_CPU_FUNCTION_CH(ProfileChannel::Render);

		m_pRenderAPI->Clear();
	}

	/* 绘制一个视图 */
	void Renderer::RenderAView(RenderView* view)
	{
		PROFILE_CPU_FUNCTION_CH(ProfileChannel::Render);

		/* 抖动只改动渲染用的投影矩阵；是否启用由后处理注册表回答，
		 * 渲染器本身不认识具体效果 */
		view->BeginTemporalFrame(PostProcess::UsesTemporalJitter(*view));

		/* Fill view uniform buffer */
		s_RenderData.ViewUniformData.ViewMatrix = view->GetCullingCamera()->GetViewMatrix();
		s_RenderData.ViewUniformData.ProjectionMatrix = view->GetRenderProjectionMatrix();
		s_RenderData.ViewUniformData.ViewProjectionMatrix = view->GetRenderViewProjectionMatrix();
		s_RenderData.ViewUniformData.InvViewProjectionMatrix = glm::inverse(view->GetRenderViewProjectionMatrix());
		s_RenderData.ViewUniformData.VelocityViewProjectionMatrix = view->GetVelocityViewProjectionMatrix();
		s_RenderData.ViewUniformData.PrevVelocityViewProjectionMatrix = view->GetPrevVelocityViewProjectionMatrix();
		s_RenderData.ViewUniformData.ViewPos = view->GetCullingCamera()->GetPosition();
		s_RenderData.pViewUniformBuffer->SetData(&s_RenderData.ViewUniformData, sizeof(ViewUniformData));

		/* 天空常量：跟着视图走（每个视图一份快照，探针烘焙也用当前帧这一份） */
		SetSkyUniforms(view->GetSkyState());

		/* 草地的世界范围：地面材质靠它在远处接管草色 */
		if (const auto grass_scene = view->GetOwnerScene(); grass_scene != nullptr)
		{
			glm::vec2 area_min(0.0f);
			glm::vec2 area_size(0.0f);
			grass_scene->GetPrimaryGrassArea(area_min, area_size);
			SetGrassArea(area_min, area_size);
		}
		else
		{
			SetGrassArea(glm::vec2(0.0f), glm::vec2(0.0f));
		}

		/* 每个相机一个 GPU 作用域：多视图（场景相机 + 编辑器相机等）的 Pass
		 * 在统计树里按相机分组，而不是全部平铺在帧根下 */
		const Camera* view_camera = view->GetCullingCamera();
		const std::string view_label = view_camera ? view_camera->GetDebugName() : std::string("View");
		GPUTimeScopeTree::Instance().BeginGPUScope(view_label.c_str());
		view->Execute();
		GPUTimeScopeTree::Instance().EndGPUScope();
	}

	void Renderer::SetGrassArea(const glm::vec2& area_min, const glm::vec2& area_size)
	{
		GrassAreaData data;
		data.AreaMin = glm::vec4(area_min, 0.0f, 0.0f);
		data.AreaSize = glm::vec4(area_size, 0.0f, 0.0f);
		s_RenderData.pGrassAreaBuffer->SetData(&data, sizeof(GrassAreaData));
	}

	void Renderer::SetSkyUniforms(const Sky::SkyState& state)
	{
		SkyUniformData data;
		data.SunDirection = glm::vec4(state.SunDirection, state.SunAngularRadius);
		/* 大气层外是白光，颜色差别全在散射里；强度按引擎亮度量级缩放，天空与光照共用一个太阳。
		 * a 装盘面的竖直压扁因子（大气折射，贴地平线时日面是扁的） */
		data.SunRadiance = glm::vec4(state.SunColor * (state.SunIntensity * Sky::kSunIrradianceScale),
			state.SunFlattening);
		data.MoonDirection = glm::vec4(state.MoonDirection, state.MoonAngularRadius);
		data.MoonRadiance = glm::vec4(state.MoonColor * (state.MoonIntensity * Sky::kSunIrradianceScale),
			state.MoonFlattening);
		data.MoonLightDirection = glm::vec4(state.MoonLightDirection, 0.0f);
		data.Rayleigh = glm::vec4(state.Atmos.RayleighBeta, state.Atmos.RayleighScaleHeight);
		data.Mie = glm::vec4(state.Atmos.MieBeta, state.Atmos.MieScaleHeight,
			state.Atmos.MieAnisotropy, state.Haze);
		data.Planet = glm::vec4(state.Atmos.PlanetRadius, state.Atmos.AtmosphereRadius,
			state.StarIntensity, state.Exposure);
		data.Stars = glm::vec4(state.PoleDirection, state.StarRotation);
		s_RenderData.pSkyUniformBuffer->SetData(&data, sizeof(SkyUniformData));
	}

	void Renderer::SetViewUniforms(const glm::mat4& view, const glm::mat4& projection, const glm::vec3& view_pos)	{
		PROFILE_CPU_FUNCTION_CH(ProfileChannel::Render);

		s_RenderData.ViewUniformData.ViewMatrix = view;
		s_RenderData.ViewUniformData.ProjectionMatrix = projection;
		s_RenderData.ViewUniformData.ViewProjectionMatrix = projection * view;
		s_RenderData.ViewUniformData.InvViewProjectionMatrix = glm::inverse(projection * view);
		/* 这些调用方（探针烘焙）不做时域累积，前后两帧当成同一帧 */
		s_RenderData.ViewUniformData.VelocityViewProjectionMatrix = projection * view;
		s_RenderData.ViewUniformData.PrevVelocityViewProjectionMatrix = projection * view;
		s_RenderData.ViewUniformData.ViewPos = view_pos;
		s_RenderData.pViewUniformBuffer->SetData(&s_RenderData.ViewUniformData, sizeof(ViewUniformData));
	}

	void Renderer::SetViewVariantDefines(const ShaderVariantDefines& defines)
	{
		s_ViewVariantDefines = defines;
	}

	const ShaderVariantDefines& Renderer::GetViewVariantDefines()
	{
		return s_ViewVariantDefines;
	}

	SharedPtr<DeviceTexture> Renderer::ExchangeCurrentShadowMap(SharedPtr<DeviceTexture> shadow_map)
	{
		auto previous = std::move(m_pCurrentShadowMap);
		m_pCurrentShadowMap = std::move(shadow_map);
		return previous;
	}

	const VertexBufferLayout& Renderer::GetObjectInstanceLayout()
	{
		static const VertexBufferLayout layout = {
			{ "a_InstanceMatrix0", BufferDataType::Float4 },
			{ "a_InstanceMatrix1", BufferDataType::Float4 },
			{ "a_InstanceMatrix2", BufferDataType::Float4 },
			{ "a_InstanceMatrix3", BufferDataType::Float4 },
			{ "a_InstanceParams", BufferDataType::Float4 },
			/* 上一帧世界矩阵：运动矢量用，不参与着色 */
			{ "a_InstancePrevMatrix0", BufferDataType::Float4 },
			{ "a_InstancePrevMatrix1", BufferDataType::Float4 },
			{ "a_InstancePrevMatrix2", BufferDataType::Float4 },
			{ "a_InstancePrevMatrix3", BufferDataType::Float4 },
		};
		return layout;
	}

	/* 草的实例布局：草不需要上一帧矩阵（风在 GPU 上算），只保留前 5 个属性 */
	const VertexBufferLayout& Renderer::GetGrassInstanceLayout()
	{
		static const VertexBufferLayout layout = {
			{ "a_InstanceMatrix0", BufferDataType::Float4 },
			{ "a_InstanceMatrix1", BufferDataType::Float4 },
			{ "a_InstanceMatrix2", BufferDataType::Float4 },
			{ "a_InstanceMatrix3", BufferDataType::Float4 },
			{ "a_InstanceParams", BufferDataType::Float4 },
		};
		return layout;
	}

	Renderer::InstanceRange Renderer::UploadObjectInstances(const ObjectInstanceData* instances, uint32_t count)
	{
		return UploadInstances(instances, count, static_cast<uint32_t>(sizeof(ObjectInstanceData)));
	}

	Renderer::InstanceRange Renderer::UploadInstances(const void* data, uint32_t count, uint32_t stride)
	{
		PROFILE_CPU_FUNCTION_CH(ProfileChannel::Render);

		InstanceRing& ring = s_RenderData.Instances;
		const uint32_t frame = s_RenderData.ViewUniformData.FrameCounter;
		if (ring.Frame != frame)
		{
			ring.Frame = frame;
			ring.Cursor = 0;
		}

		const uint32_t slot = frame % kInstanceRingFrames;
		const uint32_t bytes = count * stride;
		SharedPtr<DeviceVertexBuffer>& buffer = ring.Buffers[slot];
		uint32_t& capacity = ring.Capacity[slot];

		/* 起点对齐到本条步长的整数倍：同一帧里 144 字节的场景对象与 32 字节的地形 patch
		 * 共用这条环，不对齐的话换算出来的实例下标会指到上一段的中间 */
		const uint32_t aligned = (ring.Cursor + stride - 1) / std::max(stride, 1u) * stride;

		/* 装不下就换一块翻倍的，本帧已录的绘制还引用旧块，由命令缓冲或驱动持有到用完 */
		if (buffer == nullptr || aligned + bytes > capacity)
		{
			capacity = std::max({ kInstanceRingInitialBytes, capacity * 2, bytes });
			buffer = DeviceVertexBuffer::Create("ObjectInstances", capacity);
			ring.Cursor = 0;
		}
		else
		{
			ring.Cursor = aligned;
		}

		if (count > 0)
			buffer->SetData(data, bytes, ring.Cursor);

		/* 每段都从实例步长的整数倍起，起点换算成实例下标后 SubmitInstanced 直接用 */
		InstanceRange range{ buffer, ring.Cursor / std::max(stride, 1u) };
		ring.Cursor += bytes;
		return range;
	}

	void Renderer::Submit(const SharedPtr<Material>& material, const MeshPrimitive& mesh_primitive,
	                      const DrawParams* per_draw, uint32_t index_count, int32_t object_id)
	{
		WriteObjectId(object_id);
		SubmitDraw(material, mesh_primitive, nullptr, 0, per_draw, index_count);
	}

	void Renderer::SubmitInstanced(const SharedPtr<Material>& material, const MeshPrimitive& mesh_primitive,
		const SharedPtr<DeviceVertexBuffer>& instance_buffer, uint32_t first_instance, uint32_t instance_count,
		const DrawParams* per_draw, uint32_t index_count, const VertexBufferLayout* instance_layout,
		int32_t object_id)
	{
		WriteObjectId(object_id);
		SubmitDraw(material, mesh_primitive, instance_buffer, first_instance, per_draw, index_count, instance_count,
			nullptr, instance_layout);
	}

	void Renderer::SubmitIndirect(const SharedPtr<Material>& material, const MeshPrimitive& mesh_primitive,
		const SharedPtr<DeviceVertexBuffer>& instance_buffer,
		const SharedPtr<DeviceStorageBuffer>& args_buffer, const DrawParams* per_draw,
		const VertexBufferLayout* instance_layout, uint32_t args_offset, uint32_t instance_offset,
		int32_t object_id)
	{
		WriteObjectId(object_id);
		SubmitDraw(material, mesh_primitive, instance_buffer, 0, per_draw, 0, 0, args_buffer,
			instance_layout, args_offset, instance_offset);
	}

	void Renderer::SubmitDraw(const SharedPtr<Material>& material, const MeshPrimitive& mesh_primitive,
		const SharedPtr<DeviceVertexBuffer>& instance_buffer, uint32_t first_instance,
		const DrawParams* per_draw, uint32_t index_count, uint32_t instance_count,
		const SharedPtr<DeviceStorageBuffer>& indirect_args, const VertexBufferLayout* instance_layout,
		uint32_t indirect_args_offset, uint32_t indirect_instance_offset)
	{
		PROFILE_CPU_FUNCTION_CH(ProfileChannel::Render);

		const VertexBufferLayout& layout = (instance_layout != nullptr)
			? *instance_layout : GetObjectInstanceLayout();

		/* 绑定材质和顶点数据：Metal等后端需要在Bind前拿到VertexArray的VertexDescriptor来创建PipelineState。
		 * 变体按这一笔的来源合成：绘制级维度来自 per_draw，视图维度来自当前视图表 */
		ShaderVariantContext variant_context;
		variant_context.Draw = per_draw != nullptr ? &per_draw->VariantDefines : nullptr;
		variant_context.View = &s_ViewVariantDefines;
		const SharedPtr<DeviceShader>& draw_shader = material->GetDrawShader(variant_context);
		if (instance_buffer != nullptr)
			draw_shader->BindVertexArray(mesh_primitive.VertexArray, layout);
		else
			draw_shader->BindVertexArray(mesh_primitive.VertexArray);

		/* 绑定全局UniformBuffer（View/Object/Light），Metal后端需要显式绑定到RenderEncoder */
		s_RenderData.pViewUniformBuffer->Bind();
		s_RenderData.pObjectUniformBuffer->Bind();
		s_RenderData.pLightUniformBuffer->Bind();
		s_RenderData.pGrassAreaBuffer->Bind();
		s_RenderData.pSkyUniformBuffer->Bind();

		/* 光栅状态先于材质绑定：着色器在第一次绑定时才 link（见 OpenGLShader::LazyLink），
		 * 此时 cull face 等状态已设好，link 状态与首次绘制一致，不会被驱动重编译 */
		m_pRenderAPI->ApplyRasterState((per_draw != nullptr && per_draw->RasterStateOverride.has_value())
			? per_draw->RasterStateOverride.value()
			: material->GetRasterState());

		/* 排序把同材质的对象排在一起：上一笔写过的参数这一笔还是那些值，重复绑定可以省掉。
		 * 有覆盖参数的那一笔除外 —— 它改过 Shader 上的值，下一笔必须重写回去 */
		const uint8_t fill_missing = per_draw != nullptr ? per_draw->FillMissing : MaterialFill::None;
		const bool has_overrides = per_draw != nullptr && !per_draw->Overrides.empty();
		const uint32_t shader_generation = draw_shader->GetGeneration();
		const uint32_t state_serial = m_pRenderAPI->GetStateSerial();
		static const Material* s_LastBoundMaterial = nullptr;
		static uint32_t s_LastBoundRevision = 0;
		static uint32_t s_LastBoundGeneration = 0;
		static uint32_t s_LastBoundSerial = 0;
		static uint8_t s_LastBoundFill = 0;
		static bool s_LastHadOverrides = false;

		const bool bind_ready = !has_overrides && !s_LastHadOverrides
			&& s_LastBoundMaterial == material.get()
			&& s_LastBoundRevision == material->GetRevision()
			&& s_LastBoundGeneration == shader_generation
			&& s_LastBoundSerial == state_serial
			&& s_LastBoundFill == fill_missing;
		if (!bind_ready)
		{
			material->Bind(fill_missing, draw_shader);
			s_LastBoundMaterial = material.get();
			s_LastBoundRevision = material->GetRevision();
			s_LastBoundGeneration = shader_generation;
			s_LastBoundSerial = state_serial;
			s_LastBoundFill = fill_missing;
		}
		s_LastHadOverrides = has_overrides;

		/* per-draw 覆盖：只写当前绑定的 Shader / 纹理，不落材质对象（共享材质保持只读） */
		if (per_draw != nullptr)
		{
			for (const auto& param : per_draw->Overrides)
				material->ApplyParam(param, draw_shader);
		}

		/* 按 Shader 声明绑定当前 Pass 的阴影纹理。绘制按 shader 分组，记住上一个的查表结果就够了 */
		if (m_pCurrentShadowMap != nullptr)
		{
			/* 弱引用拿住控制块，比较控制块就不会撞上被释放后复用的地址 */
			static WeakPtr<DeviceShader> s_last_shader;
			static uint32_t s_last_generation = 0;
			static int s_shadow_binding = -1;
			const bool same_shader = !s_last_shader.owner_before(draw_shader) && !draw_shader.owner_before(s_last_shader);
			if (!same_shader || draw_shader->GetGeneration() != s_last_generation)
			{
				s_last_shader = draw_shader;
				s_last_generation = draw_shader->GetGeneration();
				s_shadow_binding = draw_shader->GetUniformBinding("u_ShadowMap");
			}
			if (s_shadow_binding >= 0)
				m_pCurrentShadowMap->Bind(static_cast<uint32_t>(s_shadow_binding));
		}

		mesh_primitive.VertexArray->Bind();


		if (indirect_args != nullptr && instance_buffer != nullptr)
		{
			/* 索引数与实例数都在参数缓冲里：一笔画完，CPU 不知道这次画了多少 */
			m_pRenderAPI->DrawIndexedIndirect(mesh_primitive.PrimitiveType, mesh_primitive.VertexArray,
				instance_buffer, indirect_instance_offset, indirect_args, indirect_args_offset, layout);
		}
		else if (instance_buffer != nullptr)
		{
			const uint32_t offset = first_instance * layout.GetStride();
			if (mesh_primitive.VertexArray->GetIndexBuffer())
				m_pRenderAPI->DrawIndexedInstanced(mesh_primitive.PrimitiveType, mesh_primitive.VertexArray,
					instance_buffer, offset, instance_count, layout, index_count);
			else
				m_pRenderAPI->DrawArraysInstanced(mesh_primitive.PrimitiveType, mesh_primitive.VertexArray,
					instance_buffer, offset, instance_count, layout);
		}
		else if (mesh_primitive.VertexArray->GetIndexBuffer())
			m_pRenderAPI->DrawIndexed(mesh_primitive.PrimitiveType, mesh_primitive.VertexArray, index_count);
		else
			m_pRenderAPI->DrawArrays(mesh_primitive.PrimitiveType, mesh_primitive.VertexArray);

		/* 渲染统计：元素数取索引数（索引绘制）或顶点数，实例化乘实例数；
		 * 间接绘制的元素数在参数块里由 GPU 读，这里只记一笔调用 */
		if (indirect_args != nullptr && instance_buffer != nullptr)
		{
			RenderStatistics::Instance().AddDrawCall(mesh_primitive.PrimitiveType, 0, 0, true);
		}
		else
		{
			const SharedPtr<IndexBuffer>& index_buffer = mesh_primitive.VertexArray->GetIndexBuffer();
			const uint32_t element_count = index_buffer != nullptr
				? (index_count > 0 ? index_count : index_buffer->GetCount())
				: mesh_primitive.VertexArray->GetVertexCount();
			RenderStatistics::Instance().AddDrawCall(mesh_primitive.PrimitiveType, element_count,
				instance_buffer != nullptr ? instance_count : 0, false);
		}

		material->Unbind();
		mesh_primitive.VertexArray->Unbind();
	}

	SharedPtr<DeviceVertexArray> Renderer::GetFullScreenVertexArray()
	{
		PROFILE_CPU_FUNCTION_CH(ProfileChannel::Render);

		/* 全屏 VAO 只建一次：原实现每次调用都新建并销毁 VAO 与顶点/索引缓冲 */
		static const SharedPtr<DeviceVertexArray> s_FullScreenVertexArray = []()
		{
			/* Vertices */
			static constexpr float vertices[3 * 4] =
			{
				-1.0f, -1.0f, 1.0f, 1.0f,
				 3.0f, -1.0f, 1.0f, 1.0f,
				-1.0f,  3.0f, 1.0f, 1.0f,
			};
			static const VertexBufferLayout vertex_buffer_layout =
			{
				{ "a_Position", BufferDataType::Float4 }
			};
			const auto vertex_buffer = DeviceVertexBuffer::Create("FullScreen_VertexBuffer", vertices, sizeof(vertices));
			vertex_buffer->SetLayout(vertex_buffer_layout);

			/* Indices */
			static constexpr uint16_t indices[3] =
			{
				0, 1, 2
			};
			const auto index_buffer = IndexBuffer::Create("FullScreen_IndexBuffer", indices, 3, IndexType::UInt16);

			/* VertexArray */
			auto vertex_array = DeviceVertexArray::Create("FullScreen_VertexArray");
			vertex_array->AddVertexBuffer(vertex_buffer);
			vertex_array->SetIndexBuffer(index_buffer);
			return vertex_array;
		}();
		return s_FullScreenVertexArray;
	}

	SharedPtr<Material> Renderer::CreateFullScreenMaterial(const SharedPtr<DeviceShader>& shader, bool additive)
	{
		auto material = CreateSharedPtr<Material>();
		auto& raster_state = material->GetRasterState();
		raster_state.EnableDepthTest = false;
		raster_state.EnableDepthWrite = false;
		if (additive)
			raster_state.SetAdditiveBlend();
		material->SetShader(shader);
		return material;
	}

	void Renderer::WriteObjectId(int32_t object_id)
	{
		if (object_id < 0)
			return;

		/* 只改 id：矩阵等字段保持上一次写入的值，草和地形不读它们 */
		s_RenderData.ObjectUniformData.ObjectId = static_cast<uint32_t>(object_id);
		s_RenderData.pObjectUniformBuffer->SetData(&s_RenderData.ObjectUniformData, sizeof(ObjectUniformData));
	}

	void Renderer::FillObjectUniformBuffer(const VisibleMeshObject& mesh_object)
	{
		static ObjectUniformData data;
		data.LocalToWorldMat = mesh_object.Local2WorldMat;
		data.PrevLocalToWorldMat = mesh_object.PrevLocal2WorldMat;
		data.ObjectId = mesh_object.ObjectId;
		data.ReceiveShadow = mesh_object.ReceiveShadow ? 1.0f : 0.0f;
		s_RenderData.pObjectUniformBuffer->SetData(&data, sizeof(ObjectUniformData));
	}

	namespace
	{
		/* 光照 UBO 的公共填充体：光源参数 + 级联矩阵数组 + 点光 / 聚光阴影块。punctual_shadow 是
		 * nullptr / invalid 时按"没有点状阴影"填；cascade_depth_spans 是各级联的深度跨度（世界单位，
		 * 折算 ConstantBias 用），为空就按跨度 1 处理。 */
		void FillLightUniformData(LightUniformData& data, const SharedPtr<Light>& light,
			const std::vector<glm::mat4>& light_vp_mats, const glm::vec4& cascade_splits,
			const PunctualShadowData* punctual_shadow, const std::vector<float>* cascade_depth_spans = nullptr)
		{
			/* 拷贝级联阴影的光照视图投影矩阵数组 */
			const uint32_t cascade_count = std::min<uint32_t>(static_cast<uint32_t>(light_vp_mats.size()), MAX_LIGHT_VIEW_PROJ);
			for (uint32_t i = 0; i < MAX_LIGHT_VIEW_PROJ; ++i)
				data.LightViewProjectionMat[i] = (i < cascade_count) ? light_vp_mats[i] : glm::mat4(1.0f);
			data.CascadeCount = cascade_count;
			data.CascadeSplits = cascade_splits;

			/* 阴影参数：常数偏移、级联过渡带带宽、法线偏移都从 ShadowMapInfo 取。
			 * 过渡带按比例给（近层带窄、远层带宽），采样端在 split 前这段距离内混合下一层。 */
			const auto& shadow_map_info = light->GetShadowMapInfo();
			data.ShadowBias = shadow_map_info ? shadow_map_info->ConstantBias : 0.0f;
			data.CascadeBlend = kCascadeBlendRatio;
			data.NormalBias = shadow_map_info ? shadow_map_info->NormalBias : 0.0f;

			/* 逐级联的 z 空间常数偏移 = 世界偏移 / 该级联的深度跨度。
			 * 深度跨度随机型 far / ShadowFar 变化，固定 z 偏移在大跨度下会被
			 * 相对精度吞掉（阴影全部消失）、小跨度下又会把阴影整体压没。 */
			data.CascadeShadowBias = glm::vec4(0.0f);
			for (uint32_t i = 0; i < cascade_count; ++i)
			{
				const float span = (cascade_depth_spans != nullptr && i < cascade_depth_spans->size())
					? (*cascade_depth_spans)[i] : 1.0f;
				data.CascadeShadowBias[i] = data.ShadowBias / std::max(span, 1e-4f);
			}

			/* 光源公共参数 */
			const auto& light_color = light->GetColor();
			data.ColorIntensity = glm::vec4(light_color.r, light_color.g, light_color.b, light->GetIntensity());
			data.LightType = static_cast<uint32_t>(light->GetLightType());
			data.LightPos = light->GetPosition();
			data.LightDir = glm::vec3(0.0f);

			/* 点光/聚光块先归零，再按类型填充 */
			data.PunctualShadowParams = glm::vec4(0.0f);
			data.PunctualLightParams = glm::vec4(0.0f);
			for (glm::mat4& punctual_mat : data.PunctualShadowMat)
				punctual_mat = glm::mat4(1.0f);

			switch (light->GetLightType())
			{
			case LightType::Directional:
			{
				const auto directional_light = StaticPtrCast<DirectionalLight>(light);
				data.LightDir = directional_light->GetDirection();
				break;
			}
			case LightType::Point:
			case LightType::Spot:
			{
				const auto* punctual_light = AsPunctualLight(light.get());
				data.PunctualLightParams.x = punctual_light->GetRange();
				/* w：阴影投影近平面 —— 采样端把世界单位的阴影偏移折算到 z 空间用
				 * （见 ShadowUtils.glsl 的 CalculatePointShadow / CalculateSpotShadow） */
				data.PunctualLightParams.w = kPunctualShadowNearPlane;

				if (light->GetLightType() == LightType::Spot)
				{
					/* 聚光：方向 = 光传播方向（u_LightDir 与方向光同语义）；锥角以 cos(半角) 下发 */
					const auto* spot_light = static_cast<const SpotLight*>(light.get());
					data.LightDir = spot_light->GetDirection();
					data.PunctualLightParams.y = std::cos(glm::radians(spot_light->GetInnerAngle() * 0.5f));
					data.PunctualLightParams.z = std::cos(glm::radians(spot_light->GetAngle() * 0.5f));
				}
				break;
			}
			default:
				break;
			}

			/* 点光/聚光阴影块：面矩阵 + 层基址 + 远平面 */
			if (punctual_shadow != nullptr && punctual_shadow->Valid)
			{
				const uint32_t face_count = std::min<uint32_t>(punctual_shadow->FaceCount, MAX_PUNCTUAL_SHADOW_FACE);
				for (uint32_t i = 0; i < face_count; ++i)
					data.PunctualShadowMat[i] = punctual_shadow->FaceMat[i];
				data.PunctualShadowParams = glm::vec4(
					static_cast<float>(punctual_shadow->BaseLayer), 1.0f, punctual_shadow->Far, 0.0f);
			}
		}
	}

	void Renderer::FillLightUniformBuffer(const SharedPtr<Light>& light, const std::vector<glm::mat4>& light_vp_mats, const glm::vec4& cascade_splits,
		const std::vector<float>* cascade_depth_spans)
	{
		static LightUniformData data;
		FillLightUniformData(data, light, light_vp_mats, cascade_splits, nullptr, cascade_depth_spans);
		s_RenderData.pLightUniformBuffer->SetData(&data, sizeof(LightUniformData));
	}

	void Renderer::FillLightUniformBuffer(const SharedPtr<Light>& light, const ShadowMapManager* shadow_maps)
	{
		static LightUniformData data;

		/* 方向光：级联数据来自本视图的阴影管理器 —— 容器里存的就是"负责投影的方向光"，
		 * 其它方向光没有级联，按无阴影填充（CascadeCount = 0，着色阶段跳过采样）。 */
		std::vector<glm::mat4> light_vp_mats;
		std::vector<float> cascade_depth_spans;
		glm::vec4 cascade_splits(0.0f);
		if (shadow_maps != nullptr && light->GetLightType() == LightType::Directional)
		{
			const auto& cascade_maps = shadow_maps->GetCascadeShadowMaps();
			if (!cascade_maps.empty() && cascade_maps[0]->GetLight().get() == light.get())
			{
				light_vp_mats.reserve(cascade_maps.size());
				cascade_depth_spans.reserve(cascade_maps.size());
				for (const auto& cascade_map : cascade_maps)
				{
					/* 只收本光源的层：容器里还可能挂着别的方向光的级联（它们没有拟合，
					 * 矩阵是单位阵），混进来会让级联数虚高，远处直接落到单位阵上 */
					if (cascade_map->GetLight().get() != light.get())
						break;

					light_vp_mats.push_back(cascade_map->GetLightViewProjectionMat());
					cascade_depth_spans.push_back(cascade_map->GetDepthRange());
				}
				cascade_splits = shadow_maps->GetCascadeSplits();
			}
		}

		/* 点光/聚光：该光源的面矩阵与层信息（无阴影时 Valid = false） */
		PunctualShadowData punctual_shadow;
		if (shadow_maps != nullptr)
			punctual_shadow = shadow_maps->GetPunctualShadowData(light.get());

		FillLightUniformData(data, light, light_vp_mats, cascade_splits, &punctual_shadow,
			cascade_depth_spans.empty() ? nullptr : &cascade_depth_spans);
		s_RenderData.pLightUniformBuffer->SetData(&data, sizeof(LightUniformData));
	}

	void Renderer::FillEmptyLightUniformBuffer()
	{
		static LightUniformData data;

		/* 逐字段归零：static 复用，上一笔的级联矩阵 / 阴影参数不能残留。
		 * 类型取方向光 —— 强度 0 时着色结果为 0，不会踩到点光的衰减分支。 */
		for (glm::mat4& mat : data.LightViewProjectionMat)
			mat = glm::mat4(1.0f);
		data.ColorIntensity = glm::vec4(0.0f);
		data.LightDir = glm::vec3(0.0f);
		data.LightType = static_cast<uint32_t>(LightType::Directional);
		data.LightPos = glm::vec3(0.0f);
		data.CascadeCount = 0;
		data.CascadeSplits = glm::vec4(0.0f);
		data.ShadowBias = 0.0f;
		data.CascadeShadowBias = glm::vec4(0.0f);
		for (glm::mat4& mat : data.PunctualShadowMat)
			mat = glm::mat4(1.0f);
		data.PunctualShadowParams = glm::vec4(0.0f);
		data.PunctualLightParams = glm::vec4(0.0f);

		s_RenderData.pLightUniformBuffer->SetData(&data, sizeof(LightUniformData));
	}
}
