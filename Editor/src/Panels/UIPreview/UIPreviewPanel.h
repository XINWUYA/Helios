#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

#include "Command/CommandStack.h"
#include "Helios/Common/Common.h"
#include "Helios/Core/KeyCodes.h"
#include "PreviewCanvas.h"
#include "UIPreviewTextEditor.h"

namespace Helios::UIEditor
{
	/* 「Game UI」面板：把项目里的 .rml 离屏渲染出来，带参考线、选框与就地拖动，
	 * 左侧可以改文档与配套样式，存盘后靠热重载自动刷新。
	 * 画布（PreviewCanvas）只负责渲染与输入，选中与变换在 PreviewSelection 里。
	 * 两种状态分工明确：设计态输入全归编辑器，一个事件都不发给控件；
	 * 交互态反过来，指针与键盘都交给控件，选框与参考线整层不画。 */
	class UIPreviewPanel final
	{
	public:
		/* 每帧推进：磁盘改了就重载，再按上次画出来的尺寸渲一帧 */
		void Update();
		/* 画整个面板（含停靠窗口的 Begin/End） */
		void OnImGuiRender();

		/* 预览拿着键盘时的按键与字符：消费掉返回 true，编辑器快捷键不再响应 */
		bool ProcessKeyDown(KeyCode key);
		bool ProcessKeyUp(KeyCode key);

		/* 外部打开一份文档：绝对路径或相对项目根都行（资源浏览器双击、Canvas 卡的按钮走这里） */
		void OpenDocumentAt(const std::string& path);

		/* 这份文档的编辑历史：面板拿着焦点时，Edit 菜单与 Ctrl+Z 先撤它。
		 * 没打开文档或还没改过时，Can 系列返回 false、名称返回 nullptr */
		bool Undo();
		bool Redo();
		[[nodiscard]] bool CanUndo() const;
		[[nodiscard]] bool CanRedo() const;
		[[nodiscard]] const char* GetUndoLabel() const;
		[[nodiscard]] const char* GetRedoLabel() const;
		/* 面板此刻拿着键盘没有：撤销归属按它切 */
		[[nodiscard]] bool HasFocus() const { return m_Focused; }
		/* 有没有还没写回文件的东西：保存按钮的可用性看它 */
		[[nodiscard]] bool HasUnsavedChanges() const;

	private:
		/* 打开一份文档（路径相对项目根） */
		void OpenDocument(const std::string& document_path);
		/* 切到交互态 / 回到设计态：收掉编辑现场或清掉控件悬停态 */
		void EnterPreviewMode(bool preview);
		/* 把面板的选择与开关写回 <项目>/.helios/editor.cfg */
		void SaveSettings();
		/* 一次存完：代码区文本 + 所有改过的元素。任一失败返回 false 并保留脏标记 */
		bool SaveAll();
		/* 把脏集合里每个元素写回 .rcss / .rml，每个文件只拼一次、写一次 */
		bool FlushDirty();
		/* 脏集合的一条：值都在元素上，这里只记"哪些项动过" */
		struct DirtyEntry
		{
			PreviewSelection::ElementKey Key{};
			/* 本地样式整份刷回 .rcss（变换也是本地样式，一并走这里） */
			bool                     Style{ false };
			/* 已清空、要从 .rcss 删掉的声明名 */
			std::vector<std::string> RemovedStyles{};
			/* 要刷回 .rml 的属性名 */
			std::vector<std::string> Attributes{};
			bool                     Text{ false };
		};
		DirtyEntry& DirtyFor(const PreviewSelection::ElementKey& key);
		void MarkPropertyDirty(const PreviewSelection::ElementKey& key,
			const ElementPropertyDesc& desc, bool removed);
		void MarkTransformDirty(const PreviewSelection::ElementKey& key);
		/* 没有 id 的元素先分一个：写回 .rcss 与撤销都要锚点 */
		PreviewSelection::ElementKey EnsureElementId(const PreviewSelection::ElementKey& key);
		/* 状态行的一句话提示（写回失败之类），几秒后自己消失 */
		void SetStatusNote(const char* note);
		/* 把当前选中元素的各项数值打一行日志：每次手势起止、写回、撤销都记一条 */
		void LogOperation(const char* action) const;
		/* 面板设置只读一次。入口先打开文档时也要提前读 —— 否则 SaveSettings 会拿默认值
		 * 把分辨率、参考线、代码列这些设置盖掉 */
		void LoadSettingsOnce();
		/* 恢复上次那份文档：只有在没有别的入口指定过时才用 */
		void RestoreLastDocument();
		/* 上次打开的文档坏了没有：文件没再改过就先不重试 */
		[[nodiscard]] bool ShouldRetryOpen(const std::string& path) const;
		/* 每份文档一条历史：换回来接着撤，外部改了文件就清掉这一份 */
		CommandStack& CurrentStack();
		/* 只查不建：撤销可用性与菜单名称用 */
		[[nodiscard]] const CommandStack* FindStack() const;
		/* 拖动手势：起手抓一份「改动前」，松手时算一条命令 */
		void BeginGesture();
		void CommitGesture();
		/* 写 UI 文本文件的唯一入口：写完回灌缓冲，并记下自己写的内容 */
		bool WriteUITextFile(const std::string& path, const std::string& text);
		/* 把一次文本改写记进历史（写回样式、代码区存盘共用）；写盘失败返回 false */
		bool ExecuteTextCommand(const char* label, const std::string& path,
			const std::string& before, const std::string& after);
		/* 代码区存盘：Ctrl / ⌘ + S 与工具行按钮共用；写盘失败返回 false */
		bool SaveText();
		/* 磁盘上的内容是不是自己刚写下去的那份（热重载回来时用来认门） */
		[[nodiscard]] bool IsOwnWrite(const std::string& path) const;

		/* 左列：文档的元素树，点行等于在画面上选中 */
		void DrawElementTree(const ImVec2& size);
		/* 预览右侧：选中元素的盒子、属性与变换，改完松手记一条命令 */
		void DrawInspector(const ImVec2& size);
		/* 属性面板的一类卡片：分类名 + 该类的属性行 */
		void DrawPropertyRow(const PreviewSelection::ElementKey& key, const ElementPropertyValue& property);
		/* 文本内容那一行：平时折行显示，点一下才换成输入框 */
		void DrawTextAreaRow(const PreviewSelection::ElementKey& key,
			const ElementPropertyDesc& desc, const ElementPropertyValue& property);
		/* 变换卡：位置 / 旋转 / 缩放与写回样式 */
		void DrawTransformCard();
		/* 属性行改完：一条命令进栈，元素上已经生效 */
		void CommitPropertyEdit(const PreviewSelection::ElementKey& key,
			const ElementPropertyDesc& desc, const std::string& before, const std::string& after);
		/* 把值写到元素上；成功就算「有未保存改动」 */
		bool ApplyPropertyValue(const PreviewSelection::ElementKey& key,
			const ElementPropertyDesc& desc, const std::string& value);
		/* 属性栏的一行数值（标签在左、控件撑满剩余宽度） */
		bool DrawInspectorFloat(const char* label, float& value, float speed, const char* format);
		/* 属性栏一次编辑的起点与结算：拖动中即时生效，松手才记一条命令 */
		void BeginInspectorEdit();
		void CommitInspectorEdit();
		/* 换选中之后把面板侧的旋转 / 缩放数值对齐，选框和元素才对得上 */
		void SyncSelectionNumbers();

		UniquePtr<PreviewCanvas> m_pCanvas{};
		std::string                   m_Path{};
		/* 打不开的文档与当时的修改时间：等文件再存一次才重试，别每帧刷日志 */
		std::string                   m_FailedPath{};
		std::filesystem::file_time_type m_FailedStamp{};
		std::vector<std::string>      m_Documents{};
		/* 设置里记的上次那份文档：面板自己决定恢复时才用 */
		std::string                   m_LastDocument{};
		uint32_t                      m_Width{ 0 };
		uint32_t                      m_Height{ 0 };
		/* 指针按在预览画面里：拖拽期间持续转发，移出去也不中断 */
		bool                          m_PointerDown{ false };
		/* 变换方式：0 移动 / 1 旋转 / 2 缩放；旋转角度与缩放倍数按选中元素累积 */
		int                           m_TransformMode{ 0 };
		float                         m_Rotate{ 0.0f };
		float                         m_Scale{ 1.0f };
		/* 每份文档一条编辑历史，按相对路径存：切走再切回来还接着撤 */
		std::map<std::string, UniquePtr<CommandStack>> m_Stacks{};
		/* 自己最近写下去的内容：热重载回来时拿它认门，不当成外部改动 */
		std::map<std::string, std::string> m_WrittenText{};
		/* 正在拖的那个元素与它的起点状态（松手时合成一条命令）。
		 * 元素没有 id 时靠先序下标认，所以两样都记 */
		PreviewSelection::ElementKey   m_GestureKey{};
		PreviewSelection::ElementState m_GestureBefore{};
		/* 正在拖某个手柄（-1 = 没有）；手柄命中优先于控件 */
		int                           m_HandleDrag{ -1 };
		/* 旋转手势：抓取时记下指针相对中心的夹角与角度起点，拖动时按角度差累加 */
		float                         m_GrabAngle{ 0.0f };
		/* 预览面板拿着键盘：按键与字符先给预览画布 */
		bool                          m_Focused{ false };
		/* 代码编辑区是否展开 */
		bool                          m_Editing{ false };
		/* 预览态：指针直接操作 UI 控件，选框、手柄与变换都停用 */
		bool                          m_PreviewMode{ false };
		/* 编辑缓冲、脏标记与 Document / Style 切换都在这里 */
		UIPreviewTextEditor           m_TextEditor{};
		/* 预览分辨率：0 = 跟随面板，其余按固定宽高渲（模拟目标设备） */
		int                           m_Resolution{ 0 };
		/* 画面缩放与平移：中键拖动平移、Ctrl / ⌘ + 滚轮缩放；参考线开关 */
		float                         m_Zoom{ 1.0f };
		ImVec2                        m_Pan{ 0.0f, 0.0f };
		bool                          m_Panning{ false };
		bool                          m_Guides{ false };
		/* 代码列占可用宽度的比例：拖中间那条分隔条调整 */
		float                         m_CodeFraction{ 0.40f };
		/* 属性列占可用宽度的比例：它在预览右侧、代码列左侧 */
		float                         m_InspectorFraction{ 0.28f };
		/* 左列开关与宽度比例 */
		bool                          m_ShowTree{ true };
		float                         m_TreeFraction{ 0.22f };
		/* 元素树每帧一拍：树的绘制、属性栏、按行选中共用这一份 */
		std::vector<PreviewSelection::ElementNode> m_Elements{};
		/* 树的搜索词：按 id / 标签 / class 过滤，命中的行连同祖先一起留下 */
		std::string                   m_TreeFilter{};
		/* 上次滚动定位到的树行：选中变了才滚一次，别每帧抢滚动条 */
		int                           m_LastTreeIndex{ -1 };
		/* 每行是否折起来了：下标与 m_Elements 一一对应 */
		std::vector<bool>             m_TreeCollapsed{};
		/* 属性栏这次编辑开始前的状态：撤销的起点 */
		PreviewSelection::ElementState m_InspectorBefore{};
		/* 属性面板：这一帧选中元素能改的属性（分类靠 Desc->Category） */
		std::vector<ElementPropertyValue> m_Properties{};
		/* 属性行的输入缓冲：按属性 key 存，打字时不会被每帧重建打断。
		 * 换元素整表清空，缓冲不会串到别的元素上 */
		struct PropertyEdit
		{
			std::string Text{};
			std::string Before{};
			bool        Active{ false };
			bool        Synced{ false };
			/* 文本内容那块正处在编辑态（点开输入框，失焦退出） */
			bool        Editing{ false };
			/* 上一次写回被文档拒绝了（颜色少写一位这类）：行上描警示色，也不记历史 */
			bool        WriteFailed{ false };
		};
		std::map<std::string, PropertyEdit> m_PropertyEdits{};
		std::string                        m_PropertyEditsOwner{};
		/* 刚点开的那一行：下一帧给输入框抢键盘焦点 */
		std::string                        m_FocusTextRow{};
		/* 取色弹层正开着的那一行与它起手的值：开着时不回灌文本，
		 * 关的时候整段合成一条历史 */
		std::string                        m_ColourRow{};
		std::string                        m_ColourBefore{};
		/* 面板状态只从设置读一次；之后每次改动回写（读改写，别覆盖别的设置） */
		bool                          m_SettingsLoaded{ false };
		/* 面板里改过元素（属性 / 变换 / 撤销走了几步）但还没写回文件 */
		bool                          m_Edited{ false };
		/* 待写回的元素：按 id#下标索引，换选中也不会串 */
		std::map<std::string, DirtyEntry> m_Dirty{};
		/* 保存后要恢复的选中 id：写 .rml 会触发重载，元素全换新 */
		std::string                   m_RestoreSelectionId{};
		/* 状态行那句提示与剩余秒数 */
		std::string                   m_StatusNote{};
		float                         m_StatusNoteTime{ 0.0f };
	};
}
