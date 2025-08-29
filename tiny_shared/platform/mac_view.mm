#include "platform_view.h"

#if PLATFORM_MACOS
#include <chrono>

#import <Cocoa/Cocoa.h>
#import <CoreVideo/CVDisplayLink.h>

#include "../skia/mac/GaneshMetalWindowContext_mac.h"
#include "../skia/mac/MacWindowInfo.h"

#define MAC_VIEW_RENDER_MAIN_THREAD 0 // Main thread was slowing down Pro Tools UI.

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

    bool _dark_mode;

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

        // Dark mode
        NSString* mode = [[NSUserDefaults standardUserDefaults] stringForKey:@"AppleInterfaceStyle"];
        _dark_mode = [mode isEqualToString:@"Dark"];
        [NSDistributedNotificationCenter.defaultCenter addObserver:self selector:@selector(themeChanged:) name:@"AppleInterfaceThemeChangedNotification" object: nil];
    }
    return self;
}

- (void)dealloc {
    if(_displayLink) {
        // Stop the display link BEFORE releasing anything in the view otherwise the display link
        // thread may call into the view and crash when it encounters something that no longer
        // exists
        CVDisplayLinkStop(_displayLink);
        CVDisplayLinkRelease(_displayLink);
    }
    
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

#if MAC_VIEW_RENDER_MAIN_THREAD
    dispatch_source_cancel(_displaySource);
#endif
}

-(void)themeChanged:(NSNotification *) notification {
    NSString* mode = [[NSUserDefaults standardUserDefaults] stringForKey:@"AppleInterfaceStyle"];
    _dark_mode = [mode isEqualToString:@"Dark"];
}

- (void)render {
    const auto time_now = tiny::System_clock::now();

    // Should we dwell?
    if (_over_pos && !_dwelt) {
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - _over_time);
        if (elapsed.count() > 2000) {
            _dwelt = tiny::try_set(_interaction.state, tiny::Dwell{*_over_pos});
        }
    }

    // Set dark mode
    _interaction.dark_mode = _dark_mode;

    // Resolve modifiers
    NSEventModifierFlags flags = [NSEvent modifierFlags];
    _interaction.modifier_keys = tiny::Modifier_keys{
        .primary = (flags & NSEventModifierFlagCommand) != 0,
        .alt = (flags & NSEventModifierFlagOption) != 0,
        .shift = (flags & NSEventModifierFlagShift) != 0,
    };

    graphics_delegate->draw(_interaction, time_now);
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
    const auto y = self.bounds.size.height - event.locationInWindow.y;
    const auto pos = tiny::Coords{event.locationInWindow.x, y};
    const auto ctrl = (event.modifierFlags & NSEventModifierFlagControl) != 0;
    if (ctrl) {
        tiny::try_set(_interaction.state, tiny::Right_click{pos});
        _over_pos = std::nullopt;
    } 
    else {
        tiny::try_set(_interaction.state, tiny::Down{pos});
        _over_pos = std::nullopt;
        _left_pos = pos;
    }
    [super mouseDown:event];
}

- (void)mouseUp:(NSEvent *)event {
    const auto y = self.bounds.size.height - event.locationInWindow.y;
    const auto pos = tiny::Coords{event.locationInWindow.x, y};
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
    const auto y = self.bounds.size.height - event.locationInWindow.y;
    const auto pos = tiny::Coords{event.locationInWindow.x, y};
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
    const auto y = self.bounds.size.height - event.locationInWindow.y;
    const auto pos = tiny::Coords{event.locationInWindow.x, y};
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
    const auto y = self.bounds.size.height - event.locationInWindow.y;
    const auto pos = tiny::Coords{event.locationInWindow.x, y};
    tiny::try_set(_interaction.state, tiny::Right_click{pos});
    _over_pos = std::nullopt;
    [super rightMouseDown:event];
}

- (void)rightMouseUp:(NSEvent *)event {
    [super rightMouseUp:event];
}

- (void)rightMouseDragged:(NSEvent *)event {
    [super rightMouseDragged:event];
}

- (void)scrollWheel:(NSEvent *)event {
    const auto deltas = tiny::Coords{event.scrollingDeltaX, event.scrollingDeltaY};
    _interaction.scroll_deltas = deltas;
    [super scrollWheel:event];
}

- (void)mouseEntered:(NSEvent *)event {
    const auto y = self.bounds.size.height - event.locationInWindow.y;
    const auto pos = tiny::Coords{event.locationInWindow.x, y};
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

auto Platform_view::teardown() -> void
{
    MacView* view = (MacView*)_view;
    [view stopDisplayLink];
}

auto Platform_view::resize(int32_t w, int32_t h) -> void
{
    _delegate->on_resize({w, h});
    [(NSView*)_view setFrameSize:NSMakeSize(w, h)];
}

auto Platform_view::redraw() -> void
{
}

// MARK: - alert

auto Platform_dialogs::message(const std::string& title, const std::string& message, Callback<> on_done) -> void
{
    // Copy to locals.
    const auto title_copy = title;
    const auto message_copy = message;

    dispatch_async(dispatch_get_main_queue(), ^{
        NSAlert* alert = [[NSAlert alloc] init];
        [alert setMessageText:[NSString stringWithUTF8String:title_copy.c_str()]];
        [alert setInformativeText:[NSString stringWithUTF8String:message_copy.c_str()]];
        [alert addButtonWithTitle:@"OK"];

        NSWindow* keyWindow = [[NSApplication sharedApplication] keyWindow];
        if (keyWindow != nil) {
            [alert beginSheetModalForWindow:keyWindow completionHandler:^(NSModalResponse returnCode) {
                on_done();
                dispatch_async(dispatch_get_main_queue(), ^{
                    [keyWindow makeKeyAndOrderFront:nil];
                });
            }];
        } 
        else {
            [alert runModal];
            on_done();
        };

        [alert release];
    });
}

auto Platform_dialogs::confirm(const std::string& title, const std::string& message, Callback<bool> on_confirm) -> void
{
    // Copy to locals.
    const auto title_copy = title;
    const auto message_copy = message;

    dispatch_async(dispatch_get_main_queue(), ^{
        NSAlert* alert = [[NSAlert alloc] init];
        [alert setMessageText:[NSString stringWithUTF8String:title_copy.c_str()]];
        [alert setInformativeText:[NSString stringWithUTF8String:message_copy.c_str()]];
        [alert addButtonWithTitle:@"OK"];
        [alert addButtonWithTitle:@"Cancel"];

        NSWindow* keyWindow = [[NSApplication sharedApplication] keyWindow];
        if (keyWindow != nil) {
            [alert beginSheetModalForWindow:keyWindow completionHandler:^(NSModalResponse returnCode) {
                on_confirm(returnCode == NSAlertFirstButtonReturn);
                dispatch_async(dispatch_get_main_queue(), ^{
                    [keyWindow makeKeyAndOrderFront:nil];
                });
            }];
        }
        else {
            NSModalResponse response = [alert runModal];
            on_confirm(response == NSAlertFirstButtonReturn);
        }

        [alert release];
    });
}

auto Platform_dialogs::text_input(const std::string& title, const std::string& message, Callback<std::string> on_text) -> void
{
    // Copy to locals. 
    const auto title_copy = title;
    const auto message_copy = message;

    dispatch_async(dispatch_get_main_queue(), ^{
        NSAlert* alert = [[NSAlert alloc] init];
        [alert setMessageText:[NSString stringWithUTF8String:title_copy.c_str()]];
        [alert setInformativeText:[NSString stringWithUTF8String:message_copy.c_str()]];
        [alert addButtonWithTitle:@"OK"];
        [alert addButtonWithTitle:@"Cancel"];

        NSTextField* input = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 200, 24)];
        [alert setAccessoryView:input];

        NSWindow* keyWindow = [[NSApplication sharedApplication] keyWindow];
        if (keyWindow != nil) {
            [alert beginSheetModalForWindow:keyWindow completionHandler:^(NSModalResponse returnCode) {
                if (returnCode == NSAlertFirstButtonReturn) {
                    NSString* inputString = [input stringValue];
                    if (inputString) {
                        const std::string result = std::string([inputString UTF8String]);
                        on_text(result);
                    }
                }
                dispatch_async(dispatch_get_main_queue(), ^{
                    [keyWindow makeKeyAndOrderFront:nil];
                });
            }];
        } 
        else {
            NSModalResponse response = [alert runModal];
            if (response == NSAlertFirstButtonReturn) {
                NSString* inputString = [input stringValue];
                if (inputString) {
                    const std::string result = std::string([inputString UTF8String]);
                    on_text(result);
                }
            }
        }

        [input release];
        [alert release];
    });
}

auto Platform_dialogs::open_url(const std::string& url) -> void
{
    // Copy to locals.
    const auto url_copy = url;

    dispatch_async(dispatch_get_main_queue(), ^{
        NSString* ns_url = [NSString stringWithUTF8String:url_copy.c_str()];
        NSURL* nsurl = [NSURL URLWithString:ns_url];
        if (nsurl) {
            [[NSWorkspace sharedWorkspace] openURL:nsurl];
        }
    });
}

} // namespace tiny

#endif