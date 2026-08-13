#include <tiny_platform/platform_view.hpp>

#include <chrono>
#include <unordered_map>
#include <unordered_set>

#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>

#include <tiny_platform/window_context.hpp>

#if __has_feature(objc_arc)
static_assert(false, "This is a non-ARC file");
#endif

@interface IosView : UIView {
    std::shared_ptr<tiny::View_delegate> _delegate;
}
- (id)initWithDelegate:(std::shared_ptr<tiny::View_delegate>)delegate;
- (void)startDisplayLink; // Raw mechanism; overridden by IosMetalView.
- (void)stopDisplayLink;
- (void)resumeDisplayLink; // Visibility entry points — these track `_link_wanted`.
- (void)suspendDisplayLink;
@end

@implementation IosView {
    CADisplayLink* _displayLink;
    BOOL _link_wanted; // Editor is on screen; survives a background/foreground round trip.

    tiny::User_interaction _interaction;
    struct Pointer_data {
        std::optional<tiny::Coords> pos_last; // Latest position, for findClosest.
        bool ended;                           // Lifted/cancelled; swept in drawRect.
    };
    
    std::unordered_map<UITouch*, Pointer_data> _active_pointers;
    std::unordered_map<UITouch*, CGPoint> _activeTouches;
    tiny::Event_stream _events;
}

- (id)initWithDelegate:(std::shared_ptr<tiny::View_delegate>)delegate {
    _delegate = delegate;
    const auto size = _delegate->get_size();
    self = [super initWithFrame:CGRectMake(0, 0, size.w, size.h)];
    if (self) {
        //
        self.multipleTouchEnabled = YES;

        //
        UITapGestureRecognizer *singleTap = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(handleSingleTap:)];
        singleTap.delaysTouchesEnded = false;
        singleTap.cancelsTouchesInView = false;
        [self addGestureRecognizer:singleTap];
        [singleTap release];

        UITapGestureRecognizer *doubleTap = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(handleDoubleTap:)];
        doubleTap.numberOfTapsRequired = 2;
        doubleTap.delaysTouchesEnded = false;
        doubleTap.cancelsTouchesInView = false;
        [self addGestureRecognizer:doubleTap];
        [doubleTap release];

        UILongPressGestureRecognizer *longPress = [[UILongPressGestureRecognizer alloc] initWithTarget:self action:@selector(handleLongPress:)];
        longPress.delaysTouchesEnded = false;
        longPress.cancelsTouchesInView = false;
        [self addGestureRecognizer:longPress];
        [longPress release];

        // We need to stop the display link when the app/host gets backgrounded.
        auto* center = [NSNotificationCenter defaultCenter];
        for (NSNotificationName name in @[UIApplicationDidEnterBackgroundNotification,
                                          NSExtensionHostDidEnterBackgroundNotification]) {
            [center addObserver:self selector:@selector(appDidEnterBackground:) name:name object:nil];
        }
        for (NSNotificationName name in @[UIApplicationWillEnterForegroundNotification,
                                          NSExtensionHostWillEnterForegroundNotification]) {
            [center addObserver:self selector:@selector(appWillEnterForeground:) name:name object:nil];
        }
    }
    return self;
}

// MARK: - App lifecycle

- (void)appDidEnterBackground:(NSNotification *)note {
    [self stopDisplayLink]; // Keeps `_link_wanted` — the editor is still on screen.

    // Drop any drawable we're holding rather than carrying it across the boundary; it
    // belongs to a layer that is about to stop vending.
    if (_delegate) _delegate->set_drawable(nullptr);
}

- (void)appWillEnterForeground:(NSNotification *)note {
    if (_link_wanted) [self startDisplayLink];
}

// Visibility entry points from `Platform_view`. These own `_link_wanted`; the
// start/stop pair below is the raw mechanism and is overridden by `IosMetalView`.
- (void)resumeDisplayLink {
    _link_wanted = YES;
    [self startDisplayLink];
}

- (void)suspendDisplayLink {
    _link_wanted = NO;
    [self stopDisplayLink];
}

- (void)startDisplayLink {
    [self stopDisplayLink];
    _displayLink = [CADisplayLink displayLinkWithTarget:self selector:@selector(onDisplayLink:)];
    [_displayLink addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
}

- (void)stopDisplayLink {
    if (_displayLink) {
        [_displayLink invalidate];
        _displayLink = nil;
    }
}

- (void)dealloc {
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [self stopDisplayLink];
    [super dealloc];
}

- (void)onDisplayLink:(CADisplayLink *)sender {
    [self drawRect:{}]; // Invalidate the view.
}

- (void)drawRect:(CGRect)rect {
    const auto time_now = tiny::System_clock::now();
    
    _interaction.events = _events.consume(tiny::Steady_clock::now());
    _delegate->draw(_interaction, time_now);
    
    // I think we could move this to touchesEnded/touchesCancelled
    std::erase_if(_active_pointers, [](auto const& pair) { return pair.second.ended; });
}

- (UITouch*)findClosest:(tiny::Coords)loc {
    UITouch* closest_touch = nullptr;
    float closest_dist = std::numeric_limits<float>::max();
    for (const auto& [touch, pointer_data] : _active_pointers) {
        if (const auto last_pos = pointer_data.pos_last) {
            float dx = last_pos->x - loc.x;
            float dy = last_pos->y - loc.y;
            float dist = dx * dx + dy * dy;
            if (dist < closest_dist) {
                closest_dist = dist;
                closest_touch = touch;
            }
        }
    }
    return closest_touch;
}

- (void)handleSingleTap:(UITapGestureRecognizer *)gesture {
    using namespace tiny;

    if (gesture.state != UIGestureRecognizerStateEnded) return;

    // Use the aggregate location: at recognition the finger is already up, so
    // `numberOfTouches` is often 0 and iterating it drops the click entirely.
    const CGPoint location = [gesture locationInView:self];
    const Coords loc{location.x, location.y};
    UITouch* closest_touch = [self findClosest:loc];

    _events.push(Event{
        .event = Pointer_click{.count = 1, .pos = loc},
        .pointer_tag = (uintptr_t)closest_touch
    });
}

- (void)handleDoubleTap:(UITapGestureRecognizer *)gesture {
    using namespace tiny;

    if (gesture.state != UIGestureRecognizerStateEnded) return;

    const CGPoint location = [gesture locationInView:self];
    const Coords loc{location.x, location.y};
    UITouch* closest_touch = [self findClosest:loc];

    _events.push(Event{
        .event = Pointer_click{.count = 2, .pos = loc},
        .pointer_tag = (uintptr_t)closest_touch
    });
}

- (void)handleLongPress:(UILongPressGestureRecognizer *)gesture {
    using namespace tiny;

    if (gesture.state != UIGestureRecognizerStateBegan) return;

    // The finger is still down at `Began`, so the aggregate location is valid here too.
    // The long-press recognizer's own allowableMovement fails it on a real drag, so this
    // won't fire mid-drag — no need to gate it here.
    const CGPoint location = [gesture locationInView:self];
    const Coords loc{location.x, location.y};
    UITouch* closest_touch = [self findClosest:loc];

    _events.push(Event{
        .event = Pointer_down{.button = Pointer_button::right, .pos = loc},
        .pointer_tag = (uintptr_t)closest_touch
    });
}

// MARK: - touches

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [super touchesBegan:touches withEvent:event];
    
    using namespace tiny;
    for (UITouch* touch in touches) {
        CGPoint location = [touch locationInView:self];
        const auto tag = (uintptr_t)touch;
        const auto pos = Coords{location.x, location.y};
        _active_pointers[touch] = Pointer_data{
            .pos_last = pos,
        };
        _activeTouches[touch] = location;
        
        _events.push(Event{
            .event = Pointer_down{.pos = pos},
            .pointer_tag = tag
        });
    }
}

- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [super touchesMoved:touches withEvent:event];

    using namespace tiny;

    // Lossless (approach #2): the platform makes no tap-vs-drag decision. Every move is
    // forwarded from the first pixel, so drag surfaces get zero-slop, pixel-0 tracking.
    // Taps stay reliable because the tap recognizer only fires for genuine taps (its own
    // movement tolerance fails it on a real drag, so it never produces a spurious click),
    // and the click is emitted unconditionally by the handlers. Any tap/drag tie-break
    // now lives in the surface's own Drag_recognizer (per-surface slop), not here.
    for (UITouch* touch in touches) {
        const CGPoint location = [touch locationInView:self];
        const auto pos = Coords{location.x, location.y};
        _activeTouches[touch] = location;

        if (auto it = _active_pointers.find(touch); it != _active_pointers.end()) {
            it->second.pos_last = pos;
        }

        const auto tag = (uintptr_t)touch;
        _events.push(Event{
            .event = Pointer_move{.pos = pos},
            .pointer_tag = tag
        });
    }
}

- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [super touchesEnded:touches withEvent:event];

    using namespace tiny;
    for (UITouch* touch in touches) {
        CGPoint location = [touch locationInView:self];
        const auto pos = Coords{location.x, location.y};
        auto it = _active_pointers.find(touch);
        if (it != _active_pointers.end()) {
            auto& pointer_data = it->second;
            pointer_data.ended = true; // Can we remove the pointer here?
        }
        _activeTouches.erase(touch); // Gesture should be processed before touch end/cancel.
        
        const auto tag = (uintptr_t)touch;
        _events.push(Event{
            .event = Pointer_up{.pos = pos},
            .pointer_tag = tag
        });
    }
}

- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [super touchesCancelled:touches withEvent:event];

    using namespace tiny;
    for (UITouch* touch in touches) {
        auto it = _active_pointers.find(touch);
        if (it != _active_pointers.end()) {
            auto& pointer_data = it->second;
            pointer_data.ended = true;
        }
        _activeTouches.erase(touch); // Gesture should be processed before touch end/cancel.
        
        CGPoint location = [touch locationInView:self];
        const auto pos = Coords{location.x, location.y};
        const auto tag = (uintptr_t)touch;
        _events.push(Event{
            .event = Pointer_cancel{.pos = pos}, // ???
            .pointer_tag = tag
        });
    }
}

- (void)pressesChanged:(NSSet<UIPress *> *)presses withEvent:(UIPressesEvent *)event {
    UIKeyModifierFlags flags = event.modifierFlags;
    _interaction.modifier_keys = {
        .primary = (flags & UIKeyModifierCommand) != 0,
        .alt = (flags & UIKeyModifierAlternate) != 0,
        .shift = (flags & UIKeyModifierShift) != 0,
    };
}

@end

// MARK: - IosMetalView

API_AVAILABLE(ios(17.0))
@interface IosMetalView : IosView <CAMetalDisplayLinkDelegate>
@end

@implementation IosMetalView {
    CAMetalDisplayLink* _metalDisplayLink;
}

- (void)startDisplayLink {
    [self stopDisplayLink];
    
    auto metal_view = [[self subviews] firstObject]; // Assume the context set us up?
    if (!metal_view) return;
    _metalDisplayLink = [[CAMetalDisplayLink alloc] initWithMetalLayer:(CAMetalLayer*)metal_view.layer];
    _metalDisplayLink.delegate = self;
    [_metalDisplayLink addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
}

- (void)stopDisplayLink {
    if (_metalDisplayLink) {
        [_metalDisplayLink invalidate];
        [_metalDisplayLink release]; // ??
        _metalDisplayLink = nil;
    }
}

- (void)metalDisplayLink:(CAMetalDisplayLink *)link needsUpdate:(CAMetalDisplayLinkUpdate *)update {
    auto drawable = update.drawable;
    _delegate->set_drawable(drawable);
    [self drawRect:{}];
}

@end

// MARK: - Platform_view

namespace tiny {

Platform_view::Platform_view(std::shared_ptr<View_delegate> delegate, bool owns_view, std::function<void()>) : _delegate{delegate}, _owns_view{owns_view}
{
    UIView* view;
    
    if (@available(iOS 17, *)) {
        view = [[IosMetalView alloc] initWithDelegate:delegate];
    } else {
        view = [[IosView alloc] initWithDelegate:delegate];
    }

    auto context = std::make_unique<Window_context>();
    context->setup({.native_handle = static_cast<void*>(view)});
    _delegate->assign_context(std::move(context));

    _view = view;
}

Platform_view::~Platform_view() 
{
    if (_owns_view) {
        [(UIView*)_view removeFromSuperview];
        [(UIView*)_view release];
    }
    _view = nullptr;
}

auto Platform_view::on_create() -> void
{

}

auto Platform_view::on_show() -> void
{
    if (auto view = static_cast<IosView*>(_view)) {
        [view resumeDisplayLink];
    }
}

auto Platform_view::on_hide() -> void
{
    if (auto view = static_cast<IosView*>(_view)) {
        [view suspendDisplayLink];
    }
}

auto Platform_view::on_destroy() -> void
{
    _delegate->invalidate_context();
}

auto Platform_view::receive_parent(void* parent) -> void
{
    [(UIView*)parent addSubview:(UIView*)_view];
}

auto Platform_view::resize(int32_t w, int32_t h) -> void
{
    [(UIView*)_view setFrame:CGRectMake(0, 0, w, h)]; // So the context can get the size from the view.
    _delegate->on_resize({w, h});
}

} // namespace tiny
