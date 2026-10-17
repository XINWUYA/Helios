#pragma once
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include <entt/entt.hpp>
#include "Helios/Scene/Entity.h"
#include "Helios/Scene/AnimationClip.h"
#include "TimelineModel.h"

namespace Helios
{
	class Scene;
	class CommandStack;
	struct AnimatorComponent;

	/* 「Timeline」面板：选中实体（或它最近的带 Animator 的祖先）的动画片段编辑。
	 * 播放头、轨道列表、关键帧增删改与框选、录制、曲线视图；片段改动走命令栈、落盘即保存。 */
	class TimelinePanel final
	{
	public:
		/* 画整个面板；visible 与窗口右上角关闭按钮同源 */
		void OnImGuiRender(Scene* scene, const Entity& selected_entity, CommandStack* command_stack, bool& visible);

		/* 录制检测：要在视口 gizmo 编辑之后调用，才能抓到同一帧的 gizmo 改动 */
		void TickRecording(Scene* scene, const Entity& selected_entity);

		/* ---- 程序化入口（自检 / 自动化验证用）---- */
		[[nodiscard]] bool IsRecording() const { return m_Recording; }
		void SetRecording(bool recording);
		/* 循环播放开关：改的是动画组件的 Loop（走组件字段命令，可撤销、会弄脏场景） */
		bool ToggleLoop();

		[[nodiscard]] int GetTrackCount() const;
		[[nodiscard]] int GetSelectedTrack() const { return m_SelectedTrack; }
		[[nodiscard]] int GetSelectedKeyCount() const { return static_cast<int>(m_Selection.size()); }
		[[nodiscard]] bool IsKeySelected(int track, int key) const;

		void SelectTrack(int index);
		/* 单选一个关键帧（清掉其它选择） */
		void SelectKey(int track, int key);

		/* 在播放头位置加一条轨道并落一个关键帧；同目标已存在时只选中它 */
		bool AddTrack(const TimelineModel::PropertyEntry& entry);
		/* 删掉选中的所有关键帧 / 当前选中的轨道 */
		bool DeleteSelectedKeys();
		bool DeleteSelectedTrack();
		/* 关键帧编辑：复制 / 粘贴到播放头 / 整体平移到某个时间增量 */
		bool CopySelectedKeys();
		bool PasteSelectedKeys();
		/* 粘到指定时间（右键菜单用：落点就是点中的那一帧） */
		bool PasteSelectedKeys(float base_time);
		bool NudgeSelectedKeys(float delta);
		/* 独立编辑一个 .anim：不依赖实体上的 Animator，录制 / 加轨道这些场景相关入口会禁用 */
		void OpenClip(SharedPtr<AnimationClip> clip);
		void CloseClip();
		[[nodiscard]] bool IsStandalone() const { return m_StandaloneClip != nullptr; }

		/* 方向键此刻归不归时间轴：鼠标停在时刻区（含曲线区）且面板有片段时为真。
		 * 主壳层据此在帧首吃掉 ImGui 的导航移动请求，方向键就只做微调，Tab 导航不受影响 */
		[[nodiscard]] bool WantsArrowKeys() const;

		/* 独立编辑时的"关闭片段"按钮矩形（自检点它） */
		[[nodiscard]] const ImVec2& GetLastCloseClipMin() const { return m_LastCloseClipMin; }
		[[nodiscard]] const ImVec2& GetLastCloseClipMax() const { return m_LastCloseClipMax; }

		/* 关键帧右键菜单：复制 / 粘贴 / 插值 / 删除 */
		[[nodiscard]] bool IsKeyContextMenuOpen() const { return m_KeyMenuOpen; }
		[[nodiscard]] const ImVec2& GetLastKeyMenuDeleteMin() const { return m_KeyMenuDeleteMin; }
		[[nodiscard]] const ImVec2& GetLastKeyMenuDeleteMax() const { return m_KeyMenuDeleteMax; }

		/* 事件：在标尺上打点、改名、删除（都进撤销） */
		bool AddEventAt(float time, const char* name);
		bool RenameEvent(int index, const char* name);
		bool RemoveEventAt(int index);
		[[nodiscard]] int GetEventCount() const;
		/* 改当前轨道的插值方式（Step / Linear / Cubic） */
		bool SetTrackInterp(KeyInterp interp);

		/* 移动播放头（与拖标尺同一路径：定格姿势、不推进） */
		void ScrubTo(float time);
		/* 播放头贴到某个关键帧上并进入定格：改值/拖帧时场景能立刻看到这一帧 */
		void JumpToKey(int track, int key);

		/* 轨道搜索：按名字过滤（自检也用它设过滤词） */
		void SetTrackFilter(const std::string& filter);
		[[nodiscard]] const char* GetTrackFilter() const { return m_TrackFilter; }
		[[nodiscard]] int GetVisibleTrackCount() const { return static_cast<int>(m_VisibleTracks.size()); }

		/* 轨道区横向缩放 / 滚动（1 倍 = 整段刚好铺满；放大后可横向滑动） */
		void SetZoom(float zoom);
		void SetScrollPixels(float scroll);
		[[nodiscard]] float GetZoom() const { return m_Zoom; }
		[[nodiscard]] float GetScrollPixels() const { return m_ScrollX; }
		[[nodiscard]] float GetContentWidth() const;
		[[nodiscard]] float GetMaxScrollPixels() const;

		/* 右侧属性编辑栏：宽度可拖，属性名一列宽度也可拖（收起后轨道区变宽） */
		void SetInspectorOpen(bool open) { m_InspectorOpen = open; }
		void SetInspectorWidth(float width) { m_InspectorWidth = width; }
		void SetInspectorLabelWidth(float width) { m_InspectorLabelWidth = width; }
		[[nodiscard]] bool IsInspectorOpen() const { return m_InspectorOpen; }

		/* 轨道行纵向滚动：行多到放不下时只滚这一层，标尺 / 属性栏 / 曲线留在原位 */
		void SetScrollRows(float pixels);
		[[nodiscard]] float GetScrollRows() const { return m_ScrollY; }
		[[nodiscard]] float GetMaxScrollRows() const;
		[[nodiscard]] float GetLastRowViewHeight() const { return m_RowViewHeight; }

		/* 曲线视图：按分量画出选中轨道的值曲线，点可直接拖 */
		void SetCurveView(bool on) { m_CurveView = on; }
		[[nodiscard]] bool IsCurveView() const { return m_CurveView; }
		void SetCurveComponent(int component);
		[[nodiscard]] int GetCurveComponent() const { return m_CurveComponent; }

		/* 上一帧的版式：自检按它算点击位置 */
		[[nodiscard]] const ImVec2& GetLastLaneMin() const { return m_LastLaneMin; }
		[[nodiscard]] const ImVec2& GetLastLaneMax() const { return m_LastLaneMax; }
		[[nodiscard]] float GetLastRowHeight() const { return m_LastRowHeight; }
		[[nodiscard]] float GetLastRulerHeight() const { return m_LastRulerHeight; }
		[[nodiscard]] float GetLastTrackListWidth() const { return m_LastTrackListWidth; }
		[[nodiscard]] float GetLastDuration() const { return m_LastDuration; }
		[[nodiscard]] float TimeToScreenX(float time) const;
		[[nodiscard]] const ImVec2& GetLastCurveMin() const { return m_LastCurveMin; }
		[[nodiscard]] const ImVec2& GetLastCurveMax() const { return m_LastCurveMax; }
		[[nodiscard]] float GetLastActionCenterX() const { return (m_LastActionMinX + m_LastActionMaxX) * 0.5f; }
		[[nodiscard]] float GetLastCurveValueMin() const { return m_LastCurveValueMin; }
		[[nodiscard]] float GetLastCurveValueMax() const { return m_LastCurveValueMax; }
		/* 删除关键帧的二次确认弹窗 */
		[[nodiscard]] bool IsDeleteConfirmOpen() const { return m_DeleteConfirmOpen; }
		[[nodiscard]] const ImVec2& GetLastConfirmDeleteMin() const { return m_LastConfirmDeleteMin; }
		[[nodiscard]] const ImVec2& GetLastConfirmDeleteMax() const { return m_LastConfirmDeleteMax; }
		/* 属性栏左缘把手 / 属性名列分隔条（自检按矩形拖它们） */
		[[nodiscard]] const ImVec2& GetLastInspectorResizeMin() const { return m_LastInspectorResizeMin; }
		[[nodiscard]] const ImVec2& GetLastInspectorResizeMax() const { return m_LastInspectorResizeMax; }
		[[nodiscard]] const ImVec2& GetLastLabelSplitMin() const { return m_LastLabelSplitMin; }
		[[nodiscard]] const ImVec2& GetLastLabelSplitMax() const { return m_LastLabelSplitMax; }
		/* 顶栏的呈现方式切换（自检按矩形点它） */
		[[nodiscard]] const ImVec2& GetLastModeSwitchMin() const { return m_LastModeSwitchMin; }
		[[nodiscard]] const ImVec2& GetLastModeSwitchMax() const { return m_LastModeSwitchMax; }
		[[nodiscard]] const ImVec2& GetLastKeyValueMin() const { return m_LastKeyValueMin; }
		[[nodiscard]] const ImVec2& GetLastKeyValueMax() const { return m_LastKeyValueMax; }

	private:
		/* 找选中实体最近的带 Animator 的祖先（选中子节点也能编辑） */
		entt::entity ResolveAnimatorEntity(Scene& scene, const Entity& selected) const;
		/* 解析本帧目标（scene / animator / clip）；换目标时先把攒着的录制提交掉 */
		void ResolveTarget(Scene* scene, const Entity& selected_entity);

		void DrawToolbar(AnimatorComponent& animator);
		void DrawAddTrackPopup(AnimatorComponent& animator);
		void DrawBody(Scene& scene, AnimatorComponent& animator);
		void DrawLanes(Scene& scene, float lane_top, float lane_bottom,
			float lane_left, float lane_right, bool rows_hovered);
		void DrawCurveArea(Scene& scene, AnimatorComponent& animator, float area_top, float area_bottom,
			float lane_left, float lane_right, bool hovered);
		/* 右侧属性编辑栏：轨道插值、关键帧时间与值（删除走右键 / Del） */
		void DrawInspector(AnimatorComponent& animator, float width, float height);
		/* 轨道列标题栏：新建轨道 + 搜索（放在标尺同一行） */
		void DrawTrackHeader(AnimatorComponent& animator, const ImVec2& origin, float track_list_width, float height);
		/* 标尺：fill_right 是标尺底色的右缘（铺到操作列），lane_right 是时间轴内容的右缘 */
		void DrawRulerAndPlayhead(AnimatorComponent& animator, const ImVec2& origin, float lane_left,
			float lane_right, float fill_right);
		/* 底部状态栏：左侧时长、右侧文件名 */
		void DrawFooterBar();
		/* 删除关键帧的二次确认（面板居中） */
		void RequestDeleteSelectedKeys();
		void DrawDeleteConfirmPopup();
		/* 标尺右键的事件菜单（新增 / 改名 / 删除） */
		void DrawEventMenu();
		/* 关键帧右键菜单（ID 在面板根上算，子窗口里 Open 才对得上） */
		void DrawKeyContextMenu();
		/* 设置选中帧的每帧插值：0 = 跟轨道，1..3 = Step / Linear / Bezier */
		void SetSelectedKeyInterp(int value);
		/* keys_mode = 关键帧模式：画整行和关键帧；曲线模式只画左侧轨道列表。
		 * row_right 是整行底色 / 分隔线画到哪（关键帧模式含操作列），关键帧只裁到 lane_right（时刻列）。 */
		void DrawTrackRows(AnimatorComponent& animator, const ImVec2& origin, float lane_left, float lane_right,
			float row_right, bool keys_mode);
		/* 底栏里的缩放比例 + 可视区域滑动条（放大后才出现，高度跟着栏高走） */
		void DrawZoomBar(float left, float top, float height, float track_width, const char* label);
		/* 把滚动量夹在 [0, 内容宽 - 视口宽] 内；1 倍时归零 */
		void ClampView();

		/* 轨道行滚动：总高、夹取，以及轨道列右缘那根竖直滚动条 */
		[[nodiscard]] float TrackRowsHeight() const;
		void ClampScrollRows();
		void ScrollRowsBy(float delta);
		void DrawRowScrollbar(float list_right, float top, float bottom);
		/* 坐标落在滚动条命中区里：轨道区的点击要先让它 */
		[[nodiscard]] bool IsRowScrollbarAt(const ImVec2& point) const;

		/* 录制：所有轨道当前值与采样值不一致就写关键帧（停手几帧后落一条命令） */
		void RecordChangedTracks(Scene& scene, AnimatorComponent& animator);

		/* 命令辅助：改动前抓一份轨道表，有实质改动才提交 */
		void BeginEdit();
		void MarkEditDirty() { m_EditDirty = true; }
		/* 片段内容变了：版本号 +1，定格姿势会在下一帧重采样 */
		void MarkClipChanged();
		void DiscardEdit();
		void CommitEdit(const char* label);
		[[nodiscard]] bool HasPendingEdit() const { return m_EditPending; }

		/* 轨道搜索：每帧重建可见行；行号（界面）与轨道下标（数据）分开 */
		void RebuildVisibleTracks();
		[[nodiscard]] int VisibleToTrack(int row) const;
		[[nodiscard]] int TrackToVisible(int track) const;

		/* 轨道开关：关掉后播放与录制都跳过这条轨道 */
		void ToggleTrackEnabled(int track);
		/* 删除指定轨道（行尾垃圾桶图标用），删完清掉选择 */
		void DeleteTrackAt(int track);

		/* 选择 */
		void ClearSelection();
		/* 复制粘贴的暂存：时间存成相对复制块起点的偏移 */
		struct CopiedKey
		{
			std::string Path;
			std::string Component;
			std::string Property;
			AnimationKey Key;
		};
		std::vector<CopiedKey> m_CopiedKeys;

		ImVec2 m_LastCloseClipMin{ 0.0f, 0.0f };
		ImVec2 m_LastCloseClipMax{ 0.0f, 0.0f };

		/* 关键帧右键菜单状态：ID、是否打开、右键落点时间、删除项矩形（自检用） */
		ImGuiID m_KeyMenuId{ 0 };
		bool    m_KeyMenuOpen{ false };
		float   m_KeyMenuTime{ 0.0f };
		ImVec2  m_KeyMenuDeleteMin{ 0.0f, 0.0f };
		ImVec2  m_KeyMenuDeleteMax{ 0.0f, 0.0f };

		/* 标尺右键的事件菜单状态 */
		float m_EventMenuTime{ 0.0f };
		int   m_EventMenuIndex{ -1 };
		char  m_EventMenuName[64]{};

		/* 撤销 / 重做会把片段轨道表整份换掉：选择里越界的轨道 / 关键帧在这里丢掉 */
		void ValidateSelection();
		void SelectSingleKey(int track, int key);
		void ToggleKeySelection(int track, int key);
		void ApplyBoxSelection(const ImVec2& box_min, const ImVec2& box_max, bool additive);

		/* 曲线分量：Float 1 个；Vec3 3 个；Vec4 4 个；Quat 按欧拉角 3 个 */
		[[nodiscard]] int CurveComponentCount(const AnimationTrack& track) const;
		/* 分量显示名：X/Y/Z/W；旋转用 Euler X/Y/Z */
		[[nodiscard]] const char* AxisName(const AnimationTrack& track, int component) const;
		[[nodiscard]] float KeyCurveValue(const AnimationTrack& track, const AnimationKey& key, int component) const;
		/* 按帧取曲线值：四元数轨道用连续分支的欧拉角（和曲线、属性栏显示一致） */
		[[nodiscard]] float KeyCurveValueAt(const AnimationTrack& track, int key_index, int component) const;
		[[nodiscard]] float SampleCurveValue(const AnimationTrack& track, float time, int component) const;
		bool WriteKeyCurveValue(AnimationTrack& track, int key_index, int component, float value);

		/* 轨道区左右留白后的时间轴内容边界与可视宽度 */
		[[nodiscard]] float LaneContentLeft() const;
		[[nodiscard]] float LaneViewWidth() const;
		[[nodiscard]] float TimeToX(float time) const;
		[[nodiscard]] float XToTime(float x) const;
		[[nodiscard]] float GetLaneDuration() const;
		[[nodiscard]] const AnimationTrack* GetSelectedTrackPtr() const;

		/* 本帧解析出来的目标（每帧重算） */
		Scene* m_pScene{ nullptr };
		entt::entity m_AnimatorEntity{ entt::null };
		SharedPtr<AnimationClip> m_Clip;

		CommandStack* m_pCommandStack{ nullptr };
		bool m_Recording{ false };
		bool m_IsVisible{ false };

		int m_SelectedTrack{ -1 };
		std::vector<TimelineModel::KeyRef> m_Selection;

		/* 关键帧拖动（整组） */
		bool m_DraggingKey{ false };
		bool m_DragChanged{ false };
		float m_DragStartTime{ 0.0f };
		/* 单帧拖动时纵向改值：起点鼠标 y、分量、分量原值 */
		float m_DragStartMouseY{ 0.0f };
		int   m_DragComponent{ 0 };
		float m_DragStartValue{ 0.0f };
		float m_DragBeforeDuration{ 0.0f };
		std::vector<TimelineModel::KeyRef> m_DragSelection;
		std::vector<AnimationTrack> m_DragBeforeTracks;

		/* 框选 */
		bool m_BoxSelecting{ false };
		/* 曲线视图里的框选（和轨道区的框选是两套，互不干扰） */
		bool   m_CurveBoxSelecting{ false };
		ImVec2 m_CurveBoxStart{ 0.0f, 0.0f };
		bool m_BoxAdditive{ false };
		ImVec2 m_BoxStart{ 0.0f, 0.0f };

		/* 标尺拖动（播放头） */
		bool m_ScrubbingRuler{ false };

		/* 编辑前后快照 */
		std::vector<AnimationTrack> m_EditBeforeTracks;
		std::vector<AnimationEvent> m_EditBeforeEvents;
		/* 独立编辑的片段；为空时跟着选中实体的 Animator 走。
		 * 独立模式下播放头等编辑状态放在这份占位组件里，面板行为保持一致 */
		SharedPtr<AnimationClip> m_StandaloneClip;
		AnimatorComponent m_StandaloneAnimator;
		/* 本帧编辑的动画组件（独立编辑 = 上面那份占位） */
		AnimatorComponent& ActiveAnimator();
		/* 有没有真实的实体 Animator：录制 / 播放这些要靠它 */
		[[nodiscard]] bool HasSceneAnimator() const
		{
			return m_StandaloneClip == nullptr && m_pScene != nullptr && m_AnimatorEntity != entt::null;
		}
		float m_EditBeforeDuration{ 0.0f };
		bool  m_EditPending{ false };
		bool  m_EditDirty{ false };

		/* 录制攒批：停手 kRecordIdleFrames 帧后提交 */
		bool m_RecordPending{ false };
		int  m_RecordIdleFrames{ 0 };

		/* 轨道搜索与可见行（界面行 -> 轨道下标） */
		char m_TrackFilter[64]{};
		std::vector<int> m_VisibleTracks;

		/* 加轨道菜单 */
		std::vector<TimelineModel::PropertyEntry> m_AddTrackEntries;
		char m_AddTrackFilter[64]{};
		bool m_AddTrackEntriesValid{ false };

		/* 曲线视图 */
		/* 默认收起：先让时刻区拿到整块宽度，要看属性再从顶栏展开（点关键帧也会自动展开） */
		bool m_InspectorOpen{ false };
		bool m_CurveView{ false };
		/* 上一帧标尺画出的刻度时间：曲线区据此画竖向指示线 */
		std::vector<float> m_LastRulerTicks;
		float m_Zoom{ 1.0f };
		float m_ScrollX{ 0.0f };
		int  m_CurveComponent{ 0 };
		bool m_DraggingCurveKey{ false };
		int  m_CurveDragTrack{ -1 };
		int  m_CurveDragKey{ -1 };
		/* 抓取偏移：按下时鼠标与关键帧圆点的像素差，拖动时减掉它才不会跳 */
		float m_CurveDragGrabDx{ 0.0f };
		float m_CurveDragGrabDy{ 0.0f };
		bool  m_CurveDragChanged{ false };

		/* 贝塞尔手柄拖动 */
		bool  m_DraggingTangent{ false };
		int   m_TangentTrack{ -1 };
		int   m_TangentKey{ -1 };
		bool  m_TangentOut{ true };
		float m_TangentGrabDy{ 0.0f };
		float m_TangentValueMin{ -1.0f };
		float m_TangentValueMax{ 1.0f };
		bool  m_TangentChanged{ false };
		float m_CurveDragValueMin{ -1.0f };
		float m_CurveDragValueMax{ 1.0f };

		/* 上一帧版式（自检与输入换算共用） */
		ImVec2 m_LastLaneMin{ 0.0f, 0.0f };
		ImVec2 m_LastLaneMax{ 0.0f, 0.0f };
		ImVec2 m_LastCurveMin{ 0.0f, 0.0f };
		ImVec2 m_LastCurveMax{ 0.0f, 0.0f };
		float m_LastRowHeight{ 20.0f };
		float m_LastRulerHeight{ 22.0f };
		float m_LastTrackListWidth{ 190.0f };
		/* 轨道行滚动：滚动量、可视高度（不含标尺，每帧更新）、拖动手感 */
		float m_ScrollY{ 0.0f };
		float m_RowViewHeight{ 0.0f };
		bool  m_DraggingRowScroll{ false };
		float m_RowScrollDragOffset{ 0.0f };
		/* 上一帧滚动条的命中区（空矩形 = 没画） */
		ImVec2 m_LastRowBarMin{ 0.0f, 0.0f };
		ImVec2 m_LastRowBarMax{ 0.0f, 0.0f };

		float m_LastActionMinX{ 0.0f };
		float m_LastActionMaxX{ 0.0f };
		/* 呈现方式切换的滑动块位置（0 = 关键帧、1 = 曲线） */
		float m_ModeSlide{ 0.0f };
		/* 属性栏宽度 / 属性名列宽（都能拖），以及拖拽起点 */
		float m_InspectorWidth{ 240.0f };
		float m_InspectorLabelWidth{ 62.0f };
		float m_InspectorResizeStartX{ 0.0f };
		float m_InspectorResizeStartWidth{ 240.0f };
		float m_LabelResizeStartX{ 0.0f };
		float m_LabelResizeStartWidth{ 62.0f };
		/* 删除确认弹窗 */
		bool m_DeleteConfirmRequested{ false };
		bool m_DeleteConfirmOpen{ false };
		int  m_DeleteConfirmCount{ 0 };
		ImVec2 m_LastConfirmDeleteMin{ 0.0f, 0.0f };
		ImVec2 m_LastConfirmDeleteMax{ 0.0f, 0.0f };
		float m_LastDuration{ 1.0f };
		float m_LastCurveValueMin{ -1.0f };
		float m_LastCurveValueMax{ 1.0f };
		ImVec2 m_LastInspectorResizeMin{ 0.0f, 0.0f };
		ImVec2 m_LastInspectorResizeMax{ 0.0f, 0.0f };
		ImVec2 m_LastLabelSplitMin{ 0.0f, 0.0f };
		ImVec2 m_LastLabelSplitMax{ 0.0f, 0.0f };
		ImVec2 m_LastModeSwitchMin{ 0.0f, 0.0f };
		ImVec2 m_LastModeSwitchMax{ 0.0f, 0.0f };
		ImVec2 m_LastKeyValueMin{ 0.0f, 0.0f };
		ImVec2 m_LastKeyValueMax{ 0.0f, 0.0f };
	};
}
