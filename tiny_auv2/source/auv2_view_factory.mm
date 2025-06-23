#import <Cocoa/Cocoa.h>

#include <memory>

#include <AudioUnit/AudioUnit.h>
#include <AudioUnit/AUCocoaUIView.h>

#include "auv2_effect.h"
#include "platform_view.h"

@interface AUV2_VIEW_CLASS : NSObject <AUCocoaUIBase>
- (NSView*)uiViewForAudioUnit:(AudioUnit)audioUnit withSize:(NSSize)preferredSize;
- (unsigned)interfaceVersion;
@end

@implementation AUV2_VIEW_CLASS

- (NSView*)uiViewForAudioUnit:(AudioUnit)audioUnit withSize:(NSSize)preferredSize {
    void* user_plugin[1] = {nullptr};
    UInt32 size = sizeof(user_plugin);

    AudioUnitGetProperty(audioUnit, kAudioUnitProperty_UserPlugin, kAudioUnitScope_Global, 0, user_plugin, &size);

    if (auto* effect = static_cast<Auv2_effect*>(user_plugin[0]); effect != nullptr) {
        return (NSView*)effect->create_view();
    }

    return nil;
}

- (unsigned)interfaceVersion {
    return 0;
}

@end