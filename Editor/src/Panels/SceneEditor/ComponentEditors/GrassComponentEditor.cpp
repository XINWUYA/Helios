#include "Pch.h"
#include "ComponentEditors.h"
#include "Material/MaterialParamUI.h"
#include "EditorUI/MaterialSlotRow.h"
#include "EditorUI/ImGuiExtensions.h"
#include "EditorUI/EditorTheme.h"
#include "Panels/SceneEditor/TerrainSculpt.h"
#include "Helios/Core/AssetManager.h"
#include "Helios/Scene/GrassField.h"
#include "Helios/Reflection/ComponentRegistry.h"
#include "Helios/Scene/Components.h"
#include "Helios/VirtualDevice/DeviceTexture.h"

namespace Helios
{
	namespace
	{
		/* 密度蒙版转一张灰度图：白 = 满密度，黑 = 擦干净。
		 * 缓存按组件地址加 revision 认，涂一笔才重传（256² 的图，一笔一帧，开销可以忽略） */
		SharedPtr<DeviceTexture> GetDensityMapTexture(const GrassComponent& component)
		{
			constexpr uint32_t n = GrassComponent::kDensityMaskSize;
			if (component.m_DensityMask.size() != size_t(n) * n)
				return nullptr;

			struct Cache
			{
				const GrassComponent* Owner{ nullptr };
				const void* Data{ nullptr };   /* 换过一份蒙版（切场景）也得重传 */
				uint64_t Revision{ 0 };
				SharedPtr<DeviceTexture> Texture;
				std::vector<uint8_t> Pixels;   /* 上传缓冲复用，拖笔刷时每帧都要重传 */
			};
			static Cache cache;

			if (cache.Owner == &component && cache.Data == component.m_DensityMask.data()
				&& cache.Revision == component.m_DensityMaskRevision && cache.Texture != nullptr)
				return cache.Texture;

			if (cache.Texture == nullptr)
			{
				TextureDesc desc;
				desc.Width = n;
				desc.Height = n;
				desc.MipLevels = 1;
				desc.Format = TextureFormat::RGBA8;
				desc.SamplerType = SamplerType::Sampler2D;
				desc.Usage = TextureUsage::Sampleable;
				cache.Texture = DeviceTexture::Create("GrassDensityMap", desc);
				if (cache.Texture == nullptr)
					return nullptr;
			}

			cache.Pixels.resize(size_t(n) * n * 4u);
			for (size_t i = 0; i < component.m_DensityMask.size(); ++i)
			{
				const uint8_t level = component.m_DensityMask[i];
				cache.Pixels[i * 4u + 0u] = level;
				cache.Pixels[i * 4u + 1u] = level;
				cache.Pixels[i * 4u + 2u] = level;
				cache.Pixels[i * 4u + 3u] = 255u;
			}
			cache.Texture->SetData(cache.Pixels.data(),
				PixelDesc{ PixelFormat::RGBA, PixelType::UnsignedByte });
			cache.Owner = &component;
			cache.Data = component.m_DensityMask.data();
			cache.Revision = component.m_DensityMaskRevision;
			return cache.Texture;
		}

		void DrawDensityMap(const GrassComponent& component)
		{
			if (component.m_DensityMask.empty())
			{
				ImGui::TextDisabled("Nothing painted, full density everywhere");
				return;
			}
			const SharedPtr<DeviceTexture> texture = GetDensityMapTexture(component);
			if (texture == nullptr)
				return;

			/* 方图跟面板同宽会太高，封顶 180 设计像素；窄到放不下就不画，免得压坏布局 */
			const float available = ImGui::GetContentRegionAvail().x;
			if (available < EditorTheme::Scaled(48.0f))
				return;
			const float extent = std::min(available, EditorTheme::Scaled(180.0f));
			const ImVec2 min = ImGui::GetCursorScreenPos();
			/* 蒙版第 0 行落在 -Z 侧，翻转 V 让 -Z 显示在图上方，跟视口里的朝向对得上 */
			ImGui::Image((ImTextureID)texture.get(), ImVec2(extent, extent),
				ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));
			ImGui::GetWindowDrawList()->AddRect(min, ImVec2(min.x + extent, min.y + extent),
				ImGui::GetColorU32(EditorTheme::Token::Border));
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("White is full density, black is bare ground\nTop is -Z, right is +X");
		}

		/* 放置路径只在这里显示：菜单那个开关是「想用 GPU」，
		 * 后端不支持计算、实例缓冲没有共享视图、地形高度没传上 GPU 时都会退回 CPU */
		void DrawPlacementRow(const GrassComponent& component)
		{
			GrassField::Placement placement = GrassField::Placement::Pending;
			if (component.m_Field != nullptr)
				placement = component.m_Field->GetPlacement();

			const char* text = "Placement: pending";
			const char* hint = "Instances are generated on the next update";
			ImVec4 color = EditorTheme::Token::TextDim;
			switch (placement)
			{
			case GrassField::Placement::Gpu:
				text = "Placement: GPU";
				hint = "Blade positions come from the compute shader";
				color = EditorTheme::Token::Text;
				break;
			case GrassField::Placement::CpuDisabled:
				text = "Placement: CPU (switch off)";
				hint = "Turn on GPU Grass Placement in the Rendering menu";
				break;
			case GrassField::Placement::CpuNoCompute:
				text = "Placement: CPU (no compute support)";
				hint = "This backend cannot run compute shaders";
				color = EditorTheme::Token::Warning;
				break;
			case GrassField::Placement::CpuNoSharedBuffer:
				text = "Placement: CPU (no shared buffer)";
				hint = "The instance buffer has no shared vertex view on this backend";
				color = EditorTheme::Token::Warning;
				break;
			case GrassField::Placement::CpuNoChunkTable:
				text = "Placement: CPU (chunk table missing)";
				hint = "The chunk index table was not created";
				color = EditorTheme::Token::Warning;
				break;
			case GrassField::Placement::CpuNoGroundHeight:
				text = "Placement: CPU (terrain heights not on GPU)";
				hint = "Grass conforms to terrain but the height buffer is missing";
				color = EditorTheme::Token::Warning;
				break;
			case GrassField::Placement::Pending:
			default:
				break;
			}

			ImGui::TextColored(color, "%s", text);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", hint);
		}

		/* 这一帧实际怎么提交的：开关只是意图，退回批表的原因也在这里，
		 * 面板和 Geometry 卡上的笔数能对着看 */
		void DrawSubmitRow(const GrassComponent& component)
		{
			GrassField::DrawPath path = GrassField::DrawPath::Pending;
			if (component.m_Field != nullptr)
				path = component.m_Field->GetDrawPath();

			const char* text = "Submit: pending";
			const char* hint = "Grass has not been submitted yet";
			ImVec4 color = EditorTheme::Token::TextDim;
			switch (path)
			{
			case GrassField::DrawPath::Indirect:
				text = "Submit: indirect";
				hint = "GPU culled, segments drawn with indirect args";
				color = EditorTheme::Token::Text;
				break;
			case GrassField::DrawPath::BatchingSwitchOff:
				text = "Submit: batched (indirect off)";
				hint = "Turn on GPU Grass Indirect Draw to cull on the GPU";
				break;
			case GrassField::DrawPath::BatchingNoIndirect:
				text = "Submit: batched (no indirect support)";
				hint = "This backend cannot draw or dispatch with indirect args";
				color = EditorTheme::Token::Warning;
				break;
			case GrassField::DrawPath::BatchingNoSlot:
				text = "Submit: batched (cull not ready)";
				hint = "The instance buffer or chunk table was not ready this frame";
				color = EditorTheme::Token::Warning;
				break;
			case GrassField::DrawPath::Chunk:
				text = "Submit: per chunk (batching off)";
				hint = "Grass Block Batching is off, one submit per chunk";
				break;
			default:
				break;
			}

			ImGui::TextColored(color, "%s", text);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", hint);
		}

		/* 草地只有一个材质槽：行版式与模型槽共用，这里只管语义 ——
		 * 清掉就没有资产来源，回内建默认外观；Reset 从来源 .mtl 重新克隆。
		 * 返回是否改动：换材质要生成快照命令（撤销与场景脏标记都靠它） */
		bool DrawGrassMaterialBlock(void* raw)
		{
			auto& component = *static_cast<GrassComponent*>(raw);
			MaterialSlotOverride& slot = component.m_MaterialSlot;
			DrawPlacementRow(component);
			DrawSubmitRow(component);
			if (MaterialParamUI::BeginPropertyGroup("##DensityPaint", "Density paint"))
			{
				auto& settings = GrassPaint::GetSettings();
				const bool ready = component.m_Field != nullptr && !component.m_Field->IsEmpty();
				ImGui::BeginDisabled(!ready);
				if (ImGui::RadioButton("Off##GrassPaint", settings.Brush == GrassPaint::Mode::None))
					settings.Brush = GrassPaint::Mode::None;
				ImGui::SameLine();
				if (ImGui::RadioButton("Add##GrassPaint", settings.Brush == GrassPaint::Mode::Add))
				{
					settings.Brush = GrassPaint::Mode::Add;
					TerrainSculpt::GetSettings().Brush = TerrainSculpt::Mode::None;
				}
				ImGui::SameLine();
				if (ImGui::RadioButton("Erase##GrassPaint", settings.Brush == GrassPaint::Mode::Erase))
				{
					settings.Brush = GrassPaint::Mode::Erase;
					TerrainSculpt::GetSettings().Brush = TerrainSculpt::Mode::None;
				}
				ImGuiExt::DrawSliderFloatUI("Radius", settings.Radius, 1.0f, 100.0f,
					EditorTheme::Token::PropertyLabelWidth, "%.1f m");
				ImGuiExt::DrawSliderFloatUI("Strength", settings.Strength, 0.1f, 1.0f,
					EditorTheme::Token::PropertyLabelWidth, "%.2f /s");
				ImGui::EndDisabled();
				ImGui::Spacing();
				DrawDensityMap(component);
				if (!component.m_DensityMask.empty())
					ImGui::TextDisabled("Painted density is saved with the scene");
			}

			/* 材质引用单独成组：它跟上面的草地参数不是一类东西，收起时也不碍事 */
			if (!MaterialParamUI::BeginPropertyGroup("##Material", "Material"))
				return false;

			const std::string material_path = !slot.SourceAssetPath.empty()
				? slot.SourceAssetPath
				: (slot.pMaterial != nullptr ? slot.pMaterial->GetPath() : std::string());

			MaterialSlotRow::State state;
			state.Label = "Source";
			state.Path = material_path;
			state.EmptyText = "Built-in default";
			state.HoverPath = material_path.empty() ? std::string() : RELATIVE_PATH(material_path);
			state.IsInstance = slot.IsInstance;
			/* 已经落在内建默认上就没有可清的东西 */
			state.CanClear = !material_path.empty();
			state.EmptyHint = "Drop a .mtl here";
			state.ClearHint = "Clear the source and fall back to the built-in default look";
			state.ResetHint = "Clone again from the source .mtl";

			const MaterialSlotRow::Result row = MaterialSlotRow::Draw(state);
			switch (row.Act)
			{
			case MaterialSlotRow::Action::Assign:
				slot.pMaterial = row.Picked;
				slot.SourceAssetPath = row.SourcePath;
				slot.IsInstance = false;
				return true;
			case MaterialSlotRow::Action::Clear:
				slot.pMaterial = GrassComponent::GetDefaultMaterial();
				slot.SourceAssetPath.clear();
				slot.IsInstance = false;
				return true;
			case MaterialSlotRow::Action::Reset:
			{
				auto source = AssetManager::Instance().GetOrLoad<Material>(slot.SourceAssetPath);
				if (source == nullptr)
					return false;

				slot.pMaterial = source->Clone();
				slot.IsInstance = true;
				return true;
			}
			default:
				return false;
			}
		}
	}

	void RegisterGrassComponentEditor()
	{
		ComponentRegistry::Instance().SetCustomDraw(typeid(GrassComponent), &DrawGrassMaterialBlock);
	}
}
