#pragma once

#include <array>

#include <AudioUnitSDK/AUEffectBase.h>

#include "plug_info.h"
#include "platform/platform_view.h"

#pragma mark - CFString and CString Utilities

static inline CFStringRef MakeCFString(const char* cStr)
{
  return CFStringCreateWithCString(0, cStr, kCFStringEncodingUTF8);
}

class CFStrLocal {
public:
  CFStrLocal(const char* cStr)
  {
    mCFStr = MakeCFString(cStr);
  }
    
  ~CFStrLocal()
  {
    CFRelease(mCFStr);
  }
    
  CFStrLocal(const CFStrLocal&) = delete;
  CFStrLocal& operator=(const CFStrLocal&) = delete;
    
  CFStringRef Get() { return mCFStr; }
    
private:
  CFStringRef mCFStr;
};

class Auv2_effect : public ausdk::AUEffectBase {
public:

    Auv2_effect(AudioUnit component) : ausdk::AUEffectBase(component) {}

    //~Auv2_effect() override {}

    OSStatus GetPropertyInfo(AudioUnitPropertyID inID, AudioUnitScope inScope, AudioUnitElement inElement, UInt32& outDataSize, bool& outWritable) override
    {
        if (inScope == kAudioUnitScope_Global) {
            switch (inID) {
                case kAudioUnitProperty_CocoaUI: {
                    outDataSize = sizeof(AudioUnitCocoaViewInfo);
                    outWritable = true;
                    return noErr;
                }
                case kAudioUnitProperty_UserPlugin: {
                    outDataSize = sizeof(void*);
                    outWritable = false;
                    return noErr;
                }
                default: break;
            }
        }

        return ausdk::AUEffectBase::GetPropertyInfo(inID, inScope, inElement, outDataSize, outWritable);
    }

    OSStatus GetProperty(AudioUnitPropertyID inID, AudioUnitScope inScope, AudioUnitElement inElement, void* outData) override
    {
        if (inScope == kAudioUnitScope_Global) {
            switch (inID) {
                case kAudioUnitProperty_CocoaUI: {
                    auto* info = static_cast<AudioUnitCocoaViewInfo*>(outData);

                    // Bundle
                    auto id = CFStrLocal{tiny::Plug_info::Auv2::bundle_id};
                    auto* bundle = CFBundleGetBundleWithIdentifier(id.Get());
                    auto* url = CFBundleCopyBundleURL(bundle);

                    info->mCocoaAUViewBundleLocation = url;
                    info->mCocoaAUViewClass[0] = CFStringCreateWithCString(0, tiny::Plug_info::Auv2::view_class, kCFStringEncodingUTF8);
                    return noErr;
                }
                case kAudioUnitProperty_UserPlugin: {
                    ((void**)outData)[0] = (void*)this;
                    return noErr;
                }
                default: break;
            }
        }
        return ausdk::AUEffectBase::GetProperty(inID, inScope, inElement, outData);
    }

    auto create_view() -> void*
    {
        platform_view = Platform_views::make_autoreleasing(_delegate);
        return platform_view->native_handle();
    }

private:

    std::shared_ptr<Graphics_delegate> _delegate = std::make_shared<Graphics_delegate>(Graphics_delegate::Size{800, 600});
    std::unique_ptr<Platform_view> platform_view{nullptr};

};