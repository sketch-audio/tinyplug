#import "SceneDelegate.h"
#import "ViewController.h"

#if TARGET_OS_IOS
@implementation SceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)connectionOptions {
    UIWindowScene *windowScene = (UIWindowScene *)scene;
    self.window = [[UIWindow alloc] initWithWindowScene:windowScene];
    self.window.rootViewController = [[ViewController alloc] init];
    [self.window makeKeyAndVisible];
}
@end
#endif
