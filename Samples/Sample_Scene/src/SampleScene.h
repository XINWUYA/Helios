#pragma once
#include <Helios.h>

namespace Helios
{
	class SampleCameraController;

	/* 加载项目里的 Scenes/test.scn 并用一台自带相机渲染 */
	class SampleSceneLayer final : public ILayer
	{
	public:
		SampleSceneLayer();
		~SampleSceneLayer() override = default;

		void OnAttached() override;
		void OnUpdate(float delta_time) override;
		void OnImGuiRender() override;

	private:
		SharedPtr<Scene> m_pScene{ nullptr };
		SharedPtr<SampleCameraController> m_pCameraController{ nullptr };
	};
}
