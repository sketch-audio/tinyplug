#include <tiny_platform/platform_view.hpp>

#include "window_registry.hpp"

#include <algorithm>
#include <chrono>
#include <unordered_map>
#include <unordered_set>

#import <CoreText/CoreText.h>
#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>

#include <tiny_platform/platform_text.hpp>
#include <tiny_platform/window_context.hpp>

#include "ios_config.hpp"

#if __has_feature(objc_arc)
static_assert(false, "This is a non-ARC file");
#endif

namespace tiny {

// A plug-in editor gains nothing from ProMotion's 120 Hz — the extra frames are pure
// render load. Applies to both backends.
inline constexpr auto max_fps = 60;

// Back off as the device heats; a throttled GPU cannot deliver the frames anyway. Unity
// pair the same policy with CAMetalDisplayLink — the freeze they traced began once the
// device reached a critical thermal state.
inline auto fps_for_thermal(NSProcessInfoThermalState state) -> int
{
    switch (state) {
        case NSProcessInfoThermalStateNominal:  return max_fps;
        case NSProcessInfoThermalStateFair:     return max_fps;
        case NSProcessInfoThermalStateSerious:  return 30;
        case NSProcessInfoThermalStateCritical: return 20;
        default:                                return max_fps;
    }
}

}

// MARK: - Text field

namespace {

auto ui_color(std::uint32_t argb) -> UIColor*
{
    return [UIColor colorWithRed:((argb >> 16) & 0xff) / 255.0 green:((argb >> 8) & 0xff) / 255.0
                            blue:(argb & 0xff) / 255.0 alpha:((argb >> 24) & 0xff) / 255.0];
}

// The editor's embedded font at its weight and size, never registered with the system; else the system font.
auto ui_font(const tiny::Text_field::Font& font) -> UIFont*
{
    if (!font.data.empty()) {
        auto* data = CFDataCreateWithBytesNoCopy(nullptr, reinterpret_cast<const UInt8*>(font.data.data()), static_cast<CFIndex>(font.data.size()), kCFAllocatorNull);
        auto* base = CTFontManagerCreateFontDescriptorFromData(data);
        CFRelease(data);
        if (base) {
            auto* variation = @{@(0x77676874): @(font.weight), @(0x6f70737a): @(font.size)}; // 'wght', 'opsz'
            auto* descriptor = CTFontDescriptorCreateCopyWithAttributes(base, (CFDictionaryRef)@{(id)kCTFontVariationAttribute: variation});
            CFRelease(base);
            auto* ct = CTFontCreateWithFontDescriptor(descriptor, font.size, nullptr);
            CFRelease(descriptor);
            if (ct) return [(UIFont*)ct autorelease]; // Toll-free bridged.
        }
    }
    const auto weight = font.weight >= 650 ? UIFontWeightBold : font.weight >= 550 ? UIFontWeightSemibold : font.weight >= 450 ? UIFontWeightMedium : UIFontWeightRegular;
    return [UIFont systemFontOfSize:font.size weight:weight];
}

} // namespace

// A field over the editor, its own delegate. Ends once: Return commits, a hardware keyboard's Escape cancels,
// a tap elsewhere in the editor does what `blur` says. Floats above the on-screen keyboard while it covers it.
@interface IosTextField : UITextField <UITextFieldDelegate>
- (id)initWithSpec:(const tiny::Text_field&)spec frame:(CGRect)frame done:(std::function<void(tiny::Text_result)>)done;
- (void)begin:(bool)selectAll;
- (void)blur;
- (void)finish:(tiny::Text_result::Kind)kind;
@end

@implementation IosTextField {
    std::function<void(tiny::Text_result)> _done;
    tiny::Text_field::Blur _blur;
    CGRect _home; // Where the editor put it; it floats up from here over the keyboard.
    bool _finished;
}

- (id)initWithSpec:(const tiny::Text_field&)spec frame:(CGRect)frame done:(std::function<void(tiny::Text_result)>)done {
    self = [super initWithFrame:frame];
    if (!self) return nil;
    _done = std::move(done);
    _blur = spec.blur;
    _home = frame;

    auto* initial = [NSString stringWithUTF8String:spec.initial.c_str()]; // nil for invalid UTF-8
    self.text = initial ? initial : @"";
    self.font = ui_font(spec.font);
    self.textColor = ui_color(spec.ink);
    self.tintColor = ui_color(spec.ink); // The caret and selection handles.
    self.backgroundColor = (spec.background >> 24) != 0 ? ui_color(spec.background) : UIColor.clearColor;
    self.textAlignment = spec.align == tiny::Text_field::Align::Left ? NSTextAlignmentLeft
                       : spec.align == tiny::Text_field::Align::Right ? NSTextAlignmentRight : NSTextAlignmentCenter;
    self.contentVerticalAlignment = UIControlContentVerticalAlignmentCenter;
    self.borderStyle = UITextBorderStyleNone;
    const auto pad = [](CGFloat w) { return [[[UIView alloc] initWithFrame:CGRectMake(0, 0, w, 1)] autorelease]; };
    self.leftView = pad(spec.inset);
    self.leftViewMode = UITextFieldViewModeAlways;
    self.rightView = pad(spec.inset);
    self.rightViewMode = UITextFieldViewModeAlways;
    self.autocorrectionType = UITextAutocorrectionTypeNo;
    self.spellCheckingType = UITextSpellCheckingTypeNo;
    self.autocapitalizationType = UITextAutocapitalizationTypeNone;
    self.returnKeyType = UIReturnKeyDone;
    if (!spec.label.empty()) self.accessibilityLabel = [NSString stringWithUTF8String:spec.label.c_str()];
    self.delegate = self;

    auto* center = [NSNotificationCenter defaultCenter];
    [center addObserver:self selector:@selector(keyboardWillChange:) name:UIKeyboardWillChangeFrameNotification object:nil];
    [center addObserver:self selector:@selector(keyboardWillHide:) name:UIKeyboardWillHideNotification object:nil];
    return self;
}

- (void)dealloc {
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [super dealloc];
}

- (void)begin:(bool)selectAll {
    [self becomeFirstResponder];
    if (selectAll) [self selectAll:nil];
    else self.selectedTextRange = [self textRangeFromPosition:self.endOfDocument toPosition:self.endOfDocument];
}

- (NSArray<UIKeyCommand*>*)keyCommands {
    auto* escape = [UIKeyCommand keyCommandWithInput:UIKeyInputEscape modifierFlags:0 action:@selector(escape:)];
    if (@available(iOS 15, *)) escape.wantsPriorityOverSystemBehavior = YES;
    return @[escape];
}

- (void)escape:(UIKeyCommand*)command {
    [self finish:tiny::Text_result::Kind::Cancelled];
}

- (BOOL)textFieldShouldReturn:(UITextField*)field {
    [self finish:tiny::Text_result::Kind::Committed];
    return NO;
}

- (void)textFieldDidEndEditing:(UITextField*)field {
    [self blur];
}

- (void)blur {
    [self finish:_blur == tiny::Text_field::Blur::Commit ? tiny::Text_result::Kind::Committed : tiny::Text_result::Kind::Cancelled];
}

// Clear of the keyboard: up just far enough, never above the editor's top.
- (void)keyboardWillChange:(NSNotification*)note {
    auto* superview = self.superview;
    if (!superview || !self.window) return;
    const auto screen = [note.userInfo[UIKeyboardFrameEndUserInfoKey] CGRectValue];
    const auto keyboard = [superview convertRect:screen fromCoordinateSpace:self.window.screen.coordinateSpace];
    auto frame = _home;
    constexpr auto gap = 8.0;
    if (CGRectIntersectsRect(keyboard, superview.bounds) && CGRectGetMaxY(frame) > CGRectGetMinY(keyboard) - gap) {
        frame.origin.y = std::max(0.0, CGRectGetMinY(keyboard) - gap - frame.size.height);
    }
    self.frame = frame;
}

- (void)keyboardWillHide:(NSNotification*)note {
    self.frame = _home;
}

- (void)finish:(tiny::Text_result::Kind)kind {
    if (_finished) return;
    _finished = true;
    auto result = tiny::Text_result{.kind = kind, .text = std::string{self.text.UTF8String ? self.text.UTF8String : ""}};
    auto done = std::move(_done);
    _done = {};
    [[NSNotificationCenter defaultCenter] removeObserver:self];

    // Out of the view after this event: UIKit is still inside our delegate callbacks.
    [self retain];
    dispatch_async(dispatch_get_main_queue(), ^{
        [self resignFirstResponder];
        [self removeFromSuperview];
        [self release];
    });
    if (done) done(std::move(result));
}

@end

// MARK: - View

@interface IosView : UIView {
    std::shared_ptr<tiny::View_delegate> _delegate;
}
- (id)initWithDelegate:(std::shared_ptr<tiny::View_delegate>)delegate;
- (void)startDisplayLink; // Raw mechanism; overridden by IosMetalView.
- (void)stopDisplayLink;
- (int)targetFps;
- (void)applyFrameRate;   // Retunes the live link; overridden by IosMetalView.
- (void)resumeDisplayLink; // Visibility entry points — these track `_link_wanted`.
- (void)suspendDisplayLink;
- (void)beginTextField:(const tiny::Text_field&)spec done:(std::function<void(tiny::Text_result)>)done;
- (void)endTextField;
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
    IosTextField* _textField; // Not retained: the subview list holds it while it's open.
}

// A native field over `spec.frame` (logical, top-down); one at a time, so an open one ends as its blur says.
- (void)beginTextField:(const tiny::Text_field&)spec done:(std::function<void(tiny::Text_result)>)done {
    if (auto* open = _textField) {
        _textField = nil;
        [open blur];
    }
    if (!self.window) {
        done({.kind = tiny::Text_result::Kind::Cancelled});
        return;
    }
    IosView* view = self; // Unretained: the view ends its field before it goes.
    auto* field = [[IosTextField alloc] initWithSpec:spec frame:CGRectMake(spec.frame.x, spec.frame.y, spec.frame.w, spec.frame.h)
                                                done:[view, done = std::move(done)](tiny::Text_result result) {
        view->_textField = nil;
        done(std::move(result));
    }];
    [self addSubview:field];
    [field release];
    _textField = field;
    [field begin:spec.select_all];
}

- (void)endTextField {
    if (auto* field = _textField) {
        _textField = nil;
        [field finish:tiny::Text_result::Kind::Cancelled];
    }
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
        [center addObserver:self selector:@selector(thermalStateChanged:)
                       name:NSProcessInfoThermalStateDidChangeNotification object:nil];
    }
    return self;
}

// MARK: - App lifecycle

- (int)targetFps {
    return tiny::fps_for_thermal([[NSProcessInfo processInfo] thermalState]);
}

- (void)applyFrameRate {
    if (!_displayLink) return;
    _displayLink.preferredFramesPerSecond = [self targetFps];
}

- (void)thermalStateChanged:(NSNotification *)note {
    [self applyFrameRate];
}

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
    [self applyFrameRate];
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

    // A tap elsewhere in the editor ends an open field (UIKit doesn't), and still reaches the editor.
    if (auto* field = _textField) {
        _textField = nil;
        [field blur];
    }
    
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
    [self applyFrameRate];
}

- (void)stopDisplayLink {
    if (_metalDisplayLink) {
        [_metalDisplayLink invalidate];
        [_metalDisplayLink release]; // ??
        _metalDisplayLink = nil;
    }
}

- (void)applyFrameRate {
    if (!_metalDisplayLink) return;
    const auto fps = static_cast<float>([self targetFps]);
    _metalDisplayLink.preferredFrameRateRange = CAFrameRateRangeMake(fps, fps, fps);
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
    
#if IOS_GRAPHICS_GPU
    if (@available(iOS 17, *)) {
        view = [[IosMetalView alloc] initWithDelegate:delegate];
    } else {
        view = [[IosView alloc] initWithDelegate:delegate];
    }
#else
    // Raster backend: a plain CADisplayLink driving CPU rasterisation. No CAMetalDisplayLink.
    view = [[IosView alloc] initWithDelegate:delegate];
#endif

    auto context = std::make_unique<Window_context>();
    context->setup({.native_handle = static_cast<void*>(view)});
    _delegate->assign_context(std::move(context));

    _view = view;
    _token = Window_registry::add(_view); // Names this window for Platform_dialogs.
}

Platform_view::~Platform_view() 
{
    if (auto* view = static_cast<IosView*>(_view)) [view endTextField];
    Window_registry::remove(_token);

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

// MARK: - Platform text

auto Platform_text::edit(const Text_field& field, std::function<void(Text_result)> on_done, Dialog_context ctx) -> void
{
    // As the dialogs: the answer arrives through the task manager, on main, or not at all once it's shut down.
    auto deliver = [ctx, on_done = std::move(on_done)](Text_result result) {
        if (on_done) ctx.tasks.on_main([on_done, result = std::move(result)] { on_done(result); });
    };
    auto* view = static_cast<IosView*>(Window_registry::resolve(ctx.window));
    if (!view) {
        deliver({.kind = Text_result::Kind::Cancelled});
        return;
    }
    [view beginTextField:field done:std::move(deliver)];
}

auto Platform_text::cancel(Window_token window) -> void
{
    if (auto* view = static_cast<IosView*>(Window_registry::resolve(window))) [view endTextField];
}

} // namespace tiny
