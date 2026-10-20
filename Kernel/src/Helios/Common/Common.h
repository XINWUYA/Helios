#pragma once
#include <memory>
#include "Export.h"

#define BIT(x) (1 << (x))
#define EXPAND_MACRO(x) x
#define STRINGIFY(x) #x
#define CONTACT(x, y) x##y
#define SAFE_DELETE(p) { if (p) { delete (p); (p) = nullptr; } }
#define SAFE_DELETE_ARRAY(p) { if (p) { delete[] (p); (p) = nullptr; } }
#define BIND_EVENT_FUNC(func) [this](auto&& ...args)->decltype(auto) { return this->func(std::forward<decltype(args)>(args)...); }

namespace Helios
{
	template <typename T>
	using UniquePtr = std::unique_ptr<T>;

	template <typename T>
	using SharedPtr = std::shared_ptr<T>;

	template <typename T>
	using WeakPtr = std::weak_ptr<T>;

	template<typename T, typename ... Args>
	constexpr UniquePtr<T> CreateUniquePtr(Args&& ... args)
	{
		return std::make_unique<T>(std::forward<Args>(args)...);
	}

	template<typename T, typename ... Args>
	constexpr SharedPtr<T> CreateSharedPtr(Args&& ... args)
	{
		return std::make_shared<T>(std::forward<Args>(args)...);
	}

	template<typename T, typename ... Args>
	constexpr SharedPtr<T> StaticPtrCast(Args&& ... args)
	{
		return std::static_pointer_cast<T>(std::forward<Args>(args)...);
	}

	template<typename T, typename ... Args>
	constexpr SharedPtr<T> DynamicPtrCast(Args&& ... args)
	{
		return std::dynamic_pointer_cast<T>(std::forward<Args>(args)...);
	}

	template<typename T, typename ... Args>
	constexpr SharedPtr<T> ConstPtrCast(Args&& ... args)
	{
		return std::const_pointer_cast<T>(std::forward<Args>(args)...);
	}

	template<typename T, typename ... Args>
	constexpr SharedPtr<T> ReinterpretPtrCast(Args&& ... args)
	{
		return std::reinterpret_pointer_cast<T>(std::forward<Args>(args)...);
	}
}