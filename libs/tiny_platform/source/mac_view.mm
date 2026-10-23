#include <tiny_platform/platform_view.hpp>

#include <algorithm>
#include <chrono>

#import <Cocoa/Cocoa.h>
#import <CoreText/CoreText.h>
#import <CoreVideo/CVDisplayLink.h>
#import <QuartzCore/QuartzCore.h> // CAMetalDisplayLink

#include <tiny_platform/platform_text.hpp>
#include <tiny_platform/window_context.hpp>

#include "window_registry.hpp"

#include "mac_config.hpp"
#ifndef TINY_MAC_VIEW
#error "TINY_MAC_VIEW must be defined in mac_config.h"
#endif

#if __has_feature(objc_arc)
static_assert(false, "This is a non-ARC file");
#endif

// MARK: - Text field

// Centres a single line vertically (NSTextFieldCell puts it at the top) and pads it `inset` across, for drawing and
// for editing alike: the field editor gets its frame from edit/selectWithFrame, not from drawing.
@interface TINY_MAC_TEXT_CELL : NSTextFieldCell
@property CGFloat inset;
@end

@implementation TINY_MAC_TEXT_CELL

- (NSRect)drawingRectForBounds:(NSRect)bounds {
    constexpr auto line_padding = 2.0; // The text container's own, on each side.
    const auto across = std::max(0.0, self.inset - line_padding);
    auto rect = NSMakeRect(bounds.origin.x + across, bounds.origin.y, std::max(0.0, bounds.size.width - 2 * across), bounds.size.height);
    const auto height = [self cellSizeForBounds:rect].height;
    if (height < rect.size.height) {
        rect.origin.y += (rect.size.height - height) / 2;
        rect.size.height = height;
    }
    return rect;
}

- (void)editWithFrame:(NSRect)rect inView:(NSView*)view editor:(NSText*)editor delegate:(id)delegate event:(NSEvent*)event {
    [super editWithFrame:[self drawingRectForBounds:rect] inView:view editor:editor delegate:delegate event:event];
}

- (void)selectWithFrame:(NSRect)rect inView:(NSView*)view editor:(NSText*)editor delegate:(id)delegate start:(NSInteger)start length:(NSInteger)length {
    [super selectWithFrame:[self drawingRectForBounds:rect] inView:view editor:editor delegate:delegate start:start length:length];
}

@end

// A field over the editor, its own delegate. Ends once: Return commits, Escape cancels, clicking away does what
// `blur` says.
@interface TINY_MAC_TEXT_FIELD : NSTextField <NSTextFieldDelegate>
- (id)initWithSpec:(const tiny::Text_field&)spec frame:(NSRect)frame done:(std::function<void(tiny::Text_result)>)done;
- (void)finish:(tiny::Text_result::Kind)kind;
@end

namespace {

auto ns_color(std::uint32_t argb) -> NSColor*
{
    return [NSColor colorWithSRGBRed:((argb >> 16) & 0xff) / 255.0 green:((argb >> 8) & 0xff) / 255.0
                                blue:(argb & 0xff) / 255.0 alpha:((argb >> 24) & 0xff) / 255.0];
}

// The editor's embedded font at its weight and size, never registered with the system; else the system font.
auto ns_font(const tiny::Text_field::Font& font) -> NSFont*
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
            if (ct) return [(NSFont*)ct autorelease];
        }
    }
    const auto weight = font.weight >= 650 ? NSFontWeightBold : font.weight >= 550 ? NSFontWeightSemibold : font.weight >= 450 ? NSFontWeightMedium : NSFontWeightRegular;
    return [NSFont systemFontOfSize:font.size weight:weight];
}

} // namespace

@implementation TINY_MAC_TEXT_FIELD {
    std::function<void(tiny::Text_result)> _done;
    tiny::Text_field::Blur _blur;
    NSResponder* _previous; // Retained: who had the keyboard before us, given it back after.
    bool _finished;
}

- (id)initWithSpec:(const tiny::Text_field&)spec frame:(NSRect)frame done:(std::function<void(tiny::Text_result)>)done {
    self = [super initWithFrame:frame];
    if (!self) return nil;
    _done = std::move(done);
    _blur = spec.blur;

    auto* initial = [NSString stringWithUTF8String:spec.initial.c_str()]; // nil for invalid UTF-8
    auto* cell = [[[TINY_MAC_TEXT_CELL alloc] initTextCell:initial ? initial : @""] autorelease];
    cell.editable = YES;
    cell.scrollable = YES;
    cell.wraps = NO;
    cell.usesSingleLineMode = YES;
    cell.inset = spec.inset;
    self.cell = cell;

    self.bezeled = NO;
    self.bordered = NO;
    self.drawsBackground = (spec.background >> 24) != 0; // Transparent: the editor's drawing shows through.
    self.backgroundColor = ns_color(spec.background);
    self.textColor = ns_color(spec.ink);
    self.font = ns_font(spec.font);
    self.alignment = spec.align == tiny::Text_field::Align::Left ? NSTextAlignmentLeft
                   : spec.align == tiny::Text_field::Align::Right ? NSTextAlignmentRight : NSTextAlignmentCenter;
    self.focusRingType = NSFocusRingTypeNone;
    if (!spec.label.empty()) self.accessibilityLabel = [NSString stringWithUTF8String:spec.label.c_str()];
    self.delegate = self;
    return self;
}

- (void)dealloc {
    [_previous release];
    [super dealloc];
}

// Takes the keyboard in `window`, remembering who had it.
- (void)beginIn:(NSWindow*)window selectAll:(bool)selectAll {
    _previous = [window.firstResponder retain];
    if (window.canBecomeKeyWindow && !window.isKeyWindow) [window makeKeyWindow];
    [window makeFirstResponder:self];
    if (auto* editor = (NSTextView*)self.currentEditor) {
        editor.insertionPointColor = self.textColor;
        if (selectAll) [editor selectAll:nil];
        else [editor setSelectedRange:NSMakeRange(editor.string.length, 0)];
    }
}

- (BOOL)control:(NSControl*)control textView:(NSTextView*)textView doCommandBySelector:(SEL)command {
    if (command == @selector(insertNewline:)) {
        [self finish:tiny::Text_result::Kind::Committed];
        return YES;
    }
    if (command == @selector(cancelOperation:) || command == @selector(complete:)) { // Escape: the field editor calls it complete:.
        [self finish:tiny::Text_result::Kind::Cancelled];
        return YES;
    }
    return NO;
}

- (void)controlTextDidEndEditing:(NSNotification*)notification {
    [self finish:_blur == tiny::Text_field::Blur::Commit ? tiny::Text_result::Kind::Committed : tiny::Text_result::Kind::Cancelled];
}

- (void)finish:(tiny::Text_result::Kind)kind {
    if (_finished) return;
    _finished = true;
    auto result = tiny::Text_result{.kind = kind, .text = std::string{self.stringValue.UTF8String ? self.stringValue.UTF8String : ""}};
    auto done = std::move(_done);
    _done = {};

    // The keyboard goes back to whoever had it, if they're still in this window.
    auto* window = self.window;
    auto* previous = [_previous isKindOfClass:[NSView class]] && ((NSView*)_previous).window == window ? _previous : nil;
    if (window && window.firstResponder == self.currentEditor) [window makeFirstResponder:previous];

    // Out of the view after this event: AppKit is still inside our delegate callbacks.
    [self retain];
    dispatch_async(dispatch_get_main_queue(), ^{
        [self removeFromSuperview];
        [self release];
    });
    if (done) done(std::move(result));
}

@end

// MARK: - View

// TINY_MAC_VIEW
@interface TINY_MAC_VIEW : NSView {
    std::shared_ptr<tiny::View_delegate> _delegate;
}
- (id)initWithDelegate:(std::shared_ptr<tiny::View_delegate>)delegate onAutorelease:(std::function<void()>)onAutorelease;
- (void)startDisplayLink;
- (void)stopDisplayLink;
- (void)draw;
- (void)detach;
- (void)beginTextField:(const tiny::Text_field&)spec done:(std::function<void(tiny::Text_result)>)done;
- (void)endTextField;
@end

// 
static auto on_display_link(CVDisplayLinkRef, const CVTimeStamp*, const CVTimeStamp*, CVOptionFlags, CVOptionFlags*, void* context) -> CVReturn
{
    auto source = static_cast<dispatch_source_t>(context);
    dispatch_source_merge_data(source, 1);
    return kCVReturnSuccess;
}

@implementation TINY_MAC_VIEW {
    std::function<void()> _on_autorelease;
    tiny::User_interaction _interaction;
    tiny::Event_stream _events;
    CVDisplayLinkRef _displayLink;
    dispatch_source_t _displaySource;
    TINY_MAC_TEXT_FIELD* _textField; // Not retained: the subview list holds it while it's open.
}

- (id)initWithDelegate:(std::shared_ptr<tiny::View_delegate>)delegate onAutorelease:(std::function<void()>)onAutorelease {
    _delegate = delegate;
    _on_autorelease = onAutorelease;
    const auto size = delegate->get_size();
    self = [super initWithFrame:NSMakeRect(0, 0, size.w, size.h)];

    if (self) {
        // Dark mode
        NSString* mode = [[NSUserDefaults standardUserDefaults] stringForKey:@"AppleInterfaceStyle"];
        const auto dark_mode = [mode isEqualToString:@"Dark"];
        _delegate->notify(tiny::Dark_mode_changed{.new_value = dark_mode > 0});

        [[NSDistributedNotificationCenter defaultCenter] addObserver:self 
                                                            selector:@selector(themeChanged:) 
                                                                name:@"AppleInterfaceThemeChangedNotification" 
                                                              object: nil];

        [[NSNotificationCenter defaultCenter] addObserver:self
                                                 selector:@selector(windowDidChangeScreen:)
                                                     name:NSWindowDidChangeScreenNotification
                                                   object:self.window];
        [[NSNotificationCenter defaultCenter] addObserver:self
                                                 selector:@selector(screenParametersChanged:)
                                                     name:NSApplicationDidChangeScreenParametersNotification
                                                   object:nil];
    }
    return self;
}

- (void)dealloc {
    [self endTextField];
    _on_autorelease(); // Need this for AUv2
    [self stopDisplayLink];
    [super dealloc];
}

// The owner is going away while AppKit still holds this view (AUv2: a host can dispose of the unit
// with its view on screen). Stop drawing and forget everything that points back at the owner.
- (void)detach {
    [self endTextField];
    [self stopDisplayLink];
    _on_autorelease = []() {};
    _delegate->detach();
}

// A native field over `spec.frame` (logical, top-down); one at a time, so an open one ends as its blur says.
- (void)beginTextField:(const tiny::Text_field&)spec done:(std::function<void(tiny::Text_result)>)done {
    if (_textField) [_textField controlTextDidEndEditing:[NSNotification notificationWithName:NSControlTextDidEndEditingNotification object:_textField]];
    _textField = nil;
    if (!self.window) {
        done({.kind = tiny::Text_result::Kind::Cancelled});
        return;
    }
    const auto frame = NSMakeRect(spec.frame.x, self.bounds.size.height - spec.frame.y - spec.frame.h, spec.frame.w, spec.frame.h);
    TINY_MAC_VIEW* view = self; // Unretained: the view ends its field before it goes.
    auto* field = [[TINY_MAC_TEXT_FIELD alloc] initWithSpec:spec frame:frame done:[view, done = std::move(done)](tiny::Text_result result) {
        view->_textField = nil;
        done(std::move(result));
    }];
    [self addSubview:field];
    [field release];
    _textField = field;
    [field beginIn:self.window selectAll:spec.select_all];
}

- (void)endTextField {
    if (auto* field = _textField) {
        _textField = nil;
        [field finish:tiny::Text_result::Kind::Cancelled];
    }
}

- (void)startDisplayLink {
    [self stopDisplayLink];

    NSScreen *screen = self.window.screen;
    if (!screen) return;

    CGDirectDisplayID displayID = [[screen.deviceDescription objectForKey:@"NSScreenNumber"] unsignedIntValue];

    _displaySource = dispatch_source_create(DISPATCH_SOURCE_TYPE_DATA_ADD, 0, 0, dispatch_get_main_queue());
    __block TINY_MAC_VIEW* self_ = self; // Need block pointer!
    dispatch_source_set_event_handler(_displaySource, ^(){
        if (self_) {
            [self_ draw];
        }
    });
    dispatch_resume(_displaySource);

    if (CVDisplayLinkCreateWithCGDisplay(displayID, &_displayLink) == kCVReturnSuccess) {
        CVDisplayLinkSetOutputCallback(_displayLink, &on_display_link, _displaySource);
        CVDisplayLinkStart(_displayLink);
    }
}

- (void)stopDisplayLink {
    if (_displayLink) {
        CVDisplayLinkStop(_displayLink);
        CVDisplayLinkRelease(_displayLink);
        _displayLink = nil;
    }

    if (_displaySource) {
        dispatch_source_cancel(_displaySource);
        _displaySource = nil;
    }
}

- (void)draw {
    const auto time_now = tiny::System_clock::now();

    // Resolve modifiers
    NSEventModifierFlags flags = [NSEvent modifierFlags];
    _interaction.modifier_keys = tiny::Modifier_keys{
        .primary = (flags & NSEventModifierFlagCommand) != 0,
        .alt = (flags & NSEventModifierFlagOption) != 0,
        .shift = (flags & NSEventModifierFlagShift) != 0,
    };

    // Copy events into the interaction.
    const auto event_list = _events.consume(tiny::Steady_clock::now());
    _interaction.events = event_list;

    // Pointer absolute position.
    const auto mouse = [NSEvent mouseLocation]; // global, bottom-up
    const auto screen_h = [NSScreen mainScreen] ? [NSScreen mainScreen].frame.size.height : 0.0;
    _interaction.pointer_abs = tiny::Coords{mouse.x, screen_h - mouse.y};

    _delegate->draw(_interaction, time_now);
    _interaction.scroll_deltas = tiny::Coords{}; // Consume deltas
    _interaction.precise_scroll = false;
}

// MARK: - notifications

-(void)themeChanged:(NSNotification *)notification {
    NSString* mode = [[NSUserDefaults standardUserDefaults] stringForKey:@"AppleInterfaceStyle"];
    const auto dark_mode = [mode isEqualToString:@"Dark"];
    _delegate->notify(tiny::Dark_mode_changed{.new_value = dark_mode > 0});
}

- (void)windowDidChangeScreen:(NSNotification *)notification {
    [self updateDisplayLinkForCurrentScreen];
}

- (void)screenParametersChanged:(NSNotification *)notification {
    [self updateDisplayLinkForCurrentScreen];
}

- (void)updateDisplayLinkForCurrentScreen {
    [self stopDisplayLink];
    [self startDisplayLink];
}

// MARK: - overrides

- (void)updateTrackingAreas {
    [super updateTrackingAreas];

    if (self.trackingAreas.count > 0) {
        for (NSTrackingArea *area in self.trackingAreas) {
            [self removeTrackingArea:area];
        }
    }

    NSTrackingAreaOptions options = NSTrackingMouseEnteredAndExited
                                  | NSTrackingMouseMoved
                                  | NSTrackingActiveAlways
                                  | NSTrackingCursorUpdate // So `cursorUpdate:` below gets called.
                                  | NSTrackingInVisibleRect;
    NSTrackingArea *area = [[NSTrackingArea alloc] initWithRect:self.bounds
                                                        options:options
                                                          owner:self
                                                       userInfo:nil];
    
    [self addTrackingArea:area];
}

// Cursor rects are per-window, and ours is the host's: contributing none leaves the host's
// own rects (Live's timeline magnifier, its splitters) showing through our view. Claim the
// cursor for our whole area instead. Author-settable cursors would cache a value here.
- (void)cursorUpdate:(NSEvent *)event {
    [[NSCursor arrowCursor] set];
}

- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    const auto size = _delegate->get_size();
    _delegate->on_resize(size); // Force scale update.
}

- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    if (self.window) {
        [self startDisplayLink];
    } else {
        [self stopDisplayLink];
    }
}

// MARK: - events

- (BOOL)acceptsFirstMouse:(NSEvent *)event {
    return YES;
}

- (void)mouseDown:(NSEvent *)event {
    using namespace tiny;
    const auto locationInView = [self convertPoint:event.locationInWindow fromView:nil];
    const auto y = self.bounds.size.height - locationInView.y;
    const auto pos = tiny::Coords{locationInView.x, y};
    const auto ctrl = (event.modifierFlags & NSEventModifierFlagControl) != 0;
    const auto button = ctrl ? Pointer_button::right : Pointer_button::left;

    _events.push(Event{
        .event = Pointer_down{button, pos}
    });

    //[super mouseDown:event]; // REAPER was resizing window on double-click.
}

- (void)mouseUp:(NSEvent *)event {
    using namespace tiny;
    const auto locationInView = [self convertPoint:event.locationInWindow fromView:nil];
    const auto y = self.bounds.size.height - locationInView.y;
    const auto pos = tiny::Coords{locationInView.x, y};
    const auto ctrl = (event.modifierFlags & NSEventModifierFlagControl) != 0;
    const auto button = ctrl ? Pointer_button::right : Pointer_button::left;
    
    _events.push(Event{
        .event = Pointer_up{button, pos}
    });

    const auto click_count = event.clickCount;

    if (click_count > 0) {
        _events.push(Event{
            .event = Pointer_click{button, static_cast<uint32_t>(click_count), pos}
        });
    }

    //[super mouseUp:event];
}

- (void)mouseMoved:(NSEvent *)event {
    using namespace tiny;
    const auto locationInView = [self convertPoint:event.locationInWindow fromView:nil];
    const auto y = self.bounds.size.height - locationInView.y;
    const auto pos = tiny::Coords{locationInView.x, y};
    [[maybe_unused]] const auto t = std::chrono::steady_clock::now();

    _events.push(Event{
        .event = Pointer_move{pos}
    });

    //[super mouseMoved:event];
}

- (void)mouseDragged:(NSEvent *)event {
    using namespace tiny;
    const auto locationInView = [self convertPoint:event.locationInWindow fromView:nil];
    const auto y = self.bounds.size.height - locationInView.y;
    const auto pos = tiny::Coords{locationInView.x, y};

    _events.push(Event{
        .event = Pointer_move{pos}
    });

    //[super mouseDragged:event];
}

- (void)rightMouseDown:(NSEvent *)event {
    using namespace tiny;
    const auto locationInView = [self convertPoint:event.locationInWindow fromView:nil];
    const auto y = self.bounds.size.height - locationInView.y;
    const auto pos = tiny::Coords{locationInView.x, y};

    _events.push(Event{
        .event = Pointer_down{Pointer_button::right, pos}
    });

    //[super rightMouseDown:event];
}

- (void)rightMouseUp:(NSEvent *)event {
    using namespace tiny;
    const auto locationInView = [self convertPoint:event.locationInWindow fromView:nil];
    const auto y = self.bounds.size.height - locationInView.y;
    const auto pos = tiny::Coords{locationInView.x, y};

    _events.push(Event{
        .event = Pointer_up{Pointer_button::right, pos}
    });

    //[super rightMouseUp:event];
}

- (void)rightMouseDragged:(NSEvent *)event {
    using namespace tiny;
    const auto locationInView = [self convertPoint:event.locationInWindow fromView:nil];
    const auto y = self.bounds.size.height - locationInView.y;
    const auto pos = tiny::Coords{locationInView.x, y};

    _events.push(Event{
        .event = Pointer_move{pos}
    });

    //[super rightMouseDragged:event];
}

- (void)scrollWheel:(NSEvent *)event {
    // Accumulate: several wheel events can land between two draws, and the whole
    // frame's travel matters when the deltas edit a parameter.
    _interaction.scroll_deltas.x += event.scrollingDeltaX;
    _interaction.scroll_deltas.y += event.scrollingDeltaY;
    _interaction.inertial_scroll = (event.momentumPhase != NSEventPhaseNone);
    _interaction.precise_scroll = event.hasPreciseScrollingDeltas;
    //[super scrollWheel:event];
}

- (void)mouseEntered:(NSEvent *)event {
    using namespace tiny;
    const auto locationInView = [self convertPoint:event.locationInWindow fromView:nil];
    const auto y = self.bounds.size.height - locationInView.y;
    const auto pos = tiny::Coords{locationInView.x, y};

    _events.push(Event{
        .event = Pointer_enter{pos}
    });

    //[super mouseEntered:event];
}

- (void)mouseExited:(NSEvent *)event {
    using namespace tiny;
    const auto locationInView = [self convertPoint:event.locationInWindow fromView:nil];
    const auto y = self.bounds.size.height - locationInView.y;
    const auto pos = tiny::Coords{locationInView.x, y};

    _events.push(Event{
        .event = Pointer_exit{pos}
    });

    //[super mouseExited:event];
}

@end

// MARK: - METAL VIEW

API_AVAILABLE(macos(14.0))
@interface TINY_MAC_METAL_VIEW : TINY_MAC_VIEW <CAMetalDisplayLinkDelegate>
@end

@implementation TINY_MAC_METAL_VIEW {
    CAMetalDisplayLink* _metalDisplayLink;
}

- (void)startDisplayLink {
    [self stopDisplayLink];
    _metalDisplayLink = [[CAMetalDisplayLink alloc] initWithMetalLayer:(CAMetalLayer*)self.layer];
    _metalDisplayLink.delegate = self;
    [_metalDisplayLink addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
}

- (void)stopDisplayLink {
    if (_metalDisplayLink) {
        [_metalDisplayLink invalidate];
        [_metalDisplayLink release]; // From metal idea.
        _metalDisplayLink = nil;
    }
}

- (void)metalDisplayLink:(CAMetalDisplayLink *)link needsUpdate:(CAMetalDisplayLinkUpdate *)update {
    auto drawable = update.drawable;
    _delegate->set_drawable(drawable);
    [self draw];
}

@end

// MARK: - Platform view

namespace tiny {

Platform_view::Platform_view(std::shared_ptr<View_delegate> delegate, bool owns_view, std::function<void()> on_release) : _owns_view{owns_view}, _delegate{delegate}
{
    NSView* view;

    // An autoreleasing view can die before us or after us; its dealloc tells us which.
    auto on_view_released = [this, on_release = std::move(on_release)]() {
        _view = nullptr;
        on_release();
    };
    if (@available(macOS 14, *)) {
        view = [[TINY_MAC_METAL_VIEW alloc] initWithDelegate:_delegate onAutorelease:on_view_released];
    } else {
        view = [[TINY_MAC_VIEW alloc] initWithDelegate:_delegate onAutorelease:on_view_released];
    }

    auto context = std::make_unique<Window_context>();
    context->setup({.native_handle = static_cast<void*>(view)});
    _delegate->assign_context(std::move(context));

    _view = view;
    _token = Window_registry::add(_view); // Names this window for Platform_dialogs.
}

Platform_view::~Platform_view()
{
    Window_registry::remove(_token);

    // Detached first either way: an autoreleasing view is still in the host's window, and even an
    // owned one can be deallocated later than this (an autorelease pool), after which nothing may
    // call back into us or our owner.
    if (_view) [(TINY_MAC_VIEW*)_view detach];
    if (_owns_view && _view) {
        [(NSView*)_view removeFromSuperview];
        [(NSView*)_view release];
    }
    _view = nullptr;
}

auto Platform_view::on_create() -> void
{

}

auto Platform_view::on_show() -> void
{
    if (auto view = static_cast<TINY_MAC_VIEW*>(_view)) {
        [view startDisplayLink];
    }
}

auto Platform_view::on_hide() -> void
{
    if (!_owns_view) return;
    if (auto view = static_cast<TINY_MAC_VIEW*>(_view)) {
        [view stopDisplayLink]; // We stop in dealloc for AUv2.
    }
}

auto Platform_view::on_destroy() -> void
{
    _delegate->invalidate_context();
}

auto Platform_view::receive_parent(void* parent) -> void
{
    [(NSView*)parent addSubview: (NSView*)_view];
}

auto Platform_view::resize(int32_t w, int32_t h) -> void
{
    [(NSView*)_view setFrameSize:NSMakeSize(w, h)];
    _delegate->on_resize({w, h});
}

// MARK: - Platform text

auto Platform_text::edit(const Text_field& field, std::function<void(Text_result)> on_done, Dialog_context ctx) -> void
{
    // As the dialogs: the answer arrives through the task manager, on main, or not at all once it's shut down.
    auto deliver = [ctx, on_done = std::move(on_done)](Text_result result) {
        if (on_done) ctx.tasks.on_main([on_done, result = std::move(result)] { on_done(result); });
    };
    auto* view = static_cast<TINY_MAC_VIEW*>(Window_registry::resolve(ctx.window));
    if (!view) {
        deliver({.kind = Text_result::Kind::Cancelled});
        return;
    }
    [view beginTextField:field done:std::move(deliver)];
}

auto Platform_text::cancel(Window_token window) -> void
{
    if (auto* view = static_cast<TINY_MAC_VIEW*>(Window_registry::resolve(window))) [view endTextField];
}

} // namespace tiny