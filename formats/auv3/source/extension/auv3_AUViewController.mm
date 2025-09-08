#import <CoreAudioKit/AUViewController.h>

#import "TargetPlatforms.h"

#import "auv3_AUAudioUnit.h"

@interface Auv3_AUViewController : AUViewController <AUAudioUnitFactory>
@property (nonatomic, retain) AUAudioUnit* audioUnit;
@end

@implementation Auv3_AUViewController
- (AUAudioUnit*)createAudioUnitWithComponentDescription:(AudioComponentDescription) desc error:(NSError **)error {
    self.audioUnit = [[Auv3_AUAudioUnit alloc] initWithComponentDescription:desc error:error];

    Auv3_AUAudioUnit* tiny_au = (Auv3_AUAudioUnit*)self.audioUnit;
    [tiny_au setupParameterTree];

    return self.audioUnit;
}

- (void)viewDidLoad {
    [super viewDidLoad];
#if TARGET_OS_IOS
    self.view.backgroundColor = [UIColor systemBackgroundColor];

    // Add label to center of screen
    UILabel *label = [[UILabel alloc] init];
    label.text = @"Hello from tinyplug AUv3.";
    label.font = [UIFont systemFontOfSize:17];
    label.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:label];

    [NSLayoutConstraint activateConstraints:@[
        [label.centerXAnchor constraintEqualToAnchor:self.view.centerXAnchor],
        [label.centerYAnchor constraintEqualToAnchor:self.view.centerYAnchor]
    ]];
#elif TARGET_OS_OSX
    // Add label to center of screen
    NSTextField *label = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 0, 0)];
    label.stringValue = @"Hello from tinyplug AUv3.";
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
#endif
}
@end

