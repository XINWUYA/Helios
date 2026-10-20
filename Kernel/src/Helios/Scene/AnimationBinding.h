#pragma once
#include "Helios/Common/Export.h"
#include <string>
#include <glm/glm.hpp>
#include <entt/entt.hpp>
#include "AnimationClip.h"

namespace Helios
{
	class Scene;
	struct ComponentDesc;
	struct FieldDesc;

	/* 轨道与场景数据的桥：路径解析、组件/字段查找、按轨道语义读写值。
	 * 运行时（播放）和编辑器（时间轴、录制）共用这一份，避免各写一套转换。 */
	namespace AnimationBinding
	{
		/* "A/B/C" 沿父子层级往下找；空路径 = 自己。找不到返回 entt::null */
		entt::entity ResolvePath(Scene& scene, entt::entity root, const std::string& path);

		const ComponentDesc* FindComponent(const std::string& name);
		const FieldDesc* FindField(const ComponentDesc& desc, const std::string& name);

		/* 一条轨道解析出来的目标（组件指针每帧现取，池扩容后旧指针会失效）。
		 * Visibility 为真时是可见性轨道：只有实体，没有组件与字段。 */
		struct Target
		{
			entt::entity Entity{ entt::null };
			const ComponentDesc* Component{ nullptr };
			const FieldDesc* Field{ nullptr };
			bool Visibility{ false };
		};

		/* 可见性轨道（Component = "Visibility", Property = "Visible"）：VisibilityComponent 是
		 * 「有则隐」的组件（缺席 = 可见），套不了「组件存在 + 字段偏移」那套，约定单独走一条路。
		 * 轨道值是 0/1，写回时 > 0.5 视为可见。 */
		[[nodiscard]] HELIOS_API bool IsVisibilityTrack(const AnimationTrack& track);
		/* 实体自己的显隐（不看父链；缺席组件 = 可见） */
		[[nodiscard]] HELIOS_API bool ReadVisibility(Scene& scene, entt::entity entity);
		/* 写实体自己的显隐：可见 = 去组件、隐藏 = 补组件并置 false（与层级眼睛同一套规范形） */
		void WriteVisibility(Scene& scene, entt::entity entity, bool visible);

		/* 解析轨道目标：目标实体存在、组件挂在上面、字段已注册 */
		HELIOS_API bool ResolveTarget(Scene& scene, entt::entity animator, const AnimationTrack& track, Target& out);
		/* 取组件指针；实体或组件不在时返回 nullptr */
		HELIOS_API void* GetComponentPtr(Scene& scene, const Target& target);

		/* 把字段当前值读成轨道值（旋转按四元数给） */
		HELIOS_API bool ReadValue(const FieldDesc& field, const void* component, TrackValueType value_type, glm::vec4& out_value);
		/* 把轨道值写回字段（四元数转欧拉角） */
		bool WriteValue(const FieldDesc& field, void* component, TrackValueType value_type, const glm::vec4& value);
	}
}
