#pragma once
#include <imgui.h>
#include "Helios/ImGui/EditorTheme.h"

namespace Helios
{
/* 设置半透明UI */
#define START_STYLE_ALPHA(alpha) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * (alpha))
#define END_STYLE_ALPHA ImGui::PopStyleVar()

/* 背景透明Button */
#define START_TRANSPARENT_BUTTON ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0))
#define END_TRANSPARENT_BUTTON ImGui::PopStyleColor()

class DeviceTexture;

/* 扩展常用的 UI（"xxxUI" 都是属性行：左标签 + 右控件，标签列宽由 label_width 统一给，
 * 默认取 EditorTheme::Token::PropertyLabelWidth）—— 各组件卡、schema 字段和自定义绘制的
 * 左边界都是对齐的。 */
namespace ImGuiExt
{
	/* 属性行的通用前置/收尾：画标签（弱化色，定宽裁剪）并把光标移到值列，
	 * 返回值列可用宽度。自定义绘制可以复用它来保持同一套行布局。
	 * 成对使用，收尾负责把行内的 ID 作用域弹回去。 */
	float BeginPropertyRow(const char* label, float label_width = EditorTheme::Token::PropertyLabelWidth);
	void EndPropertyRow();

	/* 绘制一个普通的文本UI */
	void DrawCommonTextUI(const std::string& label, const std::string& value, float label_width = EditorTheme::Token::PropertyLabelWidth);
	/* 绘制一个Color UI */
	void DrawColorUI(const std::string& label, glm::vec4& color, float label_width = EditorTheme::Token::PropertyLabelWidth);
	/* 绘制一个资源引用的UI（图片选择 + 拖入 + 悬停预览） */
	void DrawTextureUI(const std::string& label, SharedPtr<DeviceTexture>& texture, float label_width = EditorTheme::Token::PropertyLabelWidth);
	/* 绘制一个可拖动的Int UI */
	void DrawDragIntUI(const char* label, int& value, float label_width = EditorTheme::Token::PropertyLabelWidth);
	/* 绘制一个可拖动的Float UI */
	void DrawDragFloatUI(const std::string& label, float& value, float label_width = EditorTheme::Token::PropertyLabelWidth);
	/* 绘制一个可拖动的Float2 UI */
	void DrawDragFloat2UI(const char* label, glm::vec2& value, float label_width = EditorTheme::Token::PropertyLabelWidth);
	/* 绘制一个可拖动的Float3 UI */
	void DrawDragFloat3UI(const char* label, glm::vec3& value, float label_width = EditorTheme::Token::PropertyLabelWidth);
	/* 绘制一个可拖动的Float4 UI */
	void DrawDragFloat4UI(const char* label, glm::vec4& value, float label_width = EditorTheme::Token::PropertyLabelWidth);
	/* 绘制一个vec3 UI， 带XYZ */
	void DrawVec3ControlUI(const std::string& label, glm::vec3& values, float reset_value = 0.0f, float label_width = EditorTheme::Token::PropertyLabelWidth);
	/* 绘制带选中的图像按钮UI */
	void DrawCheckedImageButtonUI(const std::string& label, const SharedPtr<DeviceTexture>& texture, const ImVec2& size, bool checked = false, const std::function<void()>& button_func = []() {});
	/* 绘制一个Checkbox；返回是否被改动（调用方要"改动即生效"时用它，别每帧无条件写回） */
	bool DrawCheckboxUI(const std::string& label, bool& value, float label_width = EditorTheme::Token::PropertyLabelWidth);
	/* 绘制一个Combo */
	void DrawComboUI(const std::string& label, const std::vector<std::string>& options, int& selected_idx, const std::function<void(int)>& callback = [](int) {}, float label_width = EditorTheme::Token::PropertyLabelWidth);
	/* 绘制一个方向指示 */
	bool DrawDirectionIndicator(const std::string& label, glm::vec3& direction, float label_width = EditorTheme::Token::PropertyLabelWidth);
	/* 绘制一个询问弹窗UI(todo: 还不能用，需要调试) */
	bool DrawModalUI(const std::string& label, const std::string& content_text, bool& never_ask);
}

}
