#import "mac_view.h"
#include "platform_view.h"

@implementation TinyMacView

- (void)drawRect:(NSRect)dirtyRect {
    [super drawRect:dirtyRect]; // Call superclass's method (optional)
    
    // Set the fill color to red
    [[NSColor redColor] setFill];
    
    // Fill the entire view bounds with red
    NSRectFill(self.bounds);
}

@end

// MARK: - Platform Interface

void* CreatePlatformView(float width, float height) {
    NSView* view = [[TinyMacView alloc] initWithFrame:NSMakeRect(0, 0, width, height)];
    return (void*)view;
}

void DestroyPlatformView(void* view) {
    [(NSView*)view removeFromSuperview];
    [(NSView*)view release]; // Release the Objective-C object
}

void AttachPlatformView(void* parent, void* view) {
    [(NSView*)parent addSubview: (NSView*)view];
}
