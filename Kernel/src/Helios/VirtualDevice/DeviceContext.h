#pragma once

namespace Helios
{
	class DeviceContext
	{
	public:
		virtual ~DeviceContext() = default;

		virtual void Init() = 0;
		virtual void SwapBuffers() = 0;
	};

}