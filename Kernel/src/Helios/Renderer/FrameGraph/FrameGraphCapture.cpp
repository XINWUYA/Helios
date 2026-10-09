#include "Pch.h"
#include "FrameGraphCapture.h"
#include "Helios/Renderer/RenderAPI.h"
#include "Helios/Renderer/Renderer.h"
#include "Helios/VirtualDevice/DeviceTexture.h"

namespace Helios
{
	void FrameGraphCapture::BeginFrame()
	{
		if (!m_IsEnabled)
			return;

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

	void FrameGraphCapture::CapturePass(const std::string& pass_name,
		const std::vector<std::pair<std::string, SharedPtr<DeviceTexture>>>& outputs)
	{
		if (!m_IsEnabled)
			return;

		PassEntry& pass_entry = m_Passes[pass_name];
		pass_entry.SeenThisFrame = true;

		/* 上一帧的输出先接过来：同名输出优先复用它们的预览纹理（尺寸 / 格式没变就
		 * 不重新分配）；换新后才释放（未被复用的旧预览在这里随容器析构） */
		std::vector<Output> previous_outputs;
		previous_outputs.swap(pass_entry.Outputs);

		pass_entry.Outputs.reserve(outputs.size());
		for (const auto& output_pair : outputs)
		{
			const std::string& resource_name = output_pair.first;
			const SharedPtr<DeviceTexture>& source = output_pair.second;
			if (!IsPreviewable(source))
				continue;

			SharedPtr<DeviceTexture> preview;
			for (const Output& old_output : previous_outputs)
			{
				if (old_output.Name == resource_name && PreviewMatches(old_output.Preview, source))
				{
					preview = old_output.Preview;
					break;
				}
			}
			if (preview == nullptr)
				preview = CreatePreview(pass_name, resource_name, source);
			if (preview == nullptr)
				continue;

			Renderer::GetRenderAPI()->CopyTexture(source, preview);
			pass_entry.Outputs.push_back(Output{ resource_name, preview, ++m_SequenceCounter });
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
		pass_entry.Outputs.push_back(Output{ resource_name, texture, ++m_SequenceCounter, top_down });
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
