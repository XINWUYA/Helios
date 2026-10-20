#pragma once
#include "FrameGraphResourceHandle.h"

namespace Helios
{
	/*
	 * 用于罗列和收集FrameGraph中后面Pass需要用到的资源Handle
	 * 可通过资源Name获取
	 */
	class HELIOS_API Blackboard
	{
	public:
		Blackboard() = default;
		~Blackboard() = default;

		/* [] 重载，根据资源名直接存取 */
		FrameGraphResourceHandle& operator[](const std::string& name) noexcept;
		/* 存放到映射中 */
		void Add(const std::string& name, FrameGraphResourceHandle handle) noexcept;

		/* 根据名称获取资源 */
		template <typename ResourceType>
		FrameGraphResourceHandleTyped<ResourceType> GetResourceHandle(const std::string& name) const noexcept
		{
			return static_cast<FrameGraphResourceHandleTyped<ResourceType>>(GetResourceHandleInternal(name));
		}

	private:
		/* 根据名称获取资源 */
		[[nodiscard]] FrameGraphResourceHandle GetResourceHandleInternal(const std::string& name) const noexcept;

		/* 资源名与资源Handle的映射<name, FrameGraphResourceHandle> */
		std::unordered_map<std::string, FrameGraphResourceHandle> m_NameToResourceHandleMap{};
	};

}
