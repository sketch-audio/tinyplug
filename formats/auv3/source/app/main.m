#import "AppDelegate.h"

#if TARGET_OS_IOS
int main(int argc, char * argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([AppDelegate class]));
    }
}
#elif TARGET_OS_OSX
int main(int argc, const char * argv[]) {
    @autoreleasepool {
        AppDelegate *delegate = [[AppDelegate alloc] init];
        [NSApplication sharedApplication].delegate = delegate;
        return NSApplicationMain(argc, argv);
    }
}
#endif
