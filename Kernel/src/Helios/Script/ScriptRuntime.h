#pragma once
#include <cstddef>
#include <entt/entt.hpp>
#include <functional>
#include <vector>

namespace Helios
{
	class Scene;
	class Script;
	struct ScriptComponent;
	struct ScriptSlot;

	/* 脚本运行时：Runtime 模式下驱动场景里所有 ScriptComponent 的每个挂载点。
	 * 进 Runtime 才建实例，退出来就全部销毁 —— Edit 模式不跑脚本，场景里只留类名和参数。 */
	class HELIOS_API ScriptRuntime
	{
	public:
		/* 给每个挂载点建实例并调 OnCreate（类缺失的跳过） */
		void Start(Scene& scene);
		/* 逆序 OnDestroy 并销毁全部实例 */
		void Stop();
		/* 每帧驱动：实例列表在 Start / Stop 之间不变，实体中途失效的逐个跳过 */
		void Tick(Scene& scene, float delta_time);

		/* 热重载：存下实例的字段值 → 销毁实例 → 调 swap_module 换库 → 用新类重建 → 回灌。
		 * 回调失败（编译不过）时表里还是旧类，照样重建，运行不中断。
		 * 不在 Runtime 时只换库，不碰实例。 */
		bool Reload(Scene& scene, const std::function<bool()>& swap_module);

		[[nodiscard]] bool IsRunning() const { return m_bRunning; }
		[[nodiscard]] size_t GetInstanceCount() const { return m_Instances.size(); }
		/* 累计创建过多少实例：热重载是否真的重建了实例，看这个（地址会被分配器复用，靠不住） */
		[[nodiscard]] size_t GetCreatedCount() const { return m_CreatedCount; }
		/* 第 index 个实例挂在哪个实体上（按执行顺序）：排序结果要靠它验证 */
		[[nodiscard]] entt::entity GetInstanceEntity(size_t index) const;

		/* 上一帧的步长与进 Runtime 以来的累计时间（脚本 API 读这两项） */
		[[nodiscard]] float GetLastDeltaTime() const { return m_LastDeltaTime; }
		[[nodiscard]] float GetElapsedTime() const { return m_ElapsedTime; }

		/* 实体即将销毁：先把它挂的实例 OnDestroy 掉并从列表里摘走 */
		void DestroyInstancesFor(entt::entity entity);

	private:
		struct Instance
		{
			entt::entity Entity{ entt::null };
			size_t       Slot{ 0 };
			Script*      Object{ nullptr };
			void       (*Destroy)(Script* script){ nullptr };
		};

		void CreateInstance(Scene& scene, entt::entity entity, size_t slot_index, ScriptSlot& slot);
		void DestroyInstance(Instance& instance);

		std::vector<Instance> m_Instances;
		/* Stop 时要回挂载点里摘掉实例指针（组件比运行时活得久） */
		Scene* m_pScene{ nullptr };
		size_t m_CreatedCount{ 0 };
		float m_LastDeltaTime{ 0.0f };
		float m_ElapsedTime{ 0.0f };
		bool m_bRunning{ false };
	};
}
