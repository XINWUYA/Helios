#include "Pch.h"
#include "EditorApp.h"
#include "MainEditor/MainEditorLayer.h"
#include "SceneEditor/SceneEditorLayer.h"
#include "ModelEditor/ModelEditorLayer.h"

namespace Helios
{
	EditorApp::EditorApp() : Application("Editor", 1920, 1080, true)
	{
		auto main_layer = CreateSharedPtr<MainEditorLayer>();
		auto scene_layer = CreateSharedPtr<SceneEditorLayer>();

		/* 层级面板的材质卡：点击贴图要定位到资源浏览器 ——
		 * 两层互不认识对方，通道在装配层接上（面板不查 Layer）。 */
		scene_layer->SetAssetRevealFunc([main_layer](const std::string& relative_path)
		{
			main_layer->RevealAsset(relative_path);
		});

		/* 反方向的同一条思路：资源浏览器选中资源 → 属性面板显示它的详细内容。
		 * 选中项由浏览器在帧末发布（含"同一项被再次点选"），点实体即切回组件视图。 */
		main_layer->SetAssetSelectionSink(EditorResourceBrowser::AssetSelectionSink{
			[scene_layer](const std::vector<AssetSelectionEntry>& selection)
			{
				scene_layer->SetAssetSelection(selection);
			} });

		PushLayer(main_layer); /* 要先添加 */
		PushLayer(scene_layer);
		PushLayer(CreateSharedPtr<ModelEditorLayer>());
	}
}
