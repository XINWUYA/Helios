#include "Helios/Common/Platform.h"

#if HELIOS_PLATFORM_IOS

#include "Pch.h"
#include "MetalWindowIOS.h"
#include "MetalRenderAPI.h"

#include "Helios/Application/Application.h"
#include "Helios/Events/ApplicationEvent.h"
#include "Helios/Events/MouseEvent.h"
#include "Helios/Renderer/Renderer.h"

#import <UIKit/UIKit.h>
#import <QuartzCore/CAMetalLayer.h>

/* ---------------------------------------------------------------------------
 * ObjC 侧：视图与显示链路
 * 这两块只做转发，状态全在 IOSWindow 里，避免同一份状态跨语言两头存。
 * ------------------------------------------------------------------------- */

/* 视图的 contentScaleFactor 要等入窗并跑完一次布局才是屏幕的 scale，
 * 建窗口那一刻读它拿到的是 1，后备缓冲于是按 1x 建、再由系统放大三倍 */
static CGFloat ScreenScaleOf(UIView* view)
{
	UIScreen* screen = view.window.screen != nil ? view.window.screen : [UIScreen mainScreen];
	return screen.scale;
}

/* 安全区按逻辑点取，左上下右；引擎侧再用 scale 换成像素 */
static glm::vec4 ReadSafeArea(UIView* view)
{
	const UIEdgeInsets insets = view.safeAreaInsets;
	return glm::vec4(static_cast<float>(insets.left), static_cast<float>(insets.top),
		static_cast<float>(insets.right), static_cast<float>(insets.bottom));
}

/* 触摸与布局变化要回给 C++ 窗口对象；视图只拿一个不透明指针 */
@interface HeliosIOSView : UIView
@property(nonatomic, assign) Helios::IOSWindow* Owner;
@end

@implementation HeliosIOSView
{
	/* 一根手指点 UI、一根手指转视角：认下的触摸各自跟自己那条路，
	 * 后落的手指不会把前一根的位置顶掉 */
	UITouch* _uiTouch;
	UITouch* _lookTouch;
}

/* 视图的根层直接就是 Metal 层：iOS 上不能给 UIView 换 layer，
 * 只能靠 layerClass 指定，之后 view.layer 就是这个类型。 */
+ (Class)layerClass
{
	return [CAMetalLayer class];
}

- (instancetype)initWithFrame:(CGRect)frame
{
	self = [super initWithFrame:frame];
	if (self != nil)
	{
		self.multipleTouchEnabled = YES;
		self.backgroundColor = [UIColor blackColor];
		_uiTouch = nil;
		_lookTouch = nil;
	}
	return self;
}

- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
	if (self.Owner == nullptr)
		return;

	for (UITouch* touch in touches)
	{
		const CGPoint point = [touch locationInView:self];
		/* 先问这根手指落没落在控件上，落在控件上的归 UI，其余给视角 */
		if (_uiTouch == nil && self.Owner->ShouldClaimUITouch(point.x, point.y))
		{
			_uiTouch = touch;
			self.Owner->OnUIPointerDown(static_cast<float>(point.x), static_cast<float>(point.y));
		}
		else if (_lookTouch == nil)
		{
			_lookTouch = touch;
			self.Owner->OnPointerDown(static_cast<float>(point.x), static_cast<float>(point.y));
		}
	}
}

- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
	if (self.Owner == nullptr)
		return;

	for (UITouch* touch in touches)
	{
		const CGPoint point = [touch locationInView:self];
		if (touch == _uiTouch)
			self.Owner->OnUIPointerMoved(static_cast<float>(point.x), static_cast<float>(point.y));
		else if (touch == _lookTouch)
			self.Owner->OnPointerMoved(static_cast<float>(point.x), static_cast<float>(point.y));
	}
}

- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
	if (self.Owner == nullptr)
		return;

	for (UITouch* touch in touches)
	{
		const CGPoint point = [touch locationInView:self];
		if (touch == _uiTouch)
		{
			_uiTouch = nil;
			self.Owner->OnUIPointerUp(static_cast<float>(point.x), static_cast<float>(point.y));
		}
		else if (touch == _lookTouch)
		{
			_lookTouch = nil;
			self.Owner->OnPointerUp(static_cast<float>(point.x), static_cast<float>(point.y));
		}
	}
}

- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
	[self touchesEnded:touches withEvent:event];
}

- (void)layoutSubviews
{
	[super layoutSubviews];

	const CGSize size = self.bounds.size;
	if (self.Owner != nullptr)
		self.Owner->OnViewLayout(static_cast<float>(size.width), static_cast<float>(size.height));
}

- (void)safeAreaInsetsDidChange
{
	[super safeAreaInsetsDidChange];

	if (self.Owner != nullptr)
		self.Owner->OnSafeAreaChange();
}

@end

/* CADisplayLink 要一个 ObjC target，这里只负责把每帧转给窗口 */
@interface HeliosDisplayLinkTarget : NSObject
@property(nonatomic, assign) Helios::IOSWindow* Window;
- (void)step:(CADisplayLink*)link;
@end

@implementation HeliosDisplayLinkTarget
- (void)step:(CADisplayLink*)link
{
	(void)link;
	if (self.Window != nullptr)
		self.Window->OnDisplayLink();
}
@end

namespace Helios
{
	void RunPlatformEventLoop()
	{
		/* 移动端没有主循环：进系统 run loop，帧由 display link 推。
		 * 这里正常不会返回，返回即意味着应用要退出。 */
		@autoreleasepool
		{
			[[NSRunLoop mainRunLoop] run];
		}
	}

	/* -----------------------------------------------------------------------
	 * MetalContextIOS
	 * --------------------------------------------------------------------- */

	/* 上下文工厂：DeviceContext.cpp 依赖 GLFW，iOS 不编它，这里的 window 就是 UIView* */
	UniquePtr<DeviceContext> DeviceContext::Create(void* window)
	{
		return CreateUniquePtr<MetalContextIOS>(window);
	}

	MetalContextIOS::MetalContextIOS(void* native_view)
		: m_View(native_view)
	{
		ASSERT(native_view, "UIView is nullptr!");
	}

	MetalContextIOS::~MetalContextIOS()
	{
		if (m_RenderPassDescriptor)
			m_RenderPassDescriptor->release();
		if (m_DepthTexture)
			m_DepthTexture->release();
		/* device 与 command queue 归 MetalRenderAPI，不在这里释放 */
	}

	void MetalContextIOS::Init()
	{
		auto metal_render_api = std::dynamic_pointer_cast<MetalRenderAPI>(Renderer::GetRenderAPI());
		if (!metal_render_api)
		{
			CORE_LOG_ERROR("MetalRenderAPI is not available!");
			return;
		}

		m_Device = metal_render_api->GetDevice();

		UIView* view = (__bridge UIView*)m_View;
		const CGFloat scale = ScreenScaleOf(view);

		/* 视图的根层由 +layerClass 指定成 CAMetalLayer，这里只配参数，不换层 */
		m_MetalLayer = reinterpret_cast<CA::MetalLayer*>(view.layer);
		m_MetalLayer->setDevice(m_Device);
		m_MetalLayer->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
		/* 默认目标快照要把 drawable 当 blit 源读，不能只当渲染目标 */
		m_MetalLayer->setFramebufferOnly(false);
		/* contentsScale 与视图对齐，否则 drawable 会被降采样 */
		[(id)m_MetalLayer setContentsScale:scale];

		const CGSize bounds = view.bounds.size;
		m_MetalLayer->setDrawableSize(CGSizeMake(bounds.width * scale, bounds.height * scale));

		m_RenderPassDescriptor = MTL::RenderPassDescriptor::alloc()->init();
		RecreateDepthTexture(static_cast<uint32_t>(bounds.width * scale),
			static_cast<uint32_t>(bounds.height * scale));

		metal_render_api->SetMetalLayer(m_MetalLayer);
		metal_render_api->SetRenderPassDescriptor(m_RenderPassDescriptor);

		CORE_LOG_INFO("Metal layer created ({}x{} pixels, scale {})",
			static_cast<uint32_t>(bounds.width * scale), static_cast<uint32_t>(bounds.height * scale),
			static_cast<double>(scale));
	}

	void MetalContextIOS::SwapBuffers()
	{
		if (auto metal_render_api = std::dynamic_pointer_cast<MetalRenderAPI>(Renderer::GetRenderAPI()))
			metal_render_api->Present();
	}

	void MetalContextIOS::OnResize(uint32_t width, uint32_t height)
	{
		if (!m_MetalLayer || width == 0 || height == 0)
			return;

		m_MetalLayer->setDrawableSize(CGSizeMake(width, height));
		RecreateDepthTexture(width, height);
	}

	void MetalContextIOS::RecreateDepthTexture(uint32_t width, uint32_t height)
	{
		if (!m_Device || !m_RenderPassDescriptor || width == 0 || height == 0)
			return;

		/* drawable 尺寸跟着视图走，深度图必须严丝合缝，否则深度测试区域与颜色区域错位。
		 * texture2DDescriptor 返回 autoreleased(+0) 对象，只能释放 newTexture 那一份。 */
		if (m_DepthTexture && m_DepthWidth == width && m_DepthHeight == height)
			return;

		if (m_DepthTexture)
		{
			m_DepthTexture->release();
			m_DepthTexture = nullptr;
		}

		MTL::TextureDescriptor* depth_desc = MTL::TextureDescriptor::texture2DDescriptor(
			MTL::PixelFormatDepth32Float_Stencil8, width, height, false);
		depth_desc->setStorageMode(MTL::StorageModePrivate);
		depth_desc->setUsage(MTL::TextureUsageRenderTarget);
		m_DepthTexture = m_Device->newTexture(depth_desc);

		m_DepthWidth = width;
		m_DepthHeight = height;

		auto depth_attachment = m_RenderPassDescriptor->depthAttachment();
		depth_attachment->setTexture(m_DepthTexture);
		depth_attachment->setLoadAction(MTL::LoadActionClear);
		depth_attachment->setStoreAction(MTL::StoreActionStore);
		/* Reversed-Z：远平面深度为 0，清零取 0 */
		depth_attachment->setClearDepth(0.0);

		auto stencil_attachment = m_RenderPassDescriptor->stencilAttachment();
		stencil_attachment->setTexture(m_DepthTexture);
		stencil_attachment->setLoadAction(MTL::LoadActionClear);
		stencil_attachment->setStoreAction(MTL::StoreActionStore);
		stencil_attachment->setClearStencil(0);
	}

	/* -----------------------------------------------------------------------
	 * IOSWindow
	 * --------------------------------------------------------------------- */

	IOSWindow::IOSWindow(const WindowDesc& desc)
	{
		Build(desc);
	}

	IOSWindow::~IOSWindow()
	{
		Destroy();
	}

	void IOSWindow::Build(const WindowDesc& desc)
	{
		m_IsVSync = desc.IsVSync;

		UIWindow* window = [[UIWindow alloc] initWithFrame:[[UIScreen mainScreen] bounds]];
		UIViewController* controller = [[UIViewController alloc] init];
		HeliosIOSView* view = [[HeliosIOSView alloc] initWithFrame:window.bounds];
		view.Owner = this;

		controller.view = view;
		window.rootViewController = controller;
		[window makeKeyAndVisible];

		/* 布局跑完才有屏幕的 scale 与安全区，后面算尺寸要用 */
		[view layoutIfNeeded];

		m_Window = window;
		m_Controller = controller;
		m_View = view;
		m_ContentScale = static_cast<float>(ScreenScaleOf(view));
		m_SafeArea = ReadSafeArea(view);

		const CGSize bounds = view.bounds.size;
		m_Width = static_cast<uint32_t>(bounds.width * m_ContentScale + 0.5);
		m_Height = static_cast<uint32_t>(bounds.height * m_ContentScale + 0.5);

		CORE_LOG_INFO("Create iOS window: {}x{} pixels, content scale {}, safe area {} {} {} {}",
			m_Width, m_Height, static_cast<double>(m_ContentScale),
			m_SafeArea.x, m_SafeArea.y, m_SafeArea.z, m_SafeArea.w);
	}

	void IOSWindow::Destroy()
	{
		if (m_DisplayLink != nullptr)
		{
			[(CADisplayLink*)m_DisplayLink invalidate];
			[(id)m_DisplayLink release];
			m_DisplayLink = nullptr;
		}

		if (m_DisplayLinkTarget != nullptr)
		{
			[(id)m_DisplayLinkTarget release];
			m_DisplayLinkTarget = nullptr;
		}

		NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
		if (m_ResignActiveObserver != nullptr)
		{
			[center removeObserver:(id)m_ResignActiveObserver];
			[(id)m_ResignActiveObserver release];
			m_ResignActiveObserver = nullptr;
		}
		if (m_BecomeActiveObserver != nullptr)
		{
			[center removeObserver:(id)m_BecomeActiveObserver];
			[(id)m_BecomeActiveObserver release];
			m_BecomeActiveObserver = nullptr;
		}

		m_pContext.reset();

		if (m_View != nullptr)
		{
			((HeliosIOSView*)m_View).Owner = nullptr;
			[(id)m_View release];
			m_View = nullptr;
		}
		if (m_Controller != nullptr)
		{
			[(id)m_Controller release];
			m_Controller = nullptr;
		}
		if (m_Window != nullptr)
		{
			[(id)m_Window release];
			m_Window = nullptr;
		}
	}

	void IOSWindow::InitGraphicsContext()
	{
		m_pContext = CreateUniquePtr<MetalContextIOS>(m_View);
		m_pContext->Init();

		/* 帧由系统显示链路推：帧率天然跟着屏幕刷新，熄屏与退后台自己就停 */
		HeliosDisplayLinkTarget* target = [[HeliosDisplayLinkTarget alloc] init];
		target.Window = this;
		CADisplayLink* link = [CADisplayLink displayLinkWithTarget:target selector:@selector(step:)];
		[link addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
		m_DisplayLinkTarget = target;
		m_DisplayLink = link;

		/* 失去活动状态就停帧：退后台、来电、控制中心都会走到这里，
		 * 这期间 drawable 可能被系统收回，继续画就是崩。回到活动状态再恢复。 */
		NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
		m_ResignActiveObserver = [[center addObserverForName:UIApplicationWillResignActiveNotification
			object:nil queue:nil usingBlock:^(NSNotification* note)
			{
				(void)note;
				if (Application* app = Application::Instance())
					app->SetPaused(true);
			}] retain];
		m_BecomeActiveObserver = [[center addObserverForName:UIApplicationDidBecomeActiveNotification
			object:nil queue:nil usingBlock:^(NSNotification* note)
			{
				(void)note;
				/* 停帧期间系统可能改过视图几何，drawable 重新对齐一次 */
				this->RefreshDrawableSize();

				if (Application* app = Application::Instance())
					app->SetPaused(false);
			}] retain];
	}

	void IOSWindow::OnUpdate()
	{
		/* 呈现由 Tick 末尾调到这里；交换本身在上下文里做 */
		if (m_pContext)
			m_pContext->SwapBuffers();
	}

	void IOSWindow::SetVSync(bool enable)
	{
		/* iOS 的呈现节奏由 display link 决定，没有独立的垂直同步开关 */
		m_IsVSync = enable;
	}

	void IOSWindow::SetWindowTitle(const std::string& title)
	{
		(void)title;
	}

	bool IOSWindow::QueryPointerButtonPressed(int button) const
	{
		/* MouseCode 里 0 是左键；移动端把第一根手指当左键 */
		return button == 0 && m_PointerDown;
	}

	bool IOSWindow::SetClipboardText(const std::string& text)
	{
		[UIPasteboard generalPasteboard].string = [NSString stringWithUTF8String:text.c_str()];
		return true;
	}

	bool IOSWindow::GetClipboardText(std::string& text)
	{
		NSString* value = [UIPasteboard generalPasteboard].string;
		if (value == nil)
			return false;

		const char* utf8 = value.UTF8String;
		if (utf8 == nullptr)
			return false;

		text = utf8;
		return true;
	}

	void IOSWindow::OnPointerDown(float x, float y)
	{
		m_PointerPosition = { x, y };
		m_PointerDown = true;

		if (m_Callback)
		{
			MouseButtonPressedEvent event(0);
			m_Callback(&event);
		}
	}

	void IOSWindow::OnPointerMoved(float x, float y)
	{
		m_PointerPosition = { x, y };

		if (m_Callback)
		{
			MouseMovedEvent event(x, y);
			m_Callback(&event);
		}
	}

	void IOSWindow::OnPointerUp(float x, float y)
	{
		m_PointerPosition = { x, y };
		m_PointerDown = false;

		if (m_Callback)
		{
			MouseButtonReleasedEvent event(0);
			m_Callback(&event);
		}
	}

	bool IOSWindow::ShouldClaimUITouch(float x, float y) const
	{
		return m_UITouchTest && m_UITouchTest(glm::vec2(x, y));
	}

	/* UI 那一路不派发鼠标事件：玩家形态每帧轮询它的位置与按下状态，
	 * 派发了反而会让监听鼠标的地方把两根手指当成两路点击 */
	void IOSWindow::OnUIPointerDown(float x, float y)
	{
		m_UIPointerPosition = { x, y };
		m_UIPointerDown = true;
	}

	void IOSWindow::OnUIPointerMoved(float x, float y)
	{
		m_UIPointerPosition = { x, y };
	}

	void IOSWindow::OnUIPointerUp(float x, float y)
	{
		m_UIPointerPosition = { x, y };
		m_UIPointerDown = false;
	}

	void IOSWindow::OnViewLayout(float width_points, float height_points)
	{
		if (width_points <= 0.0f || height_points <= 0.0f || m_View == nullptr)
			return;

		/* 转屏或投到外接屏时屏幕 scale 可能变，跟着重算一次 */
		m_ContentScale = static_cast<float>(ScreenScaleOf((__bridge UIView*)m_View));
		m_SafeArea = ReadSafeArea((__bridge UIView*)m_View);

		const uint32_t width = static_cast<uint32_t>(width_points * m_ContentScale + 0.5f);
		const uint32_t height = static_cast<uint32_t>(height_points * m_ContentScale + 0.5f);
		if (width == m_Width && height == m_Height)
			return;

		m_Width = width;
		m_Height = height;

		if (m_pContext)
			m_pContext->OnResize(width, height);

		if (m_Callback)
		{
			WindowResizeEvent event(width, height);
			m_Callback(&event);
		}
	}

	void IOSWindow::OnSafeAreaChange()
	{
		if (m_View == nullptr)
			return;

		m_SafeArea = ReadSafeArea((__bridge UIView*)m_View);
	}

	void IOSWindow::OnDisplayLink()
	{
		Application* app = Application::Instance();
		if (app == nullptr || app->IsPaused())
			return;

		app->Tick();
	}

	void IOSWindow::RefreshDrawableSize()
	{
		if (m_View == nullptr)
			return;

		const CGSize bounds = ((__bridge UIView*)m_View).bounds.size;
		m_ContentScale = static_cast<float>(ScreenScaleOf((__bridge UIView*)m_View));
		m_SafeArea = ReadSafeArea((__bridge UIView*)m_View);
		m_Width = static_cast<uint32_t>(bounds.width * m_ContentScale + 0.5f);
		m_Height = static_cast<uint32_t>(bounds.height * m_ContentScale + 0.5f);

		if (m_pContext)
			m_pContext->OnResize(m_Width, m_Height);
	}
}

#endif
