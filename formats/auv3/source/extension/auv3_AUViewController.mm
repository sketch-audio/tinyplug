#import <CoreAudioKit/AUViewController.h>

#import "TargetPlatforms.h"

#import "auv3_AUAudioUnit.h"

#include "auv3_view.h"

@interface Auv3_AUViewController : AUViewController <AUAudioUnitFactory>
@property (nonatomic, retain) AUAudioUnit* audioUnit;
@end

@implementation Auv3_AUViewController {
    std::unique_ptr<tiny::Auv3_view> _view_adapter;
}

// TODO: - Get this into the plist for AUM.
- (CGSize)preferredContentSize {
    const auto size = tiny::Custom_view::preferred_size();
    return CGSizeMake(size.w, size.h);
}

- (AUAudioUnit*)createAudioUnitWithComponentDescription:(AudioComponentDescription) desc error:(NSError **)error {
    self.audioUnit = [[Auv3_AUAudioUnit alloc] initWithComponentDescription:desc error:error];

    Auv3_AUAudioUnit* tiny_au = (Auv3_AUAudioUnit*)self.audioUnit;
    [tiny_au setupParameterTree];
    
    dispatch_async(dispatch_get_main_queue(), ^{
        [self setupViewAdapter];
    });

    return self.audioUnit;
}

// main thread
- (void)setupViewAdapter {
    if (!self.audioUnit) return;
    if (!_view_adapter) {
        Auv3_AUAudioUnit* tiny_au = (Auv3_AUAudioUnit*)self.audioUnit;
        auto receiver = [tiny_au makeReceiver];
        _view_adapter = std::make_unique<tiny::Auv3_view>(receiver);
        auto* custom_view = (__bridge PlatformView*)_view_adapter->create_view();
        [self.view addSubview:custom_view];
        const auto size = self.view.bounds.size;
        _view_adapter->on_resize(static_cast<int32_t>(size.width), static_cast<int32_t>(size.height)); // Also requests a redraw.
    }
}

- (void)viewDidLoad {
    [super viewDidLoad];
    [self setupViewAdapter]; //
}

#if TARGET_OS_IOS
- (void)viewWillLayoutSubviews {
    [super viewWillLayoutSubviews];
    if (!_view_adapter) return;
    const auto size = self.view.bounds.size;
    _view_adapter->on_resize(static_cast<int32_t>(size.width), static_cast<int32_t>(size.height)); // Also requests a redraw.
}
#elif TARGET_OS_OSX
- (void)viewWillLayout {
    [super viewWillLayout];
    if (!_view_adapter) return;
    const auto size = self.view.bounds.size;
    _view_adapter->on_resize(static_cast<int32_t>(size.width), static_cast<int32_t>(size.height)); // Also requests a redraw.
}
#endif

- (void)dealloc {
    _view_adapter->teardown(); // Stop the timer!
    //[super dealloc];
}
@end
