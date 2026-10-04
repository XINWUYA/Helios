#pragma once
#include <imgui.h>

namespace Helios
{
/* 设置半透明UI */
#define START_STYLE_ALPHA(alpha) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * (alpha))
#define END_STYLE_ALPHA ImGui::PopStyleVar()

/* 背景透明Button */
#define START_TRANSPARENT_BUTTON ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0))
#define END_TRANSPARENT_BUTTON ImGui::PopStyleColor()

class DeviceTexture;

/* 扩展一些常用的UI */
namespace ImGuiExt
{
	/* 绘制一个普通的文本UI */
	void DrawCommonTextUI(const std::string& label, const std::string& value, float label_width = 100.0f);
	/* 绘制一个Color UI */
	void DrawColorUI(const std::string& label, glm::vec4& color, float label_width = 100.0f);
	/* 绘制一个资源引用的UI（图片选择 + 拖入 + 悬停预览） */
	void DrawTextureUI(const std::string& label, SharedPtr<DeviceTexture>& texture, float label_width = 100.0f);
	/* 绘制一个可拖动的Int UI */
	void DrawDragIntUI(const char* label, int& value, float label_width = 100.0f);
	/* 绘制一个可拖动的Float UI */
	void DrawDragFloatUI(const std::string& label, float& value, float label_width = 100.0f);
	/* 绘制一个可拖动的Float2 UI */
	void DrawDragFloat2UI(const char* label, glm::vec2& value, float label_width = 100.0f);
	/* 绘制一个可拖动的Float3 UI */
	void DrawDragFloat3UI(const char* label, glm::vec3& value, float label_width = 100.0f);
	/* 绘制一个可拖动的Float4 UI */
	void DrawDragFloat4UI(const char* label, glm::vec4& value, float label_width = 100.0f);
	/* 绘制一个vec3 UI， 带XYZ */
	void DrawVec3ControlUI(const std::string& label, glm::vec3& values, float reset_value = 0.0f, float label_width = 100.0f);
	/* 绘制带选中的图像按钮UI */
	void DrawCheckedImageButtonUI(const std::string& label, const SharedPtr<DeviceTexture>& texture, const ImVec2& size, bool checked = false, const std::function<void()>& button_func = []() {});
	/* 绘制一个Checkbox */
	void DrawCheckboxUI(const std::string& label, bool& value, float label_width = 100.0f);
	/* 绘制一个Combo */
	void DrawComboUI(const std::string& label, const std::vector<std::string>& options, int& selected_idx, const std::function<void(int)>& callback = [](int) {}, float label_width = 100.0f);
	/* 绘制一个方向指示 */
	bool DrawDirectionIndicator(const std::string& label, glm::vec3& direction, float label_width = 100);
	/* 绘制一个询问弹窗UI(todo: 还不能用，需要调试) */
	bool DrawModalUI(const std::string& label, const std::string& content_text, bool& never_ask);
}

}