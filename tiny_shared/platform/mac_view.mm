#import <Cocoa/Cocoa.h>

#include "platform_view.h"

// TinyMacView
@interface TinyMacView : NSView
- (id)initWithDelegate: (Graphics_delegate*)delegate;
@end

@implementation TinyMacView {
    Graphics_delegate* graphics_delegate;
}

- (id)initWithDelegate:(Graphics_delegate*)delegate {
    graphics_delegate = delegate;
    const auto size = delegate->getSize();
    self = [super initWithFrame:NSMakeRect(0, 0, size.width, size.height)];
    return self;
}

- (void)drawRect:(NSRect)dirtyRect {
    [super drawRect:dirtyRect]; // Call superclass's method (optional)
    
    // Set the fill color to red
    [[NSColor redColor] setFill];
    NSRectFill(self.bounds);

    // Graphics delegate paints blue
    if (graphics_delegate) {
        auto* cgContext = [NSGraphicsContext currentContext].CGContext;
        auto* platformContext = [NSGraphicsContext graphicsContextWithCGContext: cgContext flipped: YES].CGContext;
        graphics_delegate->draw(platformContext);
    }
}
@end

Platform_view::Platform_view(std::shared_ptr<Graphics_delegate> delegate) : _delegate{delegate}
{
    _view = [[TinyMacView alloc] initWithDelegate:_delegate.get()];
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
    [(NSView*)_view setNeedsDisplay:YES];
}

auto Platform_view::redraw() -> void
{
    [(NSView*)_view setNeedsDisplay:YES];
}