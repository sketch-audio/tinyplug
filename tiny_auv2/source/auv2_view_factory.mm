#import <Cocoa/Cocoa.h>

#include <memory>

#include <AudioUnit/AudioUnit.h>
#include <AudioUnit/AUCocoaUIView.h>

#include "plug_info.h"
#include "platform/platform_view.h"

#include "auv2_effect.h"

#ifndef TINY_AUV2_VIEW_CLASS
#error "AUv2 view class not defined."
#endif

@interface TINY_AUV2_VIEW_CLASS : NSObject <AUCocoaUIBase>
- (NSView*)uiViewForAudioUnit:(AudioUnit)audioUnit withSize:(NSSize)preferredSize;
- (unsigned)interfaceVersion;
@end

@implementation TINY_AUV2_VIEW_CLASS

- (NSView*)uiViewForAudioUnit:(AudioUnit)audioUnit withSize:(NSSize)preferredSize {
    void* user_plugin[1] = {nullptr};
    uint32_t size = sizeof(user_plugin);

    AudioUnitGetProperty(audioUnit, kAudioUnitProperty_UserPlugin, kAudioUnitScope_Global, 0, user_plugin, &size);

    if (auto* effect = static_cast<Auv2_effect*>(user_plugin[0]); effect != nullptr) {
        NSView* view = (NSView*)effect->create_view();
        return [view autorelease];
    }

    return nil;
}

- (unsigned)interfaceVersion {
    return 0;
}

@end