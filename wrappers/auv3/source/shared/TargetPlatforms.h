#import <TargetConditionals.h>

#if TARGET_OS_IOS
#import <UIKit/UIKit.h>
typedef UIViewController PlatformViewController;
typedef UIView PlatformView;
#elif TARGET_OS_OSX
#import <AppKit/AppKit.h>
typedef NSViewController PlatformViewController;
typedef NSView PlatformView;
#endif
