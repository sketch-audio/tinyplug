#include "platform.h"

#if PLATFORM_MACOS
#include <chrono>

#import <Cocoa/Cocoa.h>
#import <CoreVideo/CVDisplayLink.h>

#include "platform_view.h"
#include "window_context.h"

#include "mac_config.h"
#ifndef TINY_MAC_VIEW
#error "TINY_MAC_VIEW must be defined in mac_config.h"
#endif

// TINY_MAC_VIEW
@interface TINY_MAC_VIEW : NSView
- (id)initWithDelegate:(std::shared_ptr<tiny::View_delegate>)delegate onAutorelease:(std::function<void()>)onAutorelease;
- (void)startDisplayLink;
- (void)stopDisplayLink;
- (void)draw;
@end

// 
static auto on_display_link(CVDisplayLinkRef, const CVTimeStamp*, const CVTimeStamp*, CVOptionFlags, CVOptionFlags*, void* context) -> CVReturn
{
    auto source = static_cast<dispatch_source_t>(context);
    dispatch_source_merge_data(source, 1);
    return kCVReturnSuccess;
}

@implementation TINY_MAC_VIEW {
    std::shared_ptr<tiny::View_delegate> _delegate;
    std::function<void()> _on_autorelease;

    // Interaction state
    tiny::User_interaction _interaction;
    tiny::Pointer _pointer; // On macOS there is only one pointer.
    std::chrono::steady_clock::time_point _over_time;
    std::optional<tiny::Coords> _over_pos;
    std::optional<tiny::Coords> _left_pos;
    std::optional<tiny::Coords> _right_pos;
    std::optional<tiny::Coords> _drag_start;
    bool _dwelt;

    CVDisplayLinkRef _displayLink;
    dispatch_source_t _displaySource;
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
    _on_autorelease(); // Need this for AUv2
    [self stopDisplayLink];
    [super dealloc];
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

    // Should we dwell?
    if (_over_pos && !_dwelt) {
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - _over_time);
        if (elapsed.count() > 2000) {
            _dwelt = tiny::try_set(_pointer.state, tiny::Dwell{*_over_pos});
        }
    }

    // Resolve modifiers
    NSEventModifierFlags flags = [NSEvent modifierFlags];
    _interaction.modifier_keys = tiny::Modifier_keys{
        .primary = (flags & NSEventModifierFlagCommand) != 0,
        .alt = (flags & NSEventModifierFlagOption) != 0,
        .shift = (flags & NSEventModifierFlagShift) != 0,
    };

    // Copy pointer state into the interaction.
    _interaction.pointers = {_pointer};

    _delegate->draw(_interaction, time_now);
    tiny::try_set(_pointer.state, tiny::Consumed{});
    _interaction.scroll_deltas = tiny::Coords{0, 0}; // Consume deltas

    if (_dwelt) {
        _over_pos = std::nullopt;
        _dwelt = false;
    }
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

    NSTrackingAreaOptions options = NSTrackingMouseEnteredAndExited | NSTrackingMouseMoved | NSTrackingActiveInKeyWindow;
    NSTrackingArea *area = [[NSTrackingArea alloc] initWithRect:self.bounds
                                                        options:options
                                                          owner:self
                                                       userInfo:nil];
    
    [self addTrackingArea:area];
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

- (void)mouseDown:(NSEvent *)event {
    const auto y = self.bounds.size.height - event.locationInWindow.y;
    const auto pos = tiny::Coords{event.locationInWindow.x, y};
    const auto ctrl = (event.modifierFlags & NSEventModifierFlagControl) != 0;
    
    if (ctrl) {
        tiny::try_set(_pointer.state, tiny::Right_click{pos});
        _over_pos = std::nullopt;
    } 
    else {
        tiny::try_set(_pointer.state, tiny::Down{pos});
        _over_pos = std::nullopt;
        _left_pos = pos;
    }
    [super mouseDown:event];
}

- (void)mouseUp:(NSEvent *)event {
    const auto y = self.bounds.size.height - event.locationInWindow.y;
    const auto pos = tiny::Coords{event.locationInWindow.x, y};
    const auto click_count = event.clickCount;

    if (_drag_start) {
        tiny::try_set(_pointer.state, tiny::Drag_end{*_drag_start, pos});
        _over_pos = std::nullopt;
        _drag_start = std::nullopt;
    } 
    else if (_left_pos) {
        if (click_count == 2) {
            tiny::try_set(_pointer.state, tiny::Double_click{pos});
            _over_pos = std::nullopt;
        } 
        else {
            tiny::try_set(_pointer.state, tiny::Click{pos});
            _over_pos = std::nullopt;
        }
        _left_pos = std::nullopt;
    }
    [super mouseUp:event];
}

- (void)mouseMoved:(NSEvent *)event {
    const auto y = self.bounds.size.height - event.locationInWindow.y;
    const auto pos = tiny::Coords{event.locationInWindow.x, y};
    const auto t = std::chrono::steady_clock::now();

    tiny::try_set(_pointer.state, tiny::Over{pos});

    // Update dwell.
    if (!_over_pos || *_over_pos != pos) {
        _over_pos = pos;
        _over_time = t;
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
        tiny::try_set(_pointer.state, tiny::Drag_start{*_drag_start, pos});
        _over_pos = std::nullopt;
    } 
    else if (_drag_start) {
        tiny::try_set(_pointer.state, tiny::Drag{*_drag_start, pos});
        _over_pos = std::nullopt;
    }
    [super mouseDragged:event];
}

- (void)rightMouseDown:(NSEvent *)event {
    const auto y = self.bounds.size.height - event.locationInWindow.y;
    const auto pos = tiny::Coords{event.locationInWindow.x, y};
    tiny::try_set(_pointer.state, tiny::Right_click{pos});
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
    _interaction.inertial_scroll = (event.momentumPhase != NSEventPhaseNone); // implicit scroll stop
    [super scrollWheel:event];
}

- (void)mouseEntered:(NSEvent *)event {
    const auto y = self.bounds.size.height - event.locationInWindow.y;
    const auto pos = tiny::Coords{event.locationInWindow.x, y};
    tiny::try_set(_pointer.state, tiny::Over{pos});
    _over_pos = std::nullopt;
    [super mouseEntered:event];
}

- (void)mouseExited:(NSEvent *)event {
    tiny::try_set(_pointer.state, tiny::Consumed{});
    _over_pos = std::nullopt;
    [super mouseExited:event];
}

@end

namespace tiny {

// MARK: - Platform view

Platform_view::Platform_view(std::shared_ptr<View_delegate> delegate, bool owns_view, std::function<void()> on_release) : _owns_view{owns_view}, _delegate{delegate}
{
    NSView* view = [[TINY_MAC_VIEW alloc] initWithDelegate:_delegate onAutorelease:on_release];

    auto context = std::make_unique<Window_context>();
    context->setup({.native_handle = static_cast<void*>(view)});
    _delegate->assign_context(std::move(context));

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
    if (auto view = static_cast<TINY_MAC_VIEW*>(_view)) {
        [view stopDisplayLink];
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

// MARK: - Platform dialogs 

auto Platform_dialogs::message(const std::string& title, const std::string& message, Later<> on_done) -> void
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
            [alert beginSheetModalForWindow:keyWindow completionHandler:^(NSModalResponse /*returnCode*/) {
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

auto Platform_dialogs::confirm(const std::string& title, const std::string& message, Later<bool> on_confirm) -> void
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

auto Platform_dialogs::text_input(const std::string& title, const std::string& message, Later<std::string> on_text) -> void
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

auto Platform_dialogs::open_file(const std::string& title, const std::string& default_path, Later<std::optional<std::string>> on_open) -> void
{
    // Copy to locals.
    const auto title_copy = title;
    const auto default_path_copy = default_path;

    dispatch_async(dispatch_get_main_queue(), ^{
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        [panel setTitle:[NSString stringWithUTF8String:title_copy.c_str()]];
        if (!default_path_copy.empty()) {
            NSString* ns_path = [NSString stringWithUTF8String:default_path_copy.c_str()];
            NSURL* url = [NSURL fileURLWithPath:ns_path];
            if (url) {
                [panel setDirectoryURL:url];
            }
        }
        [panel setCanChooseFiles:YES];
        [panel setCanChooseDirectories:NO];
        [panel setAllowsMultipleSelection:NO];

        NSWindow* keyWindow = [[NSApplication sharedApplication] keyWindow];
        if (keyWindow != nil) {
            [panel beginSheetModalForWindow:keyWindow completionHandler:^(NSModalResponse result) {
                if (result == NSModalResponseOK) {
                    NSURL* selectedURL = [[panel URLs] firstObject];
                    if (selectedURL) {
                        NSString* path = [selectedURL path];
                        const std::string result_path = std::string([path UTF8String]);
                        on_open(result_path);
                    } else {
                        on_open(std::nullopt);
                    }
                } else {
                    on_open(std::nullopt);
                }
                dispatch_async(dispatch_get_main_queue(), ^{
                    [keyWindow makeKeyAndOrderFront:nil];
                });
            }];
        } 
        else {
            NSModalResponse result = [panel runModal];
            if (result == NSModalResponseOK) {
                NSURL* selectedURL = [[panel URLs] firstObject];
                if (selectedURL) {
                    NSString* path = [selectedURL path];
                    const std::string result_path = std::string([path UTF8String]);
                    on_open(result_path);
                } else {
                    on_open(std::nullopt);
                }
            } else {
                on_open(std::nullopt);
            }
        }
    });
}

} // namespace tiny

#endif