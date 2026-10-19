#include "Pch.h"
#include "Helios/Reflection/ComponentRegistration.h"
#include "Helios/ImGui/EditorTheme.h"
#include "Helios/ImGui/ImGuiExtensions.h"
#include "Helios/Scene/Components.h"
#include "Helios/Scene/SceneCommon.h"
#include "Helios/VirtualDevice/DeviceTexture.h"
#include <filesystem>
#include <imgui.h>
#include <tinyxml2.h>

namespace Helios
{
	namespace
	{
		/* 探针的字段表之外的持久化内容：天空盒纹理与烘焙结果缓存路径。
		 * 对象为空时不写属性，读的时候也接受属性缺失。 */
		void SaveProbeExtra(tinyxml2::XMLElement* element, const void* component)
		{
			const auto& probe = static_cast<const ReflectionProbeComponent*>(component)->m_ReflectionProbe;
			if (probe == nullptr)
				return;

			const SharedPtr<DeviceTexture> skybox = probe->GetSkyBoxTexture();
			if (skybox != nullptr)
				element->SetAttribute("SkyBoxPath", RELATIVE_PATH(skybox->GetPath()).c_str());

			/* 缓存路径只在有烘焙结果时才落盘：否则场景会指向一份与当前状态不符的缓存 */
			const std::string& cache_path = probe->GetBakeCachePath();
			if (probe->IsBaked() && !cache_path.empty())
				element->SetAttribute("BakeCachePath", cache_path.c_str());
		}

		void LoadProbeExtra(const tinyxml2::XMLElement* element, void* component)
		{
			auto& probe = static_cast<ReflectionProbeComponent*>(component)->m_ReflectionProbe;
			if (probe == nullptr)
				return;

			/* 同上：天空盒路径按相对资产根存储，入口即转绝对 */
			if (const char* path = element->Attribute("SkyBoxPath"); path != nullptr && *path != '\0')
				probe->SetSkyBoxTexture(DeviceTexture::Create(ABSOLUTE_PATH(path)));

			/* 命中缓存就直接恢复烘焙结果：加载后无需再烘焙 */
			if (const char* cache_path = element->Attribute("BakeCachePath"); cache_path != nullptr && *cache_path != '\0')
			{
				probe->SetBakeCachePath(cache_path);
				if (!probe->LoadBakeCache(ABSOLUTE_PATH(cache_path)))
					CORE_LOG_WARN("ReflectionProbe: bake cache '{}' is unavailable, it will be baked again", cache_path);
			}
		}

		/* ==================== 烘焙结果详情 ====================
		 * 把「烘焙了什么 / 存到哪里 / 文件多大」摊开给用户看；大小和时间的格式化走 Utils 的共用实现。 */

		/* 一张烘焙贴图的规格行：尺寸 / 格式 / mip 层数（贴图不存在时不画这一行） */
		void DrawBakeImageRow(const char* label, const SharedPtr<DeviceTexture>& texture)
		{
			if (texture == nullptr)
				return;

			const TextureDesc& desc = texture->GetTextureDesc();
			const char* format_name = GetEnumName(desc.Format);
			const std::string value = std::to_string(desc.Width) + " x " + std::to_string(desc.Height)
				+ ", " + (format_name != nullptr ? format_name : "?")
				+ ", " + std::to_string(desc.MipLevels) + (desc.MipLevels > 1 ? " mips" : " mip");

			ImGuiExt::DrawCommonTextUI(label, value);
		}

		/* 缓存文件行：磁盘上的大小与写入时间；还没写出过时说明它随场景保存产生 */
		void DrawBakeCacheFileRow(const ReflectionProbe& probe)
		{
			const std::string& cache_path = probe.GetBakeCachePath();

			std::filesystem::path relative_path;
			if (cache_path.empty() || !TryPathFromUtf8(cache_path, relative_path))
			{
				ImGuiExt::DrawCommonTextUI("CacheFile", "not written yet");
				return;
			}

			const std::filesystem::path absolute_path = g_AssetsPath / relative_path;

			std::error_code error;
			const uintmax_t size = std::filesystem::file_size(absolute_path, error);
			if (error)
			{
				ImGuiExt::DrawCommonTextUI("CacheFile", "missing on disk");
				return;
			}

			ImGuiExt::DrawCommonTextUI("CacheFile",
				FormatFileSize(size) + ", " + FormatFileWriteTime(absolute_path));
		}

		/* 烘焙结果预览：三张十字展开图并排（立方图不能直接被 2D UI 采样，
		 * 探针把它按需转成 2D —— 见 ReflectionProbe::RequestBakePreview）。
		 * 首次显示 / Rebake 后自动请求生成；鼠标悬停放大看细节。 */
		void DrawBakePreviewRow(ReflectionProbe& probe)
		{
			using PreviewState = ReflectionProbe::BakePreviewState;

			/* 已烘焙但还没有预览 → 请求生成（幂等；生成由探针的状态机在渲染帧推进） */
			if (probe.GetBakePreviewState() == PreviewState::Idle
				&& !probe.HasBakePreview() && !probe.HasBakePreviewFailed())
			{
				probe.RequestBakePreview();
			}

			if (!probe.HasBakePreview())
			{
				if (probe.HasBakePreviewFailed())
					ImGuiExt::DrawCommonTextUI("Preview", "generation failed");
				else if (probe.GetBakePreviewState() != PreviewState::Idle)
					ImGuiExt::DrawCommonTextUI("Preview", "generating...");
				return;
			}

			struct PreviewEntry
			{
				const char* Label;
				ReflectionProbe::BakePreviewKind Kind;
			};
			constexpr PreviewEntry entries[] = {
				{ "Environment", ReflectionProbe::BakePreviewKind::Environment },
				{ "Irradiance",  ReflectionProbe::BakePreviewKind::Irradiance },
				{ "Prefilter",   ReflectionProbe::BakePreviewKind::Prefilter },
			};

			/* 只画实际存在的（烘焙配置可能省掉 IRF / PF） */
			struct ShownPreview
			{
				const char* Label;
				SharedPtr<DeviceTexture> Texture;
			};
			ShownPreview shown[3];
			size_t count = 0;
			for (const PreviewEntry& entry : entries)
			{
				SharedPtr<DeviceTexture> texture = probe.GetBakePreviewTexture(entry.Kind);
				if (texture != nullptr)
					shown[count++] = ShownPreview{ entry.Label, std::move(texture) };
			}

			ImGui::Spacing();

			const float spacing = ImGui::GetStyle().ItemSpacing.x;
			const float total_width = ImGui::GetContentRegionAvail().x;
			const float image_width = (total_width - spacing * static_cast<float>(count - 1)) / static_cast<float>(count);
			const float image_height = image_width * 0.75f; /* 十字展开是 4x3 */

			const float start_x = ImGui::GetCursorPosX();
			const float start_y = ImGui::GetCursorPosY();

			for (size_t index = 0; index < count; ++index)
			{
				if (index > 0)
					ImGui::SameLine(0.0f, spacing);

				/* UV 不翻转：预览图的第 0 行（+Y 面）就在顶部 —— 与图片的 (0,1)-(1,0)
				 * 相反，那些贴图在上传前被垂直翻转（IsFlipV），数据行序与这里正相反 */
				ImGui::Image((ImTextureID)shown[index].Texture.get(), ImVec2(image_width, image_height),
					ImVec2(0, 0), ImVec2(1, 1));

				if (ImGui::IsItemHovered())
				{
					ImGui::BeginTooltip();
					ImGui::Image((ImTextureID)shown[index].Texture.get(), ImVec2(320.0f, 240.0f),
						ImVec2(0, 0), ImVec2(1, 1));
					ImGui::TextUnformatted(shown[index].Label);
					ImGui::EndTooltip();
				}
			}

			/* 名字逐张居中放在图的下面（宽度不足时以图为准，宁挤不偏） */
			const float caption_y = start_y + image_height + ImGui::GetStyle().ItemSpacing.y;
			for (size_t index = 0; index < count; ++index)
			{
				const float text_width = ImGui::CalcTextSize(shown[index].Label).x;
				const float center_x = start_x + static_cast<float>(index) * (image_width + spacing)
					+ (image_width - text_width) * 0.5f;

				ImGui::SetCursorPos(ImVec2(center_x, caption_y));
				ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::TextDim);
				ImGui::TextUnformatted(shown[index].Label);
				ImGui::PopStyleColor();
			}
		}

		/* 天空盒是资源引用，要能选择、悬停预览，所以整块自定义绘制；烘焙参数走 schema 字段。
		 * 返回值是"有没有改动"：换天空盒会生成快照命令（撤销和脏标记靠它）；Rebake 是异步操作、不算改动。 */
		bool DrawProbeBlock(void* raw)
		{
			auto& probe = static_cast<ReflectionProbeComponent*>(raw)->m_ReflectionProbe;
			if (probe == nullptr)
				return false;

			bool changed = false;

			SharedPtr<DeviceTexture> skybox = probe->GetSkyBoxTexture();
			ImGuiExt::DrawTextureUI("SkyBox", skybox);
			if (skybox != probe->GetSkyBoxTexture())
			{
				probe->SetSkyBoxTexture(skybox);
				changed = true;
			}

			ImGui::Separator();

			/* 烘焙结果是只读状态：它随「保存场景」一并落盘，写盘要等 GPU 执行完，
			 * 所以不是存档当帧就完成。状态行与上面的资源行共用同一套属性行布局。 */
			using WriteState = ReflectionProbe::BakeCacheWriteState;
			const WriteState state = probe->GetBakeCacheWriteState();

			const char* status = "Not baked";
			if (state == WriteState::Requested)
				status = "Baking...";
			else if (state == WriteState::WaitingForGPU)
				status = "Saving...";
			else if (probe->IsBaked())
				status = "Baked";

			ImGuiExt::DrawCommonTextUI("Status", status);

			if (state == WriteState::Idle && probe->IsBaked())
			{
				const std::string& cache_path = probe->GetBakeCachePath();
				ImGuiExt::DrawCommonTextUI("BakeCache",
					cache_path.empty() ? "(written with the scene)" : cache_path);

				/* 详情：三张烘焙贴图的规格（+ 预览）+ 缓存文件在磁盘上的大小与写入时间 */
				DrawBakeImageRow("Environment", probe->GetEnvCubemap());
				DrawBakeImageRow("Irradiance", probe->GetIrradianceMap());
				DrawBakeImageRow("Prefilter", probe->GetPrefilterMap());
				DrawBakePreviewRow(*probe);
				DrawBakeCacheFileRow(*probe);
			}

			/* 写盘失败不隐藏：否则用户会以为已经存下来了 */
			if (probe->HasBakeCacheWriteFailed())
				ImGuiExt::DrawCommonTextUI("BakeCache", "write failed");

			/* 重烘焙走"请求"：本帧绘制列表还画着预览图，当场 Reset 会把它们
			 * 销毁成悬垂纹理指针（渲染时崩）；实际重置推迟到下一帧 Tick */
			if (state == WriteState::Idle && ImGui::Button("Rebake"))
				probe->RequestRebake();

			return changed;
		}

		/* 探针为空时整个组件块不显示 */
		bool HasProbeObject(const void* component)
		{
			return static_cast<const ReflectionProbeComponent*>(component)->m_ReflectionProbe != nullptr;
		}
	}

	void RegisterReflectionProbeComponent()
	{
		/* 反射探针：烘焙参数以结构体整体读写，故用配置访问器逐个字段暴露 */
		MakeRegistrar<ReflectionProbeComponent>("ReflectionProbe")
			.SaveExtra(&SaveProbeExtra)
			.LoadExtra(&LoadProbeExtra)
			.Field(MakeAccessor<&ReflectionProbeComponent::m_ReflectionProbe,
					&ReflectionProbe::IsRealtime, &ReflectionProbe::SetRealtime>(),
				"Realtime", FieldType::Bool)
			.Field(MakeAccessor<&ReflectionProbeComponent::m_ReflectionProbe,
					&ReflectionProbe::GetBoxSize, &ReflectionProbe::SetBoxSize>(),
				"BoxSize", FieldType::Vec3, 0.1f, 10.0f)
			.Field(MakeAccessor<&ReflectionProbeComponent::m_ReflectionProbe,
					&ReflectionProbe::GetBlendDistance, &ReflectionProbe::SetBlendDistance>(),
				"BlendDistance", FieldType::Float, 0.01f)
			/* 探针关心的层：烘焙只捕获这些层，视图也只在与自身可见层相交时使用它 */
			.Field(MakeAccessor<&ReflectionProbeComponent::m_ReflectionProbe,
					&ReflectionProbe::GetCullingMask, &ReflectionProbe::SetCullingMask>(),
				"CullingMask", FieldType::LayerMask)
			.Field(MakeConfigAccessor<&ReflectionProbeComponent::m_ReflectionProbe,
					&ReflectionProbe::GetBakeConfig, &ReflectionProbe::SetBakeConfig,
					&ReflectionProbe::BakeConfig::EnvSize>(),
				"EnvSize", FieldType::Int)
			.Field(MakeConfigAccessor<&ReflectionProbeComponent::m_ReflectionProbe,
					&ReflectionProbe::GetBakeConfig, &ReflectionProbe::SetBakeConfig,
					&ReflectionProbe::BakeConfig::IrradianceSize>(),
				"IrradianceSize", FieldType::Int)
			.Field(MakeConfigAccessor<&ReflectionProbeComponent::m_ReflectionProbe,
					&ReflectionProbe::GetBakeConfig, &ReflectionProbe::SetBakeConfig,
					&ReflectionProbe::BakeConfig::PrefilterSize>(),
				"PrefilterSize", FieldType::Int)
			.Field(MakeConfigAccessor<&ReflectionProbeComponent::m_ReflectionProbe,
					&ReflectionProbe::GetBakeConfig, &ReflectionProbe::SetBakeConfig,
					&ReflectionProbe::BakeConfig::PrefilterMipLevels>(),
				"PrefilterMipLevels", FieldType::Int)
			.Field(MakeConfigAccessor<&ReflectionProbeComponent::m_ReflectionProbe,
					&ReflectionProbe::GetBakeConfig, &ReflectionProbe::SetBakeConfig,
					&ReflectionProbe::BakeConfig::BakeSkyBoxOnly>(),
				"BakeSkyBoxOnly", FieldType::Bool)
			.Field(MakeConfigAccessor<&ReflectionProbeComponent::m_ReflectionProbe,
					&ReflectionProbe::GetBakeConfig, &ReflectionProbe::SetBakeConfig,
					&ReflectionProbe::BakeConfig::BakeDiffuse>(),
				"BakeDiffuse", FieldType::Bool)
			.Field(MakeConfigAccessor<&ReflectionProbeComponent::m_ReflectionProbe,
					&ReflectionProbe::GetBakeConfig, &ReflectionProbe::SetBakeConfig,
					&ReflectionProbe::BakeConfig::BakeSpecular>(),
				"BakeSpecular", FieldType::Bool)
			.CustomDraw(&DrawProbeBlock)
			.Visible(&HasProbeObject)
			.Register();
	}
}
