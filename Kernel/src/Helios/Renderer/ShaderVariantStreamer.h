#pragma once
#include <deque>
#include <future>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Helios/Common/Common.h"
#include "Helios/Renderer/RenderCommon.h"
#include "Helios/Renderer/ShaderVariantCatalog.h"

namespace Helios
{
	class DeviceShader;

	/*
	 * 变体异步编译。
	 * 主线程要一个还没编过的变体时只排队、不等编译：这一帧先拿最接近的已编变体顶着，
	 * 编完由 Tick 收进资源缓存，材质下一次取就会命中精确变体。
	 * GL 的 program 要绑调用线程的上下文，只能在主线程分帧编 —— 同样不在请求的那一帧编。
	 */
	class ShaderVariantStreamer
	{
	public:
		static ShaderVariantStreamer& Instance();

		/* 已缓存返回 true；没编过返回 false 并排进队列（同一组合只排一次，失败不重排） */
		bool Request(const std::string& path, const ShaderVariantDefines& defines);

		/* 加载期批量预热：当场编完，不进队列。后台与主线程共用一把编译锁，
		 * 批量任务堆在队列里会拖慢随后的同步加载 */
		bool Preload(const std::string& path, const ShaderVariantDefines& defines);

		/* 严格模式：只认清单里的组合，缺什么都不编，直接报错。Player 用 */
		void SetStrictManifest(const std::vector<ShaderVariantItem>& items);
		void ClearStrictManifest();
		[[nodiscard]] bool IsStrict() const { return m_Strict; }
		/* 读清单文件并进入严格模式，返回条目数；读不到就不改状态 */
		size_t LoadStrictManifest(const std::string& filepath);

		/* 主线程每帧：先收后台结果入库，再按预算起新任务 */
		void Tick();

		/* 每帧最多起几个编译，默认四个；后台同时最多跑两个 */
		void SetFrameBudget(uint32_t count) { m_FrameBudget = count; }

		/* 每完成一个变体加一：材质据此判断手头的近似结果要不要重新查一次 */
		[[nodiscard]] uint32_t CompletedGeneration() const { return m_Completed; }
		[[nodiscard]] size_t Pending() const { return m_Pending.size() + m_InFlight.size(); }
		[[nodiscard]] bool Empty() const { return m_Pending.empty() && m_InFlight.empty(); }

		/* 源码改了：把失败记录清掉，重编过的变体还有机会恢复到精确版本 */
		void ClearFailed() { m_Failed.clear(); }

	private:
		ShaderVariantStreamer() = default;

		struct Item
		{
			std::string Path;
			ShaderVariantDefines Defines;

			[[nodiscard]] bool operator==(const Item& other) const
			{
				return Path == other.Path && Defines == other.Defines;
			}
		};

		/* 去重键：路径拼上 define 指纹，比逐项比较哈希表快 */
		[[nodiscard]] static std::string MakeKey(const std::string& path, const ShaderVariantDefines& defines);

		/* Metal 能后台编；GL 的 program 要绑在调用线程的上下文上，只能主线程编 */
		[[nodiscard]] static bool CanBuildAsync();
		/* 后台编译默认开。回归与基准设 HELIOS_VARIANT_ASYNC=0 走同步：
		 * 后台和主线程共用一把编译锁，异步调度会让"大量着色器连续加载"的基准变慢 */
		[[nodiscard]] static bool AsyncEnabled();
		/* 后台只做 CPU 段（读源码、转换、反射）：设备对象统一回主线程建，
		 * 这样进程退出时后台线程不会碰到已销毁的设备 */
		static SharedPtr<DeviceShader> BuildCpu(const Item& item);
		/* 编完的设备对象登记进资源缓存 */
		static bool Publish(const Item& item, const SharedPtr<DeviceShader>& shader);

		std::deque<Item> m_Pending;
		std::unordered_set<std::string> m_Queued;
		std::deque<std::pair<Item, std::future<SharedPtr<DeviceShader>>>> m_InFlight;
		/* 编译失败过一次的组合不再自动重排：源码有明显错误时免得每帧都试一遍 */
		std::unordered_set<std::string> m_Failed;
		/* 严格模式的白名单（MakeKey 形式），空集合在非严格模式下无意义 */
		std::unordered_set<std::string> m_Manifest;
		bool m_Strict{ false };

		uint32_t m_FrameBudget{ 4 };
		uint32_t m_Completed{ 0 };
	};
}
