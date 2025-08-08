#include "platform_view.h"

#if PLATFORM_MACOS
#include <chrono>

#import <Cocoa/Cocoa.h>
#import <CoreVideo/CVDisplayLink.h>

#include "../skia/mac/GaneshMetalWindowContext_mac.h"
#include "../skia/mac/MacWindowInfo.h"

#define MAC_VIEW_RENDER_MAIN_THREAD 1 // Still some issues with background thread for AUv2 in Logic? I think I actually fixed this...

// MacView
@interface MacView : NSView
- (id)initWithDelegate:(std::shared_ptr<tiny::View_delegate>)delegate;
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
    std::shared_ptr<tiny::View_delegate> graphics_delegate;

    // Interaction state
    tiny::User_interaction _interaction;
    std::chrono::steady_clock::time_point _over_time;
    std::optional<tiny::Coords> _over_pos;
    std::optional<tiny::Coords> _left_pos;
    std::optional<tiny::Coords> _right_pos;
    std::optional<tiny::Coords> _drag_start;
    bool _dwelt;

    CVDisplayLinkRef _displayLink;
#if MAC_VIEW_RENDER_MAIN_THREAD
    dispatch_source_t _displaySource;
#endif
}

- (id)initWithDelegate:(std::shared_ptr<tiny::View_delegate>)delegate {
    graphics_delegate = delegate;
    const auto size = delegate->get_size();
    self = [super initWithFrame:NSMakeRect(0, 0, size.w, size.h)];

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
    // Should we dwell?
    if (_over_pos && !_dwelt) {
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - _over_time);
        if (elapsed.count() > 2000) {
            _dwelt = tiny::try_set(_interaction.state, tiny::Dwell{*_over_pos});
        }
    }

    graphics_delegate->draw(_interaction);
    tiny::try_set(_interaction.state, tiny::Consumed{});

    if (_dwelt) {
        _over_pos = std::nullopt;
        _dwelt = false;
    }
}

- (void)updateTrackingAreas {
    [super updateTrackingAreas];

    if (self.trackingAreas.count > 0) {
        for (NSTrackingArea *area in self.trackingAreas) {
            [self removeTrackingArea:area];
        }
    }

    NSTrackingAreaOptions options = NSTrackingMouseEnteredAndExited | NSTrackingMouseMoved | NSTrackingActiveInKeyWindow;
    NSTrackingArea *area = [[NSTrackingArea alloc] initWithRect:self.bounds
                                                        options:options
                                                          owner:self
                                                       userInfo:nil];
    [self addTrackingArea:area];
}

- (void)mouseDown:(NSEvent *)event {
    _left_pos = tiny::Coords{event.locationInWindow.x, event.locationInWindow.y};
    [super mouseDown:event];
}

- (void)mouseUp:(NSEvent *)event {
    const auto pos = tiny::Coords{event.locationInWindow.x, event.locationInWindow.y};
    if (_drag_start) {
        tiny::try_set(_interaction.state, tiny::Drag_end{*_drag_start, pos});
        _over_pos = std::nullopt;
        _drag_start = std::nullopt;
    } 
    else if (_left_pos) {
        if (event.clickCount == 2) {
            tiny::try_set(_interaction.state, tiny::Double_click{pos});
            _over_pos = std::nullopt;
        } 
        else {
            tiny::try_set(_interaction.state, tiny::Click{pos});
            _over_pos = std::nullopt;
        }
        _left_pos = std::nullopt;
    }
    [super mouseUp:event];
}

- (void)mouseMoved:(NSEvent *)event {
    const auto pos = tiny::Coords{event.locationInWindow.x, event.locationInWindow.y};
    tiny::try_set(_interaction.state, tiny::Over{pos});

    // Update dwell.
    if (!_over_pos || *_over_pos != pos) {
        _over_pos = pos;
        _over_time = std::chrono::steady_clock::now();
        _dwelt = false;
    }
    [super mouseMoved:event];
}

- (void)mouseDragged:(NSEvent *)event {
    const auto pos = tiny::Coords{event.locationInWindow.x, event.locationInWindow.y};
    if (_left_pos && !_drag_start) {
        _drag_start = *_left_pos;
        _left_pos = std::nullopt;
        tiny::try_set(_interaction.state, tiny::Drag_start{*_drag_start, pos});
        _over_pos = std::nullopt;
    } 
    else if (_drag_start) {
        tiny::try_set(_interaction.state, tiny::Drag{*_drag_start, pos});
        _over_pos = std::nullopt;
    }
    [super mouseDragged:event];
}

- (void)rightMouseDown:(NSEvent *)event {
    _right_pos = tiny::Coords{event.locationInWindow.x, event.locationInWindow.y};
    [super rightMouseDown:event];
}

- (void)rightMouseUp:(NSEvent *)event {
    if (_right_pos) {
        tiny::try_set(_interaction.state, tiny::Right_click{*_right_pos});
        _over_pos = std::nullopt;
        _right_pos = std::nullopt;
    }
    [super rightMouseUp:event];
}

- (void)rightMouseDragged:(NSEvent *)event {
    _right_pos = std::nullopt;
    [super rightMouseDragged:event];
}

- (void)scrollWheel:(NSEvent *)event {
    const auto deltas = tiny::Coords{event.scrollingDeltaX, event.scrollingDeltaY};
    _interaction.scroll_deltas = deltas;
    [super scrollWheel:event];
}

- (void)mouseEntered:(NSEvent *)event {
    const auto pos = tiny::Coords{event.locationInWindow.x, event.locationInWindow.y};
    tiny::try_set(_interaction.state, tiny::Over{pos});
    _over_pos = std::nullopt;
    [super mouseEntered:event];
}

- (void)mouseExited:(NSEvent *)event {
    tiny::try_set(_interaction.state, tiny::Consumed{});
    _over_pos = std::nullopt;
    [super mouseExited:event];
}

@end

namespace tiny {

// MARK: - Platform_view

Platform_view::Platform_view(std::shared_ptr<View_delegate> delegate, bool owns_view) : _delegate{delegate}, _owns_view{owns_view}
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
    // The AUv2 view will have been autoreleased by now.
    if (_owns_view) {
        [(NSView*)_view removeFromSuperview];
        [(NSView*)_view release];
    }
    _view = nullptr;
}

auto Platform_view::receive_parent(void* parent) -> void
{
    [(NSView*)parent addSubview: (NSView*)_view];
}

auto Platform_view::resize(int32_t w, int32_t h) -> void
{
    _delegate->on_resize({w, h});
    [(NSView*)_view setFrameSize:NSMakeSize(w, h)];
}

auto Platform_view::redraw() -> void
{
}

} // namespace tiny

#endif