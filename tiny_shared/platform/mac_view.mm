#import "mac_view.h"
#include "platform_view.h"

@implementation TinyMacView

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

// MARK: - Platform Interface

void* CreatePlatformView(Graphics_delegate* delegate) {
    NSView* view = [[TinyMacView alloc] initWithDelegate:delegate];
    return (void*)view;
}

void DestroyPlatformView(void* view) {
    [(NSView*)view removeFromSuperview];
    [(NSView*)view release]; // Release the Objective-C object
}

void RedrawPlatformView(void* view, Graphics_delegate* delegate) {
    auto size = delegate->getSize();
    [(NSView*)view setFrameSize:NSMakeSize(size.width, size.height)];
    [(NSView*)view setNeedsDisplay:YES];
}

void AttachPlatformView(void* parent, void* view) {
    [(NSView*)parent addSubview: (NSView*)view];
}
