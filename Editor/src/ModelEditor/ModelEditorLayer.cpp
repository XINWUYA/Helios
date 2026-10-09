#include "Pch.h"
#include "ModelEditorLayer.h"
#include "Helios/Application/Application.h"
#include "EditorCommon.h"
#include "EditorIcons.h"
#include "PanelChrome.h"
#include "PanelRegistry.h"

namespace Helios
{
	namespace
	{
		/* 向量显示成一行紧凑文本：属性行里数字比括号更重要 */
		std::string FormatVec3(const glm::vec3& value)
		{
			char buffer[64] = {};
			snprintf(buffer, sizeof(buffer), "%.3f, %.3f, %.3f", value.x, value.y, value.z);
			return buffer;
		}

		/* 材质参数的类型名：magic_enum 给的名字顺序就是枚举声明顺序，
		 * 因此下标即 ParamType 的取值 —— 不用再手抄一张数组。 */
		const std::vector<std::string>& ParamTypeNames()
		{
			static const std::vector<std::string> names = GetEnumNames<ParamType>();
			return names;
		}

		/* 切换参数类型：按新类型填一个默认值（原先散在面板里的逻辑，抽出来一处定义） */
		void ApplyParamTypeChange(const SharedPtr<Material>& material,
		                          const MaterialParamInfo& param_info, ParamType type)
		{
			switch (type)
			{
			case ParamType::Texture:
				material->SetTexture(param_info.Name,
					TextureAssetManager::Instance().GetOrCreateTexture(ABSOLUTE_PATH("Textures/Default.png")));
				break;
			case ParamType::Int:   material->SetParameters(ParamType::Int, param_info.Name, 0); break;
			case ParamType::Float: material->SetParameters(ParamType::Float, param_info.Name, 0.0f); break;
			case ParamType::Vec2:  material->SetParameters(ParamType::Vec2, param_info.Name, glm::vec2(0.0f)); break;
			case ParamType::Vec3:  material->SetParameters(ParamType::Vec3, param_info.Name, glm::vec3(0.0f)); break;
			case ParamType::Vec4:  material->SetParameters(ParamType::Vec4, param_info.Name, glm::vec4(0.0f)); break;
			case ParamType::Mat4:  material->SetParameters(ParamType::Mat4, param_info.Name, glm::mat4(0.0f)); break;
			}
		}
	}
	ModelEditorLayer::ModelEditorLayer()
		: ILayer("ModelEditorLayer")
	{
		PROFILE_FUNCTION();
	}

	void ModelEditorLayer::OnAttached()
	{
		PROFILE_FUNCTION();

		m_pEditorCamera = CreateUniquePtr<EditorCamera>();
		m_pDefaultScene = CreateSharedPtr<Scene>();
		/* 编辑器相机登记为外部相机：每帧随场景相机一起收集渲染（见 Scene::AddExternalCamera） */
		m_pDefaultScene->AddExternalCamera(m_pEditorCamera.get());
		m_pModelInfo = CreateUniquePtr<ModelInfo>();
		m_pMaterialGroup = CreateSharedPtr<MaterialGroup>();

		/* 默认在场景中增加一盏方向光，光源颜色为白色 */
		Entity entity = m_pDefaultScene->CreateEntity("DirectionalLight");
		auto& light_component = entity.AddComponent<LightComponent>(LightType::Directional);
		light_component.m_Light->SetColor(glm::vec4(1, 1, 1, 1));
		light_component.m_Light->SetIntensity(1);
		auto& light_transform = entity.GetComponent<TransformComponent>();
		light_transform.m_Rotation = glm::vec3(0, 0, PI/2);
	}

	void ModelEditorLayer::OnDetached()
	{
		PROFILE_FUNCTION();

		/* 相机先于场景销毁：按登记契约摘除 */
		if (m_pDefaultScene && m_pEditorCamera)
			m_pDefaultScene->RemoveExternalCamera(m_pEditorCamera.get());

		ILayer::OnDetached();
	}

	void ModelEditorLayer::OnUpdate(float delta_time)
	{
		PROFILE_FUNCTION();

		if (!m_IsActivated)
			return;
		
		Renderer::SetClearColor(glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
		Renderer::Clear();

		/* 相机导航：指针在视口上才授权起手 */
		m_pEditorCamera->SetNavigationAllowed(m_IsViewportHovered);
		m_pEditorCamera->OnUpdate(delta_time);
		m_pDefaultScene->OnUpdate(delta_time);

		/* 执行本帧收集到的渲染视图：模型视口的内容在这里产生 */
		m_pDefaultScene->Render();
	}

	void ModelEditorLayer::OnImGuiRender()
	{
		PROFILE_FUNCTION();

		if (!m_IsActivated)
			return;
		
		/* 显示模型编辑UI */
		ShowModelParamsUI();
		/* 显示主场景视口 */
		ShowSceneViewportUI();
	}

	void ModelEditorLayer::OnEvent(IEvent* event)
	{
		PROFILE_FUNCTION();

		if (!m_IsActivated)
			return;

		if (!event)
			return;
	}

	/* 显示主场景视口 */
	void ModelEditorLayer::ShowSceneViewportUI()
	{
		PROFILE_FUNCTION();

		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		/* 每帧都必须调用 Begin：窗口被关闭时它返回 false，
		 * 只有继续调用才可能重新显示（关闭时不画内容即可）。 */
		if (ImGui::Begin(Panel::kModel, &m_IsViewportVisible))
		{
			/* 获取窗口范围 */
			const auto viewport_region_min = ImGui::GetWindowContentRegionMin();
			const auto viewport_region_max = ImGui::GetWindowContentRegionMax();
			const auto viewport_offset = ImGui::GetWindowPos();
			m_ViewportRegion.MinX = viewport_region_min.x + viewport_offset.x;
			m_ViewportRegion.MinY = viewport_region_min.y + viewport_offset.y;
			/* 窗口极小时 max 可能小于 min，差值为负；赋给 uint32_t 会回绕成极大值 */
			m_ViewportRegion.Width = static_cast<uint32_t>(std::max(0.0f, viewport_region_max.x - viewport_region_min.x));
			m_ViewportRegion.Height = static_cast<uint32_t>(std::max(0.0f, viewport_region_max.y - viewport_region_min.y));

			/* ImGui 给的是逻辑点，RenderTarget 按物理像素分配；
			 * ClampRenderSize 负责负值/超大值防护。 */
			const auto content_scale = Application::Instance()->GetWindow().GetContentScale();
			const auto rt_size = ClampRenderSize(
				static_cast<float>(m_ViewportRegion.Width),
				static_cast<float>(m_ViewportRegion.Height),
				content_scale);
			m_pEditorCamera->SetViewportRegion({ 0, 0, rt_size.x, rt_size.y });

			/* 指针悬停在视口上才允许相机起手导航（与场景编辑器同源） */
			m_IsViewportHovered = ImGui::IsWindowHovered();

			/* 绘制场景 */
			auto output_rt = m_pEditorCamera->GetRenderView()->GetRenderTarget();
			if (output_rt)
			{
				const auto viewport_panel_size = ImGui::GetContentRegionAvail();
				/* ImTextureID 统一存放 DeviceTexture 指针：GetTextureID() 返回的硬件
				 * 句柄在 Metal 上是 64 位指针被截断后的 32 位，不能当指针使用。 */
				ImGui::Image((ImTextureID)output_rt.get(), viewport_panel_size, ImVec2{ 0, 1 }, ImVec2{ 1, 0 });
			}

			/* 拖动资源到主窗口 */
			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("RESOURCE_BROWSER_ITEM"))
				{
					std::filesystem::path relative_path;
					if (payload->DataSize > 1 && TryPathFromUtf8Payload(
						payload->Data, static_cast<size_t>(payload->DataSize), relative_path))
						OnDragItemToScene(g_AssetsPath / relative_path);
				}
				ImGui::EndDragDropTarget();
			}

			///* Gizmos */
			//ShowOperationGizmoUI();
		}
		else
		{
			/* 视口窗口不可见：清掉悬停，避免残留的起手授权 */
			m_IsViewportHovered = false;
		}
		ImGui::End();
		ImGui::PopStyleVar();
	}

	/* 显示模型编辑UI：头部（模型文件名）+ 基本信息卡 + 每个子模型一张卡。
	 * 材质参数不再各占一层 Tree：参数名是可折叠标题，类型选择器贴右端，
	 * 值走属性行 —— 与组件字段同一套布局。 */
	void ModelEditorLayer::ShowModelParamsUI()
	{
		PROFILE_FUNCTION();

		const bool has_model = (m_pModelInfo != nullptr) && !m_pModelInfo->m_SubModelInfos.empty();

		ImGui::Begin(Panel::kModelHelper);
		{
			/* 面板标题由页签承担（"Model Helper"）；当前文件由 Base Info 卡的 Path 给出 */

			if (has_model)
			{
				ShowModelBaseInfoCard();
				ShowSubModelCards();
			}
			else
			{
				ShowEmptyModelHint();
			}
		}
		ImGui::End();
	}

	/* 基本信息：整份模型的路径、包围盒与规模 */
	void ModelEditorLayer::ShowModelBaseInfoCard()
	{
		PROFILE_FUNCTION();

		const PanelChrome::Card card = PanelChrome::BeginCard("Base Info", Icons::Id::Model);

		if (card.Open)
		{
			ImGuiExt::DrawCommonTextUI("Path", m_pModelInfo->m_Path);
			ImGuiExt::DrawCommonTextUI("AABB Min", FormatVec3(m_pModelInfo->m_AABB.first));
			ImGuiExt::DrawCommonTextUI("AABB Max", FormatVec3(m_pModelInfo->m_AABB.second));
			ImGuiExt::DrawCommonTextUI("Vertices",
				std::to_string(m_pModelInfo->m_Attributes.vertices.size() / 3));
			ImGuiExt::DrawCommonTextUI("Mesh Segments",
				std::to_string(m_pModelInfo->m_SubModelInfos.size()));
		}

		PanelChrome::EndCard(card);
	}

	/* 每个子模型一张卡：规模信息 + 它的材质参数 */
	void ModelEditorLayer::ShowSubModelCards()
	{
		PROFILE_FUNCTION();

		for (size_t i = 0; i < m_pModelInfo->m_SubModelInfos.size(); ++i)
		{
			const auto& sub_model_info = m_pModelInfo->m_SubModelInfos[i];
			const auto& material = m_pMaterialGroup->GetMaterialByIndex(i);
			if (sub_model_info == nullptr || material == nullptr)
				continue;

			ImGui::PushID(static_cast<int>(i));

			const PanelChrome::Card card = PanelChrome::BeginCard(
				sub_model_info->Name.empty() ? "Unnamed" : sub_model_info->Name.c_str(), Icons::Id::Model);

			if (card.Open)
			{
				ImGuiExt::DrawCommonTextUI("AABB Min", FormatVec3(sub_model_info->AABB.first));
				ImGuiExt::DrawCommonTextUI("AABB Max", FormatVec3(sub_model_info->AABB.second));
				ImGuiExt::DrawCommonTextUI("Vertices",
					std::to_string(sub_model_info->VertexArray->GetVertexCount()));

				/* 参数区：一行的分组标题 + 每个参数一个可折叠块 */
				ImGui::Separator();
				ImGui::TextColored(EditorTheme::Token::TextLabel, "Material Parameters");

				for (const auto& param : material->GetAllParameters())
					ShowMaterialParameter(material, sub_model_info, param.second);
			}

			PanelChrome::EndCard(card);
			ImGui::PopID();
		}
	}

	/* 一个材质参数：参数名是可折叠标题，卡身里先是类型选择器、再是值。
	 * 值按类型走属性行 —— 与组件字段同一套布局。 */
	void ModelEditorLayer::ShowMaterialParameter(const SharedPtr<Material>& material,
	                                            const SharedPtr<SubModelInfo>& sub_model_info,
	                                            const MaterialParamInfo& param_info)
	{
		PROFILE_FUNCTION();

		ImGui::PushID(param_info.Name.c_str());

		if (ImGui::TreeNodeEx(param_info.Name.c_str(),
			ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen))
		{
			/* 类型选择器放最上面：它是这个参数的"元信息"，值才是内容 */
			const std::vector<std::string>& type_names = ParamTypeNames();
			int type_index = static_cast<int>(param_info.Type);
			if (type_index < 0 || type_index >= static_cast<int>(type_names.size()))
				type_index = 0;

			ImGuiExt::DrawComboUI("DataType", type_names, type_index,
				[material, &param_info](int selected)
				{
					ApplyParamTypeChange(material, param_info, static_cast<ParamType>(selected));
				});

			if (param_info.Type == ParamType::Texture)
			{
				/* 贴图参数的 Value 是 pair<贴图, 绑定点>：按值取出来，改完再写回材质 */
				auto texture_info = std::any_cast<std::pair<SharedPtr<DeviceTexture>, uint32_t>>(param_info.Value);
				auto& texture = texture_info.first;
				TextureLoadConfig load_config = texture->GetTextureLoadConfig();

				const SharedPtr<DeviceTexture> previous = texture;
				ImGuiExt::DrawTextureUI("Texture", texture);

				/* 拖入新贴图：应用到材质。Ambient 另外记下路径 —— 材质导出走这条路径 */
				if (texture != previous)
				{
					material->SetTexture(param_info.Name, texture);
					if (param_info.Name == "Ambient")
						sub_model_info->MaterialParams.AmbientTexPath = RELATIVE_PATH(texture->GetPath());
				}

				const bool flip_changed = ImGuiExt::DrawCheckboxUI("Flip V", load_config.IsFlipV);
				const bool mips_changed = ImGuiExt::DrawCheckboxUI("Gen Mips", load_config.IsGenMips);

				/* 加载选项变了就按新选项重新加载贴图 */
				if (flip_changed || mips_changed)
				{
					material->SetTexture(param_info.Name,
						TextureAssetManager::Instance().GetOrCreateTexture(texture->GetPath(), load_config));
				}
			}
			else
			{
				switch (param_info.Type)
				{
				case ParamType::Int:
				{
					int value = std::any_cast<int>(param_info.Value);
					ImGuiExt::DrawDragIntUI("Value", value);
					material->SetParameters(ParamType::Int, param_info.Name, value);
					break;
				}
				case ParamType::Float:
				{
					float value = std::any_cast<float>(param_info.Value);
					ImGuiExt::DrawDragFloatUI("Value", value);
					material->SetParameters(ParamType::Float, param_info.Name, value);
					break;
				}
				case ParamType::Vec2:
				{
					glm::vec2 value = std::any_cast<glm::vec2>(param_info.Value);
					ImGuiExt::DrawDragFloat2UI("Value", value);
					material->SetParameters(ParamType::Vec2, param_info.Name, value);
					break;
				}
				case ParamType::Vec3:
				{
					glm::vec3 value = std::any_cast<glm::vec3>(param_info.Value);
					ImGuiExt::DrawVec3ControlUI("Value", value, 0.0f);
					material->SetParameters(ParamType::Vec3, param_info.Name, value);
					break;
				}
				case ParamType::Vec4:
				{
					glm::vec4 value = std::any_cast<glm::vec4>(param_info.Value);
					ImGuiExt::DrawDragFloat4UI("Value", value);
					material->SetParameters(ParamType::Vec4, param_info.Name, value);
					break;
				}
				default:
					/* Mat4 还没有对应的行控件：明确说"没有编辑器"，
					 * 而不是画一片空白让人以为参数丢了 */
					ImGuiExt::DrawCommonTextUI("Value", "(no editor)");
					break;
				}
			}

			ImGui::TreePop();
		}

		ImGui::PopID();
	}

	/* 未导入模型时的提示：空面板要说清"为什么是空的、该怎么办" */
	void ModelEditorLayer::ShowEmptyModelHint()
	{
		PROFILE_FUNCTION();

		static constexpr const char* kHint = "Drag an .obj file into the viewport to load a model";

		const float wrap_width = ImGui::GetContentRegionAvail().x;
		const float text_width = ImGui::CalcTextSize(kHint).x;

		ImGui::Dummy(ImVec2(0.0f, ImGui::GetContentRegionAvail().y * 0.3f));

		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap_width);
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImMax(0.0f, (wrap_width - text_width) * 0.5f));
		ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::TextDim);
		ImGui::TextUnformatted(kHint);
		ImGui::PopStyleColor();
		ImGui::PopTextWrapPos();
	}

	/* 响应拖拽文件到主窗口 */
	void ModelEditorLayer::OnDragItemToScene(const std::filesystem::path& path)
	{
		PROFILE_FUNCTION();

		const auto extension = PathToUtf8(path.extension());

		/* 模型文件 */
		if (extension == ".obj")
		{
			EDITOR_LOG_DEBUG("Import model file: {}.", PathToUtf8(path));

			/* 读取模型 */
			m_pModelInfo->Reset();
			m_pModelInfo->LoadFromObj(PathToUtf8(path));

			/* 初始化材质 */
			m_pMaterialGroup->ClearAllMaterials();
			for (size_t i = 0; i < m_pModelInfo->m_SubModelInfos.size(); ++i)
				m_pMaterialGroup->EmplaceMaterial(Material::Create(ShaderAssetManager::Instance().GetOrLoad("Shaders/DeferredShaders/GBufferMaterial.glsl")));

			/* 更新场景模型信息 */
			UpdateModel();

			return;
		}
	}

	/* 导入模型 */
	void ModelEditorLayer::ImportModel()
	{
		PROFILE_FUNCTION();

		const auto file_path = FileDialog::OpenFile("Obj(*.obj)\0*.obj\0");
		if (!file_path.empty())
		{
			/* 读取模型 */
			m_pModelInfo->Reset();
			m_pModelInfo->LoadFromObj(file_path);

			/* 初始化材质 */
			m_pMaterialGroup->ClearAllMaterials();
			for (size_t i = 0; i < m_pModelInfo->m_SubModelInfos.size(); ++i)
				m_pMaterialGroup->EmplaceMaterial(Material::Create(ShaderAssetManager::Instance().GetOrLoad("Shaders/DeferredShaders/GBufferMaterial.glsl")));

			/* 更新场景模型信息 */
			UpdateModel();

			/* 模型已就绪：层与视口窗口一并唤回，导入结果必然可见 */
			m_IsActivated = true;
			m_IsViewportVisible = true;
		}
	}

	/* 拷贝指定格式的文件到目标路径 */
	static bool CopyFileFromTo(const std::filesystem::path& src_path, const std::filesystem::path& dst_path, const std::regex& suffix)
	{
		PROFILE_FUNCTION();

		if (src_path.empty() || src_path.empty())
			return false;

		if (!std::filesystem::exists(src_path))
			return false;

		for (auto& item : std::filesystem::directory_iterator(src_path))
		{
			std::filesystem::path dst_item_path = dst_path / item.path().filename();
			if (std::filesystem::is_directory(item.status()))
			{
				std::filesystem::create_directory(dst_item_path);
				CopyFileFromTo(item.path(), dst_item_path, suffix);
			}
			else
			{
				if (std::regex_match(item.path().extension().string(), suffix))
				{
					std::filesystem::copy_file(item.path(), dst_item_path, std::filesystem::copy_options::skip_existing);
				}
			}
		}
		return true;
	}

	/* 导出模型 */
	void ModelEditorLayer::ExportMeshAndMtl()
	{
		PROFILE_FUNCTION();

		if (!m_pModelInfo || !m_pModel)
			return;

		const auto file_path = FileDialog::SaveFile("Mesh(*.mesh)\0*.mesh\0");
		if (!file_path.empty())
		{
			/* 导出模型 */
			ExportMesh(file_path);

			/* 导出材质：把导入时的材质名写进槽表（加载后覆盖表按槽名匹配） */
			for (size_t i = 0; i < m_pModelInfo->m_SubModelInfos.size(); ++i)
			{
				m_pMaterialGroup->SetEntrySlotName(static_cast<int>(i),
					m_pModelInfo->m_SubModelInfos[i]->MaterialParams.Name);
			}
			const auto& material_path = ReplaceFileSuffix(file_path, ".mtl");
			m_pMaterialGroup->Serializer(material_path);

			/* 拷贝贴图文件 */
			std::filesystem::path current_model_path = PathFromUtf8(m_pModelInfo->m_Path);
			std::filesystem::path target_path = PathFromUtf8(file_path);
			const std::regex pattern("^[\s\S]*\.(pdf|png|jpeg|jpg|tga|bmp|dds)$");
			CopyFileFromTo(current_model_path.parent_path(), target_path.parent_path(), pattern);
		}
	}

	/* 导出Mesh */
	void ModelEditorLayer::ExportMesh(const std::string& path)
	{
		PROFILE_FUNCTION();
		
		std::ofstream out_mesh_file(PathFromUtf8(path), std::ios::out | std::ios::binary);

		/* 写入子模型数量: size_t * 1 */
		size_t sub_model_count = m_pModelInfo->m_SubModelInfos.size();
		out_mesh_file.write((char*)&sub_model_count, sizeof(size_t));

		/* 逐个写入子模型信息 */
		for (int i = 0; i < m_pModelInfo->m_SubModelInfos.size(); ++i)
		{
			const auto& sub_model_info = m_pModelInfo->m_SubModelInfos[i];

			/* 写入Name的size: size_t * 1 */
			size_t name_size = sub_model_info->Name.size();
			out_mesh_file.write((char*)&name_size, sizeof(size_t));

			/* 写入Name内容：name_size */
			out_mesh_file.write(sub_model_info->Name.c_str(), name_size);

			/* 写入VertexData的大小: size_t * 1 */
			size_t data_size = sub_model_info->VertexData.size();
			out_mesh_file.write((char*)&data_size, sizeof(size_t));

			/* 写入VertexData内容: data_size */
			out_mesh_file.write((char*)(sub_model_info->VertexData.data()), data_size * sizeof(float));

			/* 写入VertexLayout */
			auto& vertex_layout = sub_model_info->VertexArray->GetVertexBuffers()[0]->GetLayout();
			auto& elements = vertex_layout.GetElements();

			/* 写入 VertexLayout的Element数量: size_t * 1 */
			size_t element_count = elements.size();
			out_mesh_file.write((char*)&element_count, sizeof(size_t));
			/* 逐个写入Element信息 */
			for (auto& element : elements)
			{
				/* 写入Name的size: size_t * 1 */
				size_t element_name_size = element.Name.size();
				out_mesh_file.write((char*)&element_name_size, sizeof(size_t));
				/* 写入Name内容：element_name_size */
				out_mesh_file.write(element.Name.c_str(), element_name_size);
				/* 写入Element的类型: uint8_t * 1 */
				out_mesh_file.write((char*)&element.Type, sizeof(uint8_t));
				/* 写入Element的偏移：size_t * 1 */
				out_mesh_file.write((char*)&element.Offset, sizeof(size_t));
				/* 写入Element的Normalized: bool * 1 */
				out_mesh_file.write((char*)&element.Normalized, sizeof(bool));
			}

			/* 写入材质索引：int * 1（= 加载侧的槽索引，与 .mtl 条目对位） */
			auto material = m_pMaterialGroup->GetMaterialByIndex(i);
			int material_id = -1;
			for (int j = 0; j < static_cast<int>(m_pMaterialGroup->GetEntries().size()); ++j)
			{
				const auto& mtl = m_pMaterialGroup->GetMaterialByIndex(j);
				if (mtl == material)
				{
					material_id = j;
					break;
				}
			}
			out_mesh_file.write((char*)&material_id, sizeof(int));

			/* 写入aabb: sizeof(glm::vec3) * 2 */
			out_mesh_file.write((char*)&sub_model_info->AABB.first, sizeof(glm::vec3));
			out_mesh_file.write((char*)&sub_model_info->AABB.second, sizeof(glm::vec3));
		}

		/* 保存Mesh文件 */
		out_mesh_file.close();
	}

	/* 更新模型 */
	void ModelEditorLayer::UpdateModel()
	{
		PROFILE_FUNCTION();

		/* 先移除场景中的其他模型 */
		m_pDefaultScene->DestroyTargetEntities<ModelComponent>();

		/* 新建模型 */
		m_pModel = CreateSharedPtr<Model>(m_pModelInfo->m_Path);
		for (size_t i = 0; i < m_pModelInfo->m_SubModelInfos.size(); ++i)
		{
			auto& sub_model_info = m_pModelInfo->m_SubModelInfos[i];
			auto& material = m_pMaterialGroup->GetMaterialByIndex(i);

			/* 更新材质 */
			UpdateMaterial(material, sub_model_info->MaterialParams);

			/* 槽：每个子模型一个（槽名 = 导入时的材质名，与导出 / 加载链对位） */
			m_pModel->AddMaterialSlot(
				sub_model_info->MaterialParams.Name.empty()
					? ("slot_" + std::to_string(i))
					: sub_model_info->MaterialParams.Name,
				material);

			SharedPtr<MeshSegment> mesh_segment = CreateSharedPtr<MeshSegment>(
				sub_model_info->Name, sub_model_info->VertexArray, static_cast<int>(i));
			mesh_segment->SetAABB(sub_model_info->AABB.first, sub_model_info->AABB.second);

			m_pModel->AddMeshSegment(mesh_segment);
		}

		/* 将模型添加到场景中 */
		Entity entity = m_pDefaultScene->CreateEntity(m_pModel->GetDebugName());
		auto& mesh_component = entity.AddComponent<ModelComponent>();
		mesh_component.m_Model = m_pModel;

		/* 根据模型大小自适应相机距离 */
		const auto& aabb_min = m_pModel->GetAABBMin();
		const auto& aabb_max = m_pModel->GetAABBMax();
		const auto aabb_height = aabb_max.y - aabb_min.y;
		const auto distance = (aabb_height) / std::tan(m_pEditorCamera->GetFov() / 2.0f);
		m_pEditorCamera->SetDistance(distance);
		m_pEditorCamera->SetFocalPoint((aabb_min + aabb_max) * 0.5f);
	}

	/* 更新材质：根据材质参数设置材质 */
	void ModelEditorLayer::UpdateMaterial(const SharedPtr<Material>& material, const MaterialParams& material_params)
	{
		PROFILE_FUNCTION();

		TextureLoadConfig load_config;
		load_config.IsFlipV = false;
		{
			/* Ambient */
			if (!material_params.AmbientTexPath.empty())
				material->SetTexture("Ambient", TextureAssetManager::Instance().GetOrCreateTexture(ABSOLUTE_PATH(material_params.AmbientTexPath), load_config));
			else
				material->SetParameters(ParamType::Vec3, "Ambient", material_params.Ambient);

			/* Diffuse/Albedo */
			if (!material_params.DiffuseTexPath.empty())
				material->SetTexture("Albedo", TextureAssetManager::Instance().GetOrCreateTexture(ABSOLUTE_PATH(material_params.DiffuseTexPath), load_config));
			else
				material->SetParameters(ParamType::Vec3, "Albedo", material_params.Diffuse);

			/* Specular */
			if (!material_params.SpecularTexPath.empty())
				material->SetTexture("Specular", TextureAssetManager::Instance().GetOrCreateTexture(ABSOLUTE_PATH(material_params.SpecularTexPath), load_config));
			else
				material->SetParameters(ParamType::Vec3, "Specular", material_params.Specular);

			/* Normal, todo: 处理Bump和Displacement */
			if (!material_params.BumpTexPath.empty())
				material->SetTexture("Normal", TextureAssetManager::Instance().GetOrCreateTexture(ABSOLUTE_PATH(material_params.BumpTexPath), load_config));
			else if (!material_params.DisplacementTexPath.empty())
				material->SetTexture("Normal", TextureAssetManager::Instance().GetOrCreateTexture(ABSOLUTE_PATH(material_params.DisplacementTexPath), load_config));
			else
				material->SetParameters(ParamType::Vec3, "Normal", glm::vec3(0.0f, 0.0f, 1.0f));

			/* Roughness */
			if (!material_params.RoughnessTexPath.empty())
				material->SetTexture("Roughness", TextureAssetManager::Instance().GetOrCreateTexture(ABSOLUTE_PATH(material_params.RoughnessTexPath), load_config));
			else
				material->SetParameters(ParamType::Float, "Roughness", material_params.Roughness);

			/* Metallic */
			if (!material_params.MetallicTexPath.empty())
				material->SetTexture("Metallic", TextureAssetManager::Instance().GetOrCreateTexture(ABSOLUTE_PATH(material_params.MetallicTexPath), load_config));
			else
				material->SetParameters(ParamType::Float, "Metallic", material_params.Metallic);

			/* Emission */
			if (!material_params.EmissionTexPath.empty())
				material->SetTexture("Emissive", TextureAssetManager::Instance().GetOrCreateTexture(ABSOLUTE_PATH(material_params.EmissionTexPath), load_config));
			else
				material->SetParameters(ParamType::Vec3, "Emissive", material_params.Emission);

			/* ClearCoat */
			material->SetParameters(ParamType::Float, "ClearCoatRoughness", material_params.ClearCoatRoughness);
			material->SetParameters(ParamType::Float, "ClearCoatThickness", material_params.ClearCoatThickness);

			/* Others */
			material->SetParameters(ParamType::Vec3, "Transmittance", material_params.Transmittance);
			material->SetParameters(ParamType::Float, "IOR", material_params.IOR);
		}
	}
}
