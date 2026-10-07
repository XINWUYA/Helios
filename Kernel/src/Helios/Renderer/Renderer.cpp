#include "Pch.h"
#include "Renderer.h"
#include "RenderView.h"
#include <Helios/VirtualDevice/DeviceFrameBuffer.h>
#include <Helios/VirtualDevice/DeviceTexture.h>
#include <Helios/VirtualDevice/DeviceShader.h>
#include <Helios/VirtualDevice/DeviceUniformBuffer.h>
#include <Helios/Scene/Camera.h>
#include <Helios/Scene/Material.h>
#include <Helios/Scene/Mesh.h>
#include <Helios/Scene/Light.h>
#include <Helios/Scene/ShadowMap.h>
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
	}

	/* Uniform data 跟着色器的 uniform block 逐字节对应（成员顺序、类型一致）。alignas(16) 是必须的：
	 * MSL 的 float4x4/float4 struct 和 GLSL std140 都要求 16 对齐。 */

	/* Per view uniform data */
	struct alignas(16) ViewUniformData
	{
		glm::mat4 ViewMatrix{ 1 };
		glm::mat4 ProjectionMatrix{ 1 };
		glm::mat4 ViewProjectionMatrix{ 1 };
		glm::vec3 ViewPos{ 0.0f };
		uint32_t FrameCounter{ 0 }; /* 当前帧数计数 */
	};

	/* Per object uniform data */
	struct alignas(16) ObjectUniformData
	{
		glm::mat4 LocalToWorldMat{ 1 };	/* 模型矩阵 */
		uint32_t ObjectId{ 0 };				/* 模型ID，用于Picking */
		float ReceiveShadow{ 1.0f };		/* 是否接受阴影（来自 ModelComponent，着色阶段乘进阴影因子） */
	};

	/* Per light uniform data */
	constexpr uint32_t MAX_LIGHT_VIEW_PROJ = 4; /* 与 Uniforms.glsl 中的数组长度保持一致 */
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

	struct RenderData
	{
		/* 全局UniformBuffers */
		ViewUniformData ViewUniformData;
		SharedPtr<DeviceUniformBuffer> pViewUniformBuffer;
		SharedPtr<DeviceUniformBuffer> pObjectUniformBuffer;
		SharedPtr<DeviceUniformBuffer> pLightUniformBuffer;
	};

	static RenderData s_RenderData;

	SharedPtr<RenderAPI> Renderer::m_pRenderAPI = nullptr;
	ResultGPUTimerNode Renderer::m_GPUTimerRoot{};
	SharedPtr<DeviceTexture> Renderer::m_pCurrentShadowMap{};

	void Renderer::Init()
	{
		PROFILE_FUNCTION();

		m_pRenderAPI = RenderAPI::Create();
		m_pRenderAPI->Init();

		s_RenderData.pViewUniformBuffer = DeviceUniformBuffer::Create(sizeof(ViewUniformData), UniformBufferBindingPoint::View);
		s_RenderData.pObjectUniformBuffer = DeviceUniformBuffer::Create(sizeof(ObjectUniformData), UniformBufferBindingPoint::Object);
		s_RenderData.pLightUniformBuffer = DeviceUniformBuffer::Create(sizeof(LightUniformData), UniformBufferBindingPoint::Light);
	}

	void Renderer::Update()
	{
		PROFILE_FUNCTION();

		RenderQueryProfiler::Instance().EndFrame();
		RenderQueryProfiler::Instance().PrepareQueryResult(m_GPUTimerRoot);

		s_RenderData.ViewUniformData.FrameCounter++;
		RenderQueryProfiler::Instance().BeginFrame(s_RenderData.ViewUniformData.FrameCounter);
	}

	void Renderer::Release()
	{
		PROFILE_FUNCTION();
		RenderQueryProfiler::Instance().Release();
	}

	void Renderer::SetViewport(uint32_t x_start, uint32_t y_start, uint32_t width, uint32_t height)
	{
		PROFILE_FUNCTION();

		m_pRenderAPI->SetViewport(x_start, y_start, width, height);
	}

	void Renderer::SetClearColor(const glm::vec4& color)
	{
		PROFILE_FUNCTION();

		m_pRenderAPI->SetClearColor(color);
	}

	void Renderer::Clear()
	{
		PROFILE_FUNCTION();

		m_pRenderAPI->Clear();
	}

	/* 绘制一个视图 */
	void Renderer::RenderAView(RenderView* view)
	{
		PROFILE_FUNCTION();

		/* Fill view uniform buffer */
		s_RenderData.ViewUniformData.ViewMatrix = view->GetCullingCamera()->GetViewMatrix();
		s_RenderData.ViewUniformData.ProjectionMatrix = view->GetCullingCamera()->GetProjectionMatrix();
		s_RenderData.ViewUniformData.ViewProjectionMatrix = view->GetCullingCamera()->GetViewProjectionMatrix();
		s_RenderData.ViewUniformData.ViewPos = view->GetCullingCamera()->GetPosition();
		s_RenderData.pViewUniformBuffer->SetData(&s_RenderData.ViewUniformData, sizeof(ViewUniformData));

		view->Execute();
	}

	void Renderer::SetViewUniforms(const glm::mat4& view, const glm::mat4& projection, const glm::vec3& view_pos)
	{
		PROFILE_FUNCTION();

		s_RenderData.ViewUniformData.ViewMatrix = view;
		s_RenderData.ViewUniformData.ProjectionMatrix = projection;
		s_RenderData.ViewUniformData.ViewProjectionMatrix = projection * view;
		s_RenderData.ViewUniformData.ViewPos = view_pos;
		s_RenderData.pViewUniformBuffer->SetData(&s_RenderData.ViewUniformData, sizeof(ViewUniformData));
	}

	SharedPtr<DeviceTexture> Renderer::ExchangeCurrentShadowMap(SharedPtr<DeviceTexture> shadow_map)
	{
		auto previous = std::move(m_pCurrentShadowMap);
		m_pCurrentShadowMap = std::move(shadow_map);
		return previous;
	}

	void Renderer::Submit(const SharedPtr<Material>& material, const MeshPrimitive& mesh_primitive,
	                      const DrawParams* per_draw, uint32_t index_count)
	{
		PROFILE_FUNCTION();

	/* 绑定材质和顶点数据：Metal等后端需要在Bind前拿到VertexArray的VertexDescriptor来创建PipelineState */
	material->GetShader()->BindVertexArray(mesh_primitive.VertexArray);

	/* 绑定全局UniformBuffer（View/Object/Light），Metal后端需要显式绑定到RenderEncoder */
	s_RenderData.pViewUniformBuffer->Bind();
	s_RenderData.pObjectUniformBuffer->Bind();
	s_RenderData.pLightUniformBuffer->Bind();

	material->Bind();

	/* per-draw 覆盖：只写当前绑定的 Shader / 纹理，不落材质对象（共享材质保持只读） */
	if (per_draw != nullptr)
	{
		for (const auto& param : per_draw->Overrides)
			material->ApplyParam(param);
	}

	/* 按 Shader 声明绑定当前 Pass 的阴影纹理。 */
	if (m_pCurrentShadowMap != nullptr)
	{
		const int shadow_binding = material->GetShader()->GetUniformBinding("u_ShadowMap");
		if (shadow_binding >= 0)
			m_pCurrentShadowMap->Bind(static_cast<uint32_t>(shadow_binding));
	}

	mesh_primitive.VertexArray->Bind();


		m_pRenderAPI->ApplyRasterState(material->GetRasterState());

		if (mesh_primitive.VertexArray->GetIndexBuffer())
			m_pRenderAPI->DrawIndexed(mesh_primitive.PrimitiveType, mesh_primitive.VertexArray, index_count);
		else
			m_pRenderAPI->DrawArrays(mesh_primitive.PrimitiveType, mesh_primitive.VertexArray);

		material->Unbind();
		mesh_primitive.VertexArray->Unbind();
	}

	SharedPtr<DeviceVertexArray> Renderer::GetFullScreenVertexArray()
	{
		PROFILE_FUNCTION();

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
	}

	void Renderer::FillObjectUniformBuffer(const VisibleMeshObject& mesh_object)
	{
		static ObjectUniformData data;
		data.LocalToWorldMat = mesh_object.Local2WorldMat;
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

			/* 阴影深度偏移：来自光源的 ShadowMapInfo::ConstantBias。 */
			if (const auto& shadow_map_info = light->GetShadowMapInfo())
				data.ShadowBias = shadow_map_info->ConstantBias;
			else
				data.ShadowBias = 0.0f;

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
}
