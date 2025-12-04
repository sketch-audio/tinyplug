#include "../platform_view.h"

#include <chrono>
#include <unordered_map>
#include <unordered_set>

#import <UIKit/UIKit.h>

#include "../window_context.h"

#if __has_feature(objc_arc)
static_assert(false, "This is a non-ARC file");
#endif

@interface IosView : UIView
- (id)initWithDelegate:(std::shared_ptr<tiny::View_delegate>)delegate;
- (void)startDisplayLink;
- (void)stopDisplayLink;
@end

@implementation IosView {
    std::shared_ptr<tiny::View_delegate> _delegate;
    CADisplayLink* _displayLink;
    
    tiny::User_interaction _interaction;
    struct Pointer_data {
        std::optional<tiny::Coords> pos_down;
        std::optional<tiny::Coords> pos_last;
        bool pending_gesture;
        bool started_drag;
        bool ended;
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
    }
    return self;
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
    
    for (auto& [touch, pointer_data] : _active_pointers) {
        pointer_data.pending_gesture = false;
    }
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
    
    if (gesture.state == UIGestureRecognizerStateEnded) {
        NSUInteger touches = [gesture numberOfTouches];
        for (NSUInteger i = 0; i < touches; ++i) {
            CGPoint location = [gesture locationOfTouch:i inView:self];
            Coords loc{location.x, location.y};
            UITouch* closest_touch = [self findClosest:loc];
            if (closest_touch) {
                auto it = _active_pointers.find(closest_touch);
                if (it != _active_pointers.end()) {
                    auto& pointer_data = it->second;
                    pointer_data.pending_gesture = true;
                    pointer_data.pos_down = std::nullopt;
                    pointer_data.started_drag = false;
                    
                    _events.push(Event{
                        .event = Pointer_click{.count = 1, .pos = loc},
                        .pointer_tag = (uintptr_t)closest_touch
                    });
                }
            }
        }
    }
}

- (void)handleDoubleTap:(UITapGestureRecognizer *)gesture {
    using namespace tiny;
    
    if (gesture.state == UIGestureRecognizerStateEnded) {
        NSUInteger touches = [gesture numberOfTouches];
        for (NSUInteger i = 0; i < touches; ++i) {
            CGPoint location = [gesture locationOfTouch:i inView:self];
            Coords loc{location.x, location.y};
            UITouch* closest_touch = [self findClosest:loc];
            if (closest_touch) {
                auto it = _active_pointers.find(closest_touch);
                if (it != _active_pointers.end()) {
                    auto& pointer_data = it->second;
                    pointer_data.pending_gesture = true;
                    pointer_data.pos_down = std::nullopt;
                    pointer_data.started_drag = false;
                    
                    _events.push(Event{
                        .event = Pointer_click{.count = 2, .pos = loc},
                        .pointer_tag = (uintptr_t)closest_touch
                    });
                }
            }
        }
    }
}

- (void)handleLongPress:(UILongPressGestureRecognizer *)gesture {
    using namespace tiny;
    
    if (gesture.state == UIGestureRecognizerStateBegan) {
        NSUInteger touches = [gesture numberOfTouches];
        for (NSUInteger i = 0; i < touches; ++i) {
            CGPoint location = [gesture locationOfTouch:i inView:self];
            Coords loc{location.x, location.y};
            UITouch* closest_touch = [self findClosest:loc];
            if (closest_touch) {
                auto it = _active_pointers.find(closest_touch);
                if (it != _active_pointers.end()) {
                    auto& pointer_data = it->second;
                    pointer_data.pending_gesture = true;
                    pointer_data.pos_down = std::nullopt;
                    pointer_data.started_drag = false;
                    
                    _events.push(Event{
                        .event = Pointer_down{.button = Pointer_button::right, .pos = loc},
                        .pointer_tag = (uintptr_t)closest_touch
                    });
                }
            }
        }
    }
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
            .pos_down = pos,
            .pos_last = pos,
            .pending_gesture = false,
            .started_drag = false,
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
    for (UITouch* touch in touches) {
        CGPoint location = [touch locationInView:self];
        const auto pos = Coords{location.x, location.y};
        auto it = _active_pointers.find(touch);
        if (it != _active_pointers.end()) {
            auto& pointer_data = it->second;
            if (const auto down_pos = pointer_data.pos_down; down_pos && pointer_data.started_drag) {
                pointer_data.pos_last = pos;
            }
            else if (const auto down_pos = pointer_data.pos_down){
                pointer_data.pos_last = pos;
                pointer_data.started_drag = true;
            }
        }
        _activeTouches[touch] = location;
        
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

// MARK: - Platform_view

namespace tiny {

Platform_view::Platform_view(std::shared_ptr<View_delegate> delegate, bool owns_view, std::function<void()>) : _delegate{delegate}, _owns_view{owns_view}
{
    UIView* view = [[IosView alloc] initWithDelegate:delegate];

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
        [view startDisplayLink];
    }
}

auto Platform_view::on_hide() -> void
{
    if (auto view = static_cast<IosView*>(_view)) {
        [view stopDisplayLink];
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
