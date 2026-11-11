#pragma once

#include "Helios/Common/Platform.h"

#if HELIOS_PLATFORM_IOS

#include "Helios/VirtualDevice/DeviceWindow.h"
#include "Helios/VirtualDevice/DeviceContext.h"
#include <Metal/Metal.hpp>
#include <QuartzCore/CAMetalLayer.hpp>
#include <glm/glm.hpp>

namespace Helios
{
	/* iOS 的 Metal 上下文：设备与命令队列由 MetalRenderAPI 持有，这里只管
	 * CAMetalLayer 与默认渲染通道的深度附件，drawable 由后端按帧去取。 */
	class MetalContextIOS final : public DeviceContext
	{
	public:
		/* native_view 是 UIView*，C++ 头里不引 UIKit，统一按 void* 传 */
		explicit MetalContextIOS(void* native_view);
		~MetalContextIOS() override;

		void Init() override;
		void SwapBuffers() override;

		/* 物理像素尺寸变了：同步 drawableSize 并按需重建深度附件 */
		void OnResize(uint32_t width, uint32_t height);

		MTL::Device* GetDevice() const { return m_Device; }

	private:
		void RecreateDepthTexture(uint32_t width, uint32_t height);

		void* m_View{ nullptr };
		MTL::Device* m_Device{ nullptr };
		CA::MetalLayer* m_MetalLayer{ nullptr };
		MTL::RenderPassDescriptor* m_RenderPassDescriptor{ nullptr };
		MTL::Texture* m_DepthTexture{ nullptr };
		uint32_t m_DepthWidth{ 0 };
		uint32_t m_DepthHeight{ 0 };
	};

	/* iOS 窗口：自己建 UIWindow 与根视图，把 CAMetalLayer 挂上去，
	 * 触摸写进指针状态，CADisplayLink 每帧推一次 Application::Tick。
	 * 尺寸一律物理像素，逻辑点只出现在触摸与 UI 里。 */
	class IOSWindow final : public DeviceWindow
	{
	public:
		explicit IOSWindow(const WindowDesc& desc);
		~IOSWindow() override;

		void OnUpdate() override;

		[[nodiscard]] uint32_t GetWidth() const override { return m_Width; }
		[[nodiscard]] uint32_t GetHeight() const override { return m_Height; }
		[[nodiscard]] float GetContentScale() const override { return m_ContentScale; }
		[[nodiscard]] bool IsVSync() const override { return m_IsVSync; }
		void SetVSync(bool enable) override;
		void SetWindowTitle(const std::string& title) override;
		[[nodiscard]] void* GetNativeWindow() const override { return m_View; }
		void SetEventCallback(const EventCallbackFunc& callback) override { m_Callback = callback; }

		/* 建 layer、挂上下文、起 display link。必须在 Renderer::Init 之后调 */
		void InitGraphicsContext() override;

		/* 移动端没有物理键盘 */
		[[nodiscard]] bool QueryKeyPressed(int key) const override { (void)key; return false; }
		[[nodiscard]] bool QueryPointerButtonPressed(int button) const override;
		[[nodiscard]] glm::vec2 QueryPointerPosition() const override { return m_PointerPosition; }
		[[nodiscard]] bool HasFocus() const override { return true; }
		[[nodiscard]] bool SetClipboardText(const std::string& text) override;
		[[nodiscard]] bool GetClipboardText(std::string& text) override;

		/* 视图转发进来的触摸，坐标是逻辑点 */
		void OnPointerDown(float x, float y);
		void OnPointerMoved(float x, float y);
		void OnPointerUp(float x, float y);
		/* 视图布局变化：同步尺寸并派发 resize 事件 */
		void OnViewLayout(float width_points, float height_points);
		/* 回前台后重新对齐 drawable：系统可能在后台改过视图几何 */
		void RefreshDrawableSize();

		/* 显示链路的一帧 */
		void OnDisplayLink();

	private:
		void Build(const WindowDesc& desc);
		void Destroy();

		void* m_Window{ nullptr };          /* UIWindow* */
		void* m_Controller{ nullptr };      /* UIViewController* */
		void* m_View{ nullptr };            /* UIView* */
		void* m_DisplayLink{ nullptr };     /* CADisplayLink* */
		void* m_DisplayLinkTarget{ nullptr }; /* 持有 window 指针的转发对象 */
		void* m_ResignActiveObserver{ nullptr };
		void* m_BecomeActiveObserver{ nullptr };

		UniquePtr<MetalContextIOS> m_pContext{ nullptr };

		uint32_t m_Width{ 0 };
		uint32_t m_Height{ 0 };
		float m_ContentScale{ 1.0f };
		bool m_IsVSync{ true };

		glm::vec2 m_PointerPosition{ 0.0f };
		bool m_PointerDown{ false };

		EventCallbackFunc m_Callback{};
	};
}

#endif
