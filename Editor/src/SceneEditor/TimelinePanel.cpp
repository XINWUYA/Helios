#include "Pch.h"
#include "TimelinePanel.h"
#include "Command/ClipEditCommand.h"
#include "Command/ComponentFieldCommand.h"
#include "EditorCommon.h"
#include "EditorIcons.h"
#include "PanelChrome.h"
#include "PanelRegistry.h"
#include "Helios/Command/CommandStack.h"
#include "Helios/Common/PathUtils.h"
#include "Helios/ImGui/EditorTheme.h"
#include "Helios/Reflection/ComponentRegistry.h"
#include "Helios/Scene/AnimationBinding.h"
#include "Helios/Scene/Components.h"
#include "Helios/Scene/Scene.h"
#include <glm/gtx/quaternion.hpp>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace Helios
{
	namespace
	{
		constexpr float kTrackListWidth = 220.0f;
		constexpr float kRulerHeight = 26.0f;
		constexpr float kLanePadding = 10.0f;
		constexpr float kRowHeight = 20.0f;
		constexpr float kRowsPerWheelNotch = 3.0f;	/* 滚一格 = 三行 */
		constexpr int kRecordIdleFrames = 8;
		constexpr float kKeyHitRadius = 5.0f;

		ImU32 ColorU32(const ImVec4& color)
		{
			return ImGui::GetColorU32(color);
		}

		bool ContainsIgnoreCase(const std::string& text, const std::string& needle)
		{
			if (needle.empty())
				return true;

			const auto lower = [](char ch) { return static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); };
			std::string haystack = text;
			std::string pattern = needle;
			for (char& ch : haystack)
				ch = lower(ch);
			for (char& ch : pattern)
				ch = lower(ch);
			return haystack.find(pattern) != std::string::npos;
		}

		std::string EntityName(Scene& scene, entt::entity entity)
		{
			if (const auto* name = scene.GetRegistry().try_get<NameComponent>(entity))
				return name->m_Name;
			return "Entity";
		}

		/* 轨道行首的小图标：按组件类型选，认不出的用实体图标 */
		Icons::Id TrackComponentIcon(const std::string& component)
		{
			if (component == "Transform") return Icons::Id::Transform;
			if (component == "Light")     return Icons::Id::Light;
			if (component == "Camera")    return Icons::Id::Camera;
			if (component == "Model")     return Icons::Id::Model;
			if (component == "Animator")  return Icons::Id::Animation;
			if (component == "Visibility") return Icons::Id::Visible;
			return Icons::Id::Entity;
		}

		float SnapTime(float time)
		{
			return std::round(std::max(time, 0.0f) * 100.0f) / 100.0f;
		}

		bool SameRef(const TimelineModel::KeyRef& ref, int track, int key)
		{
			return ref.Track == track && ref.Key == key;
		}

		/* 四元数的欧拉角有两个等价解：(x, y, z) 与 (x±180, 180-y, z±180)，同一个旋转还允许任一分量
		 * 加减 360。旋转跨过 ±90 时公式会在两支之间跳（X/Z 突然变 180），逐点取离上一点最近的那支，
		 * 曲线才不会来回翻。 */
		glm::vec3 NearestEulerBranch(const glm::vec3& reference, const glm::vec3& euler)
		{
			const auto wrap_near = [](float value, float target)
			{
				return value + 360.0f * std::round((target - value) / 360.0f);
			};

			const glm::vec3 direct(wrap_near(euler.x, reference.x), wrap_near(euler.y, reference.y),
				wrap_near(euler.z, reference.z));
			const glm::vec3 flipped(wrap_near(euler.x + 180.0f, reference.x),
				wrap_near(180.0f - euler.y, reference.y),
				wrap_near(euler.z + 180.0f, reference.z));

			const glm::vec3 direct_delta = direct - reference;
			const glm::vec3 flipped_delta = flipped - reference;
			return glm::dot(flipped_delta, flipped_delta) < glm::dot(direct_delta, direct_delta)
				? flipped : direct;
		}

		/* 某个关键帧的欧拉角：沿关键帧序列逐帧选连续分支（只对四元数轨道有意义） */
		glm::vec3 KeyEuler(const AnimationTrack& track, int key_index)
		{
			const int count = static_cast<int>(track.Keys.size());
			if (count == 0)
				return glm::vec3(0.0f);

			glm::vec3 previous(0.0f);
			glm::vec3 euler(0.0f);
			for (int index = 0; index <= std::clamp(key_index, 0, count - 1); ++index)
			{
				const glm::vec4& value = track.Keys[index].Value;
				euler = glm::degrees(glm::eulerAngles(glm::quat(value.w, value.x, value.y, value.z)));
				if (index > 0)
					euler = NearestEulerBranch(previous, euler);
				previous = euler;
			}
			return euler;
		}

		/* 放不下就从尾部截断：按整字符删，别把 UTF-8 字节切一半 */
		std::string Ellipsize(const std::string& text, float max_width)
		{
			if (max_width <= 0.0f)
				return std::string();
			if (ImGui::CalcTextSize(text.c_str()).x <= max_width)
				return text;

			std::string truncated = text;
			while (!truncated.empty() && ImGui::CalcTextSize((truncated + "...").c_str()).x > max_width)
			{
				while (!truncated.empty() && (static_cast<unsigned char>(truncated.back()) & 0xC0) == 0x80)
					truncated.pop_back();
				if (!truncated.empty())
					truncated.pop_back();
			}
			return truncated + "...";
		}

		/* 两格滑动切换（关键帧 / 曲线）：画法与性能面板的视图切换同一套 —— 底槽 + 悬停提亮 +
		 * 选中块平滑滑动；只画图标，名字走 tooltip。返回本帧被点的格（-1 = 没点）。
		 * slide 由调用方持久化（0 = 左格、1 = 右格）。 */
		int SegmentedModeSwitch(const char* id, Icons::Id left_icon, const char* left_tip,
			Icons::Id right_icon, const char* right_tip, bool right_selected, float& slide, float width)
		{
			const ImGuiStyle& style = ImGui::GetStyle();
			const float height = ImGui::GetFrameHeight();
			const float seg_width = width * 0.5f;
			const ImVec2 pos = ImGui::GetCursorScreenPos();
			const Icons::Id icons[2] = { left_icon, right_icon };
			const char* tips[2] = { left_tip, right_tip };

			int clicked = -1;
			bool hovered[2] = { false, false };
			ImGui::PushID(id);
			for (int index = 0; index < 2; ++index)
			{
				ImGui::InvisibleButton(index == 0 ? "##left" : "##right", ImVec2(seg_width, height));
				hovered[index] = ImGui::IsItemHovered();
				if (hovered[index])
					ImGui::SetTooltip("%s", tips[index]);
				if (ImGui::IsItemClicked())
					clicked = index;
				if (index == 0)
					ImGui::SameLine(0.0f, 0.0f);
			}
			ImGui::PopID();

			/* 选中块滑动：指数趋近（约 0.1s 到位），到目标即停 */
			const float target = right_selected ? 1.0f : 0.0f;
			const float dt = std::min(ImGui::GetIO().DeltaTime, 0.1f);
			slide += (target - slide) * std::min(1.0f, dt * 18.0f);
			if (std::fabs(slide - target) < 0.002f)
				slide = target;

			ImDrawList* draw_list = ImGui::GetWindowDrawList();
			draw_list->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height),
				ColorU32(EditorTheme::Token::Neutral4), style.FrameRounding);
			draw_list->AddRect(pos, ImVec2(pos.x + width, pos.y + height),
				ColorU32(EditorTheme::Token::Border), style.FrameRounding, 0, 1.0f);

			/* 未选中格悬停时的提亮底：画在滑动块之下 */
			const int selected = right_selected ? 1 : 0;
			for (int index = 0; index < 2; ++index)
			{
				if (!hovered[index] || index == selected)
					continue;
				const float seg_x = pos.x + seg_width * static_cast<float>(index);
				draw_list->AddRectFilled(ImVec2(seg_x, pos.y), ImVec2(seg_x + seg_width, pos.y + height),
					ColorU32(EditorTheme::Token::Neutral5), style.FrameRounding);
			}

			/* 选中块（滑动中：位置在两格之间插值） */
			constexpr float kBlockInset = 2.0f;
			const float block_width = seg_width - kBlockInset * 2.0f;
			const float block_x = pos.x + kBlockInset + seg_width * slide;
			const ImVec2 block_min(block_x, pos.y + kBlockInset);
			const ImVec2 block_max(block_x + block_width, pos.y + height - kBlockInset);
			const float block_rounding = std::max(1.0f, style.FrameRounding - 1.0f);
			draw_list->AddRectFilled(block_min, block_max,
				ColorU32(EditorTheme::WithAlpha(EditorTheme::Token::Accent, 0.17f)), block_rounding);
			draw_list->AddRect(block_min, block_max,
				ColorU32(EditorTheme::WithAlpha(EditorTheme::Token::Accent, 0.65f)), block_rounding, 0, 1.0f);

			for (int index = 0; index < 2; ++index)
			{
				const float seg_x = pos.x + seg_width * static_cast<float>(index);
				const float icon_size = height * 0.62f;
				const ImU32 content_color = ColorU32(index == selected || hovered[index]
					? EditorTheme::Token::Text : EditorTheme::Token::TextDim);
				Icons::Draw(draw_list, icons[index],
					ImVec2(seg_x + seg_width * 0.5f, pos.y + height * 0.5f), icon_size, content_color);
			}

			return clicked;
		}
	}

	entt::entity TimelinePanel::ResolveAnimatorEntity(Scene& scene, const Entity& selected) const
	{
		if (!selected)
			return entt::null;

		entt::entity current = static_cast<entt::entity>(selected);
		for (size_t depth = 0; current != entt::null && depth < 64; ++depth)
		{
			if (scene.GetRegistry().all_of<AnimatorComponent>(current))
				return current;
			current = scene.GetParent(current);
		}
		return entt::null;
	}

	void TimelinePanel::SetRecording(bool recording)
	{
		/* 独立编辑片段时没有实体可录：直接拒绝，别留一个亮着却什么都不写的按钮 */
		if (recording && m_StandaloneClip != nullptr)
			return;

		m_Recording = recording;

		if (!m_Recording || m_pScene == nullptr || m_AnimatorEntity == entt::null)
			return;
		if (!m_pScene->GetRegistry().all_of<AnimatorComponent>(m_AnimatorEntity))
			return;

		/* 开录制就停下播放并定格在播放头：不然改的值会被正在走的采样冲掉 */
		AnimatorComponent& animator = m_pScene->GetRegistry().get<AnimatorComponent>(m_AnimatorEntity);
		animator.m_Playing = false;
		animator.m_Scrubbing = true;
	}

	bool TimelinePanel::ToggleLoop()
	{
		if (m_pScene == nullptr || m_AnimatorEntity == entt::null)
			return false;
		if (!m_pScene->GetRegistry().all_of<AnimatorComponent>(m_AnimatorEntity))
			return false;

		AnimatorComponent& animator = m_pScene->GetRegistry().get<AnimatorComponent>(m_AnimatorEntity);
		const bool before = animator.m_Loop;
		const bool after = !before;

		/* Loop 是跟着场景存的作者数据：走组件字段命令，撤销与脏标记都跟属性面板一致 */
		const ComponentDesc* desc = ComponentRegistry::Instance().Find(std::type_index(typeid(AnimatorComponent)));
		if (m_pCommandStack != nullptr && desc != nullptr && desc->GetPtr != nullptr)
		{
			FieldDesc field;
			field.Name = "Loop";
			field.Type = FieldType::Bool;
			field.Offset = MemberOffset(&AnimatorComponent::m_Loop);
			field.ValueSize = sizeof(bool);

			Entity animator_entity{ m_AnimatorEntity, m_pScene->shared_from_this() };
			m_pCommandStack->Execute(CreateUniquePtr<ComponentFieldCommand>(
				animator_entity, *desc, field, &before, &after));
		}
		else
		{
			animator.m_Loop = after;
		}
		return true;
	}

	AnimatorComponent& TimelinePanel::ActiveAnimator()
	{
		if (m_StandaloneClip != nullptr)
			return m_StandaloneAnimator;

		return m_pScene->GetRegistry().get<AnimatorComponent>(m_AnimatorEntity);
	}

	void TimelinePanel::OpenClip(SharedPtr<AnimationClip> clip)
	{
		if (clip == nullptr)
			return;

		if (HasPendingEdit())
			CommitEdit("Record Key");
		DiscardEdit();

		m_StandaloneClip = std::move(clip);
		m_StandaloneAnimator = AnimatorComponent{};
		m_StandaloneAnimator.m_Clip = m_StandaloneClip;
		m_StandaloneAnimator.m_Time = 0.0f;
		m_Clip = m_StandaloneClip;
		m_SelectedTrack = -1;
		ClearSelection();
		m_VisibleTracks.clear();
		m_AddTrackEntriesValid = false;
		m_Zoom = 1.0f;
		m_ScrollX = 0.0f;
	}

	void TimelinePanel::CloseClip()
	{
		if (m_StandaloneClip == nullptr)
			return;

		if (HasPendingEdit())
			CommitEdit("Record Key");
		DiscardEdit();

		m_StandaloneClip.reset();
		m_Clip = nullptr;
		m_SelectedTrack = -1;
		ClearSelection();
		m_VisibleTracks.clear();
		m_AddTrackEntriesValid = false;
	}

	void TimelinePanel::ResolveTarget(Scene* scene, const Entity& selected_entity)
	{
		/* 独立编辑：直接编打开的那个片段，不跟实体走 */
		if (m_StandaloneClip != nullptr)
		{
			m_pScene = scene;
			m_AnimatorEntity = entt::null;
			m_Clip = m_StandaloneClip;
			return;
		}

		const entt::entity resolved = (scene != nullptr) ? ResolveAnimatorEntity(*scene, selected_entity) : entt::null;
		if (resolved != m_AnimatorEntity && HasPendingEdit())
			CommitEdit("Record Key");

		const SharedPtr<AnimationClip> previous_clip = m_Clip;

		m_pScene = scene;
		m_AnimatorEntity = resolved;
		m_Clip = nullptr;

		if (m_pScene != nullptr && m_AnimatorEntity != entt::null
			&& m_pScene->GetRegistry().all_of<AnimatorComponent>(m_AnimatorEntity))
		{
			m_Clip = m_pScene->GetRegistry().get<AnimatorComponent>(m_AnimatorEntity).m_Clip;
		}

		/* 换片段回到 1 倍、从头看 */
		if (m_Clip != previous_clip)
		{
			m_Zoom = 1.0f;
			m_ScrollX = 0.0f;
		}
	}

	void TimelinePanel::TickRecording(Scene* scene, const Entity& selected_entity)
	{
		if (!m_IsVisible)
			return;

		ResolveTarget(scene, selected_entity);

		if (!m_Recording || scene == nullptr || m_Clip == nullptr || m_AnimatorEntity == entt::null)
		{
			if (HasPendingEdit())
			{
				CommitEdit("Record Key");
				m_RecordPending = false;
				m_RecordIdleFrames = 0;
			}
			return;
		}

		if (!scene->GetRegistry().all_of<AnimatorComponent>(m_AnimatorEntity))
			return;

		AnimatorComponent& animator = scene->GetRegistry().get<AnimatorComponent>(m_AnimatorEntity);
		RecordChangedTracks(*scene, animator);
	}

	void TimelinePanel::OnImGuiRender(Scene* scene, const Entity& selected_entity, CommandStack* command_stack, bool& visible)
	{
		PROFILE_FUNCTION();

		m_IsVisible = visible;
		if (!visible)
		{
			if (HasPendingEdit())
				CommitEdit("Record Key");
			return;
		}

		m_pCommandStack = command_stack;
		ResolveTarget(scene, selected_entity);

		/* 和资源面板一样：面板本身不带上下内边距，顶栏 / 底栏要贴着面板边画。
		 * 这一档样式只借给 Begin 这一次 —— 窗口的内边距在 Begin 里就锁进 window->WindowPadding 了，
		 * 之后打开的弹层 / 下拉要用回主题那一档，否则像「新建轨道」的搜索框会顶着弹层上边 */
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
			ImVec2(ImGui::GetStyle().WindowPadding.x, 0.0f));
		ImGui::SetNextWindowSizeConstraints(ImVec2(380.0f, 160.0f), ImVec2(FLT_MAX, FLT_MAX));
		const bool window_open = ImGui::Begin(Panel::kTimeline, &visible);
		ImGui::PopStyleVar();

		if (!window_open)
		{
			ImGui::End();
			return;
		}

		if (m_Clip == nullptr)
		{
			m_VisibleTracks.clear();
			ImGui::TextDisabled("Select an entity with an Animator, then assign a clip in the Animator card.");
			ImGui::End();
			return;
		}

		RebuildVisibleTracks();

		/* 撤销 / 重做整份换过轨道表：先把越界的选择丢掉，后面所有读取才安全 */
		ValidateSelection();

		/* 关键帧右键菜单的 ID 在面板根上算一次：右键发生在子窗口里，各自算会得到不同的值 */
		m_KeyMenuId = ImGui::GetID("##TimelineKeyMenu");

		AnimatorComponent& animator = ActiveAnimator();

		DrawToolbar(animator);
		ImGui::Separator();

		/* 顶栏里刚点了"关闭片段"：这一帧别再用空片段往下画，下一帧按选中实体重新解析 */
		if (m_Clip == nullptr)
		{
			ImGui::End();
			return;
		}

		DrawBody(*m_pScene, animator);
		DrawFooterBar();
		DrawKeyContextMenu();
		DrawDeleteConfirmPopup();

		/* 时间轴自己的编辑快捷键 */
		if (!ImGui::GetIO().WantTextInput)
		{
			const ImGuiIO& io = ImGui::GetIO();
			const bool ctrl = io.KeyCtrl || io.KeySuper;

			/* 左右微调：鼠标停在时刻区就够了，不要求面板先拿到键盘焦点 */
			if (!ctrl && WantsArrowKeys())
			{
				if (KeyPressed(ImGuiKey_LeftArrow))
					NudgeSelectedKeys(io.KeyShift ? -0.1f : -0.01f);
				if (KeyPressed(ImGuiKey_RightArrow))
					NudgeSelectedKeys(io.KeyShift ? 0.1f : 0.01f);
			}

			/* 复制粘贴跟着焦点走：资源浏览器等面板也在用这两个快捷键 */
			if (ctrl && ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows))
			{
				if (KeyPressed(ImGuiKey_C))
					CopySelectedKeys();
				if (KeyPressed(ImGuiKey_V))
					PasteSelectedKeys();
			}
		}

		ImGui::End();
	}

	int TimelinePanel::GetTrackCount() const
	{
		return (m_Clip != nullptr) ? static_cast<int>(m_Clip->GetTracks().size()) : 0;
	}

	void TimelinePanel::RebuildVisibleTracks()
	{
		m_VisibleTracks.clear();
		if (m_Clip == nullptr)
			return;

		const std::string animator_name = (m_pScene != nullptr && m_AnimatorEntity != entt::null)
			? EntityName(*m_pScene, m_AnimatorEntity) : std::string("Root");

		for (int index = 0; index < GetTrackCount(); ++index)
		{
			const std::string label = TimelineModel::MakeTrackLabel(m_Clip->GetTracks()[index], animator_name);
			if (ContainsIgnoreCase(label, m_TrackFilter))
				m_VisibleTracks.push_back(index);
		}
	}

	int TimelinePanel::VisibleToTrack(int row) const
	{
		if (row < 0 || row >= static_cast<int>(m_VisibleTracks.size()))
			return -1;
		return m_VisibleTracks[row];
	}

	int TimelinePanel::TrackToVisible(int track) const
	{
		for (int row = 0; row < static_cast<int>(m_VisibleTracks.size()); ++row)
		{
			if (m_VisibleTracks[row] == track)
				return row;
		}
		return -1;
	}

	void TimelinePanel::SetTrackFilter(const std::string& filter)
	{
		std::snprintf(m_TrackFilter, sizeof(m_TrackFilter), "%s", filter.c_str());
		RebuildVisibleTracks();
	}

	bool TimelinePanel::IsKeySelected(int track, int key) const
	{
		return std::any_of(m_Selection.begin(), m_Selection.end(),
			[track, key](const TimelineModel::KeyRef& ref) { return SameRef(ref, track, key); });
	}

	void TimelinePanel::ClearSelection()
	{
		m_Selection.clear();
	}

	void TimelinePanel::SetSelectedKeyInterp(int value)
	{
		if (m_Clip == nullptr || m_Selection.empty())
			return;

		BeginEdit();
		for (const TimelineModel::KeyRef& selection_ref : m_Selection)
		{
			if (selection_ref.Track < 0
				|| selection_ref.Track >= static_cast<int>(m_Clip->GetTracks().size()))
				continue;
			AnimationTrack& selection_track = m_Clip->GetTracks()[selection_ref.Track];
			if (selection_ref.Key < 0
				|| selection_ref.Key >= static_cast<int>(selection_track.Keys.size()))
				continue;

			selection_track.Keys[selection_ref.Key].HasInterp = (value > 0);
			if (value > 0)
				selection_track.Keys[selection_ref.Key].Interp = static_cast<KeyInterp>(value - 1);
		}
		MarkClipChanged();
		CommitEdit("Set Key Interp");
	}

	bool TimelinePanel::WantsArrowKeys() const
	{
		/* 只看鼠标位置：悬停到时刻区就能按方向键微调，不必先点一下让面板拿焦点 */
		if (m_Clip == nullptr)
			return false;

		const ImVec2 mouse = ImGui::GetIO().MousePos;

		/* 时刻区：标尺 + 轨道行；曲线模式再加上曲线区 */
		const float lane_top = m_LastLaneMin.y - m_LastRulerHeight;
		const bool in_lanes = mouse.x >= m_LastLaneMin.x && mouse.x <= m_LastLaneMax.x
			&& mouse.y >= lane_top && mouse.y <= m_LastLaneMax.y;
		if (in_lanes)
			return true;

		return m_CurveView
			&& mouse.x >= m_LastCurveMin.x && mouse.x <= m_LastCurveMax.x
			&& mouse.y >= m_LastCurveMin.y && mouse.y <= m_LastCurveMax.y;
	}

	bool TimelinePanel::CopySelectedKeys()
	{
		if (m_Clip == nullptr || m_Selection.empty())
			return false;

		/* 以选区里最早的一帧为基准：粘到播放头时整块相对位置不变 */
		float base = FLT_MAX;
		for (const TimelineModel::KeyRef& ref : m_Selection)
		{
			if (ref.Track < 0 || ref.Track >= GetTrackCount())
				continue;
			const AnimationTrack& track = m_Clip->GetTracks()[ref.Track];
			if (ref.Key < 0 || ref.Key >= static_cast<int>(track.Keys.size()))
				continue;
			base = std::min(base, track.Keys[ref.Key].Time);
		}
		if (base == FLT_MAX)
			return false;

		m_CopiedKeys.clear();
		for (const TimelineModel::KeyRef& ref : m_Selection)
		{
			const AnimationTrack& track = m_Clip->GetTracks()[ref.Track];
			CopiedKey copied;
			copied.Path = track.Path;
			copied.Component = track.Component;
			copied.Property = track.Property;
			copied.Key = track.Keys[ref.Key];
			copied.Key.Time -= base;
			m_CopiedKeys.push_back(std::move(copied));
		}
		return true;
	}

	bool TimelinePanel::PasteSelectedKeys()
	{
		if (!m_StandaloneClip && (m_pScene == nullptr || m_AnimatorEntity == entt::null))
			return false;

		return PasteSelectedKeys(ActiveAnimator().m_Time);
	}

	bool TimelinePanel::PasteSelectedKeys(float base_time)
	{
		if (m_Clip == nullptr || m_CopiedKeys.empty())
			return false;

		const float base = std::max(base_time, 0.0f);

		BeginEdit();

		bool pasted = false;
		std::vector<TimelineModel::KeyRef> pasted_keys;
		for (const CopiedKey& copied : m_CopiedKeys)
		{
			/* 粘回同一条轨道：路径 / 组件 / 属性三样都对上 */
			int target = -1;
			for (int index = 0; index < GetTrackCount(); ++index)
			{
				const AnimationTrack& track = m_Clip->GetTracks()[index];
				if (track.Path == copied.Path && track.Component == copied.Component
					&& track.Property == copied.Property)
				{
					target = index;
					break;
				}
			}
			if (target < 0)
				continue;

			AnimationTrack& track = m_Clip->GetTracks()[target];
			AnimationKey key = copied.Key;
			key.Time = base + copied.Key.Time;
			if (key.Time < 0.0f)
				continue;

			/* 落点已有帧就整帧覆盖：值、切线、每帧插值一起带过来 */
			const int existing = TimelineModel::FindKey(track, key.Time);
			if (existing >= 0)
			{
				key.Time = track.Keys[existing].Time;
				track.Keys[existing] = key;
				pasted_keys.push_back(TimelineModel::KeyRef{ target, existing });
			}
			else
			{
				track.Keys.push_back(key);
				std::sort(track.Keys.begin(), track.Keys.end(),
					[](const AnimationKey& lhs, const AnimationKey& rhs) { return lhs.Time < rhs.Time; });
				pasted_keys.push_back(TimelineModel::KeyRef{ target, TimelineModel::FindKey(track, key.Time) });
			}
			pasted = true;
		}

		if (!pasted)
		{
			DiscardEdit();
			return false;
		}

		m_Clip->RefreshDuration();
		MarkClipChanged();
		CommitEdit("Paste Keys");

		m_Selection = pasted_keys;
		if (!m_Selection.empty())
			m_SelectedTrack = m_Selection.front().Track;
		return true;
	}

	bool TimelinePanel::NudgeSelectedKeys(float delta)
	{
		if (m_Clip == nullptr || m_Selection.empty() || std::fabs(delta) < 1e-6f)
			return false;

		std::vector<AnimationTrack>& tracks = m_Clip->GetTracks();
		const std::vector<AnimationTrack> before = tracks;
		std::vector<TimelineModel::KeyRef> moved;

		BeginEdit();
		TimelineModel::MoveKeys(tracks, before, m_Selection, delta, moved);
		if (moved.empty())
		{
			DiscardEdit();
			return false;
		}

		m_Clip->RefreshDuration();
		MarkClipChanged();
		CommitEdit("Move Keys");

		m_Selection = std::move(moved);
		if (!m_Selection.empty())
			m_SelectedTrack = m_Selection.front().Track;
		return true;
	}

	int TimelinePanel::GetEventCount() const
	{
		return (m_Clip != nullptr) ? static_cast<int>(m_Clip->GetEvents().size()) : 0;
	}

	bool TimelinePanel::AddEventAt(float time, const char* name)
	{
		if (m_Clip == nullptr || name == nullptr || *name == '\0')
			return false;

		AnimationEvent event;
		event.Time = std::max(time, 0.0f);
		event.Name = name;

		BeginEdit();
		m_Clip->AddEvent(std::move(event));
		MarkClipChanged();
		CommitEdit("Add Event");
		return true;
	}

	bool TimelinePanel::RenameEvent(int index, const char* name)
	{
		if (m_Clip == nullptr || name == nullptr || *name == '\0')
			return false;

		std::vector<AnimationEvent>& events = m_Clip->GetEvents();
		if (index < 0 || index >= static_cast<int>(events.size()))
			return false;
		if (events[index].Name == name)
			return false;

		BeginEdit();
		events[index].Name = name;
		MarkClipChanged();
		CommitEdit("Rename Event");
		return true;
	}

	bool TimelinePanel::RemoveEventAt(int index)
	{
		if (m_Clip == nullptr)
			return false;

		std::vector<AnimationEvent>& events = m_Clip->GetEvents();
		if (index < 0 || index >= static_cast<int>(events.size()))
			return false;

		BeginEdit();
		events.erase(events.begin() + index);
		MarkClipChanged();
		CommitEdit("Delete Event");
		return true;
	}

	void TimelinePanel::ValidateSelection()
	{
		if (m_Clip == nullptr)
		{
			m_Selection.clear();
			m_SelectedTrack = -1;
			return;
		}

		const int track_count = GetTrackCount();
		if (m_SelectedTrack >= track_count)
			m_SelectedTrack = -1;

		for (size_t index = 0; index < m_Selection.size();)
		{
			const TimelineModel::KeyRef& ref = m_Selection[index];
			const bool valid = ref.Track >= 0 && ref.Track < track_count
				&& ref.Key >= 0
				&& ref.Key < static_cast<int>(m_Clip->GetTracks()[ref.Track].Keys.size());
			if (valid)
			{
				++index;
				continue;
			}

			m_Selection.erase(m_Selection.begin() + static_cast<std::ptrdiff_t>(index));
		}
	}

	void TimelinePanel::SelectSingleKey(int track, int key)
	{
		m_Selection.clear();
		m_Selection.push_back({ track, key });
		m_SelectedTrack = track;
	}

	void TimelinePanel::ToggleKeySelection(int track, int key)
	{
		const auto iter = std::find_if(m_Selection.begin(), m_Selection.end(),
			[track, key](const TimelineModel::KeyRef& ref) { return SameRef(ref, track, key); });
		if (iter != m_Selection.end())
			m_Selection.erase(iter);
		else
			m_Selection.push_back({ track, key });

		m_SelectedTrack = track;
	}

	void TimelinePanel::SelectTrack(int index)
	{
		const int count = GetTrackCount();
		m_SelectedTrack = (index >= 0 && index < count) ? index : -1;
		ClearSelection();
	}

	void TimelinePanel::SelectKey(int track, int key)
	{
		if (m_Clip == nullptr || track < 0 || track >= GetTrackCount())
			return;

		const auto& keys = m_Clip->GetTracks()[track].Keys;
		if (key < 0 || key >= static_cast<int>(keys.size()))
			return;

		SelectSingleKey(track, key);
	}

	void TimelinePanel::ApplyBoxSelection(const ImVec2& box_min, const ImVec2& box_max, bool additive)
	{
		if (!additive)
			ClearSelection();

		const float lane_top = m_LastLaneMin.y;
		for (int row = 0; row < static_cast<int>(m_VisibleTracks.size()); ++row)
		{
			const int track_index = m_VisibleTracks[row];
			const auto& keys = m_Clip->GetTracks()[track_index].Keys;
			const float center_y = lane_top + row * kRowHeight + kRowHeight * 0.5f - m_ScrollY;
			if (center_y < box_min.y || center_y > box_max.y)
				continue;

			for (int key_index = 0; key_index < static_cast<int>(keys.size()); ++key_index)
			{
				const float x = TimeToScreenX(keys[key_index].Time);
				if (x < box_min.x || x > box_max.x)
					continue;
				if (!IsKeySelected(track_index, key_index))
					m_Selection.push_back({ track_index, key_index });
			}
		}

		if (!m_Selection.empty())
			m_SelectedTrack = m_Selection.front().Track;
	}

	void TimelinePanel::ScrubTo(float time)
	{
		/* 独立编辑没有场景实体，播放头记在占位组件里 */
		if (!m_StandaloneClip && !HasSceneAnimator())
			return;

		AnimatorComponent& animator = ActiveAnimator();
		animator.m_Playing = false;
		animator.m_Scrubbing = true;
		animator.m_Time = std::max(time, 0.0f);
	}

	void TimelinePanel::JumpToKey(int track, int key)
	{
		if (m_Clip == nullptr)
			return;
		if (!m_StandaloneClip && !HasSceneAnimator())
			return;
		if (!m_StandaloneClip && !m_pScene->GetRegistry().all_of<AnimatorComponent>(m_AnimatorEntity))
			return;

		AnimatorComponent& animator = ActiveAnimator();
		if (animator.m_Playing)
			return;

		if (track < 0 || track >= GetTrackCount())
			return;

		const auto& keys = m_Clip->GetTracks()[track].Keys;
		if (key < 0 || key >= static_cast<int>(keys.size()))
			return;

		/* 贴到这一帧并定格：改值 / 拖帧时场景同步显示结果 */
		animator.m_Scrubbing = true;
		animator.m_Time = keys[key].Time;
	}

	void TimelinePanel::SetCurveComponent(int component)
	{
		m_CurveComponent = std::max(component, 0);
	}

	const AnimationTrack* TimelinePanel::GetSelectedTrackPtr() const
	{
		if (m_Clip == nullptr || m_SelectedTrack < 0 || m_SelectedTrack >= GetTrackCount())
			return nullptr;
		return &m_Clip->GetTracks()[m_SelectedTrack];
	}

	float TimelinePanel::LaneContentLeft() const
	{
		return m_LastLaneMin.x + kLanePadding;
	}

	float TimelinePanel::LaneViewWidth() const
	{
		return std::max((m_LastLaneMax.x - kLanePadding) - LaneContentLeft(), 1.0f);
	}

	float TimelinePanel::GetContentWidth() const
	{
		return LaneViewWidth() * m_Zoom;
	}

	float TimelinePanel::GetMaxScrollPixels() const
	{
		const float lane_width = LaneViewWidth();
		return std::max(lane_width * m_Zoom - lane_width, 0.0f);
	}

	void TimelinePanel::ClampView()
	{
		m_Zoom = std::clamp(m_Zoom, 1.0f, 40.0f);
		const float max_scroll = GetMaxScrollPixels();
		m_ScrollX = std::clamp(m_ScrollX, 0.0f, max_scroll);
		if (m_Zoom <= 1.0001f)
			m_ScrollX = 0.0f;
	}

	void TimelinePanel::SetZoom(float zoom)
	{
		m_Zoom = std::clamp(zoom, 1.0f, 40.0f);
		ClampView();
	}

	void TimelinePanel::SetScrollPixels(float scroll)
	{
		m_ScrollX = scroll;
		ClampView();
	}

	float TimelinePanel::TrackRowsHeight() const
	{
		return static_cast<float>(m_VisibleTracks.size()) * kRowHeight;
	}

	float TimelinePanel::GetMaxScrollRows() const
	{
		return std::max(TrackRowsHeight() - m_RowViewHeight, 0.0f);
	}

	void TimelinePanel::ClampScrollRows()
	{
		m_ScrollY = std::clamp(m_ScrollY, 0.0f, GetMaxScrollRows());
	}

	void TimelinePanel::ScrollRowsBy(float delta)
	{
		m_ScrollY += delta;
		ClampScrollRows();
	}

	void TimelinePanel::SetScrollRows(float pixels)
	{
		m_ScrollY = pixels;
		ClampScrollRows();
	}

	bool TimelinePanel::IsRowScrollbarAt(const ImVec2& point) const
	{
		if (m_LastRowBarMax.x <= m_LastRowBarMin.x)
			return false;
		return point.x >= m_LastRowBarMin.x && point.x <= m_LastRowBarMax.x
			&& point.y >= m_LastRowBarMin.y && point.y <= m_LastRowBarMax.y;
	}

	float TimelinePanel::GetLaneDuration() const
	{
		const float duration = (m_Clip != nullptr) ? m_Clip->GetDuration() : 0.0f;
		return std::max(duration, 0.1f);
	}

	float TimelinePanel::TimeToX(float time) const
	{
		return LaneContentLeft() + (time / std::max(m_LastDuration, 1e-4f)) * GetContentWidth() - m_ScrollX;
	}

	float TimelinePanel::TimeToScreenX(float time) const
	{
		return TimeToX(time);
	}

	float TimelinePanel::XToTime(float x) const
	{
		const float content = std::max(GetContentWidth(), 1.0f);
		const float ratio = (x - LaneContentLeft() + m_ScrollX) / content;
		return std::clamp(ratio, 0.0f, 1.0f) * m_LastDuration;
	}

	void TimelinePanel::BeginEdit()
	{
		if (m_Clip == nullptr || m_EditPending)
			return;

		m_EditBeforeTracks = m_Clip->GetTracks();
		m_EditBeforeEvents = m_Clip->GetEvents();
		m_EditBeforeDuration = m_Clip->GetDuration();
		m_EditPending = true;
		m_EditDirty = false;
	}

	void TimelinePanel::MarkClipChanged()
	{
		if (m_Clip != nullptr)
			m_Clip->Touch();
		MarkEditDirty();
	}

	void TimelinePanel::DiscardEdit()
	{
		m_EditBeforeTracks.clear();
		m_EditBeforeEvents.clear();
		m_EditBeforeDuration = 0.0f;
		m_EditPending = false;
		m_EditDirty = false;
	}

	void TimelinePanel::CommitEdit(const char* label)
	{
		if (m_Clip == nullptr || !m_EditPending)
			return;

		if (m_EditDirty && m_pCommandStack != nullptr)
		{
			m_pCommandStack->Execute(CreateUniquePtr<ClipEditCommand>(m_Clip,
				std::move(m_EditBeforeTracks), m_Clip->GetTracks(),
				std::move(m_EditBeforeEvents), m_Clip->GetEvents(),
				m_EditBeforeDuration, m_Clip->GetDuration(), label));
		}

		DiscardEdit();
	}

	void TimelinePanel::DrawToolbar(AnimatorComponent& animator)
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		const float control = ImGui::GetFrameHeight();

		/* 与资源面板的顶栏同一套版式：贴面板上边，控件上下各留 1px，
		 * 行尾一条分隔线；整栏高 = 控件高 + 2，不再叠一层行距 */
		constexpr float kBarInset = 1.0f;
		const ImVec2 row_min = ImGui::GetCursorScreenPos();
		const float row_height = control + kBarInset * 2.0f;
		const float control_y = row_min.y + kBarInset;
		const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;

		ImGui::SetCursorScreenPos(ImVec2(row_min.x, control_y));

		/* 独立编辑片段时没有运行时驱动播放 / 循环，这几个按钮禁用 */
		ImGui::BeginDisabled(m_StandaloneClip != nullptr);

		const bool playing = animator.m_Playing;
		if (Icons::IconButton(playing ? Icons::Id::Pause : Icons::Id::Play,
			ImVec2(control, control), playing, playing ? "Pause" : "Play"))
		{
			if (playing)
			{
				/* 暂停：停下时间推进但保留当前姿势，方便对着这一帧改 */
				animator.m_Playing = false;
				animator.m_Scrubbing = true;
			}
			else
			{
				animator.m_Playing = true;
				animator.m_Scrubbing = false;
			}
		}

		ImGui::SameLine();
		if (Icons::IconButton(Icons::Id::Stop, ImVec2(control, control), false, "Stop and restore"))
		{
			animator.m_Playing = false;
			animator.m_Scrubbing = false;
			animator.m_Time = 0.0f;
		}

		ImGui::SameLine();
		if (Icons::IconButton(Icons::Id::Loop, ImVec2(control, control), animator.m_Loop,
			animator.m_Loop ? "Loop playback" : "Play once"))
		{
			ToggleLoop();
		}

		ImGui::EndDisabled();

		ImGui::SameLine();
		ImGui::BeginDisabled(m_StandaloneClip != nullptr);
		if (Icons::IconButton(Icons::Id::Record, ImVec2(control, control), m_Recording,
			m_Recording ? "Recording: edits become keyframes" : "Record"))
		{
			SetRecording(!m_Recording);
		}
		ImGui::EndDisabled();

		/* 独立编辑片段时给一个回到实体模式的出口 */
		if (m_StandaloneClip != nullptr)
		{
			ImGui::SameLine();
			if (Icons::IconButton(Icons::Id::Remove, ImVec2(control, control), false,
				"Close clip (back to the selected entity)"))
			{
				CloseClip();
			}
			m_LastCloseClipMin = ImGui::GetItemRectMin();
			m_LastCloseClipMax = ImGui::GetItemRectMax();
		}

		ImGui::SameLine(0.0f, style.ItemSpacing.x * 2.0f);
		ImGui::AlignTextToFramePadding();
		ImGui::Text("%.2f", animator.m_Time);
		ImGui::SameLine(0.0f, style.ItemSpacing.x);

		/* 关闭按钮就在上面：点下去之后 m_Clip 已经是空的了，这里别直接把片段取出来用 */
		const float clip_duration = (m_Clip != nullptr) ? m_Clip->GetDuration() : 0.0f;
		ImGui::TextDisabled("/ %.2f s", clip_duration);
		const float time_end = ImGui::GetItemRectMax().x;

		/* 右侧：关键帧 / 曲线两格切换，再往右是属性栏开关（收起 / 展开同一枚按钮） */
		const float switch_width = (control + 8.0f) * 2.0f;
		const float toggle_x = right - control;
		const float switch_x = toggle_x - style.ItemSpacing.x - switch_width;

		/* 轨道搜索就摆在切换左边：宽度吃剩下的空间，放不下就整块不画（先保切换和开关）；
		 * 过滤中时右边多一枚清除按钮，搜索框跟着让位 */
		const bool filtering = m_TrackFilter[0] != '\0';
		const float clear_x = switch_x - style.ItemSpacing.x - control;
		const float search_right = filtering ? clear_x - style.ItemSpacing.x * 0.5f
			: switch_x - style.ItemSpacing.x * 2.0f;
		const float search_width = std::min(160.0f, search_right - time_end - style.ItemSpacing.x);

		if (search_width >= 70.0f)
		{
			ImGui::SetCursorScreenPos(ImVec2(search_right - search_width, control_y));
			ImGui::SetNextItemWidth(search_width);
			Icons::BeginSearchInput();
			const bool changed = ImGui::InputTextWithHint("##TrackFilter", "Search tracks", m_TrackFilter,
				sizeof(m_TrackFilter));
			Icons::EndSearchInput();
			if (changed)
				RebuildVisibleTracks();
			if (filtering && ImGui::IsItemHovered())
				ImGui::SetTooltip("%d of %d tracks", static_cast<int>(m_VisibleTracks.size()), GetTrackCount());
		}

		if (filtering)
		{
			ImGui::SetCursorScreenPos(ImVec2(clear_x, control_y));
			if (Icons::IconButton(Icons::Id::Remove, ImVec2(control, control), false, "Clear search"))
			{
				m_TrackFilter[0] = '\0';
				RebuildVisibleTracks();
			}
		}

		ImGui::SetCursorScreenPos(ImVec2(switch_x, control_y));
		m_LastModeSwitchMin = ImVec2(switch_x, control_y);
		m_LastModeSwitchMax = ImVec2(switch_x + switch_width, control_y + control);
		const int switched = SegmentedModeSwitch("##TimelineMode", Icons::Id::Animation, "Keyframe mode",
			Icons::Id::Curve, "Curve mode", m_CurveView, m_ModeSlide, switch_width);
		if (switched >= 0)
			m_CurveView = (switched == 1);

		ImGui::SetCursorScreenPos(ImVec2(toggle_x, control_y));
		if (Icons::IconButton(m_InspectorOpen ? Icons::Id::CollapseSidebar : Icons::Id::ExpandSidebar,
			ImVec2(control, control), false,
			m_InspectorOpen ? "Collapse properties" : "Expand properties"))
		{
			m_InspectorOpen = !m_InspectorOpen;
		}

		/* 收尾：光标落到栏底，分隔线交给调用方画（与资源面板的 EndHeaderRow 一致） */
		ImGui::SetCursorScreenPos(ImVec2(row_min.x, row_min.y + row_height));
	}

	void TimelinePanel::DrawTrackHeader(AnimatorComponent& animator, const ImVec2& origin, float track_list_width, float height)
	{
		ImDrawList* draw_list = ImGui::GetWindowDrawList();
		const ImVec2 header_max(origin.x + track_list_width, origin.y + height);
		draw_list->AddRectFilled(origin, header_max, ColorU32(EditorTheme::Token::CardHeaderBg));
		draw_list->AddLine(ImVec2(header_max.x, origin.y), ImVec2(header_max.x, header_max.y),
			ColorU32(EditorTheme::Token::Neutral4), 1.0f);

		const float pad = 4.0f;
		const float button = std::min(height - 4.0f, ImGui::GetFrameHeight());

		ImGui::SetCursorScreenPos(ImVec2(origin.x + pad, origin.y + (height - button) * 0.5f));
		/* 新建类入口统一用 NewAsset，和资源浏览器 / 层级面板保持一致；
		 * 独立编辑片段时没有实体可枚举属性，这个入口禁用 */
		ImGui::BeginDisabled(m_StandaloneClip != nullptr);
		if (Icons::IconButton(Icons::Id::NewAsset, ImVec2(button, button), false,
			m_StandaloneClip != nullptr ? "Select an entity with an Animator to add tracks" : "Add Track"))
		{
			m_AddTrackEntriesValid = false;
			ImGui::OpenPopup("##AddTrackMenu");
		}
		ImGui::EndDisabled();
		DrawAddTrackPopup(animator);
	}

	void TimelinePanel::DrawFooterBar()
	{
		if (m_Clip == nullptr)
			return;

		ImGuiWindow* window = ImGui::GetCurrentWindow();
		const ImGuiStyle& style = ImGui::GetStyle();

		/* 与资源面板的路径栏同一套版式：整栏高 = 控件高，贴面板下边；
		 * 上边线自绘（Separator 会吃掉「1px + 行距」，这一栏就高出一截了） */
		const float bar_height = ImGui::GetFrameHeight();
		const float bar_top = window->Pos.y + window->Size.y - bar_height;
		/* 横坐标按窗口内算（右端要对齐内容区），画线和摆控件时再加窗口原点 */
		const float left_rel = window->DC.CursorStartPos.x - window->Pos.x;
		const float right_rel = ImGui::GetWindowContentRegionMax().x;

		ImGui::GetWindowDrawList()->AddLine(ImVec2(window->Pos.x + left_rel, bar_top - 0.5f),
			ImVec2(window->Pos.x + right_rel, bar_top - 0.5f),
			ColorU32(EditorTheme::Token::Border), 1.0f);

		/* 左：片段时长，直接拖就能改 */
		ImGui::SetCursorScreenPos(ImVec2(window->Pos.x + left_rel, bar_top));
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("Length");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(66.0f);
		float duration = m_Clip->GetDuration();
		if (ImGui::DragFloat("##ClipDuration", &duration, 0.01f, 0.1f, 600.0f, "%.2f s"))
		{
			BeginEdit();
			m_Clip->SetDuration(duration);
			MarkClipChanged();
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Clip length in seconds");
		if (ImGui::IsItemDeactivatedAfterEdit())
			CommitEdit("Clip Duration");

		const float left_end = ImGui::GetItemRectMax().x - window->Pos.x + style.ItemSpacing.x * 2.0f;

		/* 缩放放大后才出现，排在文件名左边；轨道宽度固定，栏太窄时整组不画 */
		constexpr float kZoomTrackWidth = 100.0f;
		char zoom_text[16] = {};
		float zoom_width = 0.0f;
		const bool show_zoom = m_Zoom > 1.001f && (right_rel - left_end) > 240.0f;
		if (show_zoom)
		{
			std::snprintf(zoom_text, sizeof(zoom_text), "%.0f%%", m_Zoom * 100.0f);
			zoom_width = kZoomTrackWidth + style.ItemInnerSpacing.x + ImGui::CalcTextSize(zoom_text).x;
		}
		const float zoom_gap = show_zoom ? zoom_width + style.ItemSpacing.x * 2.0f : 0.0f;

		/* 右：片段文件名，录制时前面加 REC；文本按栏高居中，放不下从尾部截断 */
		std::string name = "(unsaved clip)";
		if (!m_Clip->GetPath().empty())
			name = PathToUtf8(PathFromUtf8(m_Clip->GetPath()).filename());

		const char* rec_label = "REC";
		const float rec_width = m_Recording ? ImGui::CalcTextSize(rec_label).x + style.ItemSpacing.x : 0.0f;
		name = Ellipsize(name, right_rel - left_end - rec_width - zoom_gap);
		const float name_width = ImGui::CalcTextSize(name.c_str()).x;
		const float text_y = bar_top + (bar_height - ImGui::GetFontSize()) * 0.5f;

		/* 自己摆文本：同行前面若是按钮 / 拖条，它会为基线对齐把文字整体下移，居中值就被顶掉了 */
		const auto place_text = [&](float x)
		{
			ImGui::SetCursorScreenPos(ImVec2(window->Pos.x + x, text_y));
			window->DC.CurrLineTextBaseOffset = 0.0f;
		};

		const float name_x = right_rel - name_width;
		const float rec_x = name_x - rec_width;

		if (show_zoom)
		{
			const float zoom_right = (m_Recording ? rec_x : name_x) - style.ItemSpacing.x * 2.0f;
			DrawZoomBar(zoom_right - zoom_width, bar_top, bar_height, kZoomTrackWidth, zoom_text);
		}

		if (m_Recording)
		{
			place_text(rec_x);
			ImGui::TextColored(EditorTheme::Token::Danger, "%s", rec_label);
		}
		place_text(name_x);
		ImGui::TextDisabled("%s", name.c_str());
	}

	void TimelinePanel::DrawAddTrackPopup(AnimatorComponent& animator)
	{
		(void)animator;
		if (!ImGui::BeginPopup("##AddTrackMenu"))
			return;

		/* 版式和层级 / 资源面板的「新建」弹层一致：打开就清空并聚焦搜索框，
		 * 搜索框带放大镜、下面一条分隔线，条目是带图标的菜单行 */
		if (ImGui::IsWindowAppearing())
		{
			m_AddTrackFilter[0] = '\0';
			ImGui::SetKeyboardFocusHere();
		}

		if (!m_AddTrackEntriesValid)
		{
			m_AddTrackEntries = (m_pScene != nullptr && m_AnimatorEntity != entt::null)
				? TimelineModel::CollectAnimatableProperties(*m_pScene, m_AnimatorEntity)
				: std::vector<TimelineModel::PropertyEntry>{};
			m_AddTrackEntriesValid = true;
		}

		ImGui::SetNextItemWidth(Scaled(220.0f));
		Icons::BeginSearchInput();
		ImGui::InputTextWithHint("##AddTrackSearch", "Search...", m_AddTrackFilter,
			sizeof(m_AddTrackFilter));
		Icons::EndSearchInput();
		ImGui::Separator();

		const std::string animator_name = (m_pScene != nullptr && m_AnimatorEntity != entt::null)
			? EntityName(*m_pScene, m_AnimatorEntity) : std::string("Root");

		bool any = false;
		for (const TimelineModel::PropertyEntry& entry : m_AddTrackEntries)
		{
			const std::string owner = entry.Path.empty() ? animator_name : entry.Path;
			const std::string label = owner + " · " + entry.Component + "." + entry.Property;
			if (!ContainsIgnoreCase(label, m_AddTrackFilter))
				continue;

			any = true;
			if (PanelChrome::MenuItemWithIcon(TrackComponentIcon(entry.Component), label.c_str()))
			{
				AddTrack(entry);
				ImGui::CloseCurrentPopup();
				break;
			}
		}

		if (!any)
			ImGui::TextDisabled("No matching property");

		ImGui::EndPopup();
	}

	bool TimelinePanel::AddTrack(const TimelineModel::PropertyEntry& entry)
	{
		if (m_pScene == nullptr || m_AnimatorEntity == entt::null || m_Clip == nullptr)
			return false;

		AnimatorComponent& animator = m_pScene->GetRegistry().get<AnimatorComponent>(m_AnimatorEntity);

		/* 新加的轨道不该被搜索挡住 */
		m_TrackFilter[0] = '\0';

		const std::vector<AnimationTrack>& tracks = m_Clip->GetTracks();
		for (int index = 0; index < static_cast<int>(tracks.size()); ++index)
		{
			const AnimationTrack& track = tracks[index];
			if (track.Path == entry.Path && track.Component == entry.Component && track.Property == entry.Property)
			{
				SelectTrack(index);
				return false;
			}
		}

		/* 轨道表长度变了，正在播的会话下标会错位：先停 */
		animator.m_Playing = false;
		animator.m_Scrubbing = false;

		BeginEdit();

		AnimationTrack track;
		track.Path = entry.Path;
		track.Component = entry.Component;
		track.Property = entry.Property;
		track.ValueType = entry.ValueType;
		/* 可见性只有开 / 关两态，默认阶梯插值 */
		track.Interp = AnimationBinding::IsVisibilityTrack(track) ? KeyInterp::Step : KeyInterp::Linear;

		glm::vec4 value;
		if (TimelineModel::ReadCurrentValue(*m_pScene, m_AnimatorEntity, track, value))
			TimelineModel::InsertOrUpdateKey(track, animator.m_Time, value, track.Interp);

		m_Clip->AddTrack(std::move(track));
		m_Clip->RefreshDuration();
		MarkClipChanged();
		CommitEdit("Add Track");

		const int new_index = static_cast<int>(m_Clip->GetTracks().size()) - 1;
		SelectTrack(new_index);
		/* 新轨道接在末尾：滚到能看见它，免得以为没加上 */
		const int new_row = TrackToVisible(new_index);
		if (new_row >= 0)
		{
			const float row_top = new_row * kRowHeight;
			if (row_top + kRowHeight > m_ScrollY + m_RowViewHeight)
				m_ScrollY = row_top + kRowHeight - m_RowViewHeight;
			if (row_top < m_ScrollY)
				m_ScrollY = row_top;
			ClampScrollRows();
		}
		if (!m_Clip->GetTracks()[new_index].Keys.empty())
		{
			SelectSingleKey(new_index, 0);
			JumpToKey(new_index, 0);
		}
		return true;
	}

	bool TimelinePanel::DeleteSelectedKeys()
	{
		if (m_Clip == nullptr || m_Selection.empty())
			return false;
		if (m_pScene == nullptr || m_AnimatorEntity == entt::null)
			return false;

		BeginEdit();
		TimelineModel::RemoveKeys(m_Clip->GetTracks(), m_Selection);
		MarkClipChanged();
		CommitEdit("Delete Keys");
		ClearSelection();
		return true;
	}

	bool TimelinePanel::DeleteSelectedTrack()
	{
		if (m_SelectedTrack < 0 || m_SelectedTrack >= GetTrackCount())
			return false;

		DeleteTrackAt(m_SelectedTrack);
		return true;
	}

	void TimelinePanel::RequestDeleteSelectedKeys()
	{
		if (m_Clip == nullptr || m_Selection.empty())
			return;

		/* 这里只打标记：真正的 OpenPopup 要留在面板窗口里发，
		 * 属性栏子窗口里点删除时窗口 ID 不同，弹窗会打不开 */
		m_DeleteConfirmRequested = true;
		m_DeleteConfirmCount = static_cast<int>(m_Selection.size());
	}

	void TimelinePanel::DrawKeyContextMenu()
	{
		m_KeyMenuOpen = ImGui::BeginPopupEx(m_KeyMenuId,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings
			| ImGuiWindowFlags_NoTitleBar);
		if (!m_KeyMenuOpen)
			return;

		const bool has_selection = !m_Selection.empty();

		ImGui::BeginDisabled(!has_selection);
		if (ImGui::MenuItem("Copy", "Ctrl+C"))
		{
			CopySelectedKeys();
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndDisabled();

		ImGui::BeginDisabled(m_CopiedKeys.empty());
		if (ImGui::MenuItem("Paste", "Ctrl+V"))
		{
			/* 落点就是右键点中的那一帧 */
			PasteSelectedKeys(m_KeyMenuTime);
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndDisabled();

		ImGui::Separator();

		/* 选中帧的每帧插值：几帧不一致时不打勾 */
		int common_interp = -2;
		for (const TimelineModel::KeyRef& selection_ref : m_Selection)
		{
			if (selection_ref.Track < 0 || selection_ref.Track >= GetTrackCount())
				continue;
			const AnimationTrack& selection_track = m_Clip->GetTracks()[selection_ref.Track];
			if (selection_ref.Key < 0 || selection_ref.Key >= static_cast<int>(selection_track.Keys.size()))
				continue;

			const AnimationKey& selection_key = selection_track.Keys[selection_ref.Key];
			const int value = selection_key.HasInterp ? static_cast<int>(selection_key.Interp) + 1 : 0;
			if (common_interp == -2)
				common_interp = value;
			else if (common_interp != value)
				common_interp = -1;
		}

		if (ImGui::BeginMenu("Interpolation"))
		{
			static constexpr const char* kItems[] = { "Follow track", "Step", "Linear", "Bezier" };
			for (int index = 0; index < 4; ++index)
			{
				if (!ImGui::MenuItem(kItems[index], nullptr, common_interp == index))
					continue;

				SetSelectedKeyInterp(index);
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndMenu();
		}

		ImGui::Separator();

		/* 删除还是先弹确认框：右键是入口，误删不该由一次点击决定 */
		ImGui::BeginDisabled(!has_selection);
		if (ImGui::MenuItem("Delete", "Del"))
		{
			RequestDeleteSelectedKeys();
			ImGui::CloseCurrentPopup();
		}
		m_KeyMenuDeleteMin = ImGui::GetItemRectMin();
		m_KeyMenuDeleteMax = ImGui::GetItemRectMax();
		ImGui::EndDisabled();

		ImGui::EndPopup();
	}

	void TimelinePanel::DrawEventMenu()
	{
		if (!ImGui::BeginPopup("##TimelineEventMenu"))
			return;

		ImGui::TextDisabled(m_EventMenuIndex >= 0 ? "Event %.2f s" : "New event at %.2f s", m_EventMenuTime);
		ImGui::SetNextItemWidth(200.0f);
		ImGui::InputTextWithHint("##EventName", "Event name", m_EventMenuName, sizeof(m_EventMenuName));
		if (ImGui::IsWindowAppearing())
			ImGui::SetKeyboardFocusHere(-1);

		const bool has_name = m_EventMenuName[0] != '\0';
		if (m_EventMenuIndex >= 0)
		{
			ImGui::BeginDisabled(!has_name);
			if (ImGui::Button("Rename"))
			{
				RenameEvent(m_EventMenuIndex, m_EventMenuName);
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (ImGui::Button("Delete"))
			{
				RemoveEventAt(m_EventMenuIndex);
				ImGui::CloseCurrentPopup();
			}
		}
		else
		{
			ImGui::BeginDisabled(!has_name);
			if (ImGui::Button("Add"))
			{
				AddEventAt(m_EventMenuTime, m_EventMenuName);
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndDisabled();
		}

		ImGui::EndPopup();
	}

	void TimelinePanel::DrawDeleteConfirmPopup()
	{
		if (m_DeleteConfirmRequested)
		{
			m_DeleteConfirmRequested = false;
			ImGui::OpenPopup("Delete Keys###TimelineDeleteKeys");
		}

		/* 弹窗落在整个面板正中；只在出现那一帧定位，之后还能拖走 */
		const ImVec2 panel_min = ImGui::GetWindowPos();
		const ImVec2 panel_size = ImGui::GetWindowSize();
		ImGui::SetNextWindowPos(ImVec2(panel_min.x + panel_size.x * 0.5f, panel_min.y + panel_size.y * 0.5f),
			ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

		m_LastConfirmDeleteMin = ImVec2(0.0f, 0.0f);
		m_LastConfirmDeleteMax = ImVec2(0.0f, 0.0f);

		m_DeleteConfirmOpen = ImGui::BeginPopupModal("Delete Keys###TimelineDeleteKeys", nullptr,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
		if (!m_DeleteConfirmOpen)
			return;

		const int count = m_DeleteConfirmCount;
		ImGui::Text("Delete %d key%s?", count, count == 1 ? "" : "s");
		ImGui::TextDisabled("You can undo this with Ctrl+Z.");

		const ImGuiStyle& style = ImGui::GetStyle();
		const float button_width = 84.0f;
		const float buttons_width = button_width * 2.0f + style.ItemSpacing.x;
		/* 第一帧还没量到自适应宽度，先按当前光标位置放，下一帧就靠右了 */
		const float buttons_x = std::max(ImGui::GetWindowContentRegionMax().x - buttons_width,
			ImGui::GetCursorPosX());
		ImGui::SetCursorPosX(buttons_x);

		/* 焦点默认给取消：回车不会顺手删掉 */
		if (ImGui::IsWindowAppearing())
			ImGui::SetKeyboardFocusHere();
		if (ImGui::Button("Cancel", ImVec2(button_width, 0.0f)))
			ImGui::CloseCurrentPopup();

		ImGui::SameLine();
		ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::WithAlpha(EditorTheme::Token::Danger, 0.24f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorTheme::WithAlpha(EditorTheme::Token::Danger, 0.40f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, EditorTheme::WithAlpha(EditorTheme::Token::Danger, 0.54f));
		const bool confirmed = ImGui::Button("Delete", ImVec2(button_width, 0.0f));
		m_LastConfirmDeleteMin = ImGui::GetItemRectMin();
		m_LastConfirmDeleteMax = ImGui::GetItemRectMax();
		ImGui::PopStyleColor(3);

		if (confirmed)
		{
			DeleteSelectedKeys();
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	}

	void TimelinePanel::DeleteTrackAt(int track)
	{
		if (m_Clip == nullptr || track < 0 || track >= GetTrackCount())
			return;
		if (!m_StandaloneClip && !HasSceneAnimator())
			return;

		AnimatorComponent& animator = ActiveAnimator();
		animator.m_Playing = false;
		animator.m_Scrubbing = false;

		BeginEdit();
		std::vector<AnimationTrack>& tracks = m_Clip->GetTracks();
		tracks.erase(tracks.begin() + track);
		MarkClipChanged();
		CommitEdit("Delete Track");

		/* 这一帧剩下的绘制还得用可见轨道表：立刻按新表重建，旧下标别越界 */
		RebuildVisibleTracks();

		ClearSelection();
		if (m_SelectedTrack == track)
			m_SelectedTrack = -1;
		else if (m_SelectedTrack > track)
			--m_SelectedTrack;
	}

	void TimelinePanel::ToggleTrackEnabled(int track)
	{
		if (m_Clip == nullptr || track < 0 || track >= GetTrackCount())
			return;

		AnimationTrack& entry = m_Clip->GetTracks()[track];
		BeginEdit();
		entry.Enabled = !entry.Enabled;
		MarkClipChanged();
		CommitEdit(entry.Enabled ? "Enable Track" : "Disable Track");
	}

	bool TimelinePanel::SetTrackInterp(KeyInterp interp)
	{
		if (m_Clip == nullptr || m_SelectedTrack < 0 || m_SelectedTrack >= GetTrackCount())
			return false;

		AnimationTrack& track = m_Clip->GetTracks()[m_SelectedTrack];
		if (track.Interp == interp)
			return true;

		BeginEdit();
		track.Interp = interp;
		MarkClipChanged();
		CommitEdit("Set Interpolation");
		return true;
	}

	void TimelinePanel::DrawBody(Scene& scene, AnimatorComponent& animator)
	{
		/* 底栏贴着面板下边画（栏高 = 控件高），这里给它让出高度 + 一格行距，
		 * 免得 child 恰好填满后把外层顶出竖向滚动条 */
		const float footer_height = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y;
		const float body_height = std::max(ImGui::GetContentRegionAvail().y - footer_height,
			kRulerHeight + kRowHeight);

		/* 子窗口只当画布用：不给自己滚动条，也不接滚轮。
		 * 轨道行多了在这一层由 DrawRowScrollbar 自己滚，标尺 / 属性栏 / 曲线才不会跟着跑出可视区 */
		ImGui::BeginChild("##TimelineBody", ImVec2(0.0f, body_height), false,
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

		const ImVec2 origin = ImGui::GetCursorScreenPos();
		const float content_width = std::max(ImGui::GetContentRegionAvail().x, 320.0f);
		/* 属性栏要撑满 body：这里量的是从 origin 到内容区底部的整段高度 */
		const float body_content_height = ImGui::GetContentRegionAvail().y;

		/* 右侧属性编辑栏：宽度能拖（左缘的把手），收起后轨道区拿走这块宽度 */
		const float inspector_max = std::max(content_width - 260.0f, 180.0f);
		const float inspector_width = m_InspectorOpen
			? std::clamp(m_InspectorWidth, 180.0f, inspector_max)
			: 0.0f;
		const float gap = (inspector_width > 0.0f) ? 6.0f : 0.0f;
		const float lane_area_width = std::max(content_width - inspector_width - gap, 180.0f);

		const float track_list_width = std::min(kTrackListWidth, lane_area_width * 0.45f);

		/* 轨道区域三列：轨道名 | 时刻区域 | 操作（删除）。
		 * 时刻区域的底色 / 标尺一直铺到最右侧（操作列的图标浮在上面），不再分块；
		 * 时间轴内容仍收在操作列左边，末帧的关键帧才不会藏到垃圾桶图标底下。 */
		const bool curve_mode = m_CurveView;
		const float action_width = curve_mode ? 0.0f : 30.0f;
		const float action_left = origin.x + lane_area_width - action_width;
		const float lane_left = origin.x + track_list_width;
		const float lane_right = curve_mode ? origin.x + lane_area_width - 6.0f : action_left - 4.0f;
		const float row_right = curve_mode ? lane_left : origin.x + lane_area_width;
		const float lane_fill_right = origin.x + lane_area_width;

		const int visible_count = static_cast<int>(m_VisibleTracks.size());
		const float rows_total_height = static_cast<float>(visible_count) * kRowHeight;
		/* 曲线区：上沿比标尺低 2px、下沿离 body 底 6px；节点列跟着它的下沿收，两列底边对齐 */
		const float curve_top = origin.y + kRulerHeight + 2.0f;
		const float curve_bottom = origin.y + body_height - 6.0f;
		/* 轨道行的可视区：标尺下沿到 body 内容区底部，行多了就在这块里滚 */
		const float rows_top = origin.y + kRulerHeight;
		const float rows_bottom_limit = curve_mode ? curve_bottom : origin.y + body_content_height;
		const float rows_view_height = std::max(rows_bottom_limit - rows_top, kRowHeight);
		m_RowViewHeight = rows_view_height;
		ClampScrollRows();

		/* 曲线模式没有关键帧行：轨道矩形收成一条线（曲线区另算） */
		const float lanes_bottom = curve_mode ? rows_top
			: rows_top + std::min(std::max(rows_total_height, kRowHeight), rows_view_height);

		m_LastLaneMin = ImVec2(lane_left, rows_top);
		m_LastLaneMax = ImVec2(lane_right, lanes_bottom);
		m_LastRowHeight = kRowHeight;
		m_LastRulerHeight = kRulerHeight;
		m_LastTrackListWidth = track_list_width;
		m_LastActionMinX = curve_mode ? 0.0f : action_left;
		m_LastActionMaxX = curve_mode ? 0.0f : origin.x + lane_area_width;
		m_LastDuration = GetLaneDuration();
		ClampView();

		ImGuiIO& io = ImGui::GetIO();
		const ImVec2 mouse = io.MousePos;

		/* 标尺和轨道区拆成独立命中区：标题栏里的搜索框才不会被盖住 */
		ImGui::SetCursorScreenPos(ImVec2(lane_left, origin.y));
		ImGui::InvisibleButton("##TimelineRuler",
			ImVec2(std::max(lane_right - lane_left, 1.0f), kRulerHeight));
		const bool ruler_hovered = ImGui::IsItemHovered();

		bool rows_hovered = false;
		bool list_hovered = false;
		if (curve_mode)
		{
			ImGui::SetCursorScreenPos(ImVec2(origin.x, rows_top));
			ImGui::InvisibleButton("##TimelineTrackList", ImVec2(track_list_width, rows_view_height));
			list_hovered = ImGui::IsItemHovered();
		}
		else
		{
			/* 命中区只铺到可视区底部：轨道下方的空白处也能起框选，行滚出视口的部分不接鼠标 */
			ImGui::SetCursorScreenPos(ImVec2(origin.x, rows_top));
			ImGui::InvisibleButton("##TimelineLane", ImVec2(lane_area_width, rows_view_height));
			rows_hovered = ImGui::IsItemHovered();
		}

		/* 曲线区的命中区先摆好：滚轮缩放 / 横移要在同一帧里知道鼠标在不在曲线上 */
		m_LastCurveMin = ImVec2(0.0f, 0.0f);
		m_LastCurveMax = ImVec2(0.0f, 0.0f);
		bool curve_hovered = false;
		if (curve_mode)
		{
			m_LastCurveMin = ImVec2(lane_left, curve_top);
			m_LastCurveMax = ImVec2(lane_right, curve_bottom);

			ImGui::SetCursorScreenPos(ImVec2(lane_left, curve_top));
			ImGui::InvisibleButton("##TimelineCurve", ImVec2(std::max(lane_right - lane_left, 1.0f),
				std::max(curve_bottom - curve_top, 1.0f)));
			curve_hovered = ImGui::IsItemHovered();
		}

		/* 轨道列标题栏：新建轨道 + 搜索 */
		DrawTrackHeader(animator, origin, track_list_width, kRulerHeight);

		/* 拖标尺定播放头 */
		if (ruler_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
		{
			m_ScrubbingRuler = true;
			ScrubTo(XToTime(mouse.x));
		}
		if (m_ScrubbingRuler && ImGui::IsMouseDown(ImGuiMouseButton_Left))
			ScrubTo(XToTime(mouse.x));
		if (m_ScrubbingRuler && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
			m_ScrubbingRuler = false;

		/* 标尺右键：事件编辑，点在已有标记附近就是改它 */
		if (ruler_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
		{
			m_EventMenuTime = std::max(XToTime(mouse.x), 0.0f);
			m_EventMenuIndex = -1;

			const std::vector<AnimationEvent>& events = m_Clip->GetEvents();
			float best_distance = 8.0f;
			for (int index = 0; index < static_cast<int>(events.size()); ++index)
			{
				const float distance = std::fabs(TimeToScreenX(events[index].Time) - mouse.x);
				if (distance <= best_distance)
				{
					best_distance = distance;
					m_EventMenuIndex = index;
				}
			}

			if (m_EventMenuIndex >= 0)
				m_EventMenuTime = events[m_EventMenuIndex].Time;
			std::snprintf(m_EventMenuName, sizeof(m_EventMenuName), "%s",
				m_EventMenuIndex >= 0 ? events[m_EventMenuIndex].Name.c_str() : "Event");
			ImGui::OpenPopup("##TimelineEventMenu");
		}
		DrawEventMenu();

		/* 滚轮：Ctrl 在鼠标处缩放，Shift 横向平移，其余上下滚轨道行 */
		if ((ruler_hovered || rows_hovered || list_hovered || curve_hovered) && io.MouseWheel != 0.0f)
		{
			if (io.KeyCtrl || io.KeySuper)
			{
				const float anchor_time = XToTime(mouse.x);
				const float new_zoom = std::clamp(m_Zoom * std::pow(1.15f, io.MouseWheel), 1.0f, 40.0f);
				if (std::fabs(new_zoom - m_Zoom) > 1e-5f)
				{
					m_Zoom = new_zoom;
					m_ScrollX = (anchor_time / std::max(GetLaneDuration(), 1e-4f)) * GetContentWidth()
						- (mouse.x - LaneContentLeft());
					ClampView();
				}
			}
			else if (io.KeyShift)
			{
				m_ScrollX -= io.MouseWheel * 60.0f;
				ClampView();
			}
			else
			{
				ScrollRowsBy(-io.MouseWheel * kRowHeight * kRowsPerWheelNotch);
			}
		}

		DrawRulerAndPlayhead(animator, origin, lane_left, lane_right, lane_fill_right);
		DrawTrackRows(animator, origin, lane_left, lane_right, row_right, !curve_mode);
		DrawRowScrollbar(lane_left, rows_top, rows_top + rows_view_height);

		/* 操作列：底色与标尺 / 行底连续，只画逐行的垃圾桶；点整格删轨道 */
		if (!curve_mode)
		{
			ImDrawList* draw_list = ImGui::GetWindowDrawList();
			const float action_center_x = (action_left + row_right) * 0.5f;
			draw_list->PushClipRect(ImVec2(action_left, rows_top), ImVec2(row_right, rows_top + rows_view_height), true);
			for (int row = 0; row < visible_count; ++row)
			{
				const float row_top = rows_top + row * kRowHeight - m_ScrollY;
				if (row_top + kRowHeight < rows_top || row_top > rows_top + rows_view_height)
					continue;
				const float row_center = row_top + kRowHeight * 0.5f;
				const bool hovered_cell = mouse.x >= action_left && mouse.x <= row_right
					&& mouse.y >= row_top && mouse.y <= row_top + kRowHeight;
				if (hovered_cell)
				{
					draw_list->AddRectFilled(ImVec2(action_left + 3.0f, row_top + 2.0f),
						ImVec2(row_right - 3.0f, row_top + kRowHeight - 2.0f),
						ColorU32(EditorTheme::WithAlpha(EditorTheme::Token::Danger, 0.16f)), 3.0f);
				}
				Icons::Draw(draw_list, Icons::Id::Delete, ImVec2(action_center_x, row_center), 11.0f,
					hovered_cell ? ColorU32(EditorTheme::Token::Danger)
						: ColorU32(EditorTheme::WithAlpha(EditorTheme::Token::TextDim, 0.65f)));
			}
			draw_list->PopClipRect();

			const int hover_row = (mouse.y >= rows_top && mouse.y <= rows_top + rows_view_height)
				? static_cast<int>((mouse.y - rows_top + m_ScrollY) / kRowHeight) : -1;
			if (mouse.x >= action_left && mouse.x <= row_right && hover_row >= 0 && hover_row < visible_count)
				ImGui::SetTooltip("Delete track");

			if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && mouse.x >= action_left && mouse.x <= row_right
				&& hover_row >= 0 && hover_row < visible_count)
			{
				DeleteTrackAt(VisibleToTrack(hover_row));
			}
		}

		if (curve_mode)
		{
			/* 左侧列表：点图标开关轨道，点名字选中 */
			const float icon_left = origin.x + 7.0f;
			const float icon_right = icon_left + 11.0f;
			const auto row_from_y = [&](float y)
			{
				if (y < rows_top || y > rows_top + rows_view_height)
					return -1;
				const int row = static_cast<int>((y - rows_top + m_ScrollY) / kRowHeight);
				return (row >= 0 && row < visible_count) ? row : -1;
			};

			if (list_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !IsRowScrollbarAt(mouse))
			{
				const int row = row_from_y(mouse.y);
				if (row >= 0)
				{
					if (mouse.x >= icon_left && mouse.x <= icon_right)
						ToggleTrackEnabled(VisibleToTrack(row));
					else
						SelectTrack(VisibleToTrack(row));
				}
			}
			if (list_hovered)
			{
				const int row = row_from_y(mouse.y);
				const int hover_track = VisibleToTrack(row);
				if (hover_track >= 0 && mouse.x >= icon_left && mouse.x <= icon_right)
					ImGui::SetTooltip(m_Clip->GetTracks()[hover_track].Enabled ? "Disable track" : "Enable track");
			}
		}
		else
		{
			DrawLanes(scene, rows_top, rows_top + rows_view_height, lane_left, lane_right, rows_hovered);
		}

		/* 曲线模式：整块右侧给曲线（命中区已在上面摆好，这里只画） */
		if (curve_mode)
		{
			DrawCurveArea(scene, animator, curve_top, curve_bottom, lane_left, lane_right, curve_hovered);
		}

		/* 右侧属性编辑栏 */
		if (inspector_width > 0.0f)
		{
			const float inspector_x = origin.x + lane_area_width + gap;
			const float inspector_actual = std::max(origin.x + content_width - inspector_x, 120.0f);
			/* 撑满 body 的内容区：从 origin 一直到底部，卡片四周才不留空 */
			const float inspector_height = std::max(body_content_height, 80.0f);

			/* 左缘把手：左右拖改属性栏宽度（时刻区跟着变宽变窄） */
			ImGui::SetCursorScreenPos(ImVec2(inspector_x - 5.0f, origin.y));
			ImGui::InvisibleButton("##InspectorResize", ImVec2(6.0f, inspector_height));
			m_LastInspectorResizeMin = ImGui::GetItemRectMin();
			m_LastInspectorResizeMax = ImGui::GetItemRectMax();
			const bool resizing = ImGui::IsItemActive();
			if (ImGui::IsItemHovered() || resizing)
			{
				ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
				ImGui::GetWindowDrawList()->AddLine(ImVec2(inspector_x - 1.0f, origin.y),
					ImVec2(inspector_x - 1.0f, origin.y + inspector_height),
					ColorU32(EditorTheme::Token::Accent), 1.0f);
			}
			if (ImGui::IsItemActivated())
			{
				m_InspectorResizeStartX = ImGui::GetIO().MousePos.x;
				m_InspectorResizeStartWidth = inspector_width;
			}
			if (resizing)
			{
				m_InspectorWidth = std::clamp(m_InspectorResizeStartWidth
					- (ImGui::GetIO().MousePos.x - m_InspectorResizeStartX), 180.0f, inspector_max);
			}

			ImGui::SetCursorScreenPos(ImVec2(inspector_x, origin.y));
			DrawInspector(animator, inspector_actual, inspector_height);
		}

		ImGui::EndChild();
	}

	void TimelinePanel::DrawLanes(Scene& scene, float lane_top, float lane_bottom,
		float lane_left, float lane_right, bool rows_hovered)
	{
		ImGuiIO& io = ImGui::GetIO();
		const ImVec2 mouse = io.MousePos;
		const int visible_count = static_cast<int>(m_VisibleTracks.size());
		const bool ctrl = io.KeyCtrl || io.KeySuper;
		/* 落在轨道列右缘的滚动条上时，这一下留给滚动条 */
		const bool hovered = rows_hovered && !IsRowScrollbarAt(mouse);

		const auto row_at = [this, lane_top, lane_bottom, visible_count](float y)
		{
			if (y < lane_top || y > lane_bottom)
				return -1;
			const int row = static_cast<int>((y - lane_top + m_ScrollY) / kRowHeight);
			return (row >= 0 && row < visible_count) ? row : -1;
		};

		const auto hit_key = [this](int row, float x)
		{
			const int track_index = VisibleToTrack(row);
			if (track_index < 0)
				return -1;

			int best = -1;
			float best_delta = kKeyHitRadius;
			const auto& keys = m_Clip->GetTracks()[track_index].Keys;
			for (int index = 0; index < static_cast<int>(keys.size()); ++index)
			{
				const float delta = std::fabs(TimeToScreenX(keys[index].Time) - x);
				if (delta <= best_delta)
				{
					best = index;
					best_delta = delta;
				}
			}
			return best;
		};

		const float icon_left = lane_left - m_LastTrackListWidth + 7.0f;
		const float icon_right = icon_left + 11.0f;
		if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
		{
			const int icon_row = row_at(mouse.y);
			if (mouse.x >= icon_left && mouse.x <= icon_right && icon_row >= 0)
			{
				ToggleTrackEnabled(VisibleToTrack(icon_row));
			}
			else if (mouse.x < lane_left)
			{
				SelectTrack(VisibleToTrack(row_at(mouse.y)));
			}
			else if (mouse.x <= lane_right + 1.0f)
			{
				const int row = row_at(mouse.y);
				{
					const int track_index = VisibleToTrack(row);
					const int key = (row >= 0) ? hit_key(row, mouse.x) : -1;
					if (key >= 0 && track_index >= 0)
					{
						m_SelectedTrack = track_index;
						m_InspectorOpen = true;
						if (ctrl)
						{
							ToggleKeySelection(track_index, key);
						}
						else
						{
							if (!IsKeySelected(track_index, key))
								SelectSingleKey(track_index, key);

							const AnimationTrack& track = m_Clip->GetTracks()[track_index];
							JumpToKey(track_index, key);

							m_DraggingKey = true;
							m_DragChanged = false;
							m_DragStartTime = SnapTime(XToTime(mouse.x));
							m_DragStartMouseY = mouse.y;
							m_DragComponent = std::clamp(m_CurveComponent, 0, CurveComponentCount(track) - 1);
							m_DragStartValue = KeyCurveValueAt(track, key, m_DragComponent);
							m_DragSelection = m_Selection;
							m_DragBeforeTracks = m_Clip->GetTracks();
							m_DragBeforeDuration = m_Clip->GetDuration();
						}
					}
					else
					{
						/* 空处（含轨道下方的空白）也能起框：从下面往上拉，同样能盖住若干行 */
						if (track_index >= 0)
							m_SelectedTrack = track_index;

						m_BoxSelecting = true;
						m_BoxAdditive = ctrl;
						m_BoxStart = mouse;
						if (!ctrl)
							ClearSelection();
					}
				}
			}
		}

		/* 右键关键帧：先弹功能菜单（复制 / 粘贴 / 插值 / 删除） */
		if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
		{
			const int row = row_at(mouse.y);
			const int track_index = VisibleToTrack(row);
			const int key = hit_key(row, mouse.x);
			if (track_index >= 0 && key >= 0)
			{
				/* 点已选中的帧保留整组选择；点没选中的帧改成只选它 */
				if (!IsKeySelected(track_index, key))
					SelectSingleKey(track_index, key);

				m_KeyMenuTime = m_Clip->GetTracks()[track_index].Keys[key].Time;
				ImGui::OpenPopup(m_KeyMenuId);
			}
		}

		if (m_DraggingKey && ImGui::IsMouseDown(ImGuiMouseButton_Left))
		{
			/* 时间按 0.01s 吸附：纵向改值时鼠标的横向抖动不会把帧挪走 */
			const float delta = SnapTime(XToTime(mouse.x)) - m_DragStartTime;
			std::vector<TimelineModel::KeyRef> updated;
			TimelineModel::MoveKeys(m_Clip->GetTracks(), m_DragBeforeTracks, m_DragSelection, delta, updated);
			m_Selection = std::move(updated);

			/* 播放头跟着被拖的那一帧走，场景里看到的就是它在改的姿势 */
			if (m_Selection.size() == 1)
				JumpToKey(m_Selection.front().Track, m_Selection.front().Key);

			bool changed = std::fabs(delta) > 1e-6f;
			if (changed)
				m_DragChanged = true;

			/* 单选一帧时纵向拖动改分量值；多选只平移时间，避免互相打架 */
			if (m_DragSelection.size() == 1 && m_Selection.size() == 1)
			{
				const TimelineModel::KeyRef ref = m_Selection.front();
				if (ref.Track >= 0 && ref.Track < GetTrackCount())
				{
					AnimationTrack& track = m_Clip->GetTracks()[ref.Track];
					if (ref.Key >= 0 && ref.Key < static_cast<int>(track.Keys.size()))
					{
						const float dy = m_DragStartMouseY - mouse.y;
						if (std::fabs(dy) > 0.5f)
						{
							float step = (track.ValueType == TrackValueType::Quat) ? 0.5f : 0.01f;
							if (io.KeyShift)
								step *= 10.0f;
							if (io.KeyAlt)
								step *= 0.1f;

							const float value = m_DragStartValue + dy * step;
							if (WriteKeyCurveValue(track, ref.Key, m_DragComponent, value))
							{
								changed = true;
								m_DragChanged = true;
								ImGui::SetTooltip("%s = %.3f", AxisName(track, m_DragComponent), value);
							}
						}
					}
				}
			}

			if (changed)
				MarkClipChanged();
		}

		if (m_DraggingKey && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
		{
			m_DraggingKey = false;
			if (m_DragChanged && m_pCommandStack != nullptr && m_Clip != nullptr)
			{
				/* 拖关键帧不碰事件表：前后都传当前事件，撤销时事件保持不变 */
				m_pCommandStack->Execute(CreateUniquePtr<ClipEditCommand>(m_Clip,
					std::move(m_DragBeforeTracks), m_Clip->GetTracks(),
					m_Clip->GetEvents(), m_Clip->GetEvents(),
					m_DragBeforeDuration, m_Clip->GetDuration(), "Move Keys"));
			}
			m_DragBeforeTracks.clear();
			m_DragSelection.clear();
			m_DragChanged = false;
		}

		if (m_BoxSelecting && ImGui::IsMouseDown(ImGuiMouseButton_Left))
		{
			const ImVec2 box_min(std::min(m_BoxStart.x, mouse.x), std::min(m_BoxStart.y, mouse.y));
			const ImVec2 box_max(std::max(m_BoxStart.x, mouse.x), std::max(m_BoxStart.y, mouse.y));

			ImDrawList* draw_list = ImGui::GetWindowDrawList();
			draw_list->AddRectFilled(box_min, box_max, ColorU32(EditorTheme::WithAlpha(EditorTheme::Token::Accent, 0.18f)));
			draw_list->AddRect(box_min, box_max, ColorU32(EditorTheme::Token::Accent), 0.0f, 0, 1.0f);
		}

		if (m_BoxSelecting && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
		{
			const ImVec2 box_min(std::min(m_BoxStart.x, mouse.x), std::min(m_BoxStart.y, mouse.y));
			const ImVec2 box_max(std::max(m_BoxStart.x, mouse.x), std::max(m_BoxStart.y, mouse.y));
			ApplyBoxSelection(box_min, box_max, m_BoxAdditive);
			m_BoxSelecting = false;
		}

		/* 双击空轨道：在点击位置落一个关键帧 */
		if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)
			&& mouse.x >= lane_left && mouse.y >= lane_top)
		{
			const int row = row_at(mouse.y);
			const int track_index = VisibleToTrack(row);
			if (track_index >= 0 && hit_key(row, mouse.x) < 0)
			{
				AnimationTrack& track = m_Clip->GetTracks()[track_index];
				const float time = SnapTime(XToTime(mouse.x));

				glm::vec4 value;
				if (!AnimationClip::Sample(track, time, value))
					TimelineModel::ReadCurrentValue(scene, m_AnimatorEntity, track, value);

				BeginEdit();
				m_SelectedTrack = track_index;
				const int key = TimelineModel::InsertOrUpdateKey(track, time, value, track.Interp);
				m_Clip->RefreshDuration();
				SelectSingleKey(track_index, key);
				MarkClipChanged();
				CommitEdit("Add Key");
			}
		}

		/* 悬停在行首图标上提示开关 */
		if (hovered && !m_DraggingKey && !m_BoxSelecting && mouse.x < lane_left)
		{
			const int hover_row = row_at(mouse.y);
			const int hover_track = VisibleToTrack(hover_row);
			if (hover_track >= 0 && mouse.x >= icon_left && mouse.x <= icon_right)
				ImGui::SetTooltip(m_Clip->GetTracks()[hover_track].Enabled ? "Disable track" : "Enable track");
		}

		/* 悬停在关键帧上给一行提示：横向拖时间，纵向改值 */
		if (hovered && !m_DraggingKey && !m_BoxSelecting
			&& mouse.x >= lane_left && mouse.x <= lane_right && mouse.y >= lane_top)
		{
			const int row = row_at(mouse.y);
			const int track_index = VisibleToTrack(row);
			const int key = hit_key(row, mouse.x);
			if (key >= 0 && track_index >= 0)
			{
				const AnimationTrack& track = m_Clip->GetTracks()[track_index];
				ImGui::SetTooltip("%.2f s\nDrag horizontally for time, vertically for %s\nRight-click to delete",
					track.Keys[key].Time, AxisName(track, std::clamp(m_CurveComponent, 0, CurveComponentCount(track) - 1)));
			}
		}

		/* Del / 退格 删选中的关键帧（面板悬停且没在输入文本，同样先弹确认） */
		if (hovered && !m_Selection.empty() && !ImGui::GetIO().WantTextInput
			&& (KeyPressed(ImGuiKey_Delete) || KeyPressed(ImGuiKey_Backspace)))
		{
			RequestDeleteSelectedKeys();
		}
	}

	int TimelinePanel::CurveComponentCount(const AnimationTrack& track) const
	{
		switch (track.ValueType)
		{
		case TrackValueType::Vec3: return 3;
		case TrackValueType::Vec4: return 4;
		case TrackValueType::Quat: return 3;   /* 按欧拉角显示与编辑 */
		default:                   return 1;
		}
	}

	const char* TimelinePanel::AxisName(const AnimationTrack& track, int component) const
	{
		if (track.ValueType == TrackValueType::Quat)
		{
			static constexpr const char* kEuler[] = { "Euler X", "Euler Y", "Euler Z" };
			return kEuler[std::clamp(component, 0, 2)];
		}

		if (CurveComponentCount(track) == 1)
			return "Value";

		static constexpr const char* kAxes[] = { "X", "Y", "Z", "W" };
		return kAxes[std::clamp(component, 0, 3)];
	}

	float TimelinePanel::KeyCurveValueAt(const AnimationTrack& track, int key_index, int component) const
	{
		if (track.ValueType == TrackValueType::Quat)
			return KeyEuler(track, key_index)[std::clamp(component, 0, 2)];

		if (key_index < 0 || key_index >= static_cast<int>(track.Keys.size()))
			return 0.0f;
		return track.Keys[key_index].Value[std::clamp(component, 0, 3)];
	}

	float TimelinePanel::KeyCurveValue(const AnimationTrack& track, const AnimationKey& key, int component) const
	{
		if (track.ValueType == TrackValueType::Quat)
		{
			const glm::quat rotation(key.Value.w, key.Value.x, key.Value.y, key.Value.z);
			const glm::vec3 euler = glm::degrees(glm::eulerAngles(rotation));
			return euler[std::clamp(component, 0, 2)];
		}

		return key.Value[std::clamp(component, 0, 3)];
	}

	float TimelinePanel::SampleCurveValue(const AnimationTrack& track, float time, int component) const
	{
		glm::vec4 sampled;
		if (!AnimationClip::Sample(track, time, sampled))
			return 0.0f;

		if (track.ValueType == TrackValueType::Quat)
		{
			const glm::quat rotation(sampled.w, sampled.x, sampled.y, sampled.z);
			const glm::vec3 euler = glm::degrees(glm::eulerAngles(rotation));
			return euler[std::clamp(component, 0, 2)];
		}

		return sampled[std::clamp(component, 0, 3)];
	}

	bool TimelinePanel::WriteKeyCurveValue(AnimationTrack& track, int key_index, int component, float value)
	{
		if (key_index < 0 || key_index >= static_cast<int>(track.Keys.size()))
			return false;

		AnimationKey& key = track.Keys[key_index];
		if (track.ValueType == TrackValueType::Quat)
		{
			/* 用和界面一致的连续分支换算：显示的是哪一支，写回去就按哪一支 */
			glm::vec3 euler = KeyEuler(track, key_index);
			euler[std::clamp(component, 0, 2)] = value;
			const glm::quat edited = glm::quat(glm::radians(euler));
			key.Value = glm::vec4(edited.x, edited.y, edited.z, edited.w);
			return true;
		}

		key.Value[std::clamp(component, 0, 3)] = value;
		return true;
	}

	void TimelinePanel::DrawCurveArea(Scene& scene, AnimatorComponent& animator, float area_top, float area_bottom,
		float lane_left, float lane_right, bool hovered)
	{
		ImDrawList* draw_list = ImGui::GetWindowDrawList();
		ImGuiIO& io = ImGui::GetIO();

		draw_list->AddRectFilled(ImVec2(lane_left, area_top), ImVec2(lane_right, area_bottom),
			ColorU32(EditorTheme::Token::CardBg));
		draw_list->AddRect(ImVec2(lane_left, area_top), ImVec2(lane_right, area_bottom),
			ColorU32(EditorTheme::Token::Neutral4));

		/* 竖向指示线：和标尺刻度一一对应 */
		const ImU32 guide_color = ColorU32(EditorTheme::WithAlpha(EditorTheme::Token::TextDim, 0.55f));
		for (const float tick : m_LastRulerTicks)
		{
			const float x = TimeToScreenX(tick);
			if (x < lane_left || x > lane_right)
				continue;
			draw_list->AddLine(ImVec2(x, area_top), ImVec2(x, area_bottom), guide_color, 1.0f);
		}

		const AnimationTrack* track = GetSelectedTrackPtr();
		if (track == nullptr)
		{
			draw_list->AddText(ImVec2(lane_left + 10.0f, area_top + 8.0f),
				ColorU32(EditorTheme::Token::TextDim), "Select a track to edit its curve");
			return;
		}

		const int component_count = CurveComponentCount(*track);
		const int component = std::clamp(m_CurveComponent, 0, component_count - 1);
		const int key_count = static_cast<int>(track->Keys.size());
		const bool quat_track = (track->ValueType == TrackValueType::Quat);

		/* 四元数轨道：关键帧的欧拉角沿序列取连续分支（否则旋转过 ±90 时 X/Z 会突然变 180） */
		std::vector<glm::vec3> key_eulers;
		if (quat_track)
		{
			key_eulers.reserve(track->Keys.size());
			glm::vec3 previous(0.0f);
			for (size_t index = 0; index < track->Keys.size(); ++index)
			{
				const glm::vec4& value = track->Keys[index].Value;
				glm::vec3 euler = glm::degrees(glm::eulerAngles(glm::quat(value.w, value.x, value.y, value.z)));
				if (index > 0)
					euler = NearestEulerBranch(previous, euler);
				key_eulers.push_back(euler);
				previous = euler;
			}
		}

		/* 关键帧在曲线上的值：四元数取连续分支的欧拉角，其它轨道直接读分量 */
		const auto key_value_at = [&](int key_index, int axis) -> float
		{
			if (key_count <= 0)
				return 0.0f;
			if (quat_track)
				return key_eulers[std::clamp(key_index, 0, key_count - 1)][std::clamp(axis, 0, 2)];
			return track->Keys[std::clamp(key_index, 0, key_count - 1)].Value[std::clamp(axis, 0, 3)];
		};

		/* 每条分量各画一条曲线：值域取所有分量的并集，几条线才在同一张图里 */
		float value_min = 0.0f;
		float value_max = 0.0f;
		bool has_value = false;
		for (int key_index = 0; key_index < key_count; ++key_index)
		{
			for (int axis = 0; axis < component_count; ++axis)
			{
				const float value = key_value_at(key_index, axis);
				if (!has_value)
				{
					value_min = value_max = value;
					has_value = true;
				}
				else
				{
					value_min = std::min(value_min, value);
					value_max = std::max(value_max, value);
				}
			}
		}

		if (!has_value)
			value_min = value_max = 0.0f;
		if (value_max - value_min < 1e-4f)
		{
			value_min -= 0.5f;
			value_max += 0.5f;
		}
		const float padding = (value_max - value_min) * 0.12f;
		value_min -= padding;
		value_max += padding;

		/* 拖动期间用按下时冻结的值域：不然改一点值整张图就重新缩放，很晃 */
		if (m_DraggingCurveKey)
		{
			value_min = m_CurveDragValueMin;
			value_max = m_CurveDragValueMax;
		}

		m_LastCurveValueMin = value_min;
		m_LastCurveValueMax = value_max;

		const float graph_height = std::max(area_bottom - area_top, 1.0f);
		const auto value_to_y = [area_bottom, graph_height, value_min, value_max](float value)
		{
			const float ratio = (value - value_min) / std::max(value_max - value_min, 1e-4f);
			return area_bottom - ratio * graph_height;
		};
		draw_list->PushClipRect(ImVec2(lane_left, area_top), ImVec2(lane_right, area_bottom), true);

		/* 三条水平参考线 + 上下界标签 */
		const ImU32 grid_color = ColorU32(EditorTheme::Token::Neutral4);
		const ImU32 text_color = ColorU32(EditorTheme::Token::TextDim);
		for (int line = 0; line <= 2; ++line)
		{
			const float value = value_min + (value_max - value_min) * (line / 2.0f);
			const float y = value_to_y(value);
			draw_list->AddLine(ImVec2(lane_left, y), ImVec2(lane_right, y), grid_color, 1.0f);

			char label[32];
			std::snprintf(label, sizeof(label), "%.2f", value);
			const float label_y = std::min(std::max(y - 14.0f, area_top + 2.0f), area_bottom - 16.0f);
			draw_list->AddText(ImVec2(lane_left + 4.0f, label_y), text_color, label);
		}

		/* 分量各自的颜色：单分量的轨道沿用强调色，多分量按 XYZW 轴色区分 */
		const ImVec4 kAxisColors[] = {
			EditorTheme::Token::AxisX, EditorTheme::Token::AxisY,
			EditorTheme::Token::AxisZ, EditorTheme::Token::TextLabel,
		};
		const auto axis_color = [&](int axis)
		{
			if (component_count == 1)
				return EditorTheme::Token::Accent;
			return kAxisColors[std::clamp(axis, 0, 3)];
		};

		const ImU32 selected_color = ColorU32(EditorTheme::Token::AccentHover);
		const float step = 2.0f;
		const float sample_left = std::max(lane_left, TimeToScreenX(0.0f));
		const float sample_right = std::min(lane_right, TimeToScreenX(GetLaneDuration()));

		/* 四元数轨道：先把可见区每个采样点的欧拉角算出来，逐点选离上一点最近的分支；
		 * 起点用可见区左边最近那帧的分支，缩放 / 滚动时曲线不会整体跳到另一支 */
		std::vector<float> sample_xs;
		std::vector<glm::vec4> sample_values;
		std::vector<glm::vec3> sample_eulers;
		if (quat_track)
		{
			glm::vec3 previous(0.0f);
			bool has_previous = false;
			if (!key_eulers.empty())
			{
				int start_key = 0;
				for (int index = 0; index < key_count; ++index)
				{
					if (TimeToScreenX(track->Keys[index].Time) <= sample_left)
						start_key = index;
				}
				previous = key_eulers[start_key];
				has_previous = true;
			}

			for (float x = sample_left; x <= sample_right; x += step)
			{
				glm::vec4 sampled;
				glm::vec3 euler = previous;
				if (AnimationClip::Sample(*track, XToTime(x), sampled))
					euler = glm::degrees(glm::eulerAngles(glm::quat(sampled.w, sampled.x, sampled.y, sampled.z)));
				if (has_previous)
					euler = NearestEulerBranch(previous, euler);
				sample_xs.push_back(x);
				sample_eulers.push_back(euler);
				previous = euler;
				has_previous = true;
			}
		}
		else
		{
			/* 其它轨道也先采一遍：三个分量共用同一份采样，别一个 x 采三次 */
			for (float x = sample_left; x <= sample_right; x += step)
			{
				glm::vec4 sampled(0.0f);
				if (!AnimationClip::Sample(*track, XToTime(x), sampled))
					continue;

				sample_xs.push_back(x);
				sample_values.push_back(sampled);
			}
		}

		/* 先画非当前分量（压暗、细一档），当前分量最后画在最上面 */
		std::vector<ImVec2> points;
		for (int pass = 0; pass < (component_count > 1 ? 2 : 1); ++pass)
		{
			for (int axis = 0; axis < component_count; ++axis)
			{
				if (component_count > 1 && ((pass == 0 && axis == component) || (pass == 1 && axis != component)))
					continue;

				const bool is_active = (axis == component);
				const float alpha = !track->Enabled ? 0.25f : (is_active || component_count == 1 ? 1.0f : 0.45f);
				const ImU32 color = ColorU32(EditorTheme::WithAlpha(axis_color(axis), alpha));

				points.clear();
				if (quat_track)
				{
					for (size_t index = 0; index < sample_xs.size(); ++index)
						points.emplace_back(sample_xs[index],
							value_to_y(sample_eulers[index][std::clamp(axis, 0, 2)]));
				}
				else
				{
					for (size_t index = 0; index < sample_xs.size(); ++index)
						points.emplace_back(sample_xs[index],
							value_to_y(sample_values[index][std::clamp(axis, 0, 3)]));
				}
				if (!points.empty())
					draw_list->AddPolyline(points.data(), static_cast<int>(points.size()), color, 0,
						is_active ? 1.8f : 1.2f);

				/* 关键帧圆点：当前分量的点带选中圈，其余分量只点一个小点 */
				for (int index = 0; index < key_count; ++index)
				{
					const AnimationKey& key = track->Keys[index];
					const ImVec2 center(TimeToScreenX(key.Time), value_to_y(key_value_at(index, axis)));
					const bool selected = is_active && IsKeySelected(m_SelectedTrack, index);

					if (selected)
					{
						draw_list->AddCircleFilled(center, 4.5f, selected_color);
						draw_list->AddCircle(center, 6.0f, ColorU32(EditorTheme::Token::White12), 0, 1.0f);
					}
					else
					{
						draw_list->AddCircleFilled(center, is_active ? 3.2f : 2.4f, color);
					}
				}
			}
		}

		/* 图例：几条曲线各自的颜色与字母（贴右上角，当前分量加亮）。
		 * 底下垫一块半透明底：曲线和刻度线都会从这儿过，不垫读不清 */
		if (component_count > 1)
		{
			static constexpr const char* kAxisLabels[] = { "X", "Y", "Z", "W" };
			const float letter_gap = 7.0f;
			const float pad_x = 5.0f;
			float legend_width = pad_x * 2.0f;
			for (int axis = 0; axis < component_count; ++axis)
			{
				legend_width += ImGui::CalcTextSize(kAxisLabels[std::clamp(axis, 0, 3)]).x;
				if (axis + 1 < component_count)
					legend_width += letter_gap;
			}

			const float legend_top = area_top + 4.0f;
			const float legend_height = ImGui::GetTextLineHeight() + 2.0f;
			const ImVec2 box_min(lane_right - 8.0f - legend_width, legend_top);
			const ImVec2 box_max(box_min.x + legend_width, legend_top + legend_height);
			draw_list->AddRectFilled(box_min, box_max,
				ColorU32(EditorTheme::WithAlpha(EditorTheme::Token::Neutral1, 0.92f)), 3.0f);
			draw_list->AddRect(box_min, box_max, ColorU32(EditorTheme::Token::Neutral4), 3.0f);

			float legend_x = box_min.x + pad_x;
			for (int axis = 0; axis < component_count; ++axis)
			{
				const char* label = kAxisLabels[std::clamp(axis, 0, 3)];
				const float alpha = (axis == component) ? 1.0f : 0.55f;
				draw_list->AddText(ImVec2(legend_x, legend_top + 1.0f),
					ColorU32(EditorTheme::WithAlpha(axis_color(axis), alpha)), label);
				legend_x += ImGui::CalcTextSize(label).x + letter_gap;
			}
		}

		/* 贝塞尔手柄：Cubic、单帧、非四元数时显示选中帧的进出切线 */
		struct TangentHandle
		{
			bool  Valid{ false };
			float Time{ 0.0f };
			float Value{ 0.0f };
			bool  Out{ false };
		};
		TangentHandle out_handle;
		TangentHandle in_handle;
		const bool tangent_capable = track->Interp == KeyInterp::Cubic
			&& track->ValueType != TrackValueType::Quat
			&& m_Selection.size() == 1
			&& m_Selection.front().Track == m_SelectedTrack;

		if (tangent_capable)
		{
			const int key_index = m_Selection.front().Key;
			if (key_index >= 0 && key_index < static_cast<int>(track->Keys.size()))
			{
				const AnimationKey& key = track->Keys[key_index];
				const float key_value = KeyCurveValue(*track, key, component);
				const glm::vec4 auto_tangent = AnimationClip::ComputeAutoTangent(track->Keys, key_index);

				if (key_index + 1 < static_cast<int>(track->Keys.size()))
				{
					const float dt = (track->Keys[key_index + 1].Time - key.Time) / 3.0f;
					if (dt > 1e-4f)
					{
						const float tangent = key.HasTangent ? key.OutTangent[component] : auto_tangent[component];
						out_handle = { true, key.Time + dt, key_value + tangent * dt, true };
					}
				}
				if (key_index > 0)
				{
					const float dt = (key.Time - track->Keys[key_index - 1].Time) / 3.0f;
					if (dt > 1e-4f)
					{
						const float tangent = key.HasTangent ? key.InTangent[component] : auto_tangent[component];
						in_handle = { true, key.Time - dt, key_value - tangent * dt, false };
					}
				}

				const ImVec2 key_screen(TimeToScreenX(key.Time), value_to_y(key_value));
				const ImU32 handle_line = ColorU32(EditorTheme::WithAlpha(EditorTheme::Token::TextDim, 0.85f));
				const ImU32 handle_fill = ColorU32(EditorTheme::Token::AccentHover);
				for (const TangentHandle& handle : { out_handle, in_handle })
				{
					if (!handle.Valid)
						continue;
					const ImVec2 pos(TimeToScreenX(handle.Time), value_to_y(handle.Value));
					draw_list->AddLine(key_screen, pos, handle_line, 1.0f);
					draw_list->AddCircleFilled(pos, 3.0f, handle_fill);
					draw_list->AddCircle(pos, 4.5f, handle_line, 0, 1.0f);
				}
			}
		}

		draw_list->PopClipRect();

		/* 播放头在曲线区里也画一条，和标尺的三角对齐 */
		const float playhead_x = TimeToScreenX(animator.m_Time);
		if (playhead_x >= lane_left && playhead_x <= lane_right)
			draw_list->AddLine(ImVec2(playhead_x, area_top), ImVec2(playhead_x, area_bottom),
				ColorU32(EditorTheme::Token::Accent), 1.0f);

		const ImVec2 mouse = io.MousePos;
		const bool ctrl = io.KeyCtrl || io.KeySuper;

		/* 命中：先查当前分量，再查其它分量 —— 几条曲线叠在同一点时优先抓正在编辑的那条 */
		struct CurveHit
		{
			int Key{ -1 };
			int Component{ 0 };
		};

		int hit_order[4] = {};
		int hit_order_count = 0;
		hit_order[hit_order_count++] = component;
		for (int axis = 0; axis < component_count; ++axis)
		{
			if (axis != component)
				hit_order[hit_order_count++] = axis;
		}

		const auto hit_key = [&](float x, float y)
		{
			CurveHit best;
			float best_distance = 8.0f;
			for (int order = 0; order < hit_order_count; ++order)
			{
				const int axis = hit_order[order];
				for (int index = 0; index < key_count; ++index)
				{
					const AnimationKey& key = track->Keys[index];
					const ImVec2 center(TimeToScreenX(key.Time), value_to_y(key_value_at(index, axis)));
					const float dx = center.x - x;
					const float dy = center.y - y;
					const float distance = std::sqrt(dx * dx + dy * dy);
					if (distance < best_distance || (order == 0 && distance <= best_distance))
					{
						best.Key = index;
						best.Component = axis;
						best_distance = distance;
					}
				}
			}
			return best;
		};

		/* 本帧拖动改哪个分量：点下去那一刻定下来（点哪条曲线就改哪条） */
		int drag_component = component;

		if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
		{
			const auto hit_handle = [&](const TangentHandle& handle)
			{
				if (!handle.Valid)
					return false;
				const ImVec2 pos(TimeToScreenX(handle.Time), value_to_y(handle.Value));
				return (mouse.x - pos.x) * (mouse.x - pos.x) + (mouse.y - pos.y) * (mouse.y - pos.y) <= 49.0f;
			};

			if (tangent_capable && (hit_handle(out_handle) || hit_handle(in_handle)))
			{
				const bool is_out = hit_handle(out_handle);
				const TangentHandle& handle = is_out ? out_handle : in_handle;
				m_DraggingTangent = true;
				m_TangentChanged = false;
				m_TangentTrack = m_SelectedTrack;
				m_TangentKey = m_Selection.front().Key;
				m_TangentOut = is_out;
				m_TangentGrabDy = mouse.y - value_to_y(handle.Value);
				m_TangentValueMin = value_min;
				m_TangentValueMax = value_max;
				BeginEdit();
			}
			else
			{
			const CurveHit hit = hit_key(mouse.x, mouse.y);
			if (hit.Key >= 0)
			{
				/* 抓哪条曲线，之后就按哪个分量改值（属性栏里那几行的高亮也跟着走） */
				m_CurveComponent = hit.Component;
				drag_component = hit.Component;

				if (ctrl)
				{
					ToggleKeySelection(m_SelectedTrack, hit.Key);
				}
				else
				{
					SelectSingleKey(m_SelectedTrack, hit.Key);
					m_InspectorOpen = true;
					JumpToKey(m_SelectedTrack, hit.Key);

					const ImVec2 center(TimeToScreenX(track->Keys[hit.Key].Time),
						value_to_y(key_value_at(hit.Key, hit.Component)));
					m_CurveDragGrabDx = mouse.x - center.x;
					m_CurveDragGrabDy = mouse.y - center.y;

					m_DraggingCurveKey = true;
					m_CurveDragChanged = false;
					m_CurveDragTrack = m_SelectedTrack;
					m_CurveDragKey = hit.Key;
					m_CurveDragValueMin = value_min;
					m_CurveDragValueMax = value_max;
					BeginEdit();
				}
			}
			else
			{
				/* 空处按下：拉框选曲线上的关键帧（松手时结算），Ctrl 叠加 */
				m_CurveBoxSelecting = true;
				m_CurveBoxStart = mouse;
				m_BoxAdditive = ctrl;
				if (!ctrl)
					ClearSelection();
			}
			}
		}

		/* 右键曲线关键帧：同样先弹功能菜单 */
		if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
		{
			const CurveHit hit = hit_key(mouse.x, mouse.y);
			if (hit.Key >= 0)
			{
				if (!IsKeySelected(m_SelectedTrack, hit.Key))
					SelectSingleKey(m_SelectedTrack, hit.Key);

				m_KeyMenuTime = track->Keys[hit.Key].Time;
				ImGui::OpenPopup(m_KeyMenuId);
			}
		}

		/* 拖手柄：只改切线值，时间固定在各段 1/3 处 */
		if (m_DraggingTangent && ImGui::IsMouseDown(ImGuiMouseButton_Left))
		{
			if (m_TangentTrack >= 0 && m_TangentTrack < GetTrackCount())
			{
				AnimationTrack& tangent_track = m_Clip->GetTracks()[m_TangentTrack];
				if (m_TangentKey >= 0 && m_TangentKey < static_cast<int>(tangent_track.Keys.size()))
				{
					AnimationKey& tangent_key = tangent_track.Keys[m_TangentKey];
					if (!tangent_key.HasTangent)
					{
						const glm::vec4 auto_tangent = AnimationClip::ComputeAutoTangent(tangent_track.Keys, m_TangentKey);
						tangent_key.InTangent = auto_tangent;
						tangent_key.OutTangent = auto_tangent;
						tangent_key.HasTangent = true;
					}

					const float key_value = KeyCurveValue(tangent_track, tangent_key, component);
					float dt = 0.0f;
					if (m_TangentOut && m_TangentKey + 1 < static_cast<int>(tangent_track.Keys.size()))
						dt = (tangent_track.Keys[m_TangentKey + 1].Time - tangent_key.Time) / 3.0f;
					else if (!m_TangentOut && m_TangentKey > 0)
						dt = (tangent_key.Time - tangent_track.Keys[m_TangentKey - 1].Time) / 3.0f;

					if (dt > 1e-4f)
					{
						const float y = mouse.y - m_TangentGrabDy;
						const float ratio = (area_bottom - y) / std::max(graph_height, 1e-4f);
						const float value = m_TangentValueMin + ratio * (m_TangentValueMax - m_TangentValueMin);
						const float tangent = (value - key_value) / dt;

						if (m_TangentOut)
							tangent_key.OutTangent[component] = tangent;
						else
							tangent_key.InTangent[component] = (key_value - value) / dt;

						m_TangentChanged = true;
						MarkClipChanged();
						ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
						ImGui::SetTooltip("%s tangent = %.2f", m_TangentOut ? "Out" : "In", tangent);
					}
				}
			}
		}

		if (m_DraggingTangent && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
		{
			m_DraggingTangent = false;
			if (m_TangentChanged)
				CommitEdit("Edit Bezier Tangent");
			else
				DiscardEdit();
			m_TangentChanged = false;
		}

		if (m_DraggingCurveKey && ImGui::IsMouseDown(ImGuiMouseButton_Left))
		{
			if (m_CurveDragTrack >= 0 && m_CurveDragTrack < GetTrackCount())
			{
				AnimationTrack& drag_track = m_Clip->GetTracks()[m_CurveDragTrack];

				/* 以按下时的偏移为基准：抓在哪就一直在哪，不会一按就跳到鼠标中心 */
				const ImVec2 cursor(mouse.x - m_CurveDragGrabDx, mouse.y - m_CurveDragGrabDy);
				float target_time = XToTime(cursor.x);
				target_time = io.KeyShift ? SnapTime(target_time) : std::round(target_time * 10000.0f) / 10000.0f;

				const float range = std::max(m_CurveDragValueMax - m_CurveDragValueMin, 1e-4f);
				const float ratio = (area_bottom - cursor.y) / std::max(graph_height, 1e-4f);
				const float value = m_CurveDragValueMin + ratio * range;

				/* 只抓不拖（或偏移刚好换回原值）时不写、不脏、不进历史；
				 * 拖动中撤销把关键帧删掉时下标可能失效，先查一下 */
				const bool key_valid = m_CurveDragKey >= 0
					&& m_CurveDragKey < static_cast<int>(drag_track.Keys.size());
				const bool same_time = key_valid
					&& std::fabs(target_time - drag_track.Keys[m_CurveDragKey].Time) < 1e-4f;
				const bool same_value = key_valid
					&& std::fabs(value - KeyCurveValueAt(drag_track, m_CurveDragKey, drag_component)) < 1e-4f;
				if (key_valid && (!same_time || !same_value))
				{
					int new_index = m_CurveDragKey;
					if (TimelineModel::MoveKey(drag_track, m_CurveDragKey, target_time, new_index))
					{
						m_CurveDragKey = new_index;
						JumpToKey(m_CurveDragTrack, new_index);
						WriteKeyCurveValue(drag_track, new_index, drag_component, value);

						m_SelectedTrack = m_CurveDragTrack;
						SelectSingleKey(m_CurveDragTrack, new_index);
						m_CurveDragChanged = true;
						MarkClipChanged();

						ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
						ImGui::SetTooltip("%.2f s\n%s = %.3f", target_time,
							AxisName(drag_track, drag_component), value);
					}
				}
			}
		}

		if (m_DraggingCurveKey && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
		{
			m_DraggingCurveKey = false;
			if (m_CurveDragChanged)
				CommitEdit("Edit Curve");
			else
				DiscardEdit();
			m_CurveDragChanged = false;
		}

		if (m_CurveBoxSelecting && ImGui::IsMouseDown(ImGuiMouseButton_Left))
		{
			const ImVec2 box_min(std::min(m_CurveBoxStart.x, mouse.x), std::min(m_CurveBoxStart.y, mouse.y));
			const ImVec2 box_max(std::max(m_CurveBoxStart.x, mouse.x), std::max(m_CurveBoxStart.y, mouse.y));

			ImDrawList* draw_list = ImGui::GetWindowDrawList();
			draw_list->AddRectFilled(box_min, box_max,
				ColorU32(EditorTheme::WithAlpha(EditorTheme::Token::Accent, 0.18f)));
			draw_list->AddRect(box_min, box_max, ColorU32(EditorTheme::Token::Accent), 0.0f, 0, 1.0f);
		}

		if (m_CurveBoxSelecting && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
		{
			const ImVec2 box_min(std::min(m_CurveBoxStart.x, mouse.x), std::min(m_CurveBoxStart.y, mouse.y));
			const ImVec2 box_max(std::max(m_CurveBoxStart.x, mouse.x), std::max(m_CurveBoxStart.y, mouse.y));

			const int component = std::clamp(m_CurveComponent, 0, CurveComponentCount(*track) - 1);
			for (int index = 0; index < static_cast<int>(track->Keys.size()); ++index)
			{
				const float x = TimeToScreenX(track->Keys[index].Time);
				const float y = value_to_y(key_value_at(index, component));
				if (x < box_min.x || x > box_max.x || y < box_min.y || y > box_max.y)
					continue;
				if (!IsKeySelected(m_SelectedTrack, index))
					m_Selection.push_back({ m_SelectedTrack, index });
			}

			if (!m_Selection.empty())
				m_SelectedTrack = m_Selection.front().Track;

			m_CurveBoxSelecting = false;
		}

		/* 双击空白：按采样值补一个关键帧 */
		if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
		{
			const CurveHit hit = hit_key(mouse.x, mouse.y);
			if (hit.Key < 0)
			{
				AnimationTrack& edit_track = m_Clip->GetTracks()[m_SelectedTrack];
				const float time = SnapTime(XToTime(mouse.x));
				glm::vec4 value;
				if (!AnimationClip::Sample(edit_track, time, value))
					TimelineModel::ReadCurrentValue(scene, m_AnimatorEntity, edit_track, value);

				BeginEdit();
				const int new_key = TimelineModel::InsertOrUpdateKey(edit_track, time, value, edit_track.Interp);
				m_Clip->RefreshDuration();
				SelectSingleKey(m_SelectedTrack, new_key);
				MarkClipChanged();
				CommitEdit("Add Key");
			}
		}
	}

	void TimelinePanel::DrawZoomBar(float left, float top, float height, float track_width, const char* label)
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		ImDrawList* draw_list = ImGui::GetWindowDrawList();

		/* 底栏里的一条细轨：轨道按栏高居中，滑块比轨道高一点，看得清也抓得住 */
		constexpr float kTrackHeight = 5.0f;
		constexpr float kThumbHeight = 12.0f;
		const float track_y = top + (height - kTrackHeight) * 0.5f;
		draw_list->AddRectFilled(ImVec2(left, track_y), ImVec2(left + track_width, track_y + kTrackHeight),
			ColorU32(EditorTheme::Token::Neutral4), kTrackHeight * 0.5f);

		const float content = GetContentWidth();
		const float view_width = LaneViewWidth();
		const float max_scroll = GetMaxScrollPixels();
		const float thumb_w = std::clamp(track_width * view_width / std::max(content, 1.0f), 14.0f, track_width);
		const float ratio = max_scroll > 0.0f ? m_ScrollX / max_scroll : 0.0f;
		const ImVec2 thumb_min(left + (track_width - thumb_w) * ratio,
			track_y + (kTrackHeight - kThumbHeight) * 0.5f);
		draw_list->AddRectFilled(thumb_min, ImVec2(thumb_min.x + thumb_w, thumb_min.y + kThumbHeight),
			ColorU32(EditorTheme::Token::Accent), 3.0f);

		/* 拖滑块：滑块中心跟手，两边留半个滑块宽；整条栏高都是命中区 */
		ImGui::SetCursorScreenPos(ImVec2(left, top));
		ImGui::InvisibleButton("##TimelineZoomScroll", ImVec2(track_width, height));
		if (ImGui::IsItemActive() && max_scroll > 0.0f)
		{
			const float denom = std::max(track_width - thumb_w, 1.0f);
			const float local = ImGui::GetIO().MousePos.x - left - thumb_w * 0.5f;
			m_ScrollX = std::clamp(local / denom, 0.0f, 1.0f) * max_scroll;
			ClampView();
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Drag to scroll the timeline");

		/* 百分比：点一下回到 100% */
		const float text_x = left + track_width + style.ItemInnerSpacing.x;
		const float text_y = top + (height - ImGui::GetFontSize()) * 0.5f;
		draw_list->AddText(ImVec2(text_x, text_y), ColorU32(EditorTheme::Token::TextLabel), label);

		ImGui::SetCursorScreenPos(ImVec2(text_x, top));
		ImGui::InvisibleButton("##TimelineZoomReset", ImVec2(ImGui::CalcTextSize(label).x, height));
		if (ImGui::IsItemClicked())
		{
			m_Zoom = 1.0f;
			m_ScrollX = 0.0f;
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Reset zoom to 100%%");
	}

	void TimelinePanel::DrawRulerAndPlayhead(AnimatorComponent& animator, const ImVec2& origin,
		float lane_left, float lane_right, float fill_right)
	{
		ImDrawList* draw_list = ImGui::GetWindowDrawList();
		const float duration = GetLaneDuration();

		/* 底色铺到操作列（与行底色连续）；刻度与播放头仍按时间轴内容的右缘算 */
		draw_list->AddRectFilled(ImVec2(lane_left, origin.y), ImVec2(fill_right, origin.y + kRulerHeight),
			ColorU32(EditorTheme::Token::CardHeaderBg));

		/* 事件标记：贴在标尺下沿的小三角，悬停显示名字与时间 */
		if (m_Clip != nullptr)
		{
			const ImVec2 marker_mouse = ImGui::GetIO().MousePos;
			const float marker_base = origin.y + kRulerHeight - 1.0f;
			for (const AnimationEvent& event : m_Clip->GetEvents())
			{
				const float x = TimeToScreenX(event.Time);
				if (x < lane_left - 2.0f || x > lane_right + 2.0f)
					continue;

				draw_list->AddTriangleFilled(ImVec2(x - 4.0f, marker_base), ImVec2(x + 4.0f, marker_base),
					ImVec2(x, marker_base - 6.0f), ColorU32(EditorTheme::Token::AccentHover));

				if (std::fabs(marker_mouse.x - x) <= 5.0f
					&& marker_mouse.y >= origin.y && marker_mouse.y <= origin.y + kRulerHeight)
				{
					ImGui::SetTooltip("%s  (%.2f s)", event.Name.c_str(), event.Time);
				}
			}
		}

		/* 只铺可见时间段：放大后从可见起点开始步进 */
		const float visible_start = std::max(XToTime(lane_left), 0.0f);
		const float visible_end = std::min(XToTime(lane_right), duration);
		const float span = std::max(visible_end - visible_start, 1e-3f);

		float step = 0.01f;
		for (const float candidate : { 0.01f, 0.025f, 0.05f, 0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 5.0f, 10.0f, 20.0f, 50.0f })
		{
			step = candidate;
			if (span / step <= 8.0f)
				break;
		}

		const ImU32 tick_color = ColorU32(EditorTheme::Token::TextDim);
		m_LastRulerTicks.clear();
		const float first = std::floor(visible_start / step) * step;
		float last_label_x = -1e9f;
		int guard = 0;
		for (float time = first; time <= visible_end + step * 0.001f && guard < 400; time += step, ++guard)
		{
			if (time < -1e-4f)
				continue;

			const float x = TimeToX(time);
			if (x < lane_left - 1.0f || x > lane_right + 1.0f)
				continue;

			m_LastRulerTicks.push_back(time);

			draw_list->AddLine(ImVec2(x, origin.y + kRulerHeight - 4.0f), ImVec2(x, origin.y + kRulerHeight),
				tick_color, 1.0f);
			draw_list->AddLine(ImVec2(x, origin.y + kRulerHeight), ImVec2(x, m_LastLaneMax.y),
				ColorU32(EditorTheme::Token::Neutral4), 1.0f);

			/* 标签太挤就隔一个画一个；文字以刻度线为中心 */
			if (x - last_label_x >= 34.0f)
			{
				char label[32];
				std::snprintf(label, sizeof(label), "%.2f", time);
				const float label_width = ImGui::CalcTextSize(label).x;
				const float label_x = std::clamp(x - label_width * 0.5f, lane_left + 2.0f, lane_right - label_width - 2.0f);
				draw_list->AddText(ImVec2(label_x, origin.y + 8.0f), tick_color, label);
				last_label_x = x;
			}
		}

		const float playhead_x = TimeToX(animator.m_Time);
		if (playhead_x >= lane_left && playhead_x <= lane_right)
		{
			const ImU32 accent = ColorU32(EditorTheme::Token::Accent);
			draw_list->AddLine(ImVec2(playhead_x, origin.y + 7.0f), ImVec2(playhead_x, m_LastLaneMax.y), accent, 1.5f);
			draw_list->AddTriangleFilled(
				ImVec2(playhead_x - 5.0f, origin.y),
				ImVec2(playhead_x + 5.0f, origin.y),
				ImVec2(playhead_x, origin.y + 7.0f), accent);
		}
	}

	void TimelinePanel::DrawTrackRows(AnimatorComponent& animator, const ImVec2& origin, float lane_left,
		float lane_right, float row_right, bool keys_mode)
	{
		(void)animator;
		ImDrawList* draw_list = ImGui::GetWindowDrawList();
		const float lane_top = origin.y + kRulerHeight;

		const ImU32 row_alt = ColorU32(EditorTheme::Token::White06);
		const ImU32 row_selected = ColorU32(EditorTheme::WithAlpha(EditorTheme::Token::Accent, 0.22f));
		const ImU32 text_color = ImGui::GetColorU32(ImGuiCol_Text);
		const ImU32 dim_color = ColorU32(EditorTheme::Token::TextDim);
		const ImU32 key_color = ColorU32(EditorTheme::Token::Accent);
		const ImU32 selected_color = ColorU32(EditorTheme::Token::AccentHover);
		const ImU32 separator_color = ColorU32(EditorTheme::Token::Neutral4);
		const ImU32 accent_color = ColorU32(EditorTheme::Token::Accent);

		const std::string animator_name = (m_pScene != nullptr && m_AnimatorEntity != entt::null)
			? EntityName(*m_pScene, m_AnimatorEntity) : std::string("Root");

		const int visible_count = static_cast<int>(m_VisibleTracks.size());
		const float icon_size = 11.0f;
		const float view_bottom = lane_top + m_RowViewHeight;

		/* 只画可视区里的行：行多时省一遍顶点，滚出上下沿的半行也一起截掉 */
		const int first_row = std::clamp(static_cast<int>(std::floor(m_ScrollY / kRowHeight)),
			0, std::max(visible_count - 1, 0));
		const int last_row = std::clamp(
			static_cast<int>(std::ceil((m_ScrollY + m_RowViewHeight) / kRowHeight)), first_row, visible_count);

		draw_list->PushClipRect(ImVec2(origin.x, lane_top), ImVec2(row_right, view_bottom), true);

		for (int row = first_row; row < last_row; ++row)
		{
			const int track_index = m_VisibleTracks[row];
			const AnimationTrack& track = m_Clip->GetTracks()[track_index];

			const float row_top = lane_top + row * kRowHeight - m_ScrollY;
			const float row_center = row_top + kRowHeight * 0.5f;

			/* 行底色：斑马纹打底，选中行叠强调色 + 左侧竖条 */
			if (row % 2 == 1)
				draw_list->AddRectFilled(ImVec2(origin.x, row_top), ImVec2(row_right, row_top + kRowHeight), row_alt);
			if (track_index == m_SelectedTrack)
			{
				draw_list->AddRectFilled(ImVec2(origin.x, row_top), ImVec2(row_right, row_top + kRowHeight), row_selected);
				draw_list->AddRectFilled(ImVec2(origin.x, row_top + 2.0f), ImVec2(origin.x + 2.5f, row_top + kRowHeight - 2.0f),
					accent_color);
			}

			/* 行首组件图标（也是轨道开关，关掉的轨道整体压暗） */
			const bool enabled = track.Enabled;
			const ImVec2 icon_center(origin.x + 7.0f + icon_size * 0.5f, row_center);
			Icons::Draw(draw_list, TrackComponentIcon(track.Component), icon_center, icon_size,
				enabled ? dim_color : ColorU32(EditorTheme::WithAlpha(EditorTheme::Token::TextDim, 0.40f)));
			if (!enabled)
			{
				draw_list->AddLine(ImVec2(icon_center.x - icon_size * 0.45f, icon_center.y + icon_size * 0.45f),
					ImVec2(icon_center.x + icon_size * 0.45f, icon_center.y - icon_size * 0.45f),
					ColorU32(EditorTheme::Token::Neutral4), 1.2f);
			}

			const std::string label = TimelineModel::MakeTrackLabel(track, animator_name);
			const float text_x = origin.x + 7.0f + icon_size + 6.0f;
			draw_list->PushClipRect(ImVec2(text_x, row_top), ImVec2(lane_left - 4.0f, row_top + kRowHeight), true);
			draw_list->AddText(ImVec2(text_x, row_top + 3.0f), enabled ? text_color : dim_color, label.c_str());
			draw_list->PopClipRect();

			/* 关键帧只出现在时刻列；曲线模式没有这条通道 */
			if (!keys_mode)
				continue;

			draw_list->AddLine(ImVec2(origin.x, row_top + kRowHeight), ImVec2(row_right, row_top + kRowHeight),
				separator_color, 1.0f);

			/* 关键帧菱形：选中的描一圈亮边（放大后只画可见范围；曲线模式没有这条通道） */
			if (!keys_mode)
				continue;

			draw_list->PushClipRect(ImVec2(lane_left, row_top), ImVec2(lane_right, row_top + kRowHeight), true);
			const auto& keys = track.Keys;
			for (int key_index = 0; key_index < static_cast<int>(keys.size()); ++key_index)
			{
				const float x = TimeToX(keys[key_index].Time);
				const bool selected = IsKeySelected(track_index, key_index);
				const float radius = selected ? 5.0f : 4.0f;
				const ImU32 color = enabled
					? (selected ? selected_color : key_color)
					: ColorU32(EditorTheme::WithAlpha(EditorTheme::Token::Accent, selected ? 0.65f : 0.35f));

				draw_list->AddQuadFilled(
					ImVec2(x - radius, row_center),
					ImVec2(x, row_center - radius),
					ImVec2(x + radius, row_center),
					ImVec2(x, row_center + radius), color);

				if (selected)
				{
					draw_list->AddQuad(
						ImVec2(x - radius - 1.0f, row_center),
						ImVec2(x, row_center - radius - 1.0f),
						ImVec2(x + radius + 1.0f, row_center),
						ImVec2(x, row_center + radius + 1.0f),
						ColorU32(EditorTheme::Token::White12), 1.0f);
				}
			}
			draw_list->PopClipRect();
		}

		draw_list->PopClipRect();

		/* 空状态：没轨道和"搜索没命中"分开说 */
		if (keys_mode && visible_count == 0)
		{
			const bool filtering = m_TrackFilter[0] != '\0';
			const char* title = filtering ? "No tracks match" : "No tracks yet";
			const char* hint = filtering ? "Clear the search to see all tracks" : "Use Add Track to animate a property";

			const ImVec2 center((lane_left + lane_right) * 0.5f, lane_top + kRowHeight * 1.6f);
			Icons::Draw(draw_list, Icons::Id::Animation, ImVec2(center.x, center.y - 16.0f), 18.0f, dim_color);

			const ImVec2 title_size = ImGui::CalcTextSize(title);
			draw_list->AddText(ImVec2(center.x - title_size.x * 0.5f, center.y + 2.0f), text_color, title);

			const ImVec2 hint_size = ImGui::CalcTextSize(hint);
			draw_list->AddText(ImVec2(center.x - hint_size.x * 0.5f, center.y + 18.0f), dim_color, hint);
		}
	}

	void TimelinePanel::DrawRowScrollbar(float list_right, float top, float bottom)
	{
		m_LastRowBarMin = ImVec2(0.0f, 0.0f);
		m_LastRowBarMax = ImVec2(0.0f, 0.0f);

		const float total = TrackRowsHeight();
		const float view = bottom - top;
		const float max_scroll = GetMaxScrollRows();
		if (max_scroll <= 0.0f || view <= 0.0f)
		{
			m_DraggingRowScroll = false;
			return;
		}

		constexpr float kBarWidth = 5.0f;
		constexpr float kThumbMin = 24.0f;
		const float bar_x = list_right - 10.0f;
		const float thumb_height = std::clamp(view * view / std::max(total, 1.0f), kThumbMin, view);
		const float travel = std::max(view - thumb_height, 1.0f);
		const float thumb_y = top + travel * (m_ScrollY / max_scroll);

		/* 命中区比把手宽一截，好抓；轨道行那边的点击靠 IsRowScrollbarAt 让开 */
		m_LastRowBarMin = ImVec2(bar_x - 3.0f, top);
		m_LastRowBarMax = ImVec2(bar_x + kBarWidth + 3.0f, bottom);

		const ImVec2 mouse = ImGui::GetIO().MousePos;
		const bool bar_hovered = IsRowScrollbarAt(mouse);
		if (bar_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
		{
			/* 按住把手就顺着拖，点在空处则把把手中心挪过去 */
			const bool on_thumb = mouse.y >= thumb_y && mouse.y <= thumb_y + thumb_height;
			m_DraggingRowScroll = true;
			m_RowScrollDragOffset = on_thumb ? mouse.y - thumb_y : thumb_height * 0.5f;
		}
		if (m_DraggingRowScroll)
		{
			if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
			{
				const float local = mouse.y - top - m_RowScrollDragOffset;
				m_ScrollY = std::clamp(local / travel, 0.0f, 1.0f) * max_scroll;
				ClampScrollRows();
			}
			else
			{
				m_DraggingRowScroll = false;
			}
		}

		ImDrawList* draw_list = ImGui::GetWindowDrawList();
		draw_list->AddRectFilled(ImVec2(bar_x, top), ImVec2(bar_x + kBarWidth, bottom),
			ColorU32(EditorTheme::WithAlpha(EditorTheme::Token::Neutral4, 0.55f)), kBarWidth * 0.5f);

		const float grab_y = top + travel * (m_ScrollY / std::max(max_scroll, 1e-4f));
		const ImVec4 grab_color = (m_DraggingRowScroll || bar_hovered)
			? EditorTheme::Token::Neutral8 : EditorTheme::Token::Neutral7;
		draw_list->AddRectFilled(ImVec2(bar_x, grab_y), ImVec2(bar_x + kBarWidth, grab_y + thumb_height),
			ColorU32(grab_color), kBarWidth * 0.5f);
	}

	void TimelinePanel::DrawInspector(AnimatorComponent& animator, float width, float height)
	{
		(void)animator;

		/* 每帧插值的下拉条目：0 = 跟轨道，1..3 对应 KeyInterp */
		static constexpr const char* kKeyInterpItems[] = { "Follow track", "Step", "Linear", "Bezier" };

		ImGui::BeginChild("##TimelineInspector", ImVec2(width, height), true);
		const ImGuiStyle& style = ImGui::GetStyle();
		/* 属性栏比较窄也比较矮：行距和输入框内边距都收一点，默认高度下不用滚动 */
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, 3.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 2.0f));

		const float inner_width = ImGui::GetContentRegionAvail().x;
		const ImVec2 content_min = ImGui::GetCursorScreenPos();

		m_LastKeyValueMin = ImVec2(0.0f, 0.0f);
		m_LastKeyValueMax = ImVec2(0.0f, 0.0f);

		/* 标题栏：加高到一整行控件的高度，标题按行中线摆 */
		const float header_height = ImGui::GetFrameHeight() + 2.0f;
		ImGui::SetWindowFontScale(1.12f);
		const float title_height = ImGui::GetTextLineHeight();
		ImGui::GetWindowDrawList()->AddText(
			ImVec2(content_min.x, content_min.y + (header_height - title_height) * 0.5f),
			ColorU32(EditorTheme::Token::TextDim), "KEY PROPERTIES");
		ImGui::SetWindowFontScale(1.0f);

		ImGui::SetCursorScreenPos(ImVec2(content_min.x, content_min.y + header_height));
		ImGui::Separator();

		/* ---- 两列：左列属性名（宽度可拖）、右列值 ---- */
		const float label_width = std::clamp(m_InspectorLabelWidth, 40.0f, std::max(inner_width * 0.6f, 40.0f));
		constexpr float kLabelGap = 6.0f;
		const float value_x = content_min.x + label_width + kLabelGap;
		const float value_width = std::max(inner_width - label_width - kLabelGap, 40.0f);
		const float rows_top = ImGui::GetCursorScreenPos().y;

		/* 一行：左列属性名（可带轴色）、右列值；值那侧拿到的宽度已经扣掉两列间距 */
		const auto labeled_row = [&](const char* label_text, const ImVec4& label_color, auto&& draw_value)
		{
			const float row_y = ImGui::GetCursorScreenPos().y;
			ImGui::SetCursorScreenPos(ImVec2(content_min.x, row_y));
			ImGui::AlignTextToFramePadding();
			ImGui::PushStyleColor(ImGuiCol_Text, label_color);
			ImGui::TextUnformatted(label_text);
			ImGui::PopStyleColor();

			ImGui::SetCursorScreenPos(ImVec2(value_x, row_y));
			draw_value(value_width);
		};

		const bool has_track = m_SelectedTrack >= 0 && m_SelectedTrack < GetTrackCount();
		const bool single_key = m_Selection.size() == 1;

		TimelineModel::KeyRef ref;
		bool key_selected = false;
		if (single_key)
		{
			ref = m_Selection.front();
			key_selected = ref.Track >= 0 && ref.Track < GetTrackCount()
				&& ref.Key >= 0 && ref.Key < static_cast<int>(m_Clip->GetTracks()[ref.Track].Keys.size());
		}

		/* 插值下拉：轨道属性，选中帧时和 Time 挤在同一行 */
		const auto draw_interp = [&](float combo_width)
		{
			AnimationTrack& interp_track = m_Clip->GetTracks()[m_SelectedTrack];
			int interp = static_cast<int>(interp_track.Interp);
			const char* items[] = { "Step", "Linear", "Bezier" };
			ImGui::SetNextItemWidth(combo_width);
			if (ImGui::Combo("##TrackInterp", &interp, items, 3))
				SetTrackInterp(static_cast<KeyInterp>(interp));
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Interpolation");
		};

		if (has_track)
		{
			const AnimationTrack& track = m_Clip->GetTracks()[m_SelectedTrack];
			const std::string animator_name = (m_pScene != nullptr && m_AnimatorEntity != entt::null)
				? EntityName(*m_pScene, m_AnimatorEntity) : std::string("Root");
			const std::string label = TimelineModel::MakeTrackLabel(track, animator_name);

			labeled_row("Track", EditorTheme::Token::TextDim, [&](float row_width)
			{
				ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + row_width);
				ImGui::TextUnformatted(label.c_str());
				ImGui::PopTextWrapPos();
			});

			/* 时间与插值分两行：时间只放时间，插值单独一行，值列宽度也用满 */
			if (key_selected)
			{
				labeled_row("Time", EditorTheme::Token::TextDim, [&](float row_width)
				{
					AnimationTrack& time_track = m_Clip->GetTracks()[ref.Track];
					ImGui::SetNextItemWidth(row_width);
					float time = time_track.Keys[ref.Key].Time;
					if (ImGui::DragFloat("##InspectorKeyTime", &time, 0.005f, 0.0f, GetLaneDuration(), "%.2f s"))
					{
						BeginEdit();
						int new_index = ref.Key;
						if (TimelineModel::MoveKey(time_track, ref.Key, std::max(time, 0.0f), new_index))
						{
							m_Clip->SetDuration(std::max(m_Clip->GetDuration(), std::max(time, 0.0f)));
							m_Clip->RefreshDuration();
							SelectSingleKey(ref.Track, new_index);
							JumpToKey(ref.Track, new_index);
							MarkClipChanged();
						}
					}
					if (ImGui::IsItemDeactivatedAfterEdit())
						CommitEdit("Set Key Time");
				});
			}

			labeled_row("Interp", EditorTheme::Token::TextDim, [&](float row_width)
			{
				draw_interp(row_width);
			});
		}
		else
		{
			ImGui::TextDisabled("No track selected");
		}

		/* ---- 关键帧各分量值：一列名字一列值 ---- */
		if (m_Selection.size() == 1)
		{
			const TimelineModel::KeyRef value_ref = m_Selection.front();
			if (value_ref.Track >= 0 && value_ref.Track < static_cast<int>(m_Clip->GetTracks().size()))
			{
				AnimationTrack& edited_track = m_Clip->GetTracks()[value_ref.Track];
				if (value_ref.Key >= 0 && value_ref.Key < static_cast<int>(edited_track.Keys.size()))
				{
					AnimationKey& key = edited_track.Keys[value_ref.Key];

					/* 这一帧到下一帧的插值方式：Follow 就跟着轨道设置走 */
					labeled_row("Key", EditorTheme::Token::TextDim, [&](float row_width)
					{
						ImGui::SetNextItemWidth(row_width);
						int value = key.HasInterp ? static_cast<int>(key.Interp) + 1 : 0;
						if (ImGui::Combo("##KeyInterp", &value, kKeyInterpItems, 4))
							SetSelectedKeyInterp(value);
						if (ImGui::IsItemHovered())
							ImGui::SetTooltip("Interpolation from this key to the next one");
					});

					const int rows = CurveComponentCount(edited_track);
					const int active = std::clamp(m_CurveComponent, 0, rows - 1);
					static constexpr const char* kAxisLabels[] = { "X", "Y", "Z", "W" };
					const ImVec4 kAxisColors[] = {
						EditorTheme::Token::AxisX, EditorTheme::Token::AxisY,
						EditorTheme::Token::AxisZ, EditorTheme::Token::TextLabel,
					};

					bool value_edit_finished = false;
					bool active_rect_set = false;
					ImVec2 first_value_min(0.0f, 0.0f);
					ImVec2 first_value_max(0.0f, 0.0f);

					for (int index = 0; index < rows; ++index)
					{
						ImGui::PushID(index);
						labeled_row(kAxisLabels[std::min(index, 3)], kAxisColors[std::min(index, 3)],
							[&](float row_width)
						{
							ImGui::SetNextItemWidth(row_width);
							float component_value = KeyCurveValueAt(edited_track, value_ref.Key, index);
							const float speed = (edited_track.ValueType == TrackValueType::Quat) ? 0.5f : 0.01f;
							if (ImGui::DragFloat("##KeyValueAxis", &component_value, speed, 0.0f, 0.0f, "%.3f"))
							{
								BeginEdit();
								WriteKeyCurveValue(edited_track, value_ref.Key, index, component_value);
								JumpToKey(value_ref.Track, value_ref.Key);
								MarkClipChanged();
							}
							/* 按哪一行的值，曲线视图就跟着把哪条线当当前分量 */
							if (ImGui::IsItemActivated())
								m_CurveComponent = index;
							if (ImGui::IsItemDeactivatedAfterEdit())
								value_edit_finished = true;
						});

						const ImVec2 value_min = ImGui::GetItemRectMin();
						const ImVec2 value_max = ImGui::GetItemRectMax();
						if (index == 0)
						{
							first_value_min = value_min;
							first_value_max = value_max;
						}
						if (index == active)
						{
							m_LastKeyValueMin = value_min;
							m_LastKeyValueMax = value_max;
							active_rect_set = true;
						}
						ImGui::PopID();
					}

					if (!active_rect_set)
					{
						m_LastKeyValueMin = first_value_min;
						m_LastKeyValueMax = first_value_max;
					}

					if (value_edit_finished)
						CommitEdit("Set Key Value");
				}
			}
		}

		if (m_Selection.empty())
		{
			ImGui::TextWrapped("Select a key in the lane or curve to edit its time and value.");
		}
		else if (!single_key)
		{
			ImGui::TextDisabled("%d keys selected", static_cast<int>(m_Selection.size()));

			/* 多选时同样能改每帧插值：几帧设置不一致就显示 Mixed */
			int common = -2;
			for (const TimelineModel::KeyRef& selection_ref : m_Selection)
			{
				if (selection_ref.Track < 0 || selection_ref.Track >= GetTrackCount())
					continue;
				const AnimationTrack& selection_track = m_Clip->GetTracks()[selection_ref.Track];
				if (selection_ref.Key < 0 || selection_ref.Key >= static_cast<int>(selection_track.Keys.size()))
					continue;

				const AnimationKey& selection_key = selection_track.Keys[selection_ref.Key];
				const int value = selection_key.HasInterp ? static_cast<int>(selection_key.Interp) + 1 : 0;
				if (common == -2)
					common = value;
				else if (common != value)
					common = -1;
			}

			labeled_row("Key", EditorTheme::Token::TextDim, [&](float row_width)
			{
				const char* preview = (common >= 0) ? kKeyInterpItems[common] : "Mixed";
				ImGui::SetNextItemWidth(row_width);
				if (ImGui::BeginCombo("##KeyInterpAll", preview))
				{
					for (int index = 0; index < 4; ++index)
					{
						if (!ImGui::Selectable(kKeyInterpItems[index], common == index))
							continue;

						SetSelectedKeyInterp(index);
					}
					ImGui::EndCombo();
				}
			});
		}

		/* 属性名列宽：拖两列之间那条缝（只在有内容的高度上生效） */
		const float rows_bottom = ImGui::GetCursorScreenPos().y;
		if (rows_bottom > rows_top + 4.0f)
		{
			const float splitter_x = value_x - kLabelGap;
			ImDrawList* draw_list = ImGui::GetWindowDrawList();
			draw_list->AddLine(ImVec2(splitter_x - 0.5f, rows_top), ImVec2(splitter_x - 0.5f, rows_bottom),
				ColorU32(EditorTheme::WithAlpha(EditorTheme::Token::Neutral4, 0.55f)), 1.0f);

			ImGui::SetCursorScreenPos(ImVec2(splitter_x, rows_top));
			ImGui::InvisibleButton("##InspectorLabelSplit", ImVec2(kLabelGap, rows_bottom - rows_top));
			m_LastLabelSplitMin = ImGui::GetItemRectMin();
			m_LastLabelSplitMax = ImGui::GetItemRectMax();
			if (ImGui::IsItemHovered() || ImGui::IsItemActive())
				ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
			if (ImGui::IsItemActivated())
			{
				m_LabelResizeStartX = ImGui::GetIO().MousePos.x;
				m_LabelResizeStartWidth = label_width;
			}
			if (ImGui::IsItemActive())
			{
				m_InspectorLabelWidth = std::clamp(
					m_LabelResizeStartWidth + (ImGui::GetIO().MousePos.x - m_LabelResizeStartX),
					40.0f, std::max(inner_width * 0.6f, 40.0f));
			}
		}

		ImGui::PopStyleVar(2);
		ImGui::EndChild();
	}

	void TimelinePanel::RecordChangedTracks(Scene& scene, AnimatorComponent& animator)
	{
		if (!m_Recording || m_Clip == nullptr)
		{
			if (m_RecordPending)
			{
				CommitEdit("Record Key");
				m_RecordPending = false;
				m_RecordIdleFrames = 0;
			}
			return;
		}

		/* 先只比对、不改：有变化才抓改动前的快照，避免每帧整份拷贝 */
		struct PendingChange
		{
			int Index;
			glm::vec4 Value;
		};
		std::vector<PendingChange> changes;

		std::vector<AnimationTrack>& tracks = m_Clip->GetTracks();
		for (int index = 0; index < static_cast<int>(tracks.size()); ++index)
		{
			if (!tracks[index].Enabled)
				continue;

			glm::vec4 current;
			if (!TimelineModel::ReadCurrentValue(scene, m_AnimatorEntity, tracks[index], current))
				continue;

			glm::vec4 sampled;
			if (AnimationClip::Sample(tracks[index], animator.m_Time, sampled)
				&& TimelineModel::ValuesEqual(tracks[index].ValueType, current, sampled))
				continue;

			changes.push_back({ index, current });
		}

		if (!changes.empty())
		{
			if (!HasPendingEdit())
				BeginEdit();

			for (const PendingChange& change : changes)
				TimelineModel::InsertOrUpdateKey(tracks[change.Index], animator.m_Time, change.Value, tracks[change.Index].Interp);

			m_Clip->RefreshDuration();
			MarkClipChanged();
			if (!m_RecordPending)
			{
				m_RecordPending = true;
				m_RecordIdleFrames = 0;
			}
			return;
		}

		if (m_RecordPending && ++m_RecordIdleFrames >= kRecordIdleFrames)
		{
			CommitEdit("Record Key");
			m_RecordPending = false;
			m_RecordIdleFrames = 0;
		}
	}
}
