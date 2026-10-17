#pragma once
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include "Helios/Scene/AnimationClip.h"

namespace Helios
{
	class Scene;
	struct ComponentDesc;
	struct FieldDesc;

	/* 时间轴的纯逻辑：关键帧增删改、属性枚举、录制判定。
	 * 面板只负责画和收输入，测试直接打这一层。 */
	namespace TimelineModel
	{
		/* 关键帧引用（批量选择 / 删除用） */
		struct KeyRef
		{
			int Track{ -1 };
			int Key{ -1 };
		};

		/* 命中 / 插入 / 删除 / 移动 / 改值（时间变化后自动重排，返回新下标） */
		int FindKey(const AnimationTrack& track, float time, float tolerance = 1e-3f);
		int InsertOrUpdateKey(AnimationTrack& track, float time, const glm::vec4& value,
			KeyInterp interp = KeyInterp::Linear);
		bool RemoveKey(AnimationTrack& track, int index);
		bool MoveKey(AnimationTrack& track, int index, float new_time, int& out_new_index);
		bool SetKeyValue(AnimationTrack& track, int index, const glm::vec4& value);

		/* 批量删除：按轨道从后往前删，下标不会漂 */
		void RemoveKeys(std::vector<AnimationTrack>& tracks, const std::vector<KeyRef>& keys);

		/* 整组平移：before 是拖动前的轨道表，delta 是时间偏移；
		 * 重建受影响的轨道（落点重合的合并），并把新下标回填到 out_selection */
		void MoveKeys(std::vector<AnimationTrack>& tracks, const std::vector<AnimationTrack>& before,
			const std::vector<KeyRef>& selection, float delta, std::vector<KeyRef>& out_selection);

		/* 字段类型 -> 轨道值类型；Transform.Rotation 由调用方指定成 Quat */
		TrackValueType ValueTypeForField(const FieldDesc& field);

		/* 值比较：旋转看四元数夹角，其余看最大分量差 */
		bool ValuesEqual(TrackValueType value_type, const glm::vec4& lhs, const glm::vec4& rhs,
			float tolerance = 1e-3f);

		/* 目标实体上该属性的当前值 */
		bool ReadCurrentValue(Scene& scene, entt::entity animator, const AnimationTrack& track, glm::vec4& out_value);

		/* 录制一条轨道：当前值与 clip 在 time 的采样值不同就写关键帧，返回是否写了 */
		bool RecordTrack(Scene& scene, entt::entity animator, AnimationTrack& track, float time);

		/* 加轨道菜单的数据源：Animator 子树里所有可动画的属性 */
		struct PropertyEntry
		{
			std::string    Path;        /* 相对 Animator 的层级路径，空 = 自己 */
			std::string    EntityName;
			std::string    Component;
			std::string    Property;
			TrackValueType ValueType{ TrackValueType::Float };
		};

		std::vector<PropertyEntry> CollectAnimatableProperties(Scene& scene, entt::entity animator);

		/* 轨道显示名：路径为空时用 Animator 自己的名字 */
		std::string MakeTrackLabel(const AnimationTrack& track, const std::string& animator_name);
	}
}
