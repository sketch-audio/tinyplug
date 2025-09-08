#import "TargetPlatforms.h"

#if TARGET_OS_IOS
@interface AppDelegate : UIResponder <UIApplicationDelegate>
@end
#elif TARGET_OS_OSX
@interface AppDelegate : NSObject <NSApplicationDelegate>
@property (strong, nonatomic) NSWindow *window;
@end
#endif
