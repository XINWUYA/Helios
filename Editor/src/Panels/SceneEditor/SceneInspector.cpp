#include "Pch.h"
#include "SceneInspector.h"
#include "SceneHierarchyShared.h"
#include "ComponentEditors/ComponentEditors.h"
#include "Material/MaterialSlotEditing.h"
#include "Common/EditorCommon.h"
#include "UI/EditorIcons.h"
#include "Assets/Gif.h"
#include "Material/MaterialParamUI.h"
#include "Command/AssetFileOps.h"
#include "UI/PanelChrome.h"
#include "UI/PanelRegistry.h"
#include "Panels/PanelWindow.h"
#include "UI/Svg.h"
#include "Helios/Core/AssetManager.h"
#include "Helios/Core/TextureSettings.h"
#include "Helios/Common/PathUtils.h"
#include "Helios/Common/Utils.h"
#include "Helios/Scene/SceneCommon.h"
#include "Helios/Scene/ReflectionProbe.h"	/* .probe 详情的烘焙结果预览（BuildPreviewTexture） */
#include "Helios/Scene/TerrainAsset.h"
#include "Helios/Scene/AnimationClip.h"
#include "UI/ImGuiExtensions.h"
#include "Helios/Reflection/ComponentRegistry.h"
#include "Command/ComponentFieldCommand.h"
#include "Command/AddComponentCommand.h"
#include "Command/RemoveComponentCommand.h"
#include "Command/SetComponentOrderCommand.h"
#include "UI/EditorTheme.h"
#include "Helios/Scene/Components.h"
#include "Helios/Renderer/PostProcess/Effects/Bloom.h"
#include "Helios/Renderer/PostProcess/Effects/Fog.h"
#include "Helios/Renderer/PostProcess/Effects/Taa.h"
#include "Helios/Renderer/ShaderCompiler.h"
#include <tinyxml2.h>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <limits>
#include <unordered_map>
#include <vector>

/* 属性面板：选中实体的组件卡、资源详情与材质卡 */
namespace Helios
{
	using SceneHierarchyShared::ResolveEntityIcon;
	using SceneHierarchyShared::DrawVisibilityToggle;
	using SceneHierarchyShared::SetEntityVisibility;

	namespace
	{
		/* 卡内分区隔离线（画在材质卡那一段，详情卡与组件卡尾也要用） */
		void DrawCardSectionDivider();

		/* 只有 .glsl 源码谈得上 include 依赖；.metal 是编译产物，里面引用的 <metal_stdlib> 不是我们的 */
		bool IsGlslSource(const std::filesystem::path& path)
		{
			return LowercaseExtension(path) == ".glsl";
		}

		class ComponentCustomDrawCommand final : public ICommand
		{
		public:
			ComponentCustomDrawCommand(Entity entity, HasFunc has, ComponentRestoreFunc restore,
				ComponentSnapshot before, ComponentSnapshot after, const char* component_name)
				: m_Entity(entity), m_Has(has), m_Restore(restore),
				  m_Before(std::move(before)), m_After(std::move(after)),
				  m_Label(std::string("Edit ") + (component_name != nullptr ? component_name : "Component"))
			{
			}

			void Do() override { Apply(m_After); }
			void Undo() override { Apply(m_Before); }
			[[nodiscard]] const char* GetLabel() const override { return m_Label.c_str(); }

			/* 同一实体同一组件的连续改动可合并：拖动材质参数时每帧产生一条快照命令，
			 * 由 CommandStack 的事务窗口（Begin/EndTransaction）圈进来，只保留最初 before 与最新 after。 */
			bool TryMerge(const ICommand& next) override
			{
				const auto* next_command = dynamic_cast<const ComponentCustomDrawCommand*>(&next);
				if (next_command == nullptr)
					return false;
				if (next_command->m_Entity != m_Entity || next_command->m_Restore != m_Restore)
					return false;

				m_After = next_command->m_After;
				return true;
			}

		private:
			void Apply(const ComponentSnapshot& snapshot)
			{
				if (m_Has != nullptr && m_Has(m_Entity) && m_Restore != nullptr && snapshot.IsValid())
					m_Restore(m_Entity, snapshot);
			}

			Entity m_Entity;
			HasFunc m_Has{ nullptr };
			ComponentRestoreFunc m_Restore{ nullptr };
			ComponentSnapshot m_Before;
			ComponentSnapshot m_After;
			std::string m_Label;
		};

		/* ---- 组件卡头部图标 ----
		 * 与实体图标同一套道理：图标是编辑器的表现层概念，注册表在 Kernel、不认识 Icons，
		 * 所以映射留在这里。按类型判定（不是按名字字符串），改显示名不影响图标。 */
		Icons::Id ResolveComponentIcon(const ComponentDesc& desc, const void* component)
		{
			const std::type_index& type = desc.Type;

			if (type == typeid(TransformComponent))       return Icons::Id::Transform;
			if (type == typeid(NameComponent))            return Icons::Id::Tag;
			if (type == typeid(LayerComponent))           return Icons::Id::LayerMask;
			if (type == typeid(CameraComponent))          return Icons::Id::Camera;
			if (type == typeid(ModelComponent))           return Icons::Id::Model;
			if (type == typeid(SpriteComponent))          return Icons::Id::Sprite;
			if (type == typeid(ReflectionProbeComponent)) return Icons::Id::ReflectionProbe;
			if (type == typeid(PostProcessComponent))     return Icons::Id::PostProcess;
			if (type == typeid(BloomComponent))           return Icons::Id::Bloom;
			if (type == typeid(FogComponent))             return Icons::Id::Fog;
			if (type == typeid(SkyComponent))             return Icons::Id::Sky;
			if (type == typeid(TaaComponent))             return Icons::Id::Taa;
			if (type == typeid(GrassComponent))           return Icons::Id::Grass;
			if (type == typeid(GrassBendComponent))       return Icons::Id::GrassBend;
			if (type == typeid(TerrainComponent))         return Icons::Id::Terrain;
			if (type == typeid(AnimatorComponent))        return Icons::Id::Animation;
			if (type == typeid(ScriptComponent))          return Icons::Id::ScriptComponent;

			/* 光源是一个组件块但造型分三种：图标跟着「当前光源类型」走，
			 * 这样把 Type 从点光改成聚光时，卡头图标会立刻跟着变。
			 * 默认构造的 LightComponent 没有 Light 对象，这时退化看 m_Type。 */
			if (type == typeid(LightComponent) && component != nullptr)
			{
				const auto* light_component = static_cast<const LightComponent*>(component);
				const LightType light_type = (light_component->m_Light != nullptr)
					? light_component->m_Light->GetLightType()
					: light_component->m_Type;

				switch (light_type)
				{
				case LightType::Directional: return Icons::Id::LightDirectional;
				case LightType::Spot:        return Icons::Id::LightSpot;
				default:                     return Icons::Id::LightPoint;
				}
			}

			return Icons::Id::None;
		}

		/* 「添加组件」菜单条目的图标：光源的三种形态按注册的 Variant 名判断（lambda 里拿不到
		 * 类型信息）—— 改名的时候这里要跟着改；其余组件按类型判断（跟组件卡同一套）。 */
		Icons::Id ResolveComponentMenuIcon(const ComponentDesc& desc, const ComponentVariant& variant)
		{
			if (desc.Type == typeid(LightComponent) && variant.MenuName != nullptr)
			{
				if (std::strcmp(variant.MenuName, "Directional Light") == 0) return Icons::Id::LightDirectional;
				if (std::strcmp(variant.MenuName, "Spot Light") == 0)        return Icons::Id::LightSpot;
				if (std::strcmp(variant.MenuName, "Point Light") == 0)       return Icons::Id::LightPoint;
			}

			return ResolveComponentIcon(desc, nullptr);
		}

		/* ---- 面板头部就地编辑的组件 ----
		 * Name（头部的实体名）和 Visibility（标题栏的眼睛开关）不单独成卡，就地放在属性面板顶部。
		 * 判定收口在这里：组件列表和头部两边共用。 */
		bool IsEditedInPanelHeader(const ComponentDesc& desc)
		{
			return desc.Type == typeid(NameComponent) || desc.Type == typeid(VisibilityComponent);
		}

		/* 把 std::string 喂给 InputText：缓冲区在栈上，改完写回；
		 * 返回是否被改动（调用方通常不关心，改动检测由 DrawEditableField 统一做）。 */
		bool InputTextString(const char* id, std::string& value, ImGuiInputTextFlags flags = 0)
		{
			char buffer[256] = {};
			std::strncpy(buffer, value.c_str(), sizeof(buffer) - 1);

			if (!ImGui::InputText(id, buffer, sizeof(buffer), flags))
				return false;

			value = buffer;
			return true;
		}

		/* 面板头部的实体名：无边框、标题字体 —— 看上去就是一行标题，但可以直接改。
		 * 悬停给一点底色、聚焦给控件底色，让「这里能编辑」看得出来。 */
		void DrawHeaderNameControl(const FieldDesc& field, void* field_ptr)
		{
			IM_UNUSED(field);

			const ImGuiStyle& style = ImGui::GetStyle();
			auto& name = *static_cast<std::string*>(field_ptr);

			/* 右侧给「添加组件」按钮让出位置，输入框才不会压到按钮上 */
			const float width = ImGui::GetContentRegionAvail().x
				- ImGui::GetFrameHeight() - style.ItemInnerSpacing.x;

			ImGui::PushFont(EditorTheme::GetFonts().Title);
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x * 0.5f, 2.0f));
			ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
			ImGui::PushStyleColor(ImGuiCol_FrameBg, EditorTheme::Token::Clear);
			ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,
				EditorTheme::WithAlpha(EditorTheme::Token::Neutral5, 0.70f));
			ImGui::PushStyleColor(ImGuiCol_FrameBgActive, EditorTheme::Token::Neutral4);

			ImGui::SetNextItemWidth(ImMax(width, Scaled(40.0f)));
			InputTextString("##EntityName", name);

			ImGui::PopStyleColor(3);
			ImGui::PopStyleVar(2);
			ImGui::PopFont();
		}
	}

	SceneInspector::SceneInspector()
	{
		PROFILE_FUNCTION();

		/* 组件卡的自定义编辑（资源引用、材质槽…）由编辑器侧装配进注册表 */
		InstallComponentEditors();
	}

	void SceneInspector::SetOwnerScene(const SharedPtr<Scene>& scene)
	{
		/* 旧场景的相机不能再被当成预览目标 */
		m_pPreviewCamera = nullptr;

		m_pOwnerScene = scene;
		m_Entity = {};
		m_TerrainPanel = {};

		/* 场景已更换，字段编辑的合并窗口不再有效 */
		m_FieldEditTransactionOpen = false;
	}

	void SceneInspector::SetAssetSelection(const std::vector<AssetSelectionEntry>& selection)
	{
		m_AssetSelection = selection;

		/* 非空 = 刚点了资源（含同一项被再次点选）：切到资源详情；
		 * 空（切目录 / Esc / 选中项被删）则交回实体视图。 */
		m_AssetFocus = !m_AssetSelection.empty();
	}

	void SceneInspector::OnImGuiRender(const Entity& entity)
	{
		PROFILE_FUNCTION();

		m_Entity = entity;

		/* 相机预览逐帧挣取：本帧真的画出了预览卡（可见且展开）才置位，
		 * 换选中、切资源视图、收起卡片都会让预览自然停掉（见 DrawCameraPreviewCard） */
		m_pPreviewCamera = nullptr;

		/* 常驻竖向滚动条（AlwaysVerticalScrollbar）：滚动条出现会抽走右侧约 10px，内容若按
		 * "还没滚动条"的旧宽度排过一帧（换选中、展开卡片、拖分栏都会触发），
		 * 面板就能横向滑一下。常驻 = 宽度恒定预留，属性面板永远不能左右滑。
		 * 被别的页签挡住或折叠时不画，相机预览也随之停掉。 */
		if (PanelWindow window(Panel::kProperties, nullptr, ImGuiWindowFlags_AlwaysVerticalScrollbar); window)
		{
			if (m_AssetFocus && !m_AssetSelection.empty())
			{
				ShowAssetProperties();
			}
			else if (m_Entity)
			{
				ShowPropertiesHeader();
				ShowEntityComponents();
			}
			else
			{
				ShowEmptyPropertiesHint();
			}
		}
	}

	/* 面板头部：可见性开关（眼睛）+ 实体图标 + 实体名（就地可编辑）+ 右侧「添加组件」。
	 * 头部自带上下文 —— 不必回头看层级树才知道正在编辑哪个实体；
	 * 实体名与可见性也不再单独占卡（同一条信息只说一次，见 IsEditedInPanelHeader）。 */
	void SceneInspector::ShowPropertiesHeader()
	{
		PROFILE_FUNCTION();

		/* 行首给可见性开关留一格（控件高见方）：可见性是这个实体最直接的状态，
		 * 直接画在标题栏左边 —— 与层级行右端是同一枚眼睛、同一套交互。 */
		const float eye_size = ImGui::GetFrameHeight();
		const PanelChrome::HeaderRow header = PanelChrome::BeginHeaderRow(
			ResolveEntityIcon(m_Entity), eye_size + ImGui::GetStyle().ItemInnerSpacing.x);

		/* 实体名：就地编辑，与组件字段走同一条命令路径（可撤销）。
		 * 头部行里"由字段驱动"的只有 Name 一个 —— 取它的第一个文本字段就地绘制；
		 * Visibility 虽同在头部呈现，但画的是开关不是字段（见 IsEditedInPanelHeader）。 */
		for (const ComponentDesc& desc : ComponentRegistry::Instance().All())
		{
			if (!IsEditedInPanelHeader(desc) || desc.GetPtr == nullptr)
				continue;

			/* GetPtr 对没有该组件的实体是断言而不是返回空指针，必须先 Has 再取。 */
			if (desc.Has == nullptr || !desc.Has(m_Entity))
				break;

			void* component = desc.GetPtr(m_Entity);
			if (component == nullptr)
				break;

			for (const FieldDesc& field : desc.Fields)
			{
				if (field.Type != FieldType::String)
					continue;

				DrawEditableField(desc, m_Entity, component, field, &DrawHeaderNameControl);
				break;
			}

			break;
		}

		/* 添加组件入口贴右端，与头部同一行 */
		const float button_size = ImGui::GetFrameHeight();
		PanelChrome::PlaceHeaderAction(header, button_size);
		ShowAddComponentButton();

		/* 可见性开关放行首的空档里（名字与「添加组件」都提交完了，这里补那把空档填上；
		 * 图标/名字的起点在 BeginHeaderRow 里已按这一格让过位）。眼睛画自身状态：
		 * 点它只写自己那一行 —— 与层级行右端共用同一份按钮实现。 */
		const bool self_hidden = m_Entity.HasComponent<VisibilityComponent>()
			&& !m_Entity.GetComponent<VisibilityComponent>().m_Visible;
		if (DrawVisibilityToggle("##HeaderVisibility", header.Min,
			ImVec2(header.Min.x + eye_size, header.Min.y + eye_size), self_hidden, /*reveal_icon=*/true))
		{
			SetEntityVisibility(m_pOwnerScene, m_pCommandStack, m_Entity, self_hidden);
		}

		PanelChrome::EndHeaderRow(header);
	}

	/* 没有选中实体时的提示：空面板要说清"为什么是空的、该怎么办" */
	void SceneInspector::ShowEmptyPropertiesHint()
	{
		PROFILE_FUNCTION();

		static constexpr const char* kHint = "Select an entity to edit its components, or an asset to inspect it";

		const float wrap_width = ImGui::GetContentRegionAvail().x;
		const float text_width = ImGui::CalcTextSize(kHint).x;

		/* 垂直方向约三分之一处：视线自然会落到这里，不像顶部一行那么容易被忽略 */
		ImGui::Dummy(ImVec2(0.0f, ImGui::GetContentRegionAvail().y * 0.3f));

		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap_width);
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImMax(0.0f, (wrap_width - text_width) * 0.5f));
		ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::TextDim);
		ImGui::TextUnformatted(kHint);
		ImGui::PopStyleColor();
		ImGui::PopTextWrapPos();
	}

	/* ==================== 资源详情（属性面板） ====================
	 * 资源浏览器选中资源时显示它的详细内容。最近一次选择说了算：
	 * 点实体即切回组件视图（见 ClearAssetFocus）；这里的绘制全部只读。 */

	namespace
	{
		/* 预览图的尺寸上限：宽度吃满值列，超高 / 超宽的图按比例缩进来 */
		inline float kAssetPreviewMaxWidth() { return Scaled(220.0f); }
		inline float kAssetPreviewMaxHeight() { return Scaled(240.0f); }

		/* 高度图缩略图：灰度 + 一点明暗，只为认出形状。资产详情与地形材质卡共用 */
		SharedPtr<DeviceTexture> BuildHeightPreviewTexture(const std::string& name,
			const std::vector<float>& heights, uint32_t resolution)
		{
			if (Renderer::GetRenderAPI() == nullptr || heights.size() != static_cast<size_t>(resolution) * resolution)
				return nullptr;
			const auto sample = [&heights, resolution](uint32_t x, uint32_t z)
			{
				const uint32_t sx = std::min(x, resolution - 1);
				const uint32_t sz = std::min(z, resolution - 1);
				return heights[static_cast<size_t>(sz) * resolution + sx];
			};
			constexpr uint32_t kSide = 256;

			std::vector<uint8_t> pixels(static_cast<size_t>(kSide) * kSide * 4);
			for (uint32_t y = 0; y < kSide; ++y)
			{
				for (uint32_t x = 0; x < kSide; ++x)
				{
					const uint32_t hx = x * resolution / kSide;
					const uint32_t hz = y * resolution / kSide;
					const float height = sample(hx, hz);
					const float left = sample(hx > 0 ? hx - 1 : 0, hz);
					const float up = sample(hx, hz > 0 ? hz - 1 : 0);
					const float shade = glm::clamp(0.72f + (height - left) * 8.0f + (height - up) * 8.0f, 0.35f, 1.25f);
					const float value = glm::clamp((0.25f + height * 0.65f) * shade, 0.0f, 1.0f);

					const uint8_t gray = static_cast<uint8_t>(value * 255.0f);
					const size_t index = (static_cast<size_t>(y) * kSide + x) * 4;
					pixels[index + 0] = gray;
					pixels[index + 1] = gray;
					pixels[index + 2] = gray;
					pixels[index + 3] = 255;
				}
			}

			TextureDesc desc;
			desc.Width = kSide;
			desc.Height = kSide;
			desc.MipLevels = 1;
			desc.Format = TextureFormat::RGBA8;
			desc.SamplerType = SamplerType::Sampler2D;
			desc.Usage = TextureUsage::Sampleable;

			const SharedPtr<DeviceTexture> texture = DeviceTexture::Create(name, desc);
			if (texture == nullptr)
				return nullptr;

			PixelDesc pixel_desc;
			pixel_desc.Format = PixelFormat::RGBA;
			pixel_desc.Type = PixelType::UnsignedByte;
			texture->SetData(pixels.data(), pixel_desc);
			return texture;
		}

		SharedPtr<DeviceTexture> BuildTerrainPreviewTexture(const std::string& name, const TerrainAsset::Data& data)
		{
			if (Renderer::GetRenderAPI() == nullptr || !data.IsValid())
				return nullptr;

			return BuildHeightPreviewTexture(name, data.Heights, data.Resolution);
		}

		/* 材质里的数值参数（层颜色 / 粗糙度）：读不到就用着色器默认值兜底 */
		bool TryReadMaterialParam(const SharedPtr<Material>& material, const char* name, MaterialParamInfo& out)
		{
			if (material == nullptr)
				return false;

			for (const auto& [key, info] : material->GetAllParameters())
			{
				(void)key;
				if (info.Name == name)
				{
					out = info;
					return true;
				}
			}
			return false;
		}

		glm::vec4 ReadMaterialColor(const SharedPtr<Material>& material, const char* name, const glm::vec3& fallback)
		{
			MaterialParamInfo info;
			if (TryReadMaterialParam(material, name, info) && info.Type == ParamType::Vec3)
			{
				const glm::vec3 value = std::any_cast<glm::vec3>(info.Value);
				return glm::vec4(value, 1.0f);
			}
			return glm::vec4(fallback, 1.0f);
		}

		float ReadMaterialFloat(const SharedPtr<Material>& material, const char* name, float fallback)
		{
			MaterialParamInfo info;
			if (TryReadMaterialParam(material, name, info) && info.Type == ParamType::Float)
				return std::any_cast<float>(info.Value);
			return fallback;
		}

		/* 一行的颜色块 + 十六进制 */
		void DrawColorChipRow(const std::string& label, const glm::vec4& color)
		{
			ImGuiExt::BeginPropertyRow(label.c_str());
			const float height = ImGui::GetFrameHeight();
			ImGui::ColorButton("##Chip", ImVec4(color.r, color.g, color.b, 1.0f),
				ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker,
				ImVec2(height * 2.2f, height));
			ImGui::SameLine();

			char text[16];
			std::snprintf(text, sizeof(text), "#%02X%02X%02X",
				static_cast<int>(color.r * 255.0f + 0.5f),
				static_cast<int>(color.g * 255.0f + 0.5f),
				static_cast<int>(color.b * 255.0f + 0.5f));
			ImGui::TextUnformatted(text);
			ImGuiExt::EndPropertyRow();
		}

		/* 资源大类 -> 头部图标（与资源浏览器同一套图标语言；文件夹单独一档） */
		Icons::Id AssetIconOf(const AssetSelectionEntry& entry)
		{
			if (entry.IsFolder)
				return Icons::Id::Directory;

			switch (entry.Kind)
			{
			case AssetFileKind::Image:    return Icons::Id::FileImage;
			case AssetFileKind::Svg:      return Icons::Id::FileSvg;
			case AssetFileKind::Scene:    return Icons::Id::FileScene;
			case AssetFileKind::MtlGraph: return Icons::Id::FileMtlGraph;
			case AssetFileKind::Shader:   return Icons::Id::FileShader;
			case AssetFileKind::Model:    return Icons::Id::FileModel;
			case AssetFileKind::Material: return Icons::Id::FileMaterial;
			case AssetFileKind::Terrain:  return Icons::Id::FileTerrain;
			case AssetFileKind::Probe:    return Icons::Id::FileProbe;
			case AssetFileKind::Animation: return Icons::Id::AnimationClip;
			case AssetFileKind::CppHeader: return Icons::Id::FileCppHeader;
			case AssetFileKind::CppSource: return Icons::Id::FileCppSource;
			case AssetFileKind::Other:    return Icons::Id::File;
			}

			return Icons::Id::File;
		}

		/* 资源大类 -> 显示名（单数；资源浏览器的筛选下拉用的是复数，是另一套词表） */
		const char* AssetKindName(AssetFileKind kind, bool is_folder)
		{
			if (is_folder)
				return "Folder";

			switch (kind)
			{
			case AssetFileKind::Other:    return "File";
			case AssetFileKind::Image:    return "Image";
			case AssetFileKind::Svg:      return "SVG";
			case AssetFileKind::Scene:    return "Scene";
			case AssetFileKind::MtlGraph: return "Material Graph";
			case AssetFileKind::Shader:   return "Shader";
			case AssetFileKind::Model:    return "Model";
			case AssetFileKind::Material: return "Material";
			case AssetFileKind::Terrain:  return "Terrain";
			case AssetFileKind::Probe:    return "Reflection Probe";
			case AssetFileKind::Animation: return "Animation Clip";
			case AssetFileKind::CppHeader: return "C++ Header";
			case AssetFileKind::CppSource: return "C++ Source";
			}

			return "File";
		}

		/* 烘焙缓存里的一张图 -> 显示名（与探针卡的详情行同口径） */
		const char* BakeImageKindName(ReflectionProbeBakeCache::ImageKind kind)
		{
			switch (kind)
			{
			case ReflectionProbeBakeCache::ImageKind::Environment: return "Environment";
			case ReflectionProbeBakeCache::ImageKind::Irradiance:  return "Irradiance";
			case ReflectionProbeBakeCache::ImageKind::Prefilter:   return "Prefilter";
			}

			return "Image";
		}

	}

	void SceneInspector::ShowAssetProperties()
	{
		PROFILE_FUNCTION();

		/* 头部：资源图标 + 名字（只读 —— 改名走资源浏览器的右键菜单，资源名不是实体名） */
		const AssetSelectionEntry& first = m_AssetSelection.front();
		const PanelChrome::HeaderRow header = PanelChrome::BeginHeaderRow(AssetIconOf(first));
		PanelChrome::DrawHeaderTitle(header, first.Name, header.Right - header.TitleX);
		PanelChrome::EndHeaderRow(header);

		if (m_AssetSelection.size() == 1)
			DrawAssetDetailsCard(first);
		else
			DrawMultiAssetCard();
	}

	void SceneInspector::DrawTextureSettingsSection(const std::string& absolute_path)
	{
		TextureSettingsStore& store = TextureSettingsStore::Instance();

		/* 换贴图时重新取草稿；没改过就跟随磁盘，外部改动也能同步进来 */
		if (m_TextureSettingsPath != absolute_path)
		{
			m_TextureSettingsPath = absolute_path;
			m_TextureSettingsSaved = store.Load(absolute_path);
			m_TextureSettingsEdit = m_TextureSettingsSaved;
			m_TextureSettingsDirty = false;
		}
		else if (!m_TextureSettingsDirty)
		{
			const TextureSettings disk = store.Load(absolute_path);
			if (disk != m_TextureSettingsSaved)
			{
				m_TextureSettingsSaved = disk;
				m_TextureSettingsEdit = disk;
			}
		}

		TextureSettings& settings = m_TextureSettingsEdit;
		bool touched = false;

		DrawCardSectionDivider();
		ImGui::TextDisabled("Texture Settings");

		static const std::vector<std::string> usage_options{ "Default", "Color", "Data", "Normal", "UI", "HDR" };
		int usage = settings.Usage.has_value() ? static_cast<int>(*settings.Usage) : 0;
		ImGuiExt::DrawComboUI("Usage", usage_options, usage, [&](int picked)
		{
			if (picked == 0)
				settings.Usage.reset();
			else
				settings.Usage = static_cast<TexturePurpose>(picked);
			touched = true;
		});

		static const std::vector<std::string> srgb_options{ "Auto", "Linear", "sRGB" };
		int srgb = !settings.Srgb.has_value() ? 0 : (*settings.Srgb ? 2 : 1);
		ImGuiExt::DrawComboUI("Color Space", srgb_options, srgb, [&](int picked)
		{
			if (picked == 0)
				settings.Srgb.reset();
			else
				settings.Srgb = picked == 2;
			touched = true;
		});

		static const std::vector<std::string> wrap_options{ "Inherit", "ClampToEdge", "Repeat", "MirroredRepeat" };
		int wrap = settings.WrapMode.has_value() ? static_cast<int>(*settings.WrapMode) + 1 : 0;
		ImGuiExt::DrawComboUI("Wrap", wrap_options, wrap, [&](int picked)
		{
			if (picked == 0)
				settings.WrapMode.reset();
			else
				settings.WrapMode = static_cast<SamplerWrapMode>(picked - 1);
			touched = true;
		});

		static const std::vector<std::string> min_filter_options{
			"Inherit", "Nearest", "Linear", "NearestMipmapNearest",
			"LinearMipmapNearest", "NearestMipmapLinear", "LinearMipmapLinear"
		};
		int min_filter = settings.MinFilter.has_value() ? static_cast<int>(*settings.MinFilter) + 1 : 0;
		ImGuiExt::DrawComboUI("Min Filter", min_filter_options, min_filter, [&](int picked)
		{
			if (picked == 0)
				settings.MinFilter.reset();
			else
				settings.MinFilter = static_cast<SamplerMinFilter>(picked - 1);
			touched = true;
		});

		static const std::vector<std::string> mag_filter_options{ "Inherit", "Nearest", "Linear" };
		int mag_filter = settings.MagFilter.has_value() ? static_cast<int>(*settings.MagFilter) + 1 : 0;
		ImGuiExt::DrawComboUI("Mag Filter", mag_filter_options, mag_filter, [&](int picked)
		{
			if (picked == 0)
				settings.MagFilter.reset();
			else
				settings.MagFilter = static_cast<SamplerMagFilter>(picked - 1);
			touched = true;
		});

		static const uint8_t kAnisoValues[] = { 1, 2, 4, 8, 16 };
		static const std::vector<std::string> aniso_options{ "Inherit", "1", "2", "4", "8", "16" };
		int aniso = 0;
		if (settings.AnisoLevel.has_value())
		{
			for (int index = 0; index < 5; ++index)
			{
				if (kAnisoValues[index] == *settings.AnisoLevel)
				{
					aniso = index + 1;
					break;
				}
			}
		}
		ImGuiExt::DrawComboUI("Aniso", aniso_options, aniso, [&](int picked)
		{
			if (picked == 0)
				settings.AnisoLevel.reset();
			else
				settings.AnisoLevel = kAnisoValues[picked - 1];
			touched = true;
		});

		static const std::vector<std::string> mip_options{ "Inherit", "On", "Off" };
		int mipmaps = settings.Mipmaps.has_value() ? (*settings.Mipmaps ? 1 : 2) : 0;
		ImGuiExt::DrawComboUI("Mipmaps", mip_options, mipmaps, [&](int picked)
		{
			if (picked == 0)
				settings.Mipmaps.reset();
			else
				settings.Mipmaps = picked == 1;
			touched = true;
		});

		if (ImGui::CollapsingHeader("Advanced"))
		{
			/* 数据布局没有上层来源可继承，直接给实值（默认即不翻转、二维） */
			static const std::vector<std::string> flip_options{ "On", "Off" };
			int flip = settings.FlipV.value_or(true) ? 0 : 1;
			ImGuiExt::DrawComboUI("Flip V", flip_options, flip, [&](int picked)
			{
				settings.FlipV = picked == 0;
				touched = true;
			});

			static const std::vector<std::string> type_options{ "2D", "2DArray", "CubeMap", "3D" };
			int type = settings.SamplerType.has_value() ? static_cast<int>(*settings.SamplerType) : 0;
			ImGuiExt::DrawComboUI("Sampler Type", type_options, type, [&](int picked)
			{
				settings.SamplerType = static_cast<SamplerType>(picked);
				touched = true;
			});
		}

		if (touched)
			m_TextureSettingsDirty = settings != m_TextureSettingsSaved;

		ImGui::Spacing();
		ImGui::BeginDisabled(!m_TextureSettingsDirty);
		if (ImGui::Button("Apply##TextureSettings"))
		{
			if (store.Save(absolute_path, settings))
			{
				AssetManager::Instance().ReloadTexture(absolute_path);
				m_TextureSettingsSaved = settings;
				m_TextureSettingsDirty = false;
			}
		}
		ImGui::EndDisabled();

		if (m_TextureSettingsDirty)
		{
			ImGui::SameLine();
			ImGui::TextDisabled("Unsaved changes");
		}
	}

	void SceneInspector::DrawAssetDetailsCard(const AssetSelectionEntry& entry)
	{
		PROFILE_FUNCTION();

		const std::filesystem::path absolute_path = ResolveAssetPath(PathFromUtf8(entry.Path));
		const PanelChrome::Card card = PanelChrome::BeginCard(
			AssetKindName(entry.Kind, entry.IsFolder), AssetIconOf(entry));

		ImGuiExt::DrawCommonTextUI("Path", entry.Path);
		if (entry.IsFolder)
			ImGuiExt::DrawCommonTextUI("Items", std::to_string(entry.ChildCount));
		else
			ImGuiExt::DrawCommonTextUI("Size", FormatFileSize(entry.SizeBytes));
		ImGuiExt::DrawCommonTextUI("Modified", FormatFileWriteTime(absolute_path));

		/* 类型相关的深挖行（图片尺寸 / 场景统计）：按 (路径, 写入时间) 缓存 */
		RefreshAssetDetailRows(entry);
		for (const auto& [label, value] : m_AssetDetailRows)
			ImGuiExt::DrawCommonTextUI(label, value);

		/* 图片给一张更大的预览：与资源浏览器的缩略图共用 AssetManager 的缓存
		 * （同一张贴图不会加载两份；headless 下没有图形上下文时贴图为空，跳过即可）。 */
		if (!entry.IsFolder && entry.Kind == AssetFileKind::Image)
		{
			/* 动图（GIF）：按当前时间画那一帧（循环播放，时间与浏览器同一个 UI 时钟）；
			 * 没图集（headless / 太大）回落下面的静态首帧 */
			const SharedPtr<const Gif::Clip> gif_clip = Gif::GetClip(absolute_path);
			if (gif_clip != nullptr && gif_clip->Texture != nullptr)
			{
				ImGui::Spacing();

				const float max_width = ImMin(ImGui::GetContentRegionAvail().x, kAssetPreviewMaxWidth());
				const float aspect = gif_clip->FrameHeight / ImMax(gif_clip->FrameWidth, 1.0f);

				float width = max_width;
				float height = width * aspect;
				if (height > kAssetPreviewMaxHeight())
				{
					height = kAssetPreviewMaxHeight();
					width = height / ImMax(aspect, 1.0e-6f);
				}

				ImVec2 uv_min, uv_max;
				Gif::FrameUv(*gif_clip, Gif::FrameAt(*gif_clip, static_cast<float>(ImGui::GetTime())),
					uv_min, uv_max);
				ImGui::Image((ImTextureID)gif_clip->Texture.get(), ImVec2(width, height), uv_min, uv_max);
			}
			else
			{
				const SharedPtr<DeviceTexture> preview = AssetManager::Instance().GetOrLoad<DeviceTexture>(
					PathToUtf8(absolute_path));
				if (preview != nullptr && preview->IsLoaded())
				{
					ImGui::Spacing();

					const float max_width = ImMin(ImGui::GetContentRegionAvail().x, kAssetPreviewMaxWidth());
					const float aspect = static_cast<float>(preview->GetHeight())
						/ static_cast<float>(ImMax(preview->GetWidth(), 1u));

					float width = max_width;
					float height = width * aspect;
					if (height > kAssetPreviewMaxHeight())
					{
						height = kAssetPreviewMaxHeight();
						width = height / ImMax(aspect, 1.0e-6f);
					}

					ImGui::Image((ImTextureID)preview.get(), ImVec2(width, height),
						ImVec2(0, 1), ImVec2(1, 0));
				}
			}
		}
		/* SVG：按原色画形状（不走纹理），比例照 viewBox 装进预览框 */
		else if (entry.Kind == AssetFileKind::Svg)
		{
			const SharedPtr<const Svg::Image> preview = Svg::GetImage(absolute_path);
			if (preview != nullptr)
			{
				ImGui::Spacing();

				const float max_width = ImMin(ImGui::GetContentRegionAvail().x, kAssetPreviewMaxWidth());
				const float aspect = preview->ViewBoxSize.y / ImMax(preview->ViewBoxSize.x, 1.0f);

				float width = max_width;
				float height = width * aspect;
				if (height > kAssetPreviewMaxHeight())
				{
					height = kAssetPreviewMaxHeight();
					width = height / ImMax(aspect, 1.0e-6f);
				}

				const ImVec2 preview_min = ImGui::GetCursorScreenPos();
				ImGui::Dummy(ImVec2(width, height));
				Svg::DrawImage(ImGui::GetWindowDrawList(), *preview, preview_min,
					ImVec2(preview_min.x + width, preview_min.y + height));
			}
		}

		/* 地形资产：高度图缩略图 + 四层混色 */
		else if (entry.Kind == AssetFileKind::Terrain)
		{
			if (m_AssetTerrain.HeightTexture != nullptr)
			{
				ImGui::Spacing();
				const float side = ImMin(ImGui::GetContentRegionAvail().x, kAssetPreviewMaxWidth());
				ImGui::Image((ImTextureID)m_AssetTerrain.HeightTexture.get(), ImVec2(side, side),
					ImVec2(0, 1), ImVec2(1, 0));
			}

			if (!m_AssetTerrain.LayerColors.empty())
			{
				DrawCardSectionDivider();
				for (const auto& [label, color] : m_AssetTerrain.LayerColors)
					DrawColorChipRow(label, color);
			}
		}

		/* 图片的采样与格式：GIF 走自己的图集，不在这里配 */
		if (!entry.IsFolder && entry.Kind == AssetFileKind::Image)
		{
			if (LowercaseExtension(absolute_path) != ".gif")
				DrawTextureSettingsSection(PathToUtf8(absolute_path));
		}

		/* 模型文件（.mesh / .obj / .fbx 等）：详情卡末尾直接给模型编辑器入口
		 * （交换格式由编辑器里那条导入链先转成 .mesh 再开） */
		if (!entry.IsFolder && entry.Kind == AssetFileKind::Model)
		{
			DrawCardSectionDivider();
			DrawOpenInModelEditorButton(PathToUtf8(absolute_path));
		}

		/* shader 的 include 依赖：卡尾列出被引用的文件（项目里的那条点了就跳到资源浏览器） */
		if (!entry.IsFolder && entry.Kind == AssetFileKind::Shader && !m_AssetShaderIncludes.empty())
		{
			DrawCardSectionDivider();
			ImGuiExt::DrawCommonTextUI("Includes", std::to_string(m_AssetShaderIncludes.size()));
			for (const ShaderIncludeRow& include : m_AssetShaderIncludes)
				DrawShaderIncludeRow(include);
		}

		PanelChrome::EndCard(card);

		/* .probe：三张烘焙图各一张卡片 —— 同级平铺在详情卡之后（卡片通道不可嵌套，
		 * "卡中卡"画不出来，与材质卡排在组件卡之后是同一条路数）。
		 * 图是十字展开预览；无图形上下文（headless）时只有规格行，图跳过。 */
		for (size_t index = 0; index < m_AssetProbeCards.size(); ++index)
		{
			ProbeImageCard& probe_image = m_AssetProbeCards[index];

			/* 卡片键按索引：三张各一卡、折叠状态互不影响 */
			ImGui::PushID(static_cast<int>(index));

			const PanelChrome::Card image_card = PanelChrome::BeginCard(
				probe_image.Label.c_str(), Icons::Id::FileProbe);

			if (image_card.Open)
			{
				ImGuiExt::DrawCommonTextUI("Texture", probe_image.Spec);

				/* 多层 mip 才有的切换行：切哪层就看哪层的十字展开 —— 高层看整体结构、
				 * 低层（模糊）看细节（预滤波的高层就是更粗糙的那档）。
				 * 选项 = 层号 + 该层面边长；初始选中 = 自动挑的那层。 */
				if (probe_image.MipCount > 1)
				{
					std::vector<std::string> mip_options;
					mip_options.reserve(static_cast<size_t>(probe_image.MipCount));
					for (int level = 0; level < probe_image.MipCount; ++level)
					{
						mip_options.push_back(std::to_string(level) + " ("
							+ std::to_string(ReflectionProbe::PreviewMipFaceSize(
								probe_image.Mip0Size, static_cast<uint32_t>(level))) + ")");
					}

					int mip = probe_image.Mip;
					bool mip_changed = false;
					ImGuiExt::DrawComboUI("Mip", mip_options, mip,
						[&mip_changed](int) { mip_changed = true; });
					if (mip_changed)
						ApplyProbeCardMip(index, mip);
				}

				if (probe_image.Texture != nullptr)
				{
					ImGui::Spacing();

					/* 4:3 的十字图；宽度跟"图片预览"同一档上限，居中 */
					const float available = ImGui::GetContentRegionAvail().x;
					const float image_width = ImMin(available, kAssetPreviewMaxWidth());
					const float image_height = image_width * 0.75f;

					ImGui::SetCursorPosX(ImGui::GetCursorPosX()
						+ ImMax((available - image_width) * 0.5f, 0.0f));

					/* UV 不翻转：预览图第 0 行（+Y 面）就在顶部 —— 与图片贴图的
					 * (0,1)-(1,0) 相反，那些贴图在上传前被垂直翻转（IsFlipV） */
					ImGui::Image((ImTextureID)probe_image.Texture.get(),
						ImVec2(image_width, image_height), ImVec2(0, 0), ImVec2(1, 1));

					if (ImGui::IsItemHovered())
					{
						ImGui::BeginTooltip();
						ImGui::Image((ImTextureID)probe_image.Texture.get(),
							ImVec2(Scaled(320.0f), Scaled(240.0f)), ImVec2(0, 0), ImVec2(1, 1));
						ImGui::TextUnformatted(probe_image.Label.c_str());
						ImGui::EndTooltip();
					}
				}
			}

			PanelChrome::EndCard(image_card);
			ImGui::PopID();
		}

		/* .mtl：材质编辑卡（同 probe 图卡：同级平铺在详情卡之后）—— Shader 可选可改、
		 * 参数可就地编辑，「Apply」把编辑缓冲序列化回文件。 */
		DrawAssetMaterialCards(absolute_path);
	}

	/* 一条 include 依赖：整行可点（悬停有底色），图标与路径自己画在 Selectable 上（菜单行同一套；
	 * 自绘不进排版，超长路径由窗口裁掉、撑不出横滑）。项目里的那条点了在资源浏览器里定位。 */
	void SceneInspector::DrawShaderIncludeRow(const ShaderIncludeRow& include)
	{
		PROFILE_FUNCTION();

		const ImGuiStyle& style = ImGui::GetStyle();
		const float icon_size = ImGui::GetFontSize();

		ImGui::PushID(include.Relative.c_str());
		const bool clicked = ImGui::Selectable("##row", false,
			ImGuiSelectableFlags_SelectOnRelease | ImGuiSelectableFlags_SpanAvailWidth);
		ImGui::PopID();

		/* 图标与文字都画在 Selectable 之后：悬停底色已经落好，不会被盖住 */
		const ImVec2 min = ImGui::GetItemRectMin();
		const ImVec2 max = ImGui::GetItemRectMax();
		const float center_y = (min.y + max.y) * 0.5f;
		ImDrawList* const draw_list = ImGui::GetWindowDrawList();
		const ImU32 text_color = ImGui::GetColorU32(EditorTheme::Token::Text);

		Icons::Draw(draw_list, Icons::Id::FileShader,
			ImVec2(min.x + style.FramePadding.x + icon_size * 0.5f, center_y), icon_size, text_color);
		draw_list->AddText(ImVec2(min.x + style.FramePadding.x + icon_size + style.ItemInnerSpacing.x,
			center_y - ImGui::GetFontSize() * 0.5f), text_color, include.Relative.c_str());

		if (ImGui::IsItemHovered() && include.Revealable)
			ImGui::SetTooltip("Click to reveal in the resource browser");

		if (clicked && include.Revealable && m_AssetRevealFunc)
			m_AssetRevealFunc(include.Relative);
	}

	void SceneInspector::DrawMultiAssetCard()
	{
		PROFILE_FUNCTION();

		const PanelChrome::Card card = PanelChrome::BeginCard("Assets", Icons::Id::File);

		int kind_counts[9] = {};
		int folder_count = 0;
		uintmax_t total_bytes = 0;
		for (const AssetSelectionEntry& entry : m_AssetSelection)
		{
			total_bytes += entry.SizeBytes;
			if (entry.IsFolder)
				++folder_count;
			else
				++kind_counts[static_cast<size_t>(entry.Kind)];
		}

		ImGuiExt::DrawCommonTextUI("Items", std::to_string(m_AssetSelection.size()));
		ImGuiExt::DrawCommonTextUI("Total Size", FormatFileSize(total_bytes));

		/* 类型分布："2 Images, 1 Scene, 1 Folder"（按大类的枚举顺序，文件夹收尾） */
		std::string types;
		const auto append = [&types](int count, const char* name)
		{
			if (count <= 0)
				return;

			if (!types.empty())
				types += ", ";
			types += std::to_string(count) + " " + name + ((count > 1) ? "s" : "");
		};

		for (size_t kind = 0; kind < 9; ++kind)
			append(kind_counts[kind], AssetKindName(static_cast<AssetFileKind>(kind), false));
		append(folder_count, "Folder");

		ImGuiExt::DrawCommonTextUI("Types", types);

		PanelChrome::EndCard(card);
	}

	void SceneInspector::RefreshAssetDetailRows(const AssetSelectionEntry& entry)
	{
		PROFILE_FUNCTION();

		/* 深挖详情要打开文件（图片读文件头 / 场景解析 XML），不必每帧做：
		 * 只在选中项或它的写入时间变化时重算。 */
		const std::filesystem::path absolute_path = ResolveAssetPath(PathFromUtf8(entry.Path));

		std::error_code error;
		const std::filesystem::file_time_type write_time =
			std::filesystem::last_write_time(absolute_path, error);
		if (error)
		{
			m_AssetDetailPath.clear();
			m_AssetDetailRows.clear();
			m_AssetShaderIncludes.clear();
			m_AssetProbeCards.clear();
			m_AssetTerrain = TerrainAssetPreview{};
			m_AssetMaterial = nullptr;
			m_AssetMaterialDirty = false;
			return;
		}

		if (entry.Path == m_AssetDetailPath && write_time == m_AssetDetailWriteTime)
			return;

		m_AssetDetailPath = entry.Path;
		m_AssetDetailWriteTime = write_time;
		m_AssetDetailRows.clear();
		m_AssetShaderIncludes.clear();
		m_AssetProbeCards.clear();
		m_AssetTerrain = TerrainAssetPreview{};
		m_AssetMaterial = nullptr;
		m_AssetMaterialDirty = false;

		if (entry.Kind == AssetFileKind::Image)
		{
			int width = 0;
			int height = 0;
			int channels = 0;
			if (QueryImageInfo(PathToUtf8(absolute_path), width, height, channels))
			{
				m_AssetDetailRows.emplace_back("Dimensions",
					std::to_string(width) + " x " + std::to_string(height));
				m_AssetDetailRows.emplace_back("Channels", std::to_string(channels));
			}
		}
		else if (entry.Kind == AssetFileKind::Svg)
		{
			/* 矢量图：viewBox 说明坐标系（预览按它装框），形状数看看文件里有什么 */
			const SharedPtr<const Svg::Image> image = Svg::GetImage(absolute_path);
			if (image != nullptr)
			{
				char view_box[64];
				std::snprintf(view_box, sizeof(view_box), "%g %g %g %g",
					image->ViewBoxMin.x, image->ViewBoxMin.y, image->ViewBoxSize.x, image->ViewBoxSize.y);
				m_AssetDetailRows.emplace_back("ViewBox", view_box);
				m_AssetDetailRows.emplace_back("Shapes", std::to_string(image->Shapes.size()));
			}
		}
		else if (entry.Kind == AssetFileKind::Scene)
		{
			/* 场景统计：实体数 + 各类别的数量（本地解析一遍，结果缓存在成员里） */
			auto scene_file = std::unique_ptr<FILE, decltype(&std::fclose)>(
				OpenUtf8File(absolute_path, "rb"), &std::fclose);
			if (scene_file == nullptr)
				return;

			tinyxml2::XMLDocument doc;
			if (doc.LoadFile(scene_file.get()) != tinyxml2::XML_SUCCESS)
				return;

			const tinyxml2::XMLElement* scene_root = doc.FirstChildElement("Scene");
			const tinyxml2::XMLElement* entities_root = (scene_root != nullptr)
				? scene_root->FirstChildElement("Entities") : nullptr;
			if (entities_root == nullptr)
				return;

			int entity_count = 0;
			int light_count = 0;
			int camera_count = 0;
			int probe_count = 0;
			for (const tinyxml2::XMLElement* entity = entities_root->FirstChildElement("Entity");
				entity != nullptr; entity = entity->NextSiblingElement("Entity"))
			{
				++entity_count;
				if (entity->FirstChildElement("Light") != nullptr)
					++light_count;
				if (entity->FirstChildElement("Camera") != nullptr)
					++camera_count;
				if (entity->FirstChildElement("ReflectionProbe") != nullptr)
					++probe_count;
			}

			m_AssetDetailRows.emplace_back("Entities", std::to_string(entity_count));
			if (light_count > 0)
				m_AssetDetailRows.emplace_back("Lights", std::to_string(light_count));
			if (camera_count > 0)
				m_AssetDetailRows.emplace_back("Cameras", std::to_string(camera_count));
			if (probe_count > 0)
				m_AssetDetailRows.emplace_back("Probes", std::to_string(probe_count));
		}
		else if (entry.Kind == AssetFileKind::Animation)
		{
			/* 动画片段：直接解析文件拿时长 / 轨道数（不走资源缓存，选中即时可见） */
			AnimationClip clip;
			if (clip.Deserializer(PathToUtf8(absolute_path)))
			{
				char duration_text[32];
				std::snprintf(duration_text, sizeof(duration_text), "%.2f s", clip.GetDuration());
				m_AssetDetailRows.emplace_back("Duration", duration_text);
				m_AssetDetailRows.emplace_back("Tracks", std::to_string(clip.GetTracks().size()));
				m_AssetDetailRows.emplace_back("Loop", clip.IsLoop() ? "Yes" : "No");
			}
		}
		else if (entry.Kind == AssetFileKind::Probe)
		{
			/* 反射探针的烘焙缓存：每张立方图（环境 / 辐照度 / 预滤波）攒一张图卡
			 * （名字 + 规格 + 十字展开预览）—— 由调用方在详情卡之后同级平铺画出。
			 * 预览要上传纹理（无图形上下文时为空），规格行是纯 CPU 的，照常显示。 */
			ReflectionProbeBakeCache::Data data;
			if (!ReflectionProbeBakeCache::Read(PathToUtf8(absolute_path), data))
				return;	/* 损坏 / 版本不符：图卡留空（通用行仍显示大小与时间） */

			for (const ReflectionProbeBakeCache::Image& image : data.Images)
			{
				ProbeImageCard image_card;
				image_card.Label = BakeImageKindName(image.Kind);

				const char* format_name = GetEnumName(image.Format);
				image_card.Spec = std::to_string(image.Size) + " x " + std::to_string(image.Size)
					+ ", " + (format_name != nullptr ? format_name : "?")
					+ ", " + std::to_string(image.Mips.size())
					+ (image.Mips.size() > 1 ? " mips" : " mip");

				image_card.SourcePath = PathToUtf8(absolute_path);
				image_card.TextureName = PathToUtf8(absolute_path.filename()) + "_" + image_card.Label;
				image_card.ImageIndex = static_cast<int>(m_AssetProbeCards.size());
				image_card.MipCount = static_cast<int>(std::max<size_t>(image.Mips.size(), 1));
				image_card.Mip0Size = image.Size;
				/* 初始显示"自动挑的那层"（封面到 ~64px）—— 与探针卡里的预览同口径 */
				image_card.Mip = static_cast<int>(ReflectionProbe::AutoPreviewMip(image));
				image_card.Texture = ReflectionProbe::BuildPreviewTexture(
					image_card.TextureName, image, image_card.Mip);

				m_AssetProbeCards.push_back(std::move(image_card));
			}
		}
		else if (entry.Kind == AssetFileKind::Material)
		{
			/* 材质资产：整个文件读进编辑缓冲（单条材质） */
			auto material = CreateSharedPtr<Material>();
			if (MaterialIO::ReadFile(PathToUtf8(absolute_path), *material))
			{
				m_AssetMaterial = std::move(material);
				m_AssetDetailRows.emplace_back("Type", "Material Asset");
			}
			else
			{
				m_AssetDetailRows.emplace_back("Material", "unreadable");
			}

			m_AssetMaterialDirty = false;	/* 刚读上来的缓冲与文件一致（Apply 的禁用依据） */
		}
		else if (entry.Kind == AssetFileKind::Terrain)
		{
			/* 地形资产：高度图规格 + 高度范围 + 材质里的四层混色。 */
			TerrainAsset::Data data;
			if (!TerrainAsset::Read(PathToUtf8(absolute_path), data))
			{
				m_AssetDetailRows.emplace_back("Heightmap", "unreadable");
				return;
			}

			const uint32_t resolution = data.Resolution;
			const size_t disk_bytes = static_cast<size_t>(resolution) * resolution * sizeof(uint16_t);
			const size_t gpu_bytes = static_cast<size_t>(resolution) * resolution * sizeof(float);

			const auto [min_it, max_it] = std::minmax_element(data.Heights.begin(), data.Heights.end());
			const float min_height = min_it != data.Heights.end() ? *min_it * data.HeightScale : 0.0f;
			const float max_height = max_it != data.Heights.end() ? *max_it * data.HeightScale : 0.0f;

			char extent_text[48];
			std::snprintf(extent_text, sizeof(extent_text), "%.0f x %.0f m",
				data.Extent.x * 2.0f, data.Extent.y * 2.0f);
			char range_text[48];
			std::snprintf(range_text, sizeof(range_text), "%.1f - %.1f m", min_height, max_height);
			char scale_text[32];
			std::snprintf(scale_text, sizeof(scale_text), "%.1f m", data.HeightScale);

			m_AssetDetailRows.emplace_back("Resolution",
				std::to_string(resolution) + " x " + std::to_string(resolution));
			m_AssetDetailRows.emplace_back("Extent", extent_text);
			m_AssetDetailRows.emplace_back("HeightScale", scale_text);
			m_AssetDetailRows.emplace_back("Height range", range_text);
			m_AssetDetailRows.emplace_back("Heightmap", "16-bit, " + FormatFileSize(disk_bytes));
			m_AssetDetailRows.emplace_back("GPU texture", "R32F, " + FormatFileSize(gpu_bytes));

			/* 材质：资产里写的 .mtl，没写就用内建默认；三档颜色就是现在的"层" */
			const SharedPtr<Material> material = data.MaterialPath.empty()
				? TerrainComponent::GetDefaultMaterial()
				: AssetManager::Instance().GetOrLoad<Material>(ABSOLUTE_PATH(data.MaterialPath));

			m_AssetTerrain.MaterialLine = data.MaterialPath.empty() ? "Built-in default" : data.MaterialPath;
			m_AssetDetailRows.emplace_back("Material", m_AssetTerrain.MaterialLine);
			if (material != nullptr && material->GetShader() != nullptr)
				m_AssetDetailRows.emplace_back("Shader", RELATIVE_PATH(material->GetShader()->GetPath()));

			/* 四层：低处草、陡坡岩、低洼砂、高处雪；坡度与两条高度线决定怎么混 */
			m_AssetDetailRows.emplace_back("Layers", "4 (height + slope blend)");
			char slope_text[32];
			std::snprintf(slope_text, sizeof(slope_text), "%.2f", ReadMaterialFloat(material, "u_RockSlope", 0.45f));
			char sand_text[32];
			std::snprintf(sand_text, sizeof(sand_text), "%.2f", ReadMaterialFloat(material, "u_SandHeight", 0.06f));
			char snow_text[32];
			std::snprintf(snow_text, sizeof(snow_text), "%.2f", ReadMaterialFloat(material, "u_SnowHeight", 0.62f));
			char roughness_text[32];
			std::snprintf(roughness_text, sizeof(roughness_text), "%.2f", ReadMaterialFloat(material, "u_Roughness", 0.85f));
			m_AssetDetailRows.emplace_back("Rock slope", slope_text);
			m_AssetDetailRows.emplace_back("Sand / snow line", std::string(sand_text) + " / " + snow_text);
			m_AssetDetailRows.emplace_back("Roughness", roughness_text);

			/* 层贴图是可选的：配了的层才有纹理，其余按颜色平涂 */
			uint32_t textured_layers = 0;
			for (int layer = 0; layer < 4; ++layer)
			{
				char map_name[32];
				std::snprintf(map_name, sizeof(map_name), "u_Layer%dAlbedo", layer);
				MaterialParamInfo info;
				if (TryReadMaterialParam(material, map_name, info) && info.Type == ParamType::Texture)
					++textured_layers;
			}
			m_AssetDetailRows.emplace_back("Layer maps",
				std::to_string(textured_layers) + " / 4 textured");

			m_AssetTerrain.LayerColors = {
				{ "Grass", ReadMaterialColor(material, "u_Layer0Color", glm::vec3(0.26f, 0.36f, 0.16f)) },
				{ "Rock", ReadMaterialColor(material, "u_Layer1Color", glm::vec3(0.32f, 0.30f, 0.27f)) },
				{ "Sand", ReadMaterialColor(material, "u_Layer2Color", glm::vec3(0.55f, 0.47f, 0.34f)) },
				{ "Snow", ReadMaterialColor(material, "u_Layer3Color", glm::vec3(0.86f, 0.88f, 0.92f)) },
			};
			m_AssetTerrain.HeightTexture = BuildTerrainPreviewTexture(
				PathToUtf8(absolute_path.filename()) + "_HeightPreview", data);
		}
		else if (entry.Kind == AssetFileKind::Shader && IsGlslSource(absolute_path))
		{
			/* shader 依赖：只读源码展开一遍 include（不编译），主文件之外的都是被引用的文件 */
			std::vector<std::string> dependencies;
			ShaderCompiler::ReadFile(PathToUtf8(absolute_path), &dependencies);

			for (size_t index = 1; index < dependencies.size(); ++index)
			{
				/* 落在引擎根上的那份（内置库）浏览器树里没有：只能看，点不出结果 */
				const std::string relative = AssetRelativePath(PathFromUtf8(dependencies[index]));
				const std::filesystem::path relative_path = PathFromUtf8(relative);
				m_AssetShaderIncludes.push_back({ relative, !relative_path.is_absolute()
					&& std::filesystem::exists(g_ProjectPath / relative_path) });
			}
		}
	}

	/* 切换某张烘焙图卡的预览 mip：重开缓存文件（数据不常驻 —— 切层是低频动作，
	 * 重读一遍文件最省内存）、按新层级重生成十字展开图。
	 * 读不到（损坏 / 丢失 / 序号越界）：图留空，卡照常显示。 */
	void SceneInspector::ApplyProbeCardMip(size_t card_index, int mip)
	{
		ProbeImageCard& card = m_AssetProbeCards[card_index];
		card.Mip = mip;

		ReflectionProbeBakeCache::Data data;
		if (card.SourcePath.empty() || !ReflectionProbeBakeCache::Read(card.SourcePath, data)
			|| card.ImageIndex < 0 || static_cast<size_t>(card.ImageIndex) >= data.Images.size())
		{
			card.Texture = nullptr;
			return;
		}

		card.Texture = ReflectionProbe::BuildPreviewTexture(
			card.TextureName, data.Images[card.ImageIndex], card.Mip);
	}

	/* 数值字段：声明了滑块的按滑块画（量程来自 Range），其余照旧拖拽。
	 * 滑块没有量程就没有意义，所以缺 Range 时退回拖拽 */
	static void DrawNumericFieldControl(const FieldDesc& field, void* field_ptr)
	{
		const bool use_slider = field.AsSlider && field.MaxValue > field.MinValue;
		if (field.Type == FieldType::Int)
		{
			int& value = *reinterpret_cast<int*>(field_ptr);
			if (use_slider)
				ImGuiExt::DrawSliderIntUI(field.Name, value, static_cast<int>(field.MinValue),
					static_cast<int>(field.MaxValue));
			else
				ImGuiExt::DrawDragIntUI(field.Name, value, field.DragSpeed, field.MinValue, field.MaxValue);
			return;
		}

		float& value = *reinterpret_cast<float*>(field_ptr);
		if (use_slider)
			ImGuiExt::DrawSliderFloatUI(field.Name, value, field.MinValue, field.MaxValue);
		else
			ImGuiExt::DrawDragFloatUI(field.Name, value, field.DragSpeed, field.MinValue, field.MaxValue);
	}

	/* 按字段类型绘制单个控件，field_ptr 指向可写的字段存储 */
	void SceneInspector::DrawFieldControl(const FieldDesc& field, void* field_ptr)
	{
		switch (field.Type)
		{
		case FieldType::Bool:
			ImGuiExt::DrawCheckboxUI(field.Name, *reinterpret_cast<bool*>(field_ptr));
			break;
		case FieldType::Int:
			DrawNumericFieldControl(field, field_ptr);
			break;
		case FieldType::LayerMask:
			ImGuiExt::DrawLayerMaskUI(field.Name, *reinterpret_cast<LayerMask*>(field_ptr));
			break;
		case FieldType::Float:
			DrawNumericFieldControl(field, field_ptr);
			break;
		case FieldType::Vec2:
			ImGuiExt::DrawDragFloat2UI(field.Name, *reinterpret_cast<glm::vec2*>(field_ptr));
			break;
		case FieldType::Vec3:
		{
			auto& vec = *reinterpret_cast<glm::vec3*>(field_ptr);
			/* UniformScale：Scale 类字段 —— 拖任一分量时按住 Shift = 三轴等比 */
			const bool uniform_scale = field.Semantics != nullptr
				&& std::strcmp(field.Semantics, "UniformScale") == 0;
			if (field.Semantics != nullptr && std::strcmp(field.Semantics, "AngleDeg") == 0)
			{
				/* 内部以弧度存储、Inspector 按角度编辑 */
				glm::vec3 degree = glm::degrees(vec);
				ImGuiExt::DrawVec3ControlUI(field.Name, degree, field.ResetValue,
					EditorTheme::Token::PropertyLabelWidth, uniform_scale);
				vec = glm::radians(degree);
			}
			else
			{
				ImGuiExt::DrawVec3ControlUI(field.Name, vec, field.ResetValue,
					EditorTheme::Token::PropertyLabelWidth, uniform_scale);
			}
			break;
		}
		case FieldType::Vec4:
		case FieldType::Color:
			ImGuiExt::DrawColorUI(field.Name, *reinterpret_cast<glm::vec4*>(field_ptr));
			break;
		case FieldType::Enum:
		{
			if (field.GetEnumNames == nullptr)
				break;

			/* 枚举底层类型可能小于 int（如 uint8_t），按实际大小读写 */
			const size_t size = (field.ValueSize == 0 || field.ValueSize > sizeof(int))
				? sizeof(int) : field.ValueSize;
			int value = 0;
			std::memcpy(&value, field_ptr, size);
			ImGuiExt::DrawComboUI(field.Name, field.GetEnumNames(), value);
			std::memcpy(field_ptr, &value, size);
			break;
		}
		case FieldType::String:
		{
			/* 文本行也走统一的属性行布局，标签与其它行同宽同色 */
			const float value_width = ImGuiExt::BeginPropertyRow(field.Name);
			ImGui::SetNextItemWidth(value_width);
			InputTextString("##value", *reinterpret_cast<std::string*>(field_ptr));
			ImGuiExt::EndPropertyRow();
			break;
		}
		default:
			break;
		}
	}

	namespace
	{
		/* 字段值的字节数（定长字段由注册表给出） */
		size_t FieldByteSize(const FieldDesc& field)
		{
			return (field.ValueSize != 0) ? field.ValueSize : sizeof(float);
		}

		/* 访问器字段经 getter/setter 读写，偏移字段直接取址 */
		bool IsAccessorField(const FieldDesc& field)
		{
			return field.Get != nullptr && field.Set != nullptr;
		}
	}

	/* 单个字段的编辑：取快照 → 画控件 → 比较 → 有变化就生成历史命令。组件卡的字段和面板
	 * 头部的实体名都走这里 —— 撤销语义只有这一份实现（名字连改十次也只留一条历史）。 */
	void SceneInspector::DrawEditableField(const ComponentDesc& desc, Entity& entity, void* component,
	                                       const FieldDesc& field, const FieldDrawFunc& draw)
	{
		/* 上一帧有控件活跃、这一帧没有 → 一次编辑结束，给命令栈封口。
		 * 否则下一次编辑会并进上一条历史，撤销一次退过头。 */
		const bool item_active = ImGui::IsAnyItemActive();
		if (m_FieldEditTransactionOpen && !item_active)
		{
			m_FieldEditTransactionOpen = false;
			if (m_pCommandStack != nullptr)
				m_pCommandStack->EndTransaction();
		}

		const size_t value_size = FieldByteSize(field);
		const bool is_text = (field.Type == FieldType::String);
		const bool is_accessor = IsAccessorField(field);

		/* 编辑前的值，以及供控件操作的存储 */
		alignas(16) uint8_t before[kFieldValueCapacity] = {};
		alignas(16) uint8_t edited[kFieldValueCapacity] = {};
		std::string before_text;

		void* field_ptr = nullptr;
		if (is_accessor)
		{
			field.Get(component, edited);
			std::memcpy(before, edited, value_size);
			field_ptr = edited;
		}
		else
		{
			field_ptr = static_cast<uint8_t*>(component) + field.Offset;
			if (is_text)
				before_text = *static_cast<const std::string*>(field_ptr);
			else
				std::memcpy(before, field_ptr, value_size);
		}

		draw(field, field_ptr);

		/* 访问器字段：把编辑结果写回对象 */
		if (is_accessor)
			field.Set(component, field_ptr);

		/* 只有用户真的在操作控件时才生成命令（Gizmo 拖拽这类外部改动不产生历史）。判据要连
		 * "帧开始时活不活跃"一起看：点击类控件在松开的那帧才提交值，那一刻 ActiveId 已经清掉了
		 * —— 只看画完之后的状态，改动会整段丢掉。 */
		if (m_pCommandStack == nullptr || !(item_active || ImGui::IsAnyItemActive()))
			return;

		const bool changed = is_text
			? (before_text != *static_cast<const std::string*>(field_ptr))
			: (std::memcmp(before, field_ptr, value_size) != 0);
		if (!changed)
			return;

		if (!m_FieldEditTransactionOpen)
		{
			m_FieldEditTransactionOpen = true;
			m_pCommandStack->BeginTransaction();
		}

		if (is_text)
		{
			m_pCommandStack->Execute(CreateUniquePtr<ComponentFieldCommand>(
				entity, desc, field, before_text, *static_cast<const std::string*>(field_ptr)));
		}
		else
		{
			m_pCommandStack->Execute(CreateUniquePtr<ComponentFieldCommand>(
				entity, desc, field, before, field_ptr));
		}
	}

	/* 按字段元数据生成控件（组件卡的卡身）：字段按 Group 归拢，同组收在一个可折叠标题下。
	 * 没声明分组的组件照旧平铺；只有一组时标题是多余的，也不画。 */
	void SceneInspector::DrawComponentFieldsBySchema(const ComponentDesc& desc, Entity& entity, void* component)
	{
		struct FieldGroup
		{
			const char* Name{ nullptr };              /* 空 = 未分组 */
			std::vector<const FieldDesc*> Fields;
		};

		/* 先按可见性筛再分组：整组字段都被条件藏起来时，不留一个空标题 */
		const auto same_group = [](const char* lhs, const char* rhs)
		{
			if (lhs == nullptr || rhs == nullptr)
				return lhs == rhs;
			return std::strcmp(lhs, rhs) == 0;
		};

		std::vector<FieldGroup> groups;
		for (const FieldDesc& field : desc.Fields)
		{
			/* 条件不满足：不画该行（字段值保留，序列化照常） */
			if (!EvaluateCondition(field.Condition, component))
				continue;

			auto group = std::find_if(groups.begin(), groups.end(),
				[&](const FieldGroup& candidate) { return same_group(candidate.Name, field.Group); });
			if (group == groups.end())
			{
				groups.push_back(FieldGroup{ field.Group, {} });
				group = groups.end() - 1;
			}
			group->Fields.push_back(&field);
		}

		/* 未分组的排在前面（与材质卡的排法一致）：注册顺序只在同一组内说话 */
		std::stable_partition(groups.begin(), groups.end(),
			[](const FieldGroup& group) { return group.Name == nullptr; });

		const bool show_group_titles = groups.size() > 1;
		for (const FieldGroup& group : groups)
		{
			if (show_group_titles && group.Name != nullptr
				&& !MaterialParamUI::BeginPropertyGroup(group.Name, group.Name))
				continue;

			ImGui::PushID(group.Name != nullptr ? group.Name : "##Ungrouped");
			for (const FieldDesc* field : group.Fields)
			{
				ImGui::PushID(field->Name);
				DrawEditableField(desc, entity, component, *field, &DrawFieldControl);
				ImGui::PopID();
			}
			ImGui::PopID();
		}
	}

	/* ==================== 材质卡（Model 组件的附属，每槽一张）====================
	 * 卡片背景走绘制通道、不能嵌套 —— 材质详情就平铺在 Model 卡后面。编辑语义跟组件卡一致：
	 * 改参数自动实例化（改动那一刻才克隆写回），撤销走"组件快照 + 事务合并"。 */

	namespace
	{
		/* 槽编辑语义在 MaterialSlotEditing 里（模型编辑器共用），这里只做索引到槽名的转换 */
		void WriteBackMaterialParam(ModelComponent& component, const Model& model, int slot_index,
			ParamType type, const std::string& name, const std::any& value, bool& component_changed)
		{
			const MaterialSlot* slot = model.GetSlotByIndex(slot_index);
			if (slot == nullptr)
				return;

			MaterialSlotEditing::WriteParam(model, component.m_SlotOverrides, slot->Name,
				type, name, value, component_changed);
		}

		void WriteBackMaterialRasterState(ModelComponent& component, const Model& model, int slot_index,
			const RenderRasterState& state, bool& component_changed)
		{
			const MaterialSlot* slot = model.GetSlotByIndex(slot_index);
			if (slot == nullptr)
				return;

			MaterialSlotEditing::WriteRasterState(model, component.m_SlotOverrides, slot->Name,
				state, component_changed);
		}

		void WriteBackMaterialRenderQueue(ModelComponent& component, const Model& model, int slot_index,
			int32_t render_queue, bool& component_changed)
		{
			const MaterialSlot* slot = model.GetSlotByIndex(slot_index);
			if (slot == nullptr)
				return;

			MaterialSlotEditing::WriteRenderQueue(model, component.m_SlotOverrides, slot->Name,
				render_queue, component_changed);
		}

		void WriteBackMaterialShader(ModelComponent& component, const Model& model, int slot_index,
			const std::string& shader_path, bool& component_changed)
		{
			const MaterialSlot* slot = model.GetSlotByIndex(slot_index);
			if (slot == nullptr)
				return;

			MaterialSlotEditing::WriteShader(model, component.m_SlotOverrides, slot->Name,
				shader_path, component_changed);
		}

		/* 有效材质（覆盖 ?: 模型默认 ?: 白模）由 Components.h 的 ResolveSlotMaterial 提供 */

		/* ---- 光栅化状态（RenderRasterState）的行 ---- */

		/* 各枚举的显示名（顺序与 RenderCommon.h 里的枚举定义一一对应） */
		const std::vector<std::string>& CullModeOptions()
		{
			static const std::vector<std::string> options = { "None", "Front", "Back", "Front And Back" };
			return options;
		}

		const std::vector<std::string>& FrontFaceOptions()
		{
			static const std::vector<std::string> options = { "CW", "CCW" };
			return options;
		}

		const std::vector<std::string>& BlendEquationOptions()
		{
			static const std::vector<std::string> options = { "Add", "Subtract", "Reverse Subtract", "Min", "Max" };
			return options;
		}

		const std::vector<std::string>& BlendFuncOptions()
		{
			static const std::vector<std::string> options = {
				"Zero", "One", "Src Color", "One Minus Src Color", "Dst Color", "One Minus Dst Color",
				"Src Alpha", "One Minus Src Alpha", "Dst Alpha", "One Minus Dst Alpha", "Src Alpha Saturate" };
			return options;
		}

		const std::vector<std::string>& CompareFuncOptions()
		{
			static const std::vector<std::string> options = {
				"Less Equal", "Greater Equal", "Less", "Greater", "Equal", "Not Equal", "Always", "Never" };
			return options;
		}

		/* 光栅化状态的行：直接画在传入的 state 上；返回值 = 是否有改动
		 * （调用方决定记脏 / 实例化写回 / 热更新 —— 两种材质卡共用同一套行）。 */
		bool DrawMaterialRasterStateRows(RenderRasterState& state)
		{
			bool changed = false;

			int cull_mode = static_cast<int>(state.CullMode);
			ImGuiExt::DrawComboUI("Cull Mode", CullModeOptions(), cull_mode,
				[&](int picked) { state.CullMode = static_cast<CullMode>(picked); changed = true; });

			int front_face = static_cast<int>(state.FrontFaceType);
			ImGuiExt::DrawComboUI("Front Face", FrontFaceOptions(), front_face,
				[&](int picked) { state.FrontFaceType = static_cast<FrontFaceType>(picked); changed = true; });

			/* 位域字段不能绑 bool&：勾选框画在临时量上、改动时写回 */
			bool enable_blend = state.EnableBlend;
			if (ImGuiExt::DrawCheckboxUI("Blend", enable_blend))
			{
				state.EnableBlend = enable_blend;
				changed = true;
			}

			int blend_equation_rgb = static_cast<int>(state.BlendEquationRGB);
			ImGuiExt::DrawComboUI("Blend Op RGB", BlendEquationOptions(), blend_equation_rgb,
				[&](int picked) { state.BlendEquationRGB = static_cast<BlendEquation>(picked); changed = true; });

			int blend_equation_a = static_cast<int>(state.BlendEquationA);
			ImGuiExt::DrawComboUI("Blend Op A", BlendEquationOptions(), blend_equation_a,
				[&](int picked) { state.BlendEquationA = static_cast<BlendEquation>(picked); changed = true; });

			int blend_src_rgb = static_cast<int>(state.BlendFuncSrcRGB);
			ImGuiExt::DrawComboUI("Blend Src RGB", BlendFuncOptions(), blend_src_rgb,
				[&](int picked) { state.BlendFuncSrcRGB = static_cast<BlendFunc>(picked); changed = true; });

			int blend_src_a = static_cast<int>(state.BlendFuncSrcA);
			ImGuiExt::DrawComboUI("Blend Src A", BlendFuncOptions(), blend_src_a,
				[&](int picked) { state.BlendFuncSrcA = static_cast<BlendFunc>(picked); changed = true; });

			int blend_dst_rgb = static_cast<int>(state.BlendFuncDstRGB);
			ImGuiExt::DrawComboUI("Blend Dst RGB", BlendFuncOptions(), blend_dst_rgb,
				[&](int picked) { state.BlendFuncDstRGB = static_cast<BlendFunc>(picked); changed = true; });

			int blend_dst_a = static_cast<int>(state.BlendFuncDstA);
			ImGuiExt::DrawComboUI("Blend Dst A", BlendFuncOptions(), blend_dst_a,
				[&](int picked) { state.BlendFuncDstA = static_cast<BlendFunc>(picked); changed = true; });

			bool enable_depth_write = state.EnableDepthWrite;
			if (ImGuiExt::DrawCheckboxUI("Depth Write", enable_depth_write))
			{
				state.EnableDepthWrite = enable_depth_write;
				changed = true;
			}

			int depth_compare = static_cast<int>(state.DepthCompareFunc);
			ImGuiExt::DrawComboUI("Depth Test", CompareFuncOptions(), depth_compare,
				[&](int picked) { state.DepthCompareFunc = static_cast<CompareFunc>(picked); changed = true; });

			bool enable_color_write = state.EnableColorWrite;
			if (ImGuiExt::DrawCheckboxUI("Color Write", enable_color_write))
			{
				state.EnableColorWrite = enable_color_write;
				changed = true;
			}

			return changed;
		}

		/* 分区标题行：三角和标题从标签列左沿起排（跟卡片头同一套画法）。不用 stock 树箭头
		 * （它从 FramePadding 起画、会右移一档）；TreeNodeEx 只用来做整行命中和折叠状态，箭头推成
		 * 透明藏掉，这里自绘精确对位 —— 要在 TreeNodeEx 之后立刻调用。 */
		void DrawMaterialSectionHeader(bool open, const char* title)
		{
			const ImVec2 row_min = ImGui::GetItemRectMin();
			const float row_height = ImGui::GetItemRectSize().y;
			const float text_height = ImGui::GetFontSize();
			const float arrow_size = text_height * 0.55f; /* 与卡片头箭头同口径 */
			const float center_y = row_min.y + row_height * 0.5f;

			ImDrawList* draw_list = ImGui::GetWindowDrawList();
			PanelChrome::DrawDisclosureArrow(draw_list,
				ImVec2(row_min.x + arrow_size * 0.5f, center_y),
				arrow_size, open, ImGui::GetColorU32(EditorTheme::Token::TextDim));
			draw_list->AddText(
				ImVec2(row_min.x + arrow_size + ImGui::GetStyle().ItemInnerSpacing.x,
					center_y - text_height * 0.5f),
				ImGui::GetColorU32(EditorTheme::Token::Text), title);
		}

		/* 卡内分区隔离线：把卡尾动作与上面的内容隔开（材质卡的 Raster State 也用它）。
		 * 自绘细线（项目惯例：自绘不占行距、间距说得清；ImGui::Separator 吃「1px + 整行距」、
		 * 且 Separator 色在卡底上偏淡）——块高 = 上留 5 + 线 1 + 下留 5。 */
		void DrawCardSectionDivider()
		{
			const float top_gap = Scaled(5.0f);
			const float bottom_gap = Scaled(5.0f);
			const ImVec2 line_start = ImGui::GetCursorScreenPos();
			const float line_y = line_start.y + top_gap + 0.5f;
			ImGui::GetWindowDrawList()->AddLine(
				ImVec2(line_start.x, line_y),
				ImVec2(line_start.x + ImGui::GetContentRegionAvail().x, line_y),
				ImGui::GetColorU32(EditorTheme::Token::Border));
			ImGui::Dummy(ImVec2(0.0f, top_gap + 1.0f + bottom_gap));
		}
	}

	/* 组件自定义编辑的合并窗口封口（每帧调用；与 DrawEditableField 同一套事务规则）：
	 * 上一帧有控件活跃、这一帧没有 → 一次连续编辑结束，给命令栈封口。 */
	void SceneInspector::ServiceComponentEditTransaction()
	{
		const bool item_active = ImGui::IsAnyItemActive();
		if (m_ComponentEditTransactionOpen && !item_active)
		{
			m_ComponentEditTransactionOpen = false;
			if (m_pCommandStack != nullptr)
				m_pCommandStack->EndTransaction();
		}
	}

	/* 材质卡主体：Shader 行 + 参数行 + 渲染队列 + 光栅化状态。
	 * 材质从哪来、改动往哪写由调用方给的回调决定 —— 模型槽写回实体覆盖（首次改动自动克隆成实例），
	 * 草地直接写它自己的那个槽。resolve_live 每行都重新取一次：一次改动可能已经把材质换成实例了。 */
	bool SceneInspector::DrawMaterialCardBody(SharedPtr<Material> current,
		const std::function<SharedPtr<Material>()>& resolve_live,
		const MaterialShaderWriter& write_shader,
		const MaterialParamWriter& write_param,
		const std::function<void(int32_t)>& write_queue,
		const std::function<void(const RenderRasterState&)>& write_raster)
	{
		bool changed = false;

		if (current == nullptr)
			return false;

		/* Shader 行：点开菜单换 Shader（与模型编辑器、.mtl 卡同一个控件）。换完重新取一次材质，
		 * 参数清单按新 Shader 反射走；旧值留在材质里，切回来还在。 */
		if (const std::string picked = MaterialParamUI::DrawMaterialShaderPicker(*current); !picked.empty())
		{
			write_shader(picked);
			changed = true;
			current = resolve_live();
			if (current == nullptr)
				return changed;
		}

		MaterialParamUI::DrawMaterialParams(*current, [&]() { return resolve_live().get(); }, m_AssetRevealFunc, write_param);

		/* 渲染队列：小于 2500 不透明，2500 起透明（透明批次统一关深度写、按距离从远到近画） */
		{
			const SharedPtr<Material> live = resolve_live();
			if (live != nullptr)
			{
				int32_t render_queue = live->GetRenderQueue();
				if (MaterialParamUI::DrawMaterialRenderQueueRow(render_queue))
				{
					write_queue(render_queue);
					changed = true;
				}
			}
		}

		/* 光栅化状态：可折叠分区（默认收起；值画在临时副本上，改动时才写回）。上方压一条
		 * 隔离线。注意：TreeNodeEx 只承担命中 / 折叠状态（标签传空串、三角自绘）；
		 * NoTreePushOnOpen：内容不缩进（默认 TreePush 会 Indent 18px、整段右移错位）。 */
		DrawCardSectionDivider();
		{
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.0f, 0.0f, 0.0f)); /* 藏掉 stock 箭头（自绘对位） */
			const bool raster_open = ImGui::TreeNodeEx("##MaterialRasterState",
				ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_NoTreePushOnOpen, "%s", "");
			ImGui::PopStyleColor();
			DrawMaterialSectionHeader(raster_open, "Raster State");

			if (raster_open)
			{
				const SharedPtr<Material> live = resolve_live();
				if (live != nullptr)
				{
					RenderRasterState state = live->GetRasterState();
					if (DrawMaterialRasterStateRows(state))
					{
						write_raster(state);
						changed = true;
					}
				}
			}
		}

		return changed;
	}

	/* 模型槽的绑定：参数改动写回实体覆盖（首次改动从来源克隆成实例） */
	bool SceneInspector::DrawMaterialCardBody(ModelComponent& component, const Model& model, int slot_index)
	{
		bool changed = false;

		const auto resolve_live = [&component, &model, slot_index]()
		{
			return ResolveSlotMaterial(model, slot_index, &component.m_SlotOverrides);
		};

		const bool body_changed = DrawMaterialCardBody(resolve_live(), resolve_live,
			[&](const std::string& shader_path)
			{
				WriteBackMaterialShader(component, model, slot_index, shader_path, changed);
			},
			[&](ParamType type, const std::string& name, const std::any& value)
			{
				WriteBackMaterialParam(component, model, slot_index, type, name, value, changed);
			},
			[&](int32_t render_queue)
			{
				WriteBackMaterialRenderQueue(component, model, slot_index, render_queue, changed);
			},
			[&](const RenderRasterState& state)
			{
				WriteBackMaterialRasterState(component, model, slot_index, state, changed);
			});

		return changed || body_changed;
	}

	/* 「Open in Model Editor」按钮：文件模型才有可编辑的东西（内建 / 空模型没有路径）。
	 * 画在所在卡片的卡身末尾，铺满内容宽 —— 实体侧是 Model 组件卡，资源侧是详情卡。 */
	void SceneInspector::DrawOpenInModelEditorButton(const std::string& absolute_path)
	{
		PROFILE_FUNCTION();

		if (absolute_path.empty())
			return;

		const float button_width = ImGui::GetContentRegionAvail().x;
		if (ImGui::Button("Open in Model Editor", ImVec2(button_width, 0.0f)) && m_ModelEditorFunc)
			m_ModelEditorFunc(absolute_path);
	}

	/* Model 的材质卡：每槽一张，排在 Model 组件卡之后同级平铺。 */
	void SceneInspector::DrawModelMaterialCards(Entity entity, ModelComponent& component)
	{
		PROFILE_FUNCTION();

		if (component.m_Model == nullptr)
			return;

		const auto& slots = component.m_Model->GetMaterialSlots();
		if (slots.empty())
			return;

		const ComponentDesc* desc = ComponentRegistry::Instance().Find(std::type_index(typeid(ModelComponent)));
		if (desc == nullptr)
			return;

		for (int slot_index = 0; slot_index < static_cast<int>(slots.size()); ++slot_index)
		{
			const MaterialSlot& slot = slots[slot_index];

			/* 卡片键按槽索引：多槽各一张、折叠状态互不影响（槽名可能重名，不能当 ID） */
			ImGui::PushID(slot_index);

			const std::string title = "Material · " + slot.Name;
			const PanelChrome::Card card = PanelChrome::BeginCard(title.c_str(), Icons::Id::FileMaterial);

			/* 改了参数就地克隆出来的副本：卡头挂一枚 Instance，否则看着跟 .mtl 资产没两样 */
			const auto override_iter = component.m_SlotOverrides.find(slot.Name);
			if (override_iter != component.m_SlotOverrides.end() && override_iter->second.IsInstance
				&& override_iter->second.pMaterial != nullptr)
			{
				PanelChrome::DrawCardBadge(card, "Instance",
					"Edited copy for this slot — changes don't go into the material asset\n"
					"Reset in the Materials row re-clones from the asset");
			}

			if (card.Open)
			{
				/* 与 DrawComponentBlock 同一套撤销语义：快照 + 事务合并 */
				const bool can_record = m_pCommandStack != nullptr
					&& desc->Capture != nullptr && desc->Restore != nullptr;

				ComponentSnapshot before;
				if (can_record)
					desc->Capture(entity, before);

				const bool changed = DrawMaterialCardBody(component, *component.m_Model, slot_index);

				if (changed && can_record && before.IsValid())
				{
					if (ImGui::IsAnyItemActive() && !m_ComponentEditTransactionOpen)
					{
						m_ComponentEditTransactionOpen = true;
						m_pCommandStack->BeginTransaction();
					}

					ComponentSnapshot after;
					desc->Capture(entity, after);
					if (after.IsValid())
					{
						m_pCommandStack->Execute(CreateUniquePtr<ComponentCustomDrawCommand>(
							entity, desc->Has, desc->Restore, std::move(before), std::move(after), desc->Name));
					}
				}
			}

			PanelChrome::EndCard(card);
			ImGui::PopID();
		}
	}

	/* 草地卡的材质：草地只有一个槽，同样平铺在 Grass 组件卡之后。
	 * 编辑语义与模型槽一致 —— 第一次改参数就把共享材质克隆成这片草地自己的副本，
	 * 来源 .mtl 与其它草地不受影响；Reset 在组件卡的材质行里。 */
	void SceneInspector::DrawGrassMaterialCard(Entity entity, GrassComponent& component)
	{
		PROFILE_FUNCTION();

		if (component.GetMaterial() == nullptr)
			return;

		const ComponentDesc* desc = ComponentRegistry::Instance().Find(std::type_index(typeid(GrassComponent)));
		if (desc == nullptr)
			return;

		ImGui::PushID("##GrassMaterial");
		const PanelChrome::Card card = PanelChrome::BeginCard("Material · Grass", Icons::Id::FileMaterial);

		/* 改了参数就地克隆出来的副本：卡头挂一枚 Instance，否则看着跟 .mtl 资产没两样 */
		if (component.m_MaterialSlot.IsInstance)
		{
			PanelChrome::DrawCardBadge(card, "Instance",
				"Edited copy for this grass — changes don't go into the material asset\n"
				"Reset in the Grass card's Material row re-clones from the asset");
		}

		if (card.Open)
		{
			/* 与 DrawComponentBlock 同一套撤销语义：快照 + 事务合并 */
			const bool can_record = m_pCommandStack != nullptr
				&& desc->Capture != nullptr && desc->Restore != nullptr;

			ComponentSnapshot before;
			if (can_record)
				desc->Capture(entity, before);

			bool changed = false;
			const auto resolve_live = [&component]() { return component.GetMaterial(); };

			const bool body_changed = DrawMaterialCardBody(component.GetMaterial(), resolve_live,
				[&](const std::string& shader_path)
				{
					bool written = false;
					MaterialSlotEditing::WriteShader(component.m_MaterialSlot, shader_path, written);
					changed |= written;
				},
				[&](ParamType type, const std::string& name, const std::any& value)
				{
					bool created = false;
					const SharedPtr<Material> live = MaterialSlotEditing::MakeInstance(component.m_MaterialSlot, created);
					if (live == nullptr)
						return;

					if (type == ParamType::Texture)
						live->SetTexture(name, std::any_cast<SharedPtr<DeviceTexture>>(value));
					else
						live->SetParameters(type, name, value);
					changed = true;
				},
				[&](int32_t render_queue)
				{
					bool created = false;
					if (const SharedPtr<Material> live = MaterialSlotEditing::MakeInstance(component.m_MaterialSlot, created))
					{
						live->SetRenderQueue(render_queue);
						changed = true;
					}
				},
				[&](const RenderRasterState& state)
				{
					bool created = false;
					if (const SharedPtr<Material> live = MaterialSlotEditing::MakeInstance(component.m_MaterialSlot, created))
					{
						live->SetRasterState(state);
						changed = true;
					}
				});

			if ((changed || body_changed) && can_record && before.IsValid())
			{
				if (ImGui::IsAnyItemActive() && !m_ComponentEditTransactionOpen)
				{
					m_ComponentEditTransactionOpen = true;
					m_pCommandStack->BeginTransaction();
				}

				ComponentSnapshot after;
				desc->Capture(entity, after);
				if (after.IsValid())
				{
					m_pCommandStack->Execute(CreateUniquePtr<ComponentCustomDrawCommand>(
						entity, desc->Has, desc->Restore, std::move(before), std::move(after), desc->Name));
				}
			}
		}

		PanelChrome::EndCard(card);
		ImGui::PopID();
	}

	bool SceneInspector::DrawTerrainMaterialBody(TerrainComponent& component)
	{
		if (component.GetMaterial() == nullptr)
			return false;

		bool changed = false;
		const auto resolve_live = [&component]() { return component.GetMaterial(); };
		const bool body_changed = DrawMaterialCardBody(component.GetMaterial(), resolve_live,
			[&](const std::string& shader_path)
			{
				bool written = false;
				MaterialSlotEditing::WriteShader(component.m_MaterialSlot, shader_path, written);
				changed |= written;
			},
			[&](ParamType type, const std::string& name, const std::any& value)
			{
				bool created = false;
				const SharedPtr<Material> live = MaterialSlotEditing::MakeInstance(component.m_MaterialSlot, created);
				if (live == nullptr)
					return;
				if (type == ParamType::Texture)
					live->SetTexture(name, std::any_cast<SharedPtr<DeviceTexture>>(value));
				else
					live->SetParameters(type, name, value);
				changed = true;
			},
			[&](int32_t queue)
			{
				bool created = false;
				if (const auto live = MaterialSlotEditing::MakeInstance(component.m_MaterialSlot, created))
				{
					live->SetRenderQueue(queue);
					changed = true;
				}
			},
			[&](const RenderRasterState& state)
			{
				bool created = false;
				if (const auto live = MaterialSlotEditing::MakeInstance(component.m_MaterialSlot, created))
				{
					live->SetRasterState(state);
					changed = true;
				}
			});
		return changed || body_changed;
	}

	/* ==================== 相机预览卡（Camera 组件的附加卡）====================
	 * 「选中相机 → 看见它拍什么」：画面就是这台场景相机自己的渲染输出（场景层看到预览请求后
	 * 打开它的渲染并让它画到纹理），本卡只管版式和请求。同级平铺（卡片背景走通道、不能嵌套）。 */

	void SceneInspector::DrawCameraPreviewCard(CameraComponent& component)
	{
		PROFILE_FUNCTION();

		const SharedPtr<Camera>& camera = component.m_Camera;
		if (camera == nullptr || m_pOwnerScene == nullptr)
			return;

		ImGui::PushID("##CameraPreview");
		const PanelChrome::Card card = PanelChrome::BeginCard("Camera Preview", Icons::Id::Camera);

		if (card.Open)
		{
			/* 窗口收起 / 停靠标签页被挡住（SkipItems）时不要画面：相机不该在看不见的地方空转 */
			if (!ImGui::GetCurrentWindow()->SkipItems)
				m_pPreviewCamera = camera.get();

			/* 只认本帧真的渲染过的输出：相机关着时视图里留的是旧帧的纹理句柄。
			 * 画面宽度铺满卡片，高度按输出纹理的宽高比（与运行时窗口一致）。 */
			const RenderView* render_view = camera->GetRenderView();
			const SharedPtr<DeviceTexture> texture = render_view->IsEnableRender()
				? render_view->GetRenderTarget() : nullptr;
			if (texture != nullptr && texture->GetWidth() > 0)
			{
				const float image_width = ImGui::GetContentRegionAvail().x;
				const float image_height = image_width * static_cast<float>(texture->GetHeight())
					/ static_cast<float>(texture->GetWidth());
				ImGui::Image((ImTextureID)texture.get(), ImVec2(image_width, image_height),
					ImVec2(0, 1), ImVec2(1, 0));
			}
			else
			{
				/* 还没有画面（刚开始预览的第一帧）：占位说明，下一帧起画面就绪 */
				ImGuiExt::DrawCommonTextUI("Preview", "starting...");
			}
		}

		PanelChrome::EndCard(card);
		ImGui::PopID();
	}

	/* ==================== 材质资产编辑卡（.mtl 的资源详情）====================
	 * 资源浏览器选中 .mtl 时画在详情卡后面（每条目一张）。编辑只落编辑缓冲，点了 Apply 才
	 * 序列化回文件 —— 没保存的改动不影响场景（Model 槽位读的是加载时那份）。 */

	/* 卡身：Shader 下拉（按目录分组的子菜单）+ 参数行 + 光栅化状态行（与 Model 材质卡共用控件）。
	 * on_edit：任何改动走这里 —— 调用方记脏并做热更新（同步缓存里的共享材质）。 */
	void SceneInspector::DrawAssetMaterialBody(Material& material, const std::function<void()>& on_edit)
	{
		PROFILE_FUNCTION();

		/* Shader 行（与模型编辑器共用同一控件）：点开菜单换 Shader，选中后记脏 + 热更新 */
		if (MaterialParamUI::DrawMaterialShaderRow(material))
			on_edit();
		MaterialParamUI::DrawMaterialParams(material, [&]() { return &material; }, m_AssetRevealFunc,
			[&](ParamType type, const std::string& name, const std::any& value)
			{
				if (type == ParamType::Texture)
					material.SetTexture(name, std::any_cast<SharedPtr<DeviceTexture>>(value));
				else
					material.SetParameters(type, name, value);
				on_edit();
			});

		/* 渲染队列：小于 2500 不透明，2500 起透明（透明批次统一关深度写、按距离从远到近画） */
		int32_t render_queue = material.GetRenderQueue();
		if (MaterialParamUI::DrawMaterialRenderQueueRow(render_queue))
		{
			material.SetRenderQueue(render_queue);
			on_edit();
		}

		/* 光栅化状态：可折叠分区（默认收起；值画在临时副本上，改动时才实例化写回）。上方压一条
		 * 隔离线。注意：TreeNodeEx 只承担命中 / 折叠状态（标签传空串、三角自绘）；
		 * NoTreePushOnOpen：内容不缩进（默认 TreePush 会 Indent 18px，跟参数行错位）。 */
		DrawCardSectionDivider();
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.0f, 0.0f, 0.0f)); /* 藏掉 stock 箭头（自绘对位） */
		const bool raster_open = ImGui::TreeNodeEx("##MaterialRasterState",
			ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_NoTreePushOnOpen, "%s", "");
		ImGui::PopStyleColor();
		DrawMaterialSectionHeader(raster_open, "Raster State");
		if (raster_open)
		{
			if (DrawMaterialRasterStateRows(material.GetRasterState()))
				on_edit();
		}
	}

	/* 单条材质资产：卡身 + Apply */
	void SceneInspector::DrawAssetMaterialCards(const std::filesystem::path& absolute_path)
	{
		PROFILE_FUNCTION();

		if (m_AssetMaterial == nullptr)
			return;

		const std::string asset_path = PathToUtf8(absolute_path);
		const SharedPtr<Material> editable = m_AssetMaterial;

		const PanelChrome::Card card = PanelChrome::BeginCard("Material", Icons::Id::FileMaterial);
		if (card.Open)
		{
			DrawAssetMaterialBody(*m_AssetMaterial, [this, asset_path, editable]()
			{
				m_AssetMaterialDirty = true;
				AssetManager::Instance().Refresh<Material>(asset_path, editable);
			});

			ImGui::Spacing();
			const bool has_changes = m_AssetMaterialDirty;
			if (!has_changes)
				ImGui::BeginDisabled();
			if (ImGui::Button("Apply", ImVec2(ImGui::GetContentRegionAvail().x, 0.0f)))
				ApplyAssetMaterialEdits(absolute_path);
			if (!has_changes)
				ImGui::EndDisabled();
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::SetTooltip(has_changes ? "Save changes to this material file" : "No changes to apply");
		}

		PanelChrome::EndCard(card);
	}

	/* 「Apply」：保存修改并序列化到本地，再同步共享材质缓存 */
	void SceneInspector::ApplyAssetMaterialEdits(const std::filesystem::path& absolute_path)
	{
		PROFILE_FUNCTION();

		if (m_AssetMaterial == nullptr)
			return;

		/* 参数表按当前 Shader 收敛后再落盘（反射是集合的单一来源：面板看不到的不写进文件） */
		m_AssetMaterial->RetainShaderDeclaredParams();

		const std::string path = PathToUtf8(absolute_path);
		if (!MaterialIO::WriteFile(path, *m_AssetMaterial))
			return;

		/* 缓冲已与文件一致：按钮回到禁用（下一帧文件写入时间变化触发的重读也会再清一遍） */
		m_AssetMaterialDirty = false;
		AssetManager::Instance().Refresh<Material>(path, m_AssetMaterial);
	}

	/* 给实体挂 / 换模型（层级里拖入 .mesh 走这里）：
	 * 无组件先补组件（AddComponentCommand）、再设模型（组件快照命令）——两步都可撤销。 */
	void SceneInspector::ApplyDroppedMeshToEntity(Entity entity, const std::string& absolute_path)
	{
		PROFILE_FUNCTION();

		SharedPtr<Model> loaded_model = Model::Create(absolute_path);
		if (loaded_model == nullptr)
			return;

		const ComponentDesc* desc = ComponentRegistry::Instance().Find(std::type_index(typeid(ModelComponent)));
		if (desc == nullptr)
			return;

		if (!entity.HasComponent<ModelComponent>())
		{
			if (m_pCommandStack != nullptr && desc->Add != nullptr)
				m_pCommandStack->Execute(CreateUniquePtr<AddComponentCommand>(entity, desc->Name, desc->Add, desc->Remove));
			else
				entity.AddComponent<ModelComponent>();
		}

		/* 设模型：经组件快照命令入栈（与属性面板的 Replace .mesh 同一条撤销机制） */
		const bool can_record = m_pCommandStack != nullptr && desc->Capture != nullptr && desc->Restore != nullptr;
		ComponentSnapshot before;
		if (can_record)
			desc->Capture(entity, before);

		entity.GetComponent<ModelComponent>().m_Model = std::move(loaded_model);

		if (can_record && before.IsValid())
		{
			ComponentSnapshot after;
			desc->Capture(entity, after);
			if (after.IsValid())
			{
				m_pCommandStack->Execute(CreateUniquePtr<ComponentCustomDrawCommand>(
					entity, desc->Has, desc->Restore, std::move(before), std::move(after), desc->Name));
			}
		}
	}

	/* 绘制单个组件块：卡头（折叠箭头 + 图标 + 组件名 + 右侧菜单）+ 卡身（schema 字段 +
	 * 组件自定义绘制）。卡片版式来自 PanelChrome，各面板共用。
	 * 卡头可以拖着重排，菜单里也有逐格上移 / 下移。 */
	void SceneInspector::DrawComponentBlock(const ComponentDesc& desc, Entity& entity, void* component,
		int order_index, int order_count, const ComponentCardFooter* footer, const std::function<bool()>& body)
	{
		PROFILE_FUNCTION();

		/* 外层已 PushID(组件类型)，卡片的展开状态键因此天然按组件分开 */
		const PanelChrome::Card card = PanelChrome::BeginCard(desc.Name,
			ResolveComponentIcon(desc, component), Icons::Id::Menu, "Component options",
			true, desc.Name);

		/* 自检：记下卡头矩形（按显示顺序），卡片画完再补下沿 */
		const size_t rect_index = m_LastCardRects.size();
		m_LastCardRects.push_back({ desc.Name, card.Min,
			ImVec2(card.Min.x + card.Width, card.Min.y + card.HeaderHeight), card.HeaderHeight });

		if (card.ActionClicked)
			ImGui::OpenPopup("ComponentSettings");

		if (card.Open)
		{
			/* 组件自定义编辑的合并窗口封口（拖动材质参数等 → 每帧快照命令由事务窗口合并为一条） */
			ServiceComponentEditTransaction();

			if (!body)
				DrawComponentFieldsBySchema(desc, entity, component);
			if (body || desc.CustomDraw != nullptr)
			{
				ComponentSnapshot before;
				const bool can_record_custom_edit = m_pCommandStack != nullptr
					&& desc.Capture != nullptr && desc.Restore != nullptr;
				if (can_record_custom_edit)
					desc.Capture(entity, before);

				const bool changed = body ? body() : desc.CustomDraw(component);
				if (changed && can_record_custom_edit && before.IsValid())
				{
					/* 用户正在操作控件时开合并窗口：连续拖动只留一条历史 */
					if (ImGui::IsAnyItemActive() && !m_ComponentEditTransactionOpen)
					{
						m_ComponentEditTransactionOpen = true;
						m_pCommandStack->BeginTransaction();
					}

					ComponentSnapshot after;
					desc.Capture(entity, after);
					if (after.IsValid())
					{
						m_pCommandStack->Execute(CreateUniquePtr<ComponentCustomDrawCommand>(
							entity, desc.Has, desc.Restore, std::move(before), std::move(after), desc.Name));
					}
				}
			}

			/* 卡尾动作（Model 卡的模型编辑器入口）：排在卡身内容之后，收起时跟着藏 */
			if (footer != nullptr && footer->Label != nullptr)
			{
				DrawCardSectionDivider();
				const float button_width = ImGui::GetContentRegionAvail().x;
				if (ImGui::Button(footer->Label, ImVec2(button_width, 0.0f)) && footer->Clicked)
					footer->Clicked();
			}
		}

		const PanelChrome::CardDrop drop = PanelChrome::EndCard(card);

		/* 自检：整张卡的下沿（EndCard 已把光标推到卡下方） */
		m_LastCardRects[rect_index].Max.y =
			ImGui::GetCursorScreenPos().y - ImGui::GetStyle().ItemSpacing.y;

		/* 别的卡片拖到这张卡上：按落点插到前面 / 后面（下一帧按新顺序画） */
		if (drop.Received && drop.Payload != nullptr)
			DropComponentCard(drop.Payload, desc.Name, drop.After);

		/* 删除组件：经命令栈则可连组件数据一起还原 */
		if (ImGui::BeginPopup("ComponentSettings"))
		{
			/* 上下移动：逐格挪，点完不收起（连点一直挪），到边界置灰 */
			const ImGuiSelectableFlags stay_open = ImGuiSelectableFlags_DontClosePopups;
			if (ImGui::Selectable("Move Up", false,
				(order_index > 0) ? stay_open : (stay_open | ImGuiSelectableFlags_Disabled)))
			{
				MoveComponentCard(desc.Name, -1);
			}
			if (ImGui::Selectable("Move Down", false,
				(order_index + 1 < order_count) ? stay_open : (stay_open | ImGuiSelectableFlags_Disabled)))
			{
				MoveComponentCard(desc.Name, 1);
			}

			ImGui::Separator();

			if (ImGui::MenuItem("Remove") && desc.Remove != nullptr)
			{
				if (m_pCommandStack != nullptr)
				{
					m_pCommandStack->Execute(CreateUniquePtr<RemoveComponentCommand>(entity, desc));
				}
				else
				{
					desc.Remove(entity);
				}
			}

			ImGui::EndPopup();
		}
	}

	/* 当前可见的组件卡，按用户存的顺序排：记过的按记录先后，没记过的保持注册顺序跟在后面。
	 * 组件被移除后顺序表里的旧名字自然失效（找不到就不参与排序）。 */
	std::vector<const ComponentDesc*> SceneInspector::CollectOrderedComponentDescs()
	{
		std::vector<const ComponentDesc*> descs;

		/* 全部组件：由 ComponentRegistry 驱动，本类不认识任何具体组件类型。
		 * 例外只有一处：已在面板头部就地编辑的组件（Name）不再单独成卡。 */
		for (const ComponentDesc& desc : ComponentRegistry::Instance().All())
		{
			if (IsEditedInPanelHeader(desc))
				continue;

			if (desc.Has == nullptr || !desc.Has(m_Entity))
				continue;

			void* component = (desc.GetPtr != nullptr) ? desc.GetPtr(m_Entity) : nullptr;
			if (component == nullptr)
				continue;

			/* 组件级条件：如持有对象为空则整块不显示 */
			if (desc.Visible != nullptr && !desc.Visible(component))
				continue;

			descs.push_back(&desc);
		}

		if (m_pOwnerScene == nullptr)
			return descs;

		const std::vector<std::string>& order = m_pOwnerScene->GetComponentOrder(m_Entity);
		if (order.empty())
			return descs;

		std::unordered_map<std::string, size_t> ranks;
		ranks.reserve(order.size());
		for (size_t i = 0; i < order.size(); ++i)
			ranks.emplace(order[i], i);

		/* 记过的按记录先后排，没记过的等价、排在后面（stable_sort 保持注册顺序）。
		 * 没记过的一律用同一个大下标，比较才满足严格弱序 */
		constexpr size_t kUnranked = std::numeric_limits<size_t>::max();
		const auto rank_of = [&ranks](const ComponentDesc& desc)
		{
			const auto iter = ranks.find(desc.Name);
			return (iter != ranks.end()) ? iter->second : kUnranked;
		};

		std::stable_sort(descs.begin(), descs.end(),
			[&rank_of](const ComponentDesc* lhs, const ComponentDesc* rhs)
			{
				return rank_of(*lhs) < rank_of(*rhs);
			});

		return descs;
	}

	std::vector<std::string> SceneInspector::VisibleComponentOrder()
	{
		std::vector<std::string> names;
		for (const ComponentDesc* desc : CollectOrderedComponentDescs())
			names.emplace_back(desc->Name);
		return names;
	}

	void SceneInspector::StoreComponentCardOrder(const std::vector<std::string>& names)
	{
		if (m_pOwnerScene == nullptr || !m_Entity)
			return;

		/* 顺序变了才记一笔：拖拽 / 上移下移都进编辑历史，场景的未保存标记也跟着亮 */
		std::vector<std::string> before = m_pOwnerScene->GetComponentOrder(m_Entity);
		if (before == names)
			return;

		if (m_pCommandStack != nullptr)
		{
			m_pCommandStack->Execute(CreateUniquePtr<SetComponentOrderCommand>(
				m_pOwnerScene, m_Entity, std::move(before), names));
		}
		else
		{
			m_pOwnerScene->SetComponentOrder(m_Entity, names);
		}
	}

	bool SceneInspector::MoveComponentCard(const std::string& name, int delta)
	{
		if (delta == 0)
			return false;

		std::vector<std::string> order = VisibleComponentOrder();
		const auto found = std::find(order.begin(), order.end(), name);
		if (found == order.end())
			return false;

		const int index = static_cast<int>(found - order.begin());
		const int target = index + delta;
		if (target < 0 || target >= static_cast<int>(order.size()))
			return false;

		std::swap(order[static_cast<size_t>(index)], order[static_cast<size_t>(target)]);
		StoreComponentCardOrder(order);
		return true;
	}

	void SceneInspector::DropComponentCard(const std::string& source, const std::string& target, bool after)
	{
		if (source == target)
			return;

		std::vector<std::string> order = VisibleComponentOrder();
		const auto source_it = std::find(order.begin(), order.end(), source);
		if (source_it == order.end())
			return;

		order.erase(source_it);

		/* 摘掉 source 之后再找落点：同一张卡上拖到自己的上 / 下半区不会跑位 */
		const auto target_it = std::find(order.begin(), order.end(), target);
		if (target_it == order.end())
			return;

		order.insert(after ? target_it + 1 : target_it, source);
		StoreComponentCardOrder(order);
	}

	void SceneInspector::ShowEntityComponents()
	{
		PROFILE_FUNCTION();

		m_LastCardRects.clear();

		const std::vector<const ComponentDesc*> descs = CollectOrderedComponentDescs();
		const int count = static_cast<int>(descs.size());

		for (int i = 0; i < count; ++i)
		{
			const ComponentDesc& desc = *descs[static_cast<size_t>(i)];
			void* component = (desc.GetPtr != nullptr) ? desc.GetPtr(m_Entity) : nullptr;

			ImGui::PushID(static_cast<int>(desc.Type.hash_code()));

			/* Model 卡尾部的模型编辑器入口：文件模型才有路径可交（内建 / 空模型不画） */
			ComponentCardFooter model_footer;
			if (component != nullptr && desc.Type == std::type_index(typeid(ModelComponent)))
			{
				const SharedPtr<Model>& model = static_cast<ModelComponent*>(component)->m_Model;
				BuiltinModelType builtin_type{};
				if (model != nullptr && !model->TryGetBuiltinType(builtin_type))
				{
					const std::string path = model->GetPath();
					model_footer.Label = "Open in Model Editor";
					model_footer.Clicked = [this, path]()
					{
						if (m_ModelEditorFunc)
							m_ModelEditorFunc(path);
					};
				}
			}

			std::function<bool()> body;
			if (desc.Type == std::type_index(typeid(TerrainComponent)) && desc.CustomDraw != nullptr)
				body = [&]() { return DrawTerrainPanel(desc, m_Entity, *static_cast<TerrainComponent*>(component)); };
			DrawComponentBlock(desc, m_Entity, component, i, count,
				model_footer.Label != nullptr ? &model_footer : nullptr, body);

			/* 卡头菜单里的 Remove 当场就把组件销毁了，上面拿的指针已经悬空，
			 * 附加卡动笔前重新查一次，组件没了就跳过。 */
			void* live = (desc.Has != nullptr && desc.GetPtr != nullptr && desc.Has(m_Entity))
				? desc.GetPtr(m_Entity)
				: nullptr;

			/* Model 的材质卡 / Camera 的预览卡：各自排在组件卡之后同级平铺
			 * （卡片背景走绘制通道、通道不可嵌套 —— 附加卡画不进组件卡里）。 */
			if (live != nullptr)
			{
				if (desc.Type == std::type_index(typeid(ModelComponent)))
					DrawModelMaterialCards(m_Entity, *static_cast<ModelComponent*>(live));
				else if (desc.Type == std::type_index(typeid(CameraComponent)))
					DrawCameraPreviewCard(*static_cast<CameraComponent*>(live));
				else if (desc.Type == std::type_index(typeid(GrassComponent)))
					DrawGrassMaterialCard(m_Entity, *static_cast<GrassComponent*>(live));
				else if (desc.Type == std::type_index(typeid(TerrainComponent)))
					DrawTerrainMaterialCard(m_Entity, *static_cast<TerrainComponent*>(live));
			}
			ImGui::PopID();
		}
	}

	/* 增加组件按钮：菜单项来自注册表（bAddable == false 的组件不出现，如 Name / Transform）。
	 * 组件一多，菜单很难找 —— 顶部给一个过滤框，打开即自动聚焦。
	 * 图标是全编辑器「新建 / 添加」入口统一的那枚加号。 */
	void SceneInspector::ShowAddComponentButton()
	{
		PROFILE_FUNCTION();

		const float button_size = ImGui::GetFrameHeight();
		if (Icons::IconButton(Icons::Id::NewAsset, ImVec2(button_size, button_size), false, "Add Component"))
			ImGui::OpenPopup("AddComponentPopup");

		if (!ImGui::BeginPopup("AddComponentPopup"))
			return;

		if (ImGui::IsWindowAppearing())
		{
			m_AddComponentFilter[0] = '\0';
			ImGui::SetKeyboardFocusHere();
		}

		ImGui::SetNextItemWidth(Scaled(220.0f));
		Icons::BeginSearchInput();
		ImGui::InputTextWithHint("##AddComponentSearch", "Search...", m_AddComponentFilter,
			sizeof(m_AddComponentFilter));
		Icons::EndSearchInput();
		ImGui::Separator();

		const std::string needle = ToLowercase(m_AddComponentFilter);
		bool any_match = false;

		for (const ComponentDesc& desc : ComponentRegistry::Instance().All())
		{
			if (!desc.bAddable)
				continue;

			for (const ComponentVariant& variant : desc.Variants)
			{
				if (variant.Add == nullptr)
					continue;

				const char* label = (variant.MenuName != nullptr) ? variant.MenuName : desc.Name;
				if (!ContainsCaseInsensitive(label, needle))
					continue;

				any_match = true;

				if (PanelChrome::MenuItemWithIcon(ResolveComponentMenuIcon(desc, variant), label))
				{
					if (m_pCommandStack != nullptr)
					{
						m_pCommandStack->Execute(CreateUniquePtr<AddComponentCommand>(
							m_Entity, desc.Name, variant.Add, desc.Remove));
					}
					else
					{
						variant.Add(m_Entity);
					}

					ImGui::CloseCurrentPopup();
				}
			}
		}

		if (!any_match)
			ImGui::TextDisabled("No matching component");

		ImGui::EndPopup();
	}
}
