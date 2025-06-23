#pragma once

#include <AudioUnitSDK/AUEffectBase.h>

#include "cmake_defines.h"
#include "platform_view.h"

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

    ~Auv2_effect() override
    {
        if (platform_view != nullptr) {
            DestroyPlatformView(platform_view);
            platform_view = nullptr;
        }
    };

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
                    auto id = CFStrLocal{bundle_id.c_str()};
                    auto* bundle = CFBundleGetBundleWithIdentifier(id.Get());
                    auto* url = CFBundleCopyBundleURL(bundle);

                    info->mCocoaAUViewBundleLocation = url;
                    info->mCocoaAUViewClass[0] = CFStringCreateWithCString(0, view_class.c_str(), kCFStringEncodingUTF8);
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
        if (platform_view != nullptr) {
            DestroyPlatformView(platform_view);
            platform_view = nullptr;
        }

        platform_view = CreatePlatformView(_delegate.get());
        return platform_view;
    }

private:

    const std::string bundle_id{tiny::Cmake_defines::base_identifier + ".component"};
    const std::string view_class{"AUV2_VIEW_CLASS"};

    std::unique_ptr<Graphics_delegate> _delegate = std::make_unique<Graphics_delegate>(Graphics_delegate::Size{800, 600});
    void* platform_view{nullptr};

};