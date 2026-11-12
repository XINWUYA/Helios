#include "SampleScene.h"

#include <imgui.h>

namespace Helios
{
	SampleSceneLayer::SampleSceneLayer() : GameLayer(true)
	{
	}

	void SampleSceneLayer::OnUpdate(float delta_time)
	{
		/* 游戏输入的闸，与编辑器 Game 视图同一道：面板拿着输入时脚本读到的按键与指针一律为空 */
		const ImGuiIO& io = ImGui::GetIO();
		Input::SetGameInput(!io.WantCaptureMouse && !io.WantCaptureKeyboard,
			InputViewport{ glm::vec2(0.0f), glm::vec2(io.DisplaySize.x, io.DisplaySize.y) });

		GameLayer::OnUpdate(delta_time);
	}

	void SampleSceneLayer::OnImGuiRender()
	{
		/* 视图纹理铺满整窗，面板叠在它上面。纹理行序与 ImGui 上下相反，UV 要翻一下 */
		if (Camera* camera = GetCamera())
		{
			if (const SharedPtr<DeviceTexture> texture = camera->GetRenderView()->GetRenderTarget())
			{
				ImGui::GetBackgroundDrawList()->AddImage((ImTextureID)texture.get(),
					ImVec2(0.0f, 0.0f), ImGui::GetIO().DisplaySize, ImVec2{ 0, 1 }, ImVec2{ 1, 0 });
			}
		}

		ImGui::Begin("Stats");
		ImGui::Text("Scene: %s", GetScenePath().c_str());
		ImGui::Text("Scripts: %s", GetScripts().GetStatus().c_str());
		ImGui::Text("- Drag the joystick to move.");
		ImGui::Text("- Drag anywhere else to look around.");
		if (!GetScriptFault().empty())
			ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "Script error: %s", GetScriptFault().c_str());
		ImGui::End();
	}

	bool SampleSceneLayer::IsPointerBlocked() const
	{
		return ImGui::GetIO().WantCaptureMouse;
	}

	bool SampleSceneLayer::IsKeyboardBlocked() const
	{
		return ImGui::GetIO().WantCaptureKeyboard;
	}
}
