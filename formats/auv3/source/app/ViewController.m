#import "ViewController.h"

@implementation ViewController

#if TARGET_OS_IOS
- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor systemBackgroundColor];

    // Add label to center of screen
    UILabel *label = [[UILabel alloc] init];
    label.text = @"This is a simple app that embeds your AUv3 plug-in.";
    label.font = [UIFont systemFontOfSize:17];
    label.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:label];

    [NSLayoutConstraint activateConstraints:@[
        [label.centerXAnchor constraintEqualToAnchor:self.view.centerXAnchor],
        [label.centerYAnchor constraintEqualToAnchor:self.view.centerYAnchor]
    ]];
}
#elif TARGET_OS_OSX
- (void)loadView {
    self.view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 600, 400)];
    self.view.wantsLayer = YES;
    self.view.layer.backgroundColor = [[NSColor windowBackgroundColor] CGColor];

    // Add label to center of screen
    NSTextField *label = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 0, 0)];
    label.stringValue = @"This is a simple app that embeds your AUv3 plug-in.";
    label.font = [NSFont systemFontOfSize:17];
    label.alignment = NSTextAlignmentCenter;
    label.bezeled = NO;
    label.drawsBackground = NO;
    label.editable = NO;
    label.selectable = NO;
    label.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:label];

    [NSLayoutConstraint activateConstraints:@[
        [label.centerXAnchor constraintEqualToAnchor:self.view.centerXAnchor],
        [label.centerYAnchor constraintEqualToAnchor:self.view.centerYAnchor]
    ]];
}
#endif

@end
