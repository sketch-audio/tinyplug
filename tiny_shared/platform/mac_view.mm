#include "platform_view.h"

#if PLATFORM_MACOS
#import <Cocoa/Cocoa.h>
#import <CoreVideo/CVDisplayLink.h>

#include "../skia/mac/GaneshMetalWindowContext_mac.h"
#include "../skia/mac/MacWindowInfo.h"

#define MAC_VIEW_RENDER_MAIN_THREAD 1 // Still some issues with background thread for AUv2 in Logic

// MacView
@interface MacView : NSView
- (id)initWithDelegate:(std::shared_ptr<Graphics_delegate>)delegate;
- (void)startDisplayLink;
- (void)stopDisplayLink;
- (void)render;
@end

// 
static CVReturn DisplayLinkCallback(CVDisplayLinkRef displayLink, const CVTimeStamp* now, const CVTimeStamp* outputTime, CVOptionFlags flagsIn, CVOptionFlags* flagsOut, void* context) {
#if MAC_VIEW_RENDER_MAIN_THREAD
    dispatch_source_t source = (__bridge dispatch_source_t)context;
    dispatch_source_merge_data(source, 1);
#else
    MacView* view = (MacView*)context;
    [view render];
#endif
    return kCVReturnSuccess;
}

@implementation MacView {
    std::shared_ptr<Graphics_delegate> graphics_delegate;
    CVDisplayLinkRef _displayLink;
#if MAC_VIEW_RENDER_MAIN_THREAD
    dispatch_source_t _displaySource;
#endif
}

- (id)initWithDelegate:(std::shared_ptr<Graphics_delegate>)delegate {
    graphics_delegate = delegate;
    const auto size = delegate->getSize();
    self = [super initWithFrame:NSMakeRect(0, 0, size.width, size.height)];

    if (self) {
#if MAC_VIEW_RENDER_MAIN_THREAD
        _displaySource = dispatch_source_create(DISPATCH_SOURCE_TYPE_DATA_ADD, 0, 0, dispatch_get_main_queue());
        __block MacView* self_ = self; // Need block pointer!
        dispatch_source_set_event_handler(_displaySource, ^(){
            if (self_) {
                [self_ render];
            }
        });
        dispatch_resume(_displaySource);
#endif
        
        CVDisplayLinkCreateWithActiveCGDisplays(&_displayLink);

#if MAC_VIEW_RENDER_MAIN_THREAD
        CVDisplayLinkSetOutputCallback(_displayLink, DisplayLinkCallback, _displaySource);
#else
        CVDisplayLinkSetOutputCallback(_displayLink, DisplayLinkCallback, self);
#endif
        
        [self startDisplayLink];
    }
    return self;
}

- (void)dealloc {
    [self stopDisplayLink];
    CVDisplayLinkRelease(_displayLink);

#if MAC_VIEW_RENDER_MAIN_THREAD
    dispatch_source_cancel(_displaySource);
#endif

    [super dealloc];
}

- (void)startDisplayLink {
    CVDisplayLinkStart(_displayLink);
}

- (void)stopDisplayLink {
    if (_displayLink) {
        CVDisplayLinkStop(_displayLink);
    }
}

- (void)render {
    graphics_delegate->draw();
}

- (void)drawRect:(NSRect)dirtyRect {
    [super drawRect:dirtyRect];
    // CPU
    // if (graphics_delegate) {
    //     auto* cgContext = [NSGraphicsContext currentContext].CGContext;
    //     auto* platformContext = [NSGraphicsContext graphicsContextWithCGContext: cgContext flipped: YES].CGContext;
    //     graphics_delegate->draw(platformContext);
    // }
}
@end

Platform_view::Platform_view(std::shared_ptr<Graphics_delegate> delegate) : _delegate{delegate}
{
    NSView* view = [[MacView alloc] initWithDelegate:_delegate];

    auto window_info = skwindow::MacWindowInfo{view};
    auto display_params = std::make_unique<const skwindow::DisplayParams>();
    auto context = skwindow::MakeGaneshMetalForMac(window_info, std::move(display_params));
    _delegate->set_context(std::move(context));

    _view = view;
}

Platform_view::~Platform_view()
{
    [(NSView*)_view removeFromSuperview];
    [(NSView*)_view release];
    _view = nullptr;
}

auto Platform_view::receive_parent(void* parent) -> void
{
    [(NSView*)parent addSubview: (NSView*)_view];
}

auto Platform_view::resize(int32_t w, int32_t h) -> void
{
    auto size = _delegate->getSize();
    [(NSView*)_view setFrameSize:NSMakeSize(size.width, size.height)];
    //[(NSView*)_view setNeedsDisplay:YES]; // CPU
}

auto Platform_view::redraw() -> void
{
    //[(NSView*)_view setNeedsDisplay:YES]; // CPU
}
#endif