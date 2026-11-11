#include "Helios/Common/Platform.h"

#if HELIOS_PLATFORM_IOS

#include "Pch.h"

#include "Helios/Application/Application.h"
#include "Helios/Core/Logger.h"

#import <UIKit/UIKit.h>

namespace Helios
{
	/* 由 Main.cpp 提供 */
	UniquePtr<Application> CreateApplication();
}

/* iOS 上 Player 没有自己的主循环：UIApplicationMain 接管 run loop，
 * 窗口与渲染器在 Application 构造里建好，帧由窗口的 display link 推。 */
@interface HeliosPlayerDelegate : NSObject <UIApplicationDelegate>
@end

@implementation HeliosPlayerDelegate
{
	Helios::UniquePtr<Helios::Application> _app;
}

- (BOOL)application:(UIApplication*)application didFinishLaunchingWithOptions:(NSDictionary*)launchOptions
{
	(void)application;
	(void)launchOptions;

	Helios::Logger::Init();
	_app = Helios::CreateApplication();
	return YES;
}

@end

int main(int argc, char* argv[])
{
	@autoreleasepool
	{
		return UIApplicationMain(argc, argv, nil, NSStringFromClass([HeliosPlayerDelegate class]));
	}
}

#endif
