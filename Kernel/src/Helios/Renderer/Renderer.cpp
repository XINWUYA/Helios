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
	};

	/* Per light uniform data */
	constexpr uint32_t MAX_LIGHT_VIEW_PROJ = 4; /* 与 Uniforms.glsl 中的数组长度保持一致 */
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
		/* 阴影深度偏移（来自 ShadowMapInfo::ConstantBias），缓解阴影失真（peter-panning / acne）。
		 * 置于 Light UBO 中，避免作为独立 uniform 被遗漏赋值。 */
		float ShadowBias{ 0.0f };
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

	void Renderer::Submit(const SharedPtr<Material>& material, const MeshPrimitive& mesh_primitive, uint32_t index_count)
	{
		PROFILE_FUNCTION();

	/* 绑定材质和顶点数据：Metal等后端需要在Bind前拿到VertexArray的VertexDescriptor来创建PipelineState */
	material->GetShader()->BindVertexArray(mesh_primitive.VertexArray);

	/* 绑定全局UniformBuffer（View/Object/Light），Metal后端需要显式绑定到RenderEncoder */
	s_RenderData.pViewUniformBuffer->Bind();
	s_RenderData.pObjectUniformBuffer->Bind();
	s_RenderData.pLightUniformBuffer->Bind();

	material->Bind();

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
		s_RenderData.pObjectUniformBuffer->SetData(&data, sizeof(ObjectUniformData));
	}

	void Renderer::FillLightUniformBuffer(const SharedPtr<Light>& light, const std::vector<glm::mat4>& light_vp_mats, const glm::vec4& cascade_splits)
	{
		static LightUniformData data;
		/* 拷贝级联阴影的光照视图投影矩阵数组 */
		const uint32_t cascade_count = std::min<uint32_t>(static_cast<uint32_t>(light_vp_mats.size()), MAX_LIGHT_VIEW_PROJ);
		for (uint32_t i = 0; i < MAX_LIGHT_VIEW_PROJ; ++i)
		{
			if (i < cascade_count)
				data.LightViewProjectionMat[i] = light_vp_mats[i];
			else
				data.LightViewProjectionMat[i] = glm::mat4(1.0f);
		}
		data.CascadeCount = cascade_count;
		data.CascadeSplits = cascade_splits;

		/* 阴影深度偏移：来自光源的 ShadowMapInfo::ConstantBias。 */
		if (const auto& shadow_map_info = light->GetShadowMapInfo())
			data.ShadowBias = shadow_map_info->ConstantBias;
		else
			data.ShadowBias = 0.0f;

		const auto& light_color = light->GetColor();
		data.ColorIntensity = glm::vec4(light_color.r, light_color.g, light_color.b, light->GetIntensity());
		data.LightType = static_cast<uint32_t>(light->GetLightType());
		data.LightPos = light->GetPosition();

		switch (light->GetLightType())
		{
		case LightType::Directional:
		{
			auto directional_light = StaticPtrCast<DirectionalLight>(light);
			data.LightDir = directional_light->GetDirection();

			break;
		}
		default:
			break;
		}
		
		s_RenderData.pLightUniformBuffer->SetData(&data, sizeof(LightUniformData));
	}

	void Renderer::FillLightUniformBuffer(const SharedPtr<Light>& light)
	{
		FillLightUniformBuffer(light, {}, glm::vec4(0.0f));
	}
}
