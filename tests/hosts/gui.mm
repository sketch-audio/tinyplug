#include "gui.hpp"

#import <AppKit/AppKit.h>
#import <AudioToolbox/AudioToolbox.h>
#import <AudioUnit/AUCocoaUIView.h>

namespace tiny::hosts {

auto gui_init() -> void
{
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    [NSApp finishLaunching];
}

Window::Window(double width, double height)
{
    NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(100, 100, width, height)
                                                   styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
    window.releasedWhenClosed = NO;
    window.alphaValue = 0;
    // Not movable: a synthetic mouse-down where AppKit thinks the window can be dragged enters its
    // drag-tracking loop, which waits for a real mouse-up from the event queue and never gets one.
    window.movable = NO;
    [window makeKeyAndOrderFront:nil];
    _window = (__bridge_retained void*)window;
}

Window::~Window()
{
    NSWindow* window = (__bridge_transfer NSWindow*)_window;
    [window orderOut:nil];
    [window close];
}

auto Window::content() const -> void* { return (__bridge void*)((__bridge NSWindow*)_window).contentView; }

auto Window::editor_view() const -> void*
{
    return (__bridge void*)((__bridge NSWindow*)_window).contentView.subviews.lastObject;
}

auto Window::add(void* view) -> void
{
    NSView* v = (__bridge NSView*)view;
    NSView* content = ((__bridge NSWindow*)_window).contentView;
    [v setFrameOrigin:NSMakePoint(0, content.bounds.size.height - v.frame.size.height)];
    [content addSubview:v];
}

auto Window::remove(void* view) -> void { [(__bridge NSView*)view removeFromSuperview]; }

namespace {

// Straight to the view under the point, as NSWindow would route it. Going through -[NSWindow sendEvent:]
// instead can enter AppKit's mouse-hysteresis loop, which waits on the real event queue forever.
auto deliver(NSView* root, NSEventType type, NSPoint at, NSInteger clicks) -> void
{
    NSWindow* window = root.window;
    NSEvent* e = [NSEvent mouseEventWithType:type location:at modifierFlags:0 timestamp:NSProcessInfo.processInfo.systemUptime
                                windowNumber:window.windowNumber context:nil eventNumber:0 clickCount:clicks pressure:1];
    NSView* target = [root hitTest:[root.superview convertPoint:at fromView:nil]];
    if (!target) return;
    switch (type) {
        case NSEventTypeLeftMouseDown: [target mouseDown:e]; break;
        case NSEventTypeLeftMouseDragged: [target mouseDragged:e]; break;
        case NSEventTypeLeftMouseUp: [target mouseUp:e]; break;
        case NSEventTypeRightMouseDown: [target rightMouseDown:e]; break;
        case NSEventTypeRightMouseUp: [target rightMouseUp:e]; break;
        default: [target mouseMoved:e]; break;
    }
}

} // namespace

auto send_input(void* view, Random& random, int events) -> void
{
    NSView* v = (__bridge NSView*)view;
    NSWindow* window = v.window;
    if (!window || v.bounds.size.width <= 0 || v.bounds.size.height <= 0) return;
    auto point = [&] {
        const auto p = NSMakePoint(random.real(0, v.bounds.size.width), random.real(0, v.bounds.size.height));
        return [v convertPoint:p toView:nil];
    };
    for (auto n = 0; n < events; ++n) {
        switch (random.below(6)) {
            case 0: { const auto p = point(); deliver(v, NSEventTypeLeftMouseDown, p, 1); deliver(v, NSEventTypeLeftMouseUp, p, 1); break; }
            case 1: {
                auto p = point();
                deliver(v, NSEventTypeLeftMouseDown, p, 1);
                for (auto k = random.below(8); k > 0; --k) {
                    p.x += random.real(-40, 40);
                    p.y += random.real(-40, 40);
                    deliver(v, NSEventTypeLeftMouseDragged, p, 1);
                }
                deliver(v, NSEventTypeLeftMouseUp, p, 1);
                break;
            }
            case 2: { const auto p = point(); for (auto c = 1; c <= 2; ++c) { deliver(v, NSEventTypeLeftMouseDown, p, c); deliver(v, NSEventTypeLeftMouseUp, p, c); } break; }
            case 3: { const auto p = point(); deliver(v, NSEventTypeRightMouseDown, p, 1); deliver(v, NSEventTypeRightMouseUp, p, 1); break; }
            case 4: deliver(v, NSEventTypeMouseMoved, point(), 0); break;
            default: {
                const auto p = [window convertPointToScreen:point()];
                CGEventRef cg = CGEventCreateScrollWheelEvent(nullptr, kCGScrollEventUnitPixel, 2,
                                                              static_cast<int32_t>(random.real(-30, 30)), static_cast<int32_t>(random.real(-30, 30)));
                const auto screen_h = NSScreen.screens.firstObject.frame.size.height;
                CGEventSetLocation(cg, CGPointMake(p.x, screen_h - p.y));
                NSEvent* e = [NSEvent eventWithCGEvent:cg];
                CFRelease(cg);
                NSView* target = e ? [v hitTest:[v.superview convertPoint:[window convertPointFromScreen:p] fromView:nil]] : nil;
                if (target) [target scrollWheel:e];
                break;
            }
        }
    }
}

auto make_au_cocoa_view(void* audio_unit) -> void*
{
    auto unit = static_cast<AudioUnit>(audio_unit);
    auto size = UInt32{};
    if (AudioUnitGetPropertyInfo(unit, kAudioUnitProperty_CocoaUI, kAudioUnitScope_Global, 0, &size, nullptr) != noErr || size == 0) return nullptr;
    auto* info = static_cast<AudioUnitCocoaViewInfo*>(std::malloc(size));
    if (AudioUnitGetProperty(unit, kAudioUnitProperty_CocoaUI, kAudioUnitScope_Global, 0, info, &size) != noErr) { std::free(info); return nullptr; }
    Class factory_class = NSClassFromString((__bridge NSString*)info->mCocoaAUViewClass[0]);
    const auto classes = (size - sizeof(CFURLRef)) / sizeof(CFStringRef);
    CFRelease(info->mCocoaAUViewBundleLocation);
    for (auto i = size_t{}; i < classes; ++i) CFRelease(info->mCocoaAUViewClass[i]);
    std::free(info);
    if (!factory_class) return nullptr;
    id<AUCocoaUIBase> factory = [[factory_class alloc] init];
    NSView* view = [factory uiViewForAudioUnit:unit withSize:NSMakeSize(0, 0)];
    return view ? (__bridge_retained void*)view : nullptr;
}

auto release_view(void* view) -> void
{
    if (view) (void)(__bridge_transfer NSView*)view;
}

} // namespace tiny::hosts
