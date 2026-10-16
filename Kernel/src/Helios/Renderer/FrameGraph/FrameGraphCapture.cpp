#include "Pch.h"
#include "FrameGraphCapture.h"
#include "Helios/Renderer/RenderAPI.h"
#include "Helios/Renderer/Renderer.h"
#include "Helios/VirtualDevice/DeviceTexture.h"

namespace Helios
{
	namespace
	{
		/* 深度预览显示范围的帧龄节流：同一路输出每隔这么多帧才重新回读刷新 ——
		 * 范围随场景变化，~1 秒的滞后对调试预览不可见，但省下每帧的全幅回读 */
		constexpr uint64_t kRangeRefreshAge = 60;
		/* 首刷（含跨帧拷贝延迟）的快速重试上限：连续刷到空内容这么多次后只走帧龄节流 */
		constexpr uint8_t kEmptyRangeRetries = 3;
	}

	void FrameGraphCapture::BeginFrame()
	{
		if (!m_IsEnabled)
			return;

		++m_FrameSerial;

		/* 上一帧挂起的显示范围刷新在这里落地：此刻上一帧的命令已提交、预览拷贝可回读
		 * （帧中回读只能拿到拷贝前的旧数据）。每帧至多一路（挂起时选取的"最旧"输出）。 */
		for (auto& pass_entry : m_Passes)
		{
			PassEntry& entry = pass_entry.second;
			if (entry.PendingRangeOutput.empty())
				continue;

			for (Output& output : entry.Outputs)
			{
				if (output.Name == entry.PendingRangeOutput)
				{
					RefreshDisplayRange(output);
					break;
				}
			}
			entry.PendingRangeOutput.clear();
		}

		for (auto& pass_entry : m_Passes)
			pass_entry.second.SeenThisFrame = false;
	}

	void FrameGraphCapture::EndFrame()
	{
		if (!m_IsEnabled)
			return;

		/* 本帧未出现的 Pass（管线切换 / 被剔除 / 视口关闭）：条目连同预览纹理一起淘汰 */
		for (auto iter = m_Passes.begin(); iter != m_Passes.end();)
		{
			if (iter->second.SeenThisFrame)
				++iter;
			else
				iter = m_Passes.erase(iter);
		}
	}

	void FrameGraphCapture::SetLayerNames(const std::string& resource_name, std::vector<std::string> layer_names)
	{
		m_LayerNames[resource_name] = std::move(layer_names);
	}

	void FrameGraphCapture::CapturePass(const std::string& pass_name,
		const std::vector<std::pair<std::string, SharedPtr<DeviceTexture>>>& outputs)
	{
		if (!m_IsEnabled)
			return;

		/* 离屏渲染目标的行序由后端上报（Metal 顶行在前）——与默认目标快照同一约定，
		 * 预览才与源纹理 / Xcode 抓帧方向一致 */
		const bool top_down = Renderer::GetRenderAPI()->IsRenderTargetTopDown();

		PassEntry& pass_entry = m_Passes[pass_name];
		pass_entry.SeenThisFrame = true;

		/* 上一帧的输出先接过来：同名输出优先复用它们的预览纹理（尺寸 / 格式没变就
		 * 不重新分配；显示范围等字段一并接续）；换新后才释放（未被复用的旧预览在这里
		 * 随容器析构） */
		std::vector<Output> previous_outputs;
		previous_outputs.swap(pass_entry.Outputs);

		pass_entry.Outputs.reserve(outputs.size());
		for (const auto& output_pair : outputs)
		{
			const std::string& resource_name = output_pair.first;
			const SharedPtr<DeviceTexture>& source = output_pair.second;
			if (!IsPreviewable(source))
				continue;

			const auto find_previous = [&](const std::string& name) -> Output*
			{
				for (Output& old_output : previous_outputs)
				{
					if (old_output.Name == name && PreviewMatches(old_output.Preview, source))
						return &old_output;
				}
				return nullptr;
			};

			/* 接续上一帧同名输出的显示范围（纹理复用则范围同样有效） */
			const auto append_output = [&](std::string name, Output* previous)
			{
				Output output{ std::move(name), resource_name, previous->Preview,
					++m_SequenceCounter, top_down };
				output.DisplayMin = previous->DisplayMin;
				output.DisplayMax = previous->DisplayMax;
				output.RangeFrame = previous->RangeFrame;
				output.HasRange = previous->HasRange;
				/* 空内容重试计数一并接续：否则每帧都按首刷重试，日志逐帧刷屏 */
				output.EmptyRetries = previous->EmptyRetries;
				output.RangeReadback = std::move(previous->RangeReadback); /* 回读缓冲随输出迁移复用 */
				pass_entry.Outputs.push_back(std::move(output));
			};

			/* 注册过"逐层显示名"的 2D 数组：按层展开成多条输出 —— 每层一条、各配独立
			 * 预览（名字缺省 "资源名 #层号"，不因注册表不齐而丢层） */
			const auto layer_names_iter = m_LayerNames.find(resource_name);
			const uint32_t layer_count = source->GetTextureDesc().Depth;
			if (layer_names_iter != m_LayerNames.end() && layer_count > 1)
			{
				for (uint32_t layer = 0; layer < layer_count; ++layer)
				{
					std::string display_name = (layer < layer_names_iter->second.size())
						? layer_names_iter->second[layer]
						: resource_name + " #" + std::to_string(layer);

					Output* previous = find_previous(display_name);
					if (previous != nullptr)
					{
						Renderer::GetRenderAPI()->CopyTexture(source, previous->Preview, layer);
						append_output(std::move(display_name), previous);
						continue;
					}

					SharedPtr<DeviceTexture> preview = CreatePreview(pass_name, display_name, source);
					if (preview == nullptr)
						continue;

					Renderer::GetRenderAPI()->CopyTexture(source, preview, layer);
					Output output{ std::move(display_name), resource_name, preview, ++m_SequenceCounter, top_down };
					pass_entry.Outputs.push_back(std::move(output));
				}
				continue;
			}

			Output* previous = find_previous(resource_name);
			if (previous != nullptr)
			{
				Renderer::GetRenderAPI()->CopyTexture(source, previous->Preview);
				append_output(resource_name, previous);
				continue;
			}

			SharedPtr<DeviceTexture> preview = CreatePreview(pass_name, resource_name, source);
			if (preview == nullptr)
				continue;

			Renderer::GetRenderAPI()->CopyTexture(source, preview);
			Output output{ resource_name, resource_name, preview, ++m_SequenceCounter, top_down };
			pass_entry.Outputs.push_back(std::move(output));
		}

		/* 深度预览的显示范围：每帧至多挂起一路"最旧"的输出刷新。首刷未出内容会快速
		 * 重试（拷贝提交有跨帧延迟），连续空内容到上限后回到帧龄节流（空层不每帧回读）。 */
		if (pass_entry.PendingRangeOutput.empty())
		{
			Output* stale_output = nullptr;
			for (Output& output : pass_entry.Outputs)
			{
				const bool is_depth = output.Preview != nullptr
					&& output.Preview->GetTextureDesc().Format == TextureFormat::Depth32F;
				if (!is_depth)
					continue;

				const bool boot_retry = !output.HasRange && output.EmptyRetries < kEmptyRangeRetries;
				const bool aged = (m_FrameSerial - output.RangeFrame) >= kRangeRefreshAge;
				if (!boot_retry && !aged)
					continue;

				if (stale_output == nullptr || output.RangeFrame < stale_output->RangeFrame)
					stale_output = &output;
			}
			if (stale_output != nullptr)
				pass_entry.PendingRangeOutput = stale_output->Name;
		}
	}

	void FrameGraphCapture::RefreshDisplayRange(Output& output)
	{
		float min_value = 1.0f;
		float max_value = 0.0f;
		bool has_content = false;

		/* 用输出自带的复用缓冲回读：常态下 resize 是 no-op，只付 getBytes + 扫描的成本
		 * （避免每次重建全幅 —— Debug 下 50ms+） */
		std::vector<uint8_t>& pixels = output.RangeReadback;
		if (output.Preview != nullptr && output.Preview->ReadbackPixels(pixels, 0, 0))
		{
			for (size_t offset = 0; offset + 4 <= pixels.size(); offset += 4)
			{
				float value = 0.0f;
				std::memcpy(&value, pixels.data() + offset, sizeof(float));
				if (!(value > 0.0f)) /* 清屏值 0（没被写过）不参与范围 */
					continue;
				min_value = has_content ? std::min(min_value, value) : value;
				max_value = has_content ? std::max(max_value, value) : value;
				has_content = true;
			}
		}

		output.DisplayMin = min_value;
		output.DisplayMax = max_value;
		if (output.Preview != nullptr)
			output.Preview->SetDisplayRange(min_value, max_value);
		output.RangeFrame = m_FrameSerial;
		if (has_content)
		{
			output.HasRange = true;
			output.EmptyRetries = 0;
		}
		else
		{
			/* 空内容（含首刷读到拷贝前的旧状态）：限定次数的快速重试，之后只走帧龄节流 */
			output.EmptyRetries = static_cast<uint8_t>(
				std::min<uint32_t>(output.EmptyRetries + 1u, 255u));
		}
	}

	void FrameGraphCapture::CaptureSnapshot(const std::string& pass_name,
		const std::string& resource_name, const SharedPtr<DeviceTexture>& texture, bool top_down)
	{
		if (!m_IsEnabled || texture == nullptr)
			return;

		PassEntry& pass_entry = m_Passes[pass_name];
		pass_entry.SeenThisFrame = true;

		/* 快照纹理由 RenderAPI 持有（跨帧复用），这里整体替换本 Pass 的输出 */
		pass_entry.Outputs.clear();
		pass_entry.Outputs.push_back(Output{ resource_name, resource_name, texture, ++m_SequenceCounter, top_down });
	}

	const std::vector<FrameGraphCapture::Output>* FrameGraphCapture::GetPassOutputs(const std::string& pass_name) const
	{
		const auto iter = m_Passes.find(pass_name);
		if (iter == m_Passes.end() || iter->second.Outputs.empty())
			return nullptr;

		return &iter->second.Outputs;
	}

	const FrameGraphCapture::Output* FrameGraphCapture::GetResourcePreview(const std::string& resource_name) const
	{
		/* 遍历所有 Pass 的输出找该资源的最新快照（Sequence 单调递增 = 帧内执行顺序） */
		const Output* latest = nullptr;
		for (const auto& pass_entry : m_Passes)
		{
			for (const Output& output : pass_entry.second.Outputs)
			{
				if (output.Name == resource_name
					&& (latest == nullptr || output.Sequence > latest->Sequence))
					latest = &output;
			}
		}
		return latest;
	}

	size_t FrameGraphCapture::GetCapturedOutputCount() const
	{
		size_t count = 0;
		for (const auto& pass_entry : m_Passes)
			count += pass_entry.second.Outputs.size();

		return count;
	}

	bool FrameGraphCapture::IsPreviewable(const SharedPtr<DeviceTexture>& texture)
	{
		if (texture == nullptr)
			return false;

		const auto& desc = texture->GetTextureDesc();

		/* 多重采样纹理不能作为 blit 拷贝的源（需要先 resolve）；立方图 / 3D 的
		 * "整幅"预览语义不明确，暂不参与预览。2D 数组取第 0 层（阴影图数组的
		 * 层 = 级联 / 光源面，第 0 层最有代表性）。 */
		if (desc.Samples > 1)
			return false;
		if (desc.SamplerType != SamplerType::Sampler2D && desc.SamplerType != SamplerType::Sampler2DArray)
			return false;

		return IsDisplayableFormat(desc.Format);
	}

	bool FrameGraphCapture::IsDisplayableFormat(TextureFormat format)
	{
		/* 白名单：ImGui 渲染路径能按可显示方式采样的格式。深度格式走 depth2d 变体（灰度）；整数
		 * 格式（R32I 这些）排除在外 —— ImGui 按浮点采样，绑整数纹理在 Metal 校验层会报类型不匹配。 */
		if (IsDepthFormat(format))
			return true;

		switch (format)
		{
		case TextureFormat::R8:
		case TextureFormat::R8_SNorm:
		case TextureFormat::RG8:
		case TextureFormat::RG8_SNorm:
		case TextureFormat::RGB8:
		case TextureFormat::s_RGB8:
		case TextureFormat::RGB8_SNorm:
		case TextureFormat::RGBA8:
		case TextureFormat::s_RGBA8:
		case TextureFormat::RGBA8_SNorm:
		case TextureFormat::R16F:
		case TextureFormat::RG16F:
		case TextureFormat::RGBA16F:
		case TextureFormat::R32F:
		case TextureFormat::RGB32F:
		case TextureFormat::RGBA32F:
		case TextureFormat::R11G11B10F:
		case TextureFormat::R10G10B10A2:
		case TextureFormat::RGB9_E5:
		case TextureFormat::RGB565:
		case TextureFormat::RGBA4:
			return true;
		default:
			return false;
		}
	}

	bool FrameGraphCapture::PreviewMatches(const SharedPtr<DeviceTexture>& preview,
		const SharedPtr<DeviceTexture>& source)
	{
		if (preview == nullptr || source == nullptr)
			return false;

		const TextureDesc& preview_desc = preview->GetTextureDesc();
		const TextureDesc& source_desc = source->GetTextureDesc();
		return preview_desc.Width == source_desc.Width
			&& preview_desc.Height == source_desc.Height
			&& preview_desc.Format == source_desc.Format;
	}

	SharedPtr<DeviceTexture> FrameGraphCapture::CreatePreview(const std::string& pass_name,
		const std::string& resource_name, const SharedPtr<DeviceTexture>& source)
	{
		const TextureDesc& source_desc = source->GetTextureDesc();

		TextureDesc preview_desc;
		preview_desc.Width = source_desc.Width;
		preview_desc.Height = source_desc.Height;
		preview_desc.MipLevels = 1;
		preview_desc.Samples = 1;
		preview_desc.Format = source_desc.Format;
		preview_desc.SamplerType = SamplerType::Sampler2D;
		preview_desc.Usage = TextureUsage::Sampleable;

		return DeviceTexture::Create("FrameGraphCapture/" + pass_name + "/" + resource_name, preview_desc);
	}
}
