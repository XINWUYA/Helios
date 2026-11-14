#include "Pch.h"
#include "Helios/Application/Application.h"
#include "Helios/Common/AssetPath.h"
#include "Helios/Core/Logger.h"

#import <UIKit/UIKit.h>

namespace
{
	/* UIApplicationMain 没法带参数，工厂先存这里，启动完成时再取 */
	Helios::ApplicationFactory s_Factory;
}

/* iOS 没有自己的主循环：UIApplicationMain 接管 run loop，
 * 窗口与渲染器在 Application 构造里建好，帧由窗口的 display link 推 */
@interface HeliosAppDelegate : NSObject <UIApplicationDelegate>
@end

@implementation HeliosAppDelegate
{
	Helios::UniquePtr<Helios::Application> _app;
}

- (BOOL)application:(UIApplication*)application didFinishLaunchingWithOptions:(NSDictionary*)launchOptions
{
	(void)application;
	(void)launchOptions;

	_app = s_Factory();
	return YES;
}

@end

namespace Helios
{
	int RunApplication(int argc, char** argv, ApplicationFactory factory)
	{
		/* 命令行参数在 iOS 上也认（simctl launch 后面接参数），显式工程优先于随包那份 */
		Logger::Init();
		ParseProjectArgument(argc, argv);
		ParseSceneArgument(argc, argv);
		s_Factory = std::move(factory);

		@autoreleasepool
		{
			return UIApplicationMain(argc, argv, nil, NSStringFromClass([HeliosAppDelegate class]));
		}
	}
}
