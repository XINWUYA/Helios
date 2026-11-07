#include "Pch.h"
#include "Fog.h"
#include <Helios/Core/AssetManager.h>
#include <Helios/Renderer/FrameGraph/FrameGraph.h>
#include <Helios/Renderer/Renderer.h>
#include <Helios/Renderer/RenderView.h>
#include <Helios/Scene/Entity.h>
#include <Helios/Scene/Material.h>
#include <Helios/Scene/Mesh.h>
#include <Helios/Scene/SceneCommon.h>
#include <Helios/Scene/Sky.h>
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

			/* 从「后处理对象」收集雾设置（实体没挂 FogComponent = 不写入） */
			void CaptureFog(const Entity& owner, PostProcessState& state)
			{
				if (owner.HasComponent<FogComponent>())
					state.Set(owner.GetComponent<FogComponent>().GetFogSettings());
			}

			bool IsFogEnabled(const PostProcessState& state)
			{
				const FogSettings* settings = state.Get<FogSettings>();
				return settings != nullptr && settings->Enabled;
			}

			/* 雾色跟天空共用一套大气参数：瑞利系数定蓝底，浑浊度掺灰白，
			 * 低太阳把暖色压进来。夜里整体压暗，别让雾比天空还亮。 */
			glm::vec3 ComputeSkyFogColor(const Sky::SkyState& sky)
			{
				const glm::vec3& beta = sky.Atmos.RayleighBeta;
				const float max_beta = std::max(beta.x, std::max(beta.y, beta.z));
				const glm::vec3 rayleigh = beta / std::max(max_beta, 1.0e-9f);

				const float haze = std::clamp(sky.Haze * 0.35f, 0.0f, 0.8f);
				glm::vec3 color = glm::mix(rayleigh, glm::vec3(1.0f), haze);
				/* 远景雾没有天空那么蓝，往灰白拉一截 */
				color = glm::mix(glm::vec3(1.0f), color, 0.55f);

				/* 低太阳：SunColor 是穿过整层大气的透过率，越贴地平线越暖 */
				const float low_sun = 1.0f - std::clamp(sky.SunDirection.y, 0.0f, 1.0f);
				const glm::vec3 sun = glm::clamp(sky.SunColor, glm::vec3(0.0f), glm::vec3(1.0f));
				const glm::vec3 warm = glm::clamp(color * sun * 1.6f, glm::vec3(0.0f), glm::vec3(1.0f));
				color = glm::mix(color, warm, low_sun * 0.7f);

				/* 夜里只留一成多，雾才不会在月光下泛白 */
				const float daylight = std::clamp(sky.SunDirection.y * 4.0f + 0.15f, 0.0f, 1.0f);
				return glm::clamp(color * glm::mix(0.12f, 1.0f, daylight), glm::vec3(0.0f), glm::vec3(1.0f));
			}

			TextureHandle BuildFog(RenderView& render_view, TextureHandle input)
			{
				PROFILE_CPU_FUNCTION_CH(ProfileChannel::Render);

				const FogSettings* settings = render_view.GetPostProcessState().Get<FogSettings>();
				if (settings == nullptr || !settings->Enabled)
					return input;

				const auto& viewport_region = render_view.GetViewportRegion();
				if (viewport_region.Width == 0 || viewport_region.Height == 0)
					return input;

				auto& frame_graph = render_view.GetFrameGraph();
				const auto depth = PostProcess::FindSceneDepth(*frame_graph);
				if (!depth)
					return input;

				static BuiltinAsset<DeviceShader> s_fog_shader("Shaders/PostProcessShaders/Fog.glsl");
				const auto fog_shader = s_fog_shader.Get();

				/* 反投影用当帧的逆矩阵（含抖动），与深度同源；逐像素求逆太贵 */
				const glm::mat4 inv_view_projection = glm::inverse(render_view.GetRenderViewProjectionMatrix());

				const FogSettings fog = *settings;
				const glm::vec3 fog_color = fog.UseSkyColor ? ComputeSkyFogColor(render_view.GetSkyState()) : fog.Color;

				FrameGraphTexture::Descriptor output_desc;
				output_desc.Width = viewport_region.Width;
				output_desc.Height = viewport_region.Height;
				output_desc.TextureFormat = TextureFormat::RGBA16F;

				struct FogPassData
				{
					TextureHandle SceneColor;
					TextureHandle Depth;
					TextureHandle Output;
				};

				auto fog_pass = frame_graph->AddPass<FogPassData>("FogPass",
					[&, input, depth, output_desc](FrameGraphBuilder& builder, FogPassData& data)
					{
						data.SceneColor = input;
						data.Depth = depth;
						data.Output = builder.CreateTexture("FogOutput", output_desc);
						builder.BindInputResource(data.SceneColor, FrameGraphTexture::Usage::Sampleable);
						builder.BindInputResource(data.Depth, FrameGraphTexture::Usage::Sampleable);
						builder.BindOutputResource(data.Output,
							FrameGraphTexture::Usage::ColorAttachment | FrameGraphTexture::Usage::Sampleable);

						FrameGraphPassInfo::Descriptor pass_desc;
						pass_desc.Attachments.ColorAttachments(0) = data.Output;
						pass_desc.ViewportRegion = ViewportRegion{ 0, 0, output_desc.Width, output_desc.Height };
						builder.CreateRenderPass("FogPassRenderTarget", pass_desc);

						/* 输出可能要留给编辑器视图叠加，不能随资源生命周期回收 */
						builder.AsSideEffect();
					},
					[&render_view, fog_shader, inv_view_projection, fog_color, fog]
					(const FrameGraphResources& resources, const FogPassData& data)
					{
						const auto render_pass_info = resources.GetPassRenderTarget();
						render_view.EmplacePassFrameBuffer("FogPass", render_pass_info);

						render_pass_info->Bind();
						{
							auto material = Renderer::CreateFullScreenMaterial(fog_shader);
							material->SetTexture("u_SceneColor", resources.Get(data.SceneColor).Texture);
							material->SetTexture("u_Depth", resources.Get(data.Depth).Texture);
							material->SetParameters(ParamType::Mat4, "u_InvViewProjection", inv_view_projection);
							material->SetParameters(ParamType::Vec4, "u_FogColorOpacity",
								glm::vec4(fog_color, fog.MaxOpacity));
							material->SetParameters(ParamType::Vec4, "u_FogShape",
								glm::vec4(fog.Density, fog.HeightFalloff, fog.BaseHeight, fog.StartDistance));
							material->SetParameters(ParamType::Vec4, "u_FogScatter",
								glm::vec4(fog.SunScatter, fog.Anisotropy, fog.SkyAffect, 0.0f));
							Renderer::Submit(material, Renderer::GetFullScreenVertexArray());
						}
						render_pass_info->Unbind();
						Renderer::GetRenderAPI()->Flush();
					});

				return fog_pass->GetData().Output;
			}
		}

		void RegisterFogEffect()
		{
			PostProcessEffectRegistry::Instance().Add(EffectDesc{
				"Fog", Phase::Scene, &CaptureFog, &IsFogEnabled, nullptr, &BuildFog });
		}
	}
}
