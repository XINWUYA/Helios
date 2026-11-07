#include "Pch.h"
#include "Taa.h"
#include <Helios/Core/AssetManager.h>
#include <Helios/Renderer/FrameGraph/FrameGraph.h>
#include <Helios/Renderer/RenderView.h>
#include <Helios/Renderer/Renderer.h>
#include <Helios/Scene/Entity.h>
#include <Helios/Scene/Material.h>
#include <Helios/Scene/Mesh.h>
#include <Helios/Scene/SceneCommon.h>
#include <Helios/VirtualDevice/DeviceFrameBuffer.h>
#include <Helios/VirtualDevice/DeviceShader.h>
#include <Helios/VirtualDevice/DeviceTexture.h>
#include "../PostProcessPasses.h"

namespace Helios
{
	namespace PostProcess
	{
		namespace
		{
			using TextureHandle = FrameGraphResourceHandleTyped<FrameGraphTexture>;

			/* 从「后处理对象」收集 TAA 设置（实体没挂 TaaComponent = 不写入） */
			void CaptureTaa(const Entity& owner, PostProcessState& state)
			{
				if (owner.HasComponent<TaaComponent>())
					state.Set(owner.GetComponent<TaaComponent>().GetTaaSettings());
			}

			bool IsTaaEnabled(const PostProcessState& state)
			{
				const TaaSettings* settings = state.Get<TaaSettings>();
				return settings != nullptr && settings->Enabled;
			}

			/* 抖动与"是否生效"必须同源：两者分家会出现"画面在抖但没有累积" */
			bool UsesTaaJitter(const PostProcessState& state)
			{
				return IsTaaEnabled(state);
			}

			TextureHandle BuildTaa(RenderView& render_view, TextureHandle input)
			{
				PROFILE_CPU_FUNCTION_CH(ProfileChannel::Render);

				const TaaSettings* settings = render_view.GetPostProcessState().Get<TaaSettings>();
				if (settings == nullptr || !settings->Enabled)
					return input;

				const auto& viewport_region = render_view.GetViewportRegion();
				if (viewport_region.Width < 4 || viewport_region.Height < 4)
					return input;

				auto& frame_graph = render_view.GetFrameGraph();
				const auto depth = PostProcess::FindSceneDepth(*frame_graph);
				const auto velocity = frame_graph->GetBlackboard().GetResourceHandle<FrameGraphTexture>("Velocity");
				/* 缺深度或速度就没法重投影，宁可不做也不留拖影 */
				if (!depth || !velocity)
					return input;

				/* 双缓冲的 ping-pong：写哪张由抖动相位决定，相邻帧奇偶不同 */
				const uint32_t index = render_view.GetJitterIndex() & 1u;
				const auto history_read = render_view.AcquireHistory("TaaHistory" + std::to_string(index ^ 1u),
					viewport_region.Width, viewport_region.Height, TextureFormat::RGBA16F);
				const auto history_write = render_view.AcquireHistory("TaaHistory" + std::to_string(index),
					viewport_region.Width, viewport_region.Height, TextureFormat::RGBA16F);
				if (history_read == nullptr || history_write == nullptr)
					return input;

				const auto read_handle = frame_graph->ImportTexture("TaaHistoryRead", history_read);
				const auto write_handle = frame_graph->ImportTexture("TaaHistoryWrite", history_write);

				static BuiltinAsset<DeviceShader> s_resolve_shader("Shaders/PostProcessShaders/TaaResolve.glsl");
				const auto resolve_shader = s_resolve_shader.Get();

				/* 回投影用 CPU 算好的逆矩阵：shader 里逐像素求逆太贵 */
				const glm::mat4 inv_view_projection = glm::inverse(render_view.GetRenderViewProjectionMatrix());
				const glm::vec2 texel_size(
					1.0f / static_cast<float>(viewport_region.Width),
					1.0f / static_cast<float>(viewport_region.Height));
				const glm::vec2 texture_size(
					static_cast<float>(viewport_region.Width),
					static_cast<float>(viewport_region.Height));
				/* 历史占比封顶：留一成当前帧，画面不至于完全跟不动 */
				const float history_weight = std::clamp(settings->HistoryWeight, 0.0f, 0.98f);
				const float sharpness = std::max(settings->Sharpness, 0.0f);

				struct TaaPassData
				{
					TextureHandle SceneColor;
					TextureHandle Depth;
					TextureHandle Velocity;
					TextureHandle HistoryRead;
					TextureHandle HistoryWrite;
					bool HistoryValid{ false };
				};

				frame_graph->AddPass<TaaPassData>("TaaResolve",
					[&, input, depth, velocity, read_handle, write_handle](FrameGraphBuilder& builder, TaaPassData& data)
					{
						data.SceneColor = input;
						data.Depth = depth;
						data.Velocity = velocity;
						data.HistoryRead = read_handle;
						data.HistoryWrite = write_handle;
						/* 首帧 / resize / 换场景 / 相机瞬移：只吃当前帧 */
						data.HistoryValid = render_view.IsHistoryValid();

						builder.BindInputResource(data.SceneColor, FrameGraphTexture::Usage::Sampleable);
						builder.BindInputResource(data.Depth, FrameGraphTexture::Usage::Sampleable);
						builder.BindInputResource(data.Velocity, FrameGraphTexture::Usage::Sampleable);
						builder.BindInputResource(data.HistoryRead, FrameGraphTexture::Usage::Sampleable);
						builder.BindOutputResource(data.HistoryWrite,
							FrameGraphTexture::Usage::ColorAttachment | FrameGraphTexture::Usage::Sampleable);

						FrameGraphPassInfo::Descriptor pass_desc;
						pass_desc.Attachments.ColorAttachments(0) = data.HistoryWrite;
						pass_desc.ViewportRegion = viewport_region;
						builder.CreateRenderPass("TaaResolveRenderTarget", pass_desc);

						/* 输出的历史帧要被下一帧的 TAA 读，不能被图裁剪剔除 */
						builder.AsSideEffect();
					},
					[&, resolve_shader, inv_view_projection, texel_size, texture_size,
						history_weight, sharpness]
					(const FrameGraphResources& resources, const TaaPassData& data)
					{
						const auto render_pass_info = resources.GetPassRenderTarget();
						render_view.EmplacePassFrameBuffer("TaaResolve", render_pass_info);

						render_pass_info->Bind();
						{
							auto material = Renderer::CreateFullScreenMaterial(resolve_shader);
							material->SetTexture("u_SceneColor", resources.Get(data.SceneColor).Texture);
							material->SetTexture("u_Depth", resources.Get(data.Depth).Texture);
							material->SetTexture("u_Velocity", resources.Get(data.Velocity).Texture);
							material->SetTexture("u_History", resources.Get(data.HistoryRead).Texture);
							material->SetParameters(ParamType::Mat4, "u_InvViewProjection", inv_view_projection);
							material->SetParameters(ParamType::Vec2, "u_TexelSize", texel_size);
							material->SetParameters(ParamType::Vec2, "u_TextureSize", texture_size);
							material->SetParameters(ParamType::Float, "u_HistoryWeight", history_weight);
							material->SetParameters(ParamType::Float, "u_Sharpness", sharpness);
							material->SetParameters(ParamType::Int, "u_HistoryValid", data.HistoryValid ? 1 : 0);
							Renderer::Submit(material, Renderer::GetFullScreenVertexArray());
						}
						render_pass_info->Unbind();
						Renderer::GetRenderAPI()->Flush();

						/* 这一帧的历史写好了，下一帧可以直接拿来混合 */
						render_view.MarkHistoryValid();
					});

				return write_handle;
			}
		}

		void RegisterTaaEffect()
		{
			PostProcessEffectRegistry::Instance().Add(EffectDesc{
				"TAA", Phase::Temporal, &CaptureTaa, &IsTaaEnabled, &UsesTaaJitter, &BuildTaa });
		}
	}
}
