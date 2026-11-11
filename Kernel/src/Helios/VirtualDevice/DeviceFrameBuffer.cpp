#include "Pch.h"
#include "DeviceFrameBuffer.h"
#include "Helios/Renderer/Renderer.h"
#include "GraphicsAPI/OpenGL/OpenGLFrameBuffer.h"
#if HELIOS_PLATFORM_APPLE
#include "GraphicsAPI/Metal/MetalFrameBuffer.h"
#endif

namespace Helios
{
	DeviceFrameBuffer::DeviceFrameBuffer(std::string name, FrameBufferDesc desc)
		: m_DebugName(std::move(name)), m_FrameBufferDesc(std::move(desc))
	{
	}

	/* 创建FrameBuffer */
	SharedPtr<DeviceFrameBuffer> DeviceFrameBuffer::Create(const std::string& name, const FrameBufferDesc& desc)
	{
		switch (Renderer::CurrentAPI())
		{
		case RenderAPI::None:
			CORE_LOG_ERROR("RenderAPI can't be None!");
			return nullptr;
		case RenderAPI::OpenGL:
			return CreateSharedPtr<OpenGLFrameBuffer>(name, desc);
#if HELIOS_PLATFORM_APPLE
		case RenderAPI::Metal:
			return CreateSharedPtr<MetalFrameBuffer>(name, desc);
#endif
		default:
			CORE_LOG_ERROR("Unknown RenderAPI is unsupported!");
			return nullptr;
		}
	}}
