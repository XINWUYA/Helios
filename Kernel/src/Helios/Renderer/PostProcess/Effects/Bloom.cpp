#include "Pch.h"
#include "Bloom.h"
#include <Helios/Core/AssetManager.h>
#include <Helios/Renderer/FrameGraph/FrameGraph.h>
#include <Helios/Renderer/Renderer.h>
#include <Helios/Renderer/RenderView.h>
#include <Helios/Scene/Entity.h>
#include <Helios/Scene/Material.h>
#include <Helios/Scene/Mesh.h>
#include <Helios/Scene/SceneCommon.h>
#include <Helios/VirtualDevice/DeviceFrameBuffer.h>
#include <Helios/VirtualDevice/DeviceShader.h>
#include <Helios/VirtualDevice/DeviceTexture.h>

namespace Helios
{
	namespace PostProcess
	{
		namespace
		{
			/* 金字塔上限 5 级（1/2 → 1/32）；逐级减半、任一边 < 8 提前截止 */
			constexpr uint32_t kMaxMipCount = 5;
			constexpr uint32_t kMinMipSize = 8;

			using TextureHandle = FrameGraphResourceHandleTyped<FrameGraphTexture>;

			/* 从「后处理对象」收集 Bloom 设置（实体没挂 BloomComponent = 不写入） */
			void CaptureBloom(const Entity& owner, PostProcessState& state)
			{
				if (owner.HasComponent<BloomComponent>())
					state.Set(owner.GetComponent<BloomComponent>().GetBloomSettings());
			}

			/* 快照里 Bloom 是否会真的生效（默认目标路径的"旁路到纹理"判定用） */
			bool IsBloomEnabled(const PostProcessState& state)
			{
				const BloomSettings* settings = state.Get<BloomSettings>();
				return settings != nullptr && settings->Enabled;
			}

			/* 离屏 Pass 的渲染区域取输出纹理本身（原点 0,0 + 纹理尺寸）。
			 * 目标与视图尺寸不同时（半分辨率链等），用视图区域会把内容按比例错位 */
			ViewportRegion FullTargetRegion(const FrameGraphTexture::Descriptor& desc)
			{
				return ViewportRegion{ 0, 0, desc.Width, desc.Height };
			}

			/* 全屏效果材质：不测深度也不写；additive = One/One 加法混合（升采样回叠用） */
			SharedPtr<Material> MakeFullScreenMaterial(const SharedPtr<DeviceShader>& shader, bool additive)
			{
				auto material = CreateSharedPtr<Material>();
				auto& raster_state = material->GetRasterState();
				raster_state.EnableDepthTest = false;
				raster_state.EnableDepthWrite = false;
				if (additive)
				{
					raster_state.EnableBlend = true;
					raster_state.BlendEquationRGB = BlendEquation::Add;
					raster_state.BlendEquationA = BlendEquation::Add;
					raster_state.BlendFuncSrcRGB = BlendFunc::One;
					raster_state.BlendFuncSrcA = BlendFunc::One;
					raster_state.BlendFuncDstRGB = BlendFunc::One;
					raster_state.BlendFuncDstA = BlendFunc::One;
				}
				material->SetShader(shader);
				return material;
			}

			/* 链的一步：半分辨率金字塔（Prefilter → 降采样链 → tent 升采样回叠 → 合成）。
			 * 未启用 / 视口过小 → 原样返回 input（不建任何 Pass）。 */
			TextureHandle BuildBloom(RenderView& render_view, TextureHandle input)
			{
				PROFILE_FUNCTION();

				const BloomSettings* settings = render_view.GetPostProcessState().Get<BloomSettings>();
				if (settings == nullptr || !settings->Enabled)
					return input;

				const auto& viewport_region = render_view.GetViewportRegion();
				if (viewport_region.Width < kMinMipSize * 2 || viewport_region.Height < kMinMipSize * 2)
					return input;

				/* 级数：半分辨率起步、逐级减半，任一边 < 8 或到达上限停（至少一级） */
				uint32_t mip_count = 1;
				{
					uint32_t w = std::max(1u, viewport_region.Width / 2);
					uint32_t h = std::max(1u, viewport_region.Height / 2);
					while (mip_count < kMaxMipCount)
					{
						w = std::max(1u, w / 2);
						h = std::max(1u, h / 2);
						if (w < kMinMipSize || h < kMinMipSize)
							break;
						++mip_count;
					}
				}

				auto& frame_graph = render_view.GetFrameGraph();

				const auto prefilter_shader = AssetManager::Instance().GetOrLoad<DeviceShader>(
					ABSOLUTE_PATH("Shaders/PostProcessShaders/BloomPrefilter.glsl"));
				const auto downsample_shader = AssetManager::Instance().GetOrLoad<DeviceShader>(
					ABSOLUTE_PATH("Shaders/PostProcessShaders/BloomDownsample.glsl"));
				const auto upsample_shader = AssetManager::Instance().GetOrLoad<DeviceShader>(
					ABSOLUTE_PATH("Shaders/PostProcessShaders/BloomUpsample.glsl"));
				const auto composite_shader = AssetManager::Instance().GetOrLoad<DeviceShader>(
					ABSOLUTE_PATH("Shaders/PostProcessShaders/BloomComposite.glsl"));

				const auto mip_desc = [&viewport_region](uint32_t level)
				{
					FrameGraphTexture::Descriptor desc;
					desc.Width = std::max(1u, viewport_region.Width >> (level + 1));
					desc.Height = std::max(1u, viewport_region.Height >> (level + 1));
					desc.TextureFormat = TextureFormat::RGBA16F;
					return desc;
				};

				const auto mip_texel_size = [&mip_desc](uint32_t level)
				{
					const auto desc = mip_desc(level);
					return glm::vec2(1.0f / static_cast<float>(desc.Width), 1.0f / static_cast<float>(desc.Height));
				};

				std::vector<TextureHandle> mips(mip_count);

				/* ---- Prefilter：场景色 → 软膝阈值提取 → mip0 ---- */
				{
					struct PrefilterPassData
					{
						TextureHandle Input;
						TextureHandle Output;
					};

					const BloomSettings bloom = *settings;  /* exec 阶段用值捕获（快照指针不跨阶段持有） */
					const auto output_desc = mip_desc(0);

					auto prefilter_pass = frame_graph->AddPass<PrefilterPassData>("BloomPrefilter",
						[&, input, output_desc](FrameGraphBuilder& builder, PrefilterPassData& data)
						{
							data.Input = input;
							data.Output = builder.CreateTexture("BloomMip0", output_desc);
							builder.BindInputResource(data.Input, FrameGraphTexture::Usage::Sampleable);
							builder.BindOutputResource(data.Output,
								FrameGraphTexture::Usage::ColorAttachment | FrameGraphTexture::Usage::Sampleable);

							FrameGraphPassInfo::Descriptor pass_desc;
							pass_desc.Attachments.ColorAttachments(0) = data.Output;
							pass_desc.ViewportRegion = FullTargetRegion(output_desc);
							builder.CreateRenderPass("BloomPrefilterRenderTarget", pass_desc);
						},
						[&render_view, prefilter_shader, bloom](const FrameGraphResources& resources, const PrefilterPassData& data)
						{
							const auto render_pass_info = resources.GetPassRenderTarget();
							render_view.EmplacePassFrameBuffer("BloomPrefilter", render_pass_info);

							render_pass_info->Bind();
							{
								auto material = MakeFullScreenMaterial(prefilter_shader, false);
								material->SetTexture("u_SceneColor", resources.Get(data.Input).Texture);
								material->SetParameters(ParamType::Float, "u_Threshold", bloom.Threshold);
								Renderer::Submit(material, Renderer::GetFullScreenVertexArray());
							}
							render_pass_info->Unbind();
							Renderer::GetRenderAPI()->Flush();
						});

					mips[0] = prefilter_pass->GetData().Output;
				}

				/* ---- 降采样链：mip_i → mip_{i+1}（4 点盒式） ---- */
				for (uint32_t level = 0; level + 1 < mip_count; ++level)
				{
					struct DownsamplePassData
					{
						TextureHandle Input;
						TextureHandle Output;
					};

					const TextureHandle source = mips[level];
					const auto output_desc = mip_desc(level + 1);
					const glm::vec2 texel_size = mip_texel_size(level);
					const std::string pass_name = "BloomDownsample" + std::to_string(level);
					const std::string texture_name = "BloomMip" + std::to_string(level + 1);

					auto downsample_pass = frame_graph->AddPass<DownsamplePassData>(pass_name,
						[&, source, output_desc, texture_name](FrameGraphBuilder& builder, DownsamplePassData& data)
						{
							data.Input = source;
							data.Output = builder.CreateTexture(texture_name, output_desc);
							builder.BindInputResource(data.Input, FrameGraphTexture::Usage::Sampleable);
							builder.BindOutputResource(data.Output,
								FrameGraphTexture::Usage::ColorAttachment | FrameGraphTexture::Usage::Sampleable);

							FrameGraphPassInfo::Descriptor pass_desc;
							pass_desc.Attachments.ColorAttachments(0) = data.Output;
							pass_desc.ViewportRegion = FullTargetRegion(output_desc);
							/* 级号入名：FrameBuffer 缓存按名字复用，重名会导致各级每帧走"同帧重名"
							 * 的临时分配路径（白建 GPU 对象与日志噪音） */
							builder.CreateRenderPass(pass_name + "RenderTarget", pass_desc);
						},
						[&render_view, downsample_shader, texel_size, pass_name](const FrameGraphResources& resources, const DownsamplePassData& data)
						{
							const auto render_pass_info = resources.GetPassRenderTarget();
							render_view.EmplacePassFrameBuffer(pass_name, render_pass_info);

							render_pass_info->Bind();
							{
								auto material = MakeFullScreenMaterial(downsample_shader, false);
								material->SetTexture("u_Source", resources.Get(data.Input).Texture);
								material->SetParameters(ParamType::Vec2, "u_TexelSize", texel_size);
								Renderer::Submit(material, Renderer::GetFullScreenVertexArray());
							}
							render_pass_info->Unbind();
							Renderer::GetRenderAPI()->Flush();
						});

					mips[level + 1] = downsample_pass->GetData().Output;
				}

				/* ---- 升采样回叠：mip_{i+1} →（3×3 tent × Scatter，加法混合）→ mip_i ---- */
				for (uint32_t level = mip_count - 1; level > 0; --level)
				{
					struct UpsamplePassData
					{
						TextureHandle Source;  /* 读：下一级（更小） */
						TextureHandle Target;  /* 写：本机（PreserveContent + One/One 回叠） */
					};

					const TextureHandle source = mips[level];
					const TextureHandle target = mips[level - 1];
					const glm::vec2 texel_size = mip_texel_size(level);
					const float scatter = settings->Scatter;
					const std::string pass_name = "BloomUpsample" + std::to_string(level - 1);
					/* 回叠目标是上一级 mip，其尺寸即本 Pass 的渲染区域 */
					const auto target_desc = mip_desc(level - 1);

					frame_graph->AddPass<UpsamplePassData>(pass_name,
						[&, source, target, target_desc](FrameGraphBuilder& builder, UpsamplePassData& data)
						{
							data.Source = source;
							data.Target = target;
							builder.BindInputResource(data.Source, FrameGraphTexture::Usage::Sampleable);
							/* 叠加型 Pass：产物是"写进已有资源"，绑定为输入 + 声明副作用，
							 * 否则会被图裁剪剔除（同 SkyPass / AxisPass） */
							builder.BindInputResource(data.Target, FrameGraphTexture::Usage::ColorAttachment);

							FrameGraphPassInfo::Descriptor pass_desc;
							pass_desc.Attachments.ColorAttachments(0) = data.Target;
							pass_desc.PreserveContent = true;
							pass_desc.ViewportRegion = FullTargetRegion(target_desc);
							/* 同上：升采样各级各自一个名字，FrameBuffer 跨帧复用 */
							builder.CreateRenderPass(pass_name + "RenderTarget", pass_desc);

							builder.AsSideEffect();
						},
						[&render_view, upsample_shader, texel_size, scatter, pass_name](const FrameGraphResources& resources, const UpsamplePassData& data)
						{
							const auto render_pass_info = resources.GetPassRenderTarget();
							render_view.EmplacePassFrameBuffer(pass_name, render_pass_info);

							render_pass_info->Bind();
							{
								auto material = MakeFullScreenMaterial(upsample_shader, true);
								material->SetTexture("u_Source", resources.Get(data.Source).Texture);
								material->SetParameters(ParamType::Vec2, "u_TexelSize", texel_size);
								material->SetParameters(ParamType::Float, "u_Scatter", scatter);
								Renderer::Submit(material, Renderer::GetFullScreenVertexArray());
							}
							render_pass_info->Unbind();
							Renderer::GetRenderAPI()->Flush();
						});
				}

				/* ---- 合成：场景色 + bloom × Intensity → PostProcessOutput（RGBA8 全尺寸） ---- */
				struct CompositePassData
				{
					TextureHandle SceneColor;
					TextureHandle BloomTexture;
					TextureHandle Output;
				};

				const TextureHandle bloom_mip0 = mips[0];
				const float intensity = settings->Intensity;

				auto composite_pass = frame_graph->AddPass<CompositePassData>("BloomComposite",
					[&, input, bloom_mip0](FrameGraphBuilder& builder, CompositePassData& data)
					{
						FrameGraphTexture::Descriptor output_desc;
						output_desc.Width = viewport_region.Width;
						output_desc.Height = viewport_region.Height;
						output_desc.TextureFormat = TextureFormat::RGBA8;

						data.SceneColor = input;
						data.BloomTexture = bloom_mip0;
						data.Output = builder.CreateTexture("PostProcessOutput", output_desc);
						builder.BindInputResource(data.SceneColor, FrameGraphTexture::Usage::Sampleable);
						builder.BindInputResource(data.BloomTexture, FrameGraphTexture::Usage::Sampleable);
						builder.BindOutputResource(data.Output,
							FrameGraphTexture::Usage::ColorAttachment | FrameGraphTexture::Usage::Sampleable);

						FrameGraphPassInfo::Descriptor pass_desc;
						pass_desc.Attachments.ColorAttachments(0) = data.Output;
						pass_desc.ViewportRegion = FullTargetRegion(output_desc);
						builder.CreateRenderPass("BloomCompositeRenderTarget", pass_desc);

						/* 输出供视图显示 / 编辑器叠加（图外消费者）：不能随资源生命周期销毁 */
						builder.AsSideEffect();
					},
					[&render_view, composite_shader, intensity](const FrameGraphResources& resources, const CompositePassData& data)
					{
						const auto render_pass_info = resources.GetPassRenderTarget();
						render_view.EmplacePassFrameBuffer("BloomComposite", render_pass_info);

						render_pass_info->Bind();
						{
							auto material = MakeFullScreenMaterial(composite_shader, false);
							material->SetTexture("u_SceneColor", resources.Get(data.SceneColor).Texture);
							material->SetTexture("u_BloomTexture", resources.Get(data.BloomTexture).Texture);
							material->SetParameters(ParamType::Float, "u_Intensity", intensity);
							Renderer::Submit(material, Renderer::GetFullScreenVertexArray());
						}
						render_pass_info->Unbind();
						Renderer::GetRenderAPI()->Flush();
					});

				return composite_pass->GetData().Output;
			}
		}

		void RegisterBloomEffect()
		{
			PostProcessEffectRegistry::Instance().Add(EffectDesc{
				"Bloom", Phase::Scene, &CaptureBloom, &IsBloomEnabled, &BuildBloom });
		}
	}
}
