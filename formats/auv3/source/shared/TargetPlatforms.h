#import <TargetConditionals.h>

#if TARGET_OS_IOS
#import <UIKit/UIKit.h>
typedef UIViewController PlatformViewController;
#elif TARGET_OS_OSX
#import <AppKit/AppKit.h>
typedef NSViewController PlatformViewController;
#endif
