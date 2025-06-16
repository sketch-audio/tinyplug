#import <Cocoa/Cocoa.h>

#include "platform_view.h"

@interface TinyMacView : NSView
{
    Graphics_delegate* graphics_delegate;
}

-(id) initWithDelegate: (Graphics_delegate*)delegate;

@end