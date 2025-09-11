#include "platform_view.h"

#if PLATFORM_IOS
#include <chrono>
#include <unordered_map>

#import <UIKit/UIKit.h>

#define SK_METAL
#include "ios/WindowContextFactory_ios.h" // tiny_deps
#undef SK_METAL

@interface IosView : UIView
- (id)initWithDelegate:(std::shared_ptr<tiny::View_delegate>)delegate;
- (void)teardown;
@end

@implementation IosView {
    std::shared_ptr<tiny::View_delegate> _delegate;
    CADisplayLink* _displayLink;

    // Interaction state
    tiny::User_interaction _interaction;
    std::optional<tiny::Coords> _touch_down;
    bool _pending_gesture;
    bool _started_drag;
    
    struct Tagged_interaction {
        tiny::User_interaction interaction;
        std::optional<tiny::Coords> pos_down;
        std::optional<tiny::Coords> pos_last;
        bool pending_gesture;
        bool started_drag;
    };
    std::unordered_map<UITouch*, Tagged_interaction> _interactions;

    std::unordered_map<UITouch*, CGPoint> _activeTouches;
}

- (id)initWithDelegate:(std::shared_ptr<tiny::View_delegate>)delegate {
    _delegate = delegate;
    const auto size = _delegate->get_size();
    self = [super initWithFrame:CGRectMake(0, 0, size.w, size.h)];
    if (self) {
        _displayLink = [CADisplayLink displayLinkWithTarget:self selector:@selector(onDisplayLink:)];
        [_displayLink addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];

        //
        UITapGestureRecognizer *singleTap = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(handleSingleTap:)];
        singleTap.delaysTouchesEnded = false;
        singleTap.cancelsTouchesInView = false;
        [self addGestureRecognizer:singleTap];

        UITapGestureRecognizer *doubleTap = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(handleDoubleTap:)];
        doubleTap.numberOfTapsRequired = 2;
        doubleTap.delaysTouchesEnded = false;
        doubleTap.cancelsTouchesInView = false;
        [self addGestureRecognizer:doubleTap];

        UILongPressGestureRecognizer *longPress = [[UILongPressGestureRecognizer alloc] initWithTarget:self action:@selector(handleLongPress:)];
        longPress.delaysTouchesEnded = false;
        longPress.cancelsTouchesInView = false;
        [self addGestureRecognizer:longPress];
    }
    return self;
}

- (void)teardown {
    [_displayLink invalidate];
    _displayLink = nil;
}

- (void)dealloc {
    [self teardown];
    [super dealloc];
}

- (void)onDisplayLink:(CADisplayLink *)sender {
    [self drawRect:{}];
}

- (void)drawRect:(CGRect)rect {
    const auto time_now = tiny::System_clock::now();
    _delegate->draw(_interaction, time_now, false);
    tiny::try_set(_interaction.state, tiny::Consumed{});
    _pending_gesture = false;
}

- (void)handleSingleTap:(UITapGestureRecognizer *)gesture {
    if (gesture.state == UIGestureRecognizerStateEnded) {
        // for touch in gesture touches (have to go by index and get location)
        // find closest tagged interaction
        // try set that interaction
        // etc.
        // TODO: -
        
        CGPoint location = [gesture locationInView:self];
        tiny::try_set(_interaction.state, tiny::Click{{location.x, location.y}});
        _pending_gesture = true;
        _touch_down = std::nullopt;
        _started_drag = false;
        
        // get number of touches
        NSUInteger touches = [gesture numberOfTouches];
        for (NSUInteger i = 0; i < touches; ++i) {
            CGPoint location = [gesture locationOfTouch:i inView:self];
            // log the location
            NSLog(@"Single tap touch %lu at (%f, %f)", (unsigned long)i, location.x, location.y);
        }

        // log current touch map:
        for (auto const& [touch, point] : _activeTouches) {
            NSLog(@"Active touch at (%f, %f)", point.x, point.y);
        }
    }
}

- (void)handleDoubleTap:(UITapGestureRecognizer *)gesture {
    if (gesture.state == UIGestureRecognizerStateEnded) {
        // for touch in gesture touches (have to go by index and get location)
        // find closest tagged interaction
        // try set that interaction
        // etc.
        // TODO: -
        
        CGPoint location = [gesture locationInView:self];
        tiny::try_set(_interaction.state, tiny::Double_click{{location.x, location.y}});
        _pending_gesture = true;
        _touch_down = std::nullopt;
        _started_drag = false;
        
        // get number of touches
        NSUInteger touches = [gesture numberOfTouches];
        for (NSUInteger i = 0; i < touches; ++i) {
            CGPoint location = [gesture locationOfTouch:i inView:self];
            // log the location
            NSLog(@"Double tap touch %lu at (%f, %f)", (unsigned long)i, location.x, location.y);
        }

        // log current touch map:
        for (auto const& [touch, point] : _activeTouches) {
            NSLog(@"Active touch at (%f, %f)", point.x, point.y);
        }
    }
}

- (void)handleLongPress:(UILongPressGestureRecognizer *)gesture {
    if (gesture.state == UIGestureRecognizerStateBegan) {
        // for touch in gesture touches (have to go by index and get location)
        // find closest tagged interaction
        // try set that interaction
        // etc.
        // TODO: -
        
        CGPoint location = [gesture locationInView:self];
        tiny::try_set(_interaction.state, tiny::Right_click{{location.x, location.y}});
        _pending_gesture = true;
        _touch_down = std::nullopt;
        _started_drag = false;
        
        // get number of touches
        NSUInteger touches = [gesture numberOfTouches];
        for (NSUInteger i = 0; i < touches; ++i) {
            CGPoint location = [gesture locationOfTouch:i inView:self];
            // log the location
            NSLog(@"Long press touch %lu at (%f, %f)", (unsigned long)i, location.x, location.y);
        }

        // log current touch map:
        for (auto const& [touch, point] : _activeTouches) {
            NSLog(@"Active touch at (%f, %f)", point.x, point.y);
        }
    }
}

// MARK: - touches

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [super touchesBegan:touches withEvent:event];
    
    // for touch in touches
    // insert new tagged interaction with state down
//    for (UITouch* touch in touches) {
//        CGPoint location = [touch locationInView:self];
//        const auto pos = tiny::Coords{location.x, location.y};
//        
//        _interactions[touch] = Tagged_interaction{
//            .interaction = {.state = tiny::Down{pos}},
//            .pos_down = pos,
//            .pos_last = pos,
//            .pending_gesture = false,
//            .started_drag = false,
//        };
//    }
    
    UITouch* touch = [touches anyObject];
    CGPoint location = [touch locationInView:self];
    const auto pos = tiny::Coords{location.x, location.y};
    tiny::try_set(_interaction.state, tiny::Down{pos});
    _touch_down = pos;
    _started_drag = false;

    for (UITouch* t in touches) {
        CGPoint loc = [t locationInView:self];
        _activeTouches[t] = loc;
    }
    
    NSLog(@"touches began:");
}

- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [super touchesMoved:touches withEvent:event];
    
    // for touch in touches, find the tagged interaction and try_set the state.
//    for (UITouch* touch in touches) {
//        CGPoint location = [touch locationInView:self];
//        const auto pos = tiny::Coords{location.x, location.y};
//        auto it = _interactions.find(touch);
//        if (it != _interactions.end()) {
//            auto& tagged = it->second;
//            if (const auto down_pos = tagged.pos_down; down_pos && tagged.started_drag) {
//                tiny::try_set(tagged.interaction.state, tiny::Drag{.fpos = *down_pos, .tpos = pos});
//                tagged.pos_last = pos;
//            }
//            else if (const auto down_pos = tagged.pos_down){
//                tiny::try_set(tagged.interaction.state, tiny::Drag_start{.fpos = *down_pos, .tpos = pos});
//                tagged.pos_last = pos;
//                tagged.started_drag = true;
//            }
//        }
//    }
    
    UITouch* touch = [touches anyObject];
    CGPoint location = [touch locationInView:self];
    const auto pos = tiny::Coords{location.x, location.y};
    if (const auto down_pos = _touch_down; down_pos && _started_drag) {
        tiny::try_set(_interaction.state, tiny::Drag{.fpos = *down_pos, .tpos = pos});
    }
    else if (const auto down_pos = _touch_down){
        tiny::try_set(_interaction.state, tiny::Drag_start{.fpos = *down_pos, .tpos = pos});
        _started_drag = true;
    }

    for (UITouch* t in touches) {
        CGPoint loc = [t locationInView:self];
        _activeTouches[t] = loc;
    }
    
    NSLog(@"touches moved:");
}

- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [super touchesEnded:touches withEvent:event];

    // for touch in touches, find the tagged interaction and try_set the state.
//    for (UITouch* touch in touches) {
//        CGPoint location = [touch locationInView:self];
//        const auto pos = tiny::Coords{location.x, location.y};
//        auto it = _interactions.find(touch);
//        if (it != _interactions.end()) {
//            auto& tagged = it->second;
//            if (const auto down_pos = tagged.pos_down; down_pos && tagged.started_drag) {
//                tiny::try_set(tagged.interaction.state, tiny::Drag_end{.fpos = *down_pos, .tpos = pos});
//            }
//            else if (!tagged.pending_gesture) {
//                tiny::try_set(tagged.interaction.state, tiny::Consumed{});
//            }
//            //_interactions.erase(it); // Don't dispose we have to process it!
//        }
//    }

    UITouch* touch = [touches anyObject];
    CGPoint location = [touch locationInView:self];
    const auto pos = tiny::Coords{location.x, location.y};
    if (const auto down_pos = _touch_down; down_pos && _started_drag) {
        tiny::try_set(_interaction.state, tiny::Drag_end{.fpos = *down_pos, .tpos = pos});
    }
    else if (!_pending_gesture) {
        tiny::try_set(_interaction.state, tiny::Consumed{});
    }
    _touch_down = std::nullopt;
    _started_drag = false;

    for (UITouch* t in touches) {
        _activeTouches.erase(t);
    }
    
    NSLog(@"touches ended:");
}

- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [super touchesCancelled:touches withEvent:event];

    // for touch in touches, find the tagged interaction and try_set the state to consumed.
//    for (UITouch* touch in touches) {
//        auto it = _interactions.find(touch);
//        if (it != _interactions.end()) {
//            auto& tagged = it->second;
//            tiny::try_set(tagged.interaction.state, tiny::Consumed{});
//            //_interactions.erase(it); // Don't dispose we have to process it!
//        }
//    }
    
    tiny::try_set(_interaction.state, tiny::Consumed{});
    _touch_down = std::nullopt;
    _started_drag = false;

    for (UITouch* t in touches) {
        _activeTouches.erase(t);
    }
    
    NSLog(@"touches cancelled:");
}

@end

// MARK: - Platform_view

namespace tiny {

Platform_view::Platform_view(std::shared_ptr<View_delegate> delegate, bool owns_view) : _delegate{delegate}, _owns_view{owns_view} {
    UIView* view = [[IosView alloc] initWithDelegate:delegate];

    auto window_info = skwindow::IOSWindowInfo{
        .fView = view,
    };
    auto display_params = std::make_unique<const skwindow::DisplayParams>();
    auto context = skwindow::MakeMetalForIOS(window_info, std::move(display_params));
    _delegate->set_context(std::move(context));

    _view = view;
}

Platform_view::~Platform_view() {
    if (_owns_view) {
        [(UIView*)_view removeFromSuperview];
        [(UIView*)_view release];
    }
    _view = nullptr;
}

auto Platform_view::receive_parent(void* parent) -> void
{
    [(UIView*)parent addSubview:(UIView*)_view];
}

auto Platform_view::teardown() -> void
{
    [(IosView*)_view teardown]; // Stop timer
}

auto Platform_view::resize(int32_t w, int32_t h) -> void
{
    [(UIView*)_view setFrame:CGRectMake(0, 0, w, h)]; // So the context can get the size from the view.
    _delegate->on_resize({w, h});
}

auto Platform_view::redraw() -> void
{
    [(UIView*)_view setNeedsDisplay];
}

// MARK: - alert

} // namespace tiny
#endif
