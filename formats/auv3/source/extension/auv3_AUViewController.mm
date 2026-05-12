#import <CoreAudioKit/AUViewController.h>

#import "TargetPlatforms.h"

#import "auv3_AUAudioUnit.h"

#include "auv3_view.h"
#include "plug_editor.h"

#if !__has_feature(objc_arc)
static_assert(false, "ARC must be enabled for this file");
#endif

@interface Auv3_AUViewController : AUViewController <AUAudioUnitFactory>
@property (nonatomic, retain) AUAudioUnit* audioUnit;

// Use this to attach an externally-created AU (e.g. one already wrapped in
// an AVAudioUnit and inserted in an AVAudioEngine). Handles the same editor +
// parameter-tree wiring that createAudioUnitWithComponentDescription: does.
- (void)bindToAudioUnit:(AUAudioUnit *)au;
@end

@implementation Auv3_AUViewController {
    std::unique_ptr<tiny::Auv3_view> _view_adapter;
    std::shared_ptr<tiny::Plug_editor> _editor;
    tiny::Task_manager _tasks;
}

// TODO: - Get this into the plist for AUM.
- (CGSize)preferredContentSize {
    const auto size = tiny::Plug_editor::preferred_size();
    return CGSizeMake(size.w, size.h);
}

- (AUAudioUnit*)createAudioUnitWithComponentDescription:(AudioComponentDescription) desc error:(NSError **)error {
    AUAudioUnit *au = [[Auv3_AUAudioUnit alloc] initWithComponentDescription:desc error:error];
    if (!au) return nil;
    [self bindToAudioUnit:au];
    return au;
}

- (void)bindToAudioUnit:(AUAudioUnit *)au {
    if (!au) return;
    self.audioUnit = au;

    if (!_editor) {
        _editor = std::make_shared<tiny::Plug_editor>(_tasks.actor());
    }
    Auv3_AUAudioUnit* auv3 = (Auv3_AUAudioUnit*)au;
    [auv3 setupParameterTree];
    [auv3 setEditor:_editor];

    dispatch_async(dispatch_get_main_queue(), ^{
        [self setupViewAdapter];
    });
}

// main thread
- (void)setupViewAdapter {
    if (!self.audioUnit) return;
    if (!_view_adapter) {
        Auv3_AUAudioUnit* auv3 = (Auv3_AUAudioUnit*)self.audioUnit;
        auto receiver = [auv3 makeReceiver];
        _view_adapter = std::make_unique<tiny::Auv3_view>(tiny::Auv3_view::Deps{_editor.get(), receiver, &_tasks});
        auto* custom_view = (__bridge PlatformView*)_view_adapter->create_view(); // also on_create
        [self.view addSubview:custom_view];
        const auto size = self.view.bounds.size;
        _view_adapter->on_resize(static_cast<int32_t>(size.width), static_cast<int32_t>(size.height));
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

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    if (!_view_adapter) return;
    _view_adapter->on_show();
}

- (void)viewWillDisappear:(BOOL)animated {
    [super viewWillDisappear:animated];
    if (!_view_adapter) return;
    _view_adapter->on_hide();
}
#elif TARGET_OS_OSX
- (void)viewWillLayout {
    [super viewWillLayout];
    if (!_view_adapter) return;
    const auto size = self.view.bounds.size;
    _view_adapter->on_resize(static_cast<int32_t>(size.width), static_cast<int32_t>(size.height)); // Also requests a redraw.
}

- (void)viewWillAppear {
    [super viewWillAppear];
    if (!_view_adapter) return;
    _view_adapter->on_show();
}

- (void)viewWillDisappear {
    [super viewWillDisappear];
    if (!_view_adapter) return;
    _view_adapter->on_hide();
}

#endif

- (void)dealloc {
    if (!_view_adapter) return;
    _view_adapter->on_destroy();
    _view_adapter = nullptr;
    //[super dealloc];
}
@end
