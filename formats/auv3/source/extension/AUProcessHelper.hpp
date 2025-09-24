#pragma once

#import <AudioToolbox/AudioToolbox.h>
#import <AVFoundation/AVFoundation.h>

#include <vector>
#include "DSPKernel.hpp"

// MARK:- AUProcessHelper Utility Class
class AUProcessHelper
{
    static constexpr auto max_ichannels{2};
    static constexpr auto max_ochannels{2};
    static constexpr auto max_schannels{2};
public:
    AUProcessHelper(DSPKernel& kernel, UInt32 inputChannelCount, UInt32 outputChannelCount)
    : mKernel{kernel},
    mInputChannelCount(inputChannelCount),
    mOutputChannelCount(outputChannelCount),
    mInputBuffers(max_ichannels), // Here we reserve the max space.
    mSidechainBuffers(max_schannels),
    mOutputBuffers(max_ochannels) {
        assert(inputChannelCount > 0 && inputChannelCount <= max_ichannels);
        assert(outputChannelCount > 0 && outputChannelCount <= max_ochannels);
    }

    /**
     This function handles the event list processing and rendering loop for you.
     Call it inside your internalRenderBlock.
     */
    void processWithEvents(AudioBufferList* inBufferList, AudioBufferList* sidechainBufferList, AudioBufferList* outBufferList, AudioTimeStamp const *timestamp, AUAudioFrameCount frameCount, AURenderEvent const *events) {

        AUEventSampleTime now = AUEventSampleTime(timestamp->mSampleTime);
        AUAudioFrameCount framesRemaining = frameCount;
        AURenderEvent const *nextEvent = events; // events is a linked list, at the beginning, the nextEvent is the first event

        auto callProcess = [this] (AudioBufferList* inBufferListPtr,
                                   AudioBufferList* sidechainBufferList,
                                   AudioBufferList* outBufferListPtr,
                                   AUEventSampleTime now,
                                   AUAudioFrameCount frameCount,
                                   AUAudioFrameCount const frameOffset) {
            
            const auto num_ichannels = inBufferListPtr->mNumberBuffers;
            assert(num_ichannels == mInputChannelCount && "Channel mismatch!");
            for (int channel = 0; channel < num_ichannels; ++channel) {
                mInputBuffers[channel] = (const float*)inBufferListPtr->mBuffers[channel].mData  + frameOffset;
            }
            
            auto num_schannels = 0;
            if (sidechainBufferList != nil) {
                num_schannels = sidechainBufferList->mNumberBuffers;
                for (int channel = 0; channel < num_schannels; ++channel) {
                    mSidechainBuffers[channel] = (const float*)inBufferListPtr->mBuffers[channel].mData  + frameOffset;
                }
            }
            
            const auto num_ochannels = outBufferListPtr->mNumberBuffers;
            assert(num_ochannels == mOutputChannelCount && "Channel mismatch!");
            for (int channel = 0; channel < num_ochannels; ++channel) {
                mOutputBuffers[channel] = (float*)outBufferListPtr->mBuffers[channel].mData + frameOffset;
            }
            
            // Make the spans with the right channel counts.
            mKernel.process({mInputBuffers.begin(), static_cast<size_t>(num_ichannels)},
                            {mSidechainBuffers.begin(), static_cast<size_t>(num_schannels)},
                            {mOutputBuffers.begin(), static_cast<size_t>(num_ochannels)},
                            now, frameCount);
        };
        
        while (framesRemaining > 0) {
            // If there are no more events, we can process the entire remaining segment and exit.
            if (nextEvent == nullptr) {
                AUAudioFrameCount const frameOffset = frameCount - framesRemaining;
                callProcess(inBufferList, sidechainBufferList, outBufferList, now, framesRemaining, frameOffset);
                return;
            }

            // **** start late events late.
            auto timeZero = AUEventSampleTime(0);
            auto headEventTime = nextEvent->head.eventSampleTime;
            AUAudioFrameCount framesThisSegment = AUAudioFrameCount(std::max(timeZero, headEventTime - now));

            // Compute everything before the next event.
            if (framesThisSegment > 0) {
                AUAudioFrameCount const frameOffset = frameCount - framesRemaining;

                callProcess(inBufferList, sidechainBufferList, outBufferList, now, framesThisSegment, frameOffset);

                // Advance frames.
                framesRemaining -= framesThisSegment;

                // Advance time.
                now += AUEventSampleTime(framesThisSegment);
            }

            nextEvent = performAllSimultaneousEvents(now, nextEvent);
        }
    }

    AURenderEvent const * performAllSimultaneousEvents(AUEventSampleTime now, AURenderEvent const *event) {
        do {
            mKernel.handleOneEvent(now, event);
            
            // Go to next event.
            event = event->head.next;

            // While event is not null and is simultaneous (or late).
        } while (event && event->head.eventSampleTime <= now);
        return event;
    }
private:
    DSPKernel& mKernel;
    UInt32 mInputChannelCount;
    UInt32 mOutputChannelCount;
    std::vector<const float*> mInputBuffers;
    std::vector<const float*> mSidechainBuffers;
    std::vector<float*> mOutputBuffers;
};
