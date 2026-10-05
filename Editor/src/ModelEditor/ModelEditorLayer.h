#pragma once
#include "EditorBuiltinCamera.h"
#include "EditorResourceBrowser.h"
#include "ModelInfo.h"

namespace Helios
{
	/* 模型编辑器
	 * ：模型导入
	 * ：材质编辑
	 */
	class ModelEditorLayer final : public ILayer
	{
	public:
		ModelEditorLayer();
		~ModelEditorLayer() override = default;

		void OnAttached() override;
		void OnDetached() override;
		void OnUpdate(float delta_time) override;
		void OnImGuiRender() override;
		void OnEvent(IEvent* event) override;

		/* 激活窗口 */
		void Active(bool active = true) { m_IsActivated = active; }
		bool IsActivated() const { return m_IsActivated; }

		/* 模型视口窗口显隐（与 m_IsActivated 分开，理由同 SceneEditorLayer） */
		void SetViewportVisible(bool visible) { m_IsViewportVisible = visible; }
		[[nodiscard]] bool IsViewportVisible() const { return m_IsViewportVisible; }

		/* 导入模型 */
		void ImportModel();
		/* 导出模型(.mesh & .mtl) */
		void ExportMeshAndMtl();
		/* 导出Mesh */
		void ExportMesh(const std::string& path);

	private:
		/* 显示主场景视口 */
		void ShowSceneViewportUI();
		/* 显示模型编辑UI */
		void ShowModelParamsUI();
		/* 模型信息：基本信息卡 + 每个子模型一张卡 */
		void ShowModelBaseInfoCard();
		void ShowSubModelCards();
		/* 一个材质参数：参数名做成可折叠标题，类型选择器贴右端，值走属性行 */
		void ShowMaterialParameter(const SharedPtr<Material>& material,
		                          const SharedPtr<SubModelInfo>& sub_model_info,
		                          const MaterialParamInfo& param_info);
		/* 未导入模型时的空状态提示 */
		void ShowEmptyModelHint();
		/* 响应拖拽文件到主窗口 */
		void OnDragItemToScene(const std::filesystem::path& path);

		/* 更新模型 */
		void UpdateModel();
		/* 更新材质：当用户修改时根据材质参数设置材质 */
		void UpdateMaterial(const SharedPtr<Material>& material, const MaterialParams& material_params);

		/* 默认场景 */
		SharedPtr<Scene> m_pDefaultScene{ nullptr };
		/* 编辑器相机 */
		UniquePtr<EditorCamera> m_pEditorCamera{ nullptr };
		/* 视口范围: x: width_min; y: height_min; z: width_max; w: height_max */
		ViewportRegion m_ViewportRegion{};

		/* 从文件中加载模型信息 */
		UniquePtr<ModelInfo> m_pModelInfo{ nullptr };

		/* 当前模型 */
		SharedPtr<Model> m_pModel{ nullptr };
		/* 当前模型对应的材质组，其中每个Material对应一个子模型 */
		SharedPtr<MaterialGroup> m_pMaterialGroup{ nullptr };

		/* 当前窗口是否被激活 */
		bool m_IsActivated{ false };
		/* 模型视口窗口是否显示 */
		bool m_IsViewportVisible{ true };
	};
}
