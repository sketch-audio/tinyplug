#import "auv3_AUAudioUnit.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreAudioKit/AUViewController.h>

#import "AUProcessHelper.hpp"
#import "BufferedAudioBus.hpp"
#import "DSPKernel.hpp"

#include "plug_info.h"

@interface Auv3_AUAudioUnit()
@property (nonatomic, readwrite) AUParameterTree *parameterTree;
@property AUAudioUnitBusArray *inputBusArray;
@property AUAudioUnitBusArray *outputBusArray;
@property (nonatomic, readonly) AUAudioUnitBus *outputBus;
@end

@implementation Auv3_AUAudioUnit {
    // Latency reporting.
    dispatch_source_t _dispatchTimer;
    NSTimeInterval _latency;
    
    // C++ members need to be ivars; they would be copied on access if they were properties.
    bool _parameterTreeSetup;
    DSPKernel _kernel;
    
    BufferedInputBus _inputBus;
#if TINY_WANTS_SIDECHAIN
    BufferedInputBus _sidechainBus;
#endif
    NSArray<AUAudioUnitBus *> *_inputBuses;
    
    std::unique_ptr<AUProcessHelper> _processHelper;
}

@synthesize parameterTree = _parameterTree;

- (instancetype)initWithComponentDescription:(AudioComponentDescription)componentDescription options:(AudioComponentInstantiationOptions)options error:(NSError **)outError {
    self = [super initWithComponentDescription:componentDescription options:options error:outError];
    
    if (self == nil) { return nil; }
    
    [self setupAudioBuses];
    _parameterTreeSetup = false;
    
    return self;
}

#pragma mark - AUAudioUnit Setup

- (void)setupAudioBuses {
    // Create the output bus first
    AVAudioFormat *format = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:44100 channels:2];
    _outputBus = [[AUAudioUnitBus alloc] initWithFormat:format error:nil];
    _outputBus.maximumChannelCount = 2;
    
    // Create the input and output busses.
    _inputBus.init(format, 2);
    auto numBuses = 1;
    
#if TINY_WANTS_SIDECHAIN
    // Sidechain?
    _sidechainBus.init(format, 2);
    numBuses += 1;
#endif
    
    NSMutableArray *temp = [[NSMutableArray alloc] initWithCapacity:numBuses];
    for (auto i = decltype(numBuses){}; i < numBuses; ++i) {
        AUAudioUnitBus *bus = [[AUAudioUnitBus alloc] initWithFormat:format error: nil];
        [temp addObject:bus];
    }
    _inputBuses = [temp copy];
    
    // Create the input and output bus arrays.
    _inputBusArray  = [[AUAudioUnitBusArray alloc] initWithAudioUnit:self
                                                             busType:AUAudioUnitBusTypeInput
                                                              busses: _inputBuses];
    // then an array with it
    _outputBusArray = [[AUAudioUnitBusArray alloc] initWithAudioUnit:self
                                                             busType:AUAudioUnitBusTypeOutput
                                                              busses: @[_outputBus]];
}

- (void)setupParameterTree {
    if (_parameterTreeSetup == false) {
        _parameterTree = [AUParameterTree createTreeWithChildren:@[]];
        
        // Send the Parameter default values to the Kernel before setting up the parameter callbacks, so that the defaults set in the Kernel.hpp don't propagate back to the AUParameters via GetParameter
        for (AUParameter *param in _parameterTree.allParameters) {
            _kernel.setParameter(param.address, param.value);
        }
        
        [self setupParameterCallbacks];
        
        _parameterTreeSetup = true;
    }
}

- (void)setupParameterCallbacks {
    // Make a local pointer to the kernel to avoid capturing self.
    
    __block DSPKernel *kernel = &_kernel;
    
    // implementorValueObserver is called when a parameter changes value.
    _parameterTree.implementorValueObserver = ^(AUParameter *param, AUValue value) {
        kernel->setParameter(param.address, value);
    };
    
    // implementorValueProvider is called when the value needs to be refreshed.
    _parameterTree.implementorValueProvider = ^(AUParameter *param) {
        return kernel->getParameter(param.address);
    };
    
    // A function to provide string representations of parameter values.
    _parameterTree.implementorStringFromValueCallback = ^(AUParameter *param, const AUValue *__nullable valuePtr) {
        AUValue value = valuePtr == nil ? param.value : *valuePtr;
        return [NSString stringWithFormat:@"%.1f", value];
    };
}

#pragma mark - AUAudioUnit Overrides

- (AUAudioFrameCount)maximumFramesToRender {
    return _kernel.maximumFramesToRender();
}

- (void)setMaximumFramesToRender:(AUAudioFrameCount)maximumFramesToRender {
    _kernel.setMaximumFramesToRender(maximumFramesToRender);
}

// If an audio unit has input, an audio unit's audio input connection points.
// Subclassers must override this property getter and should return the same object every time.
// See sample code.
- (AUAudioUnitBusArray *)inputBusses {
    return _inputBusArray;
}

// An audio unit's audio output connection points.
// Subclassers must override this property getter and should return the same object every time.
// See sample code.
- (AUAudioUnitBusArray *)outputBusses {
    return _outputBusArray;
}

- (void)setShouldBypassEffect:(BOOL)shouldBypassEffect {
    _kernel.setBypass(shouldBypassEffect);
}

- (BOOL)shouldBypassEffect {
    return _kernel.isBypassed();
}

- (NSArray<NSNumber *> *)channelCapabilities {
    return @[@2, @2, @1, @1]; // Allow mono.
}

- (NSTimeInterval)latency {
    return _latency;
}

- (NSIndexSet *)supportedViewConfigurations:(NSArray<AUAudioUnitViewConfiguration *> *)availableViewConfigurations {
    // https://forum.juce.com/t/auv3-resizing-issue-on-macos-not-ios/43811/10
    if (availableViewConfigurations.count == 0) {
        return [NSIndexSet indexSet]; // Return empty index set for empty array
    }
    
    // Create an index set for all indices from 0 to count-1
    NSIndexSet *indexSet = [NSIndexSet indexSetWithIndexesInRange:NSMakeRange(0, availableViewConfigurations.count)];
    return indexSet;
}

// Allocate resources required to render.
// Subclassers should call the superclass implementation.
- (BOOL)allocateRenderResourcesAndReturnError:(NSError **)outError {
    const auto inputChannelCount = [self.inputBusses objectAtIndexedSubscript:0].format.channelCount;
    const auto outputChannelCount = [self.outputBusses objectAtIndexedSubscript:0].format.channelCount;
    
    if (outputChannelCount != inputChannelCount) {
        if (outError) {
            *outError = [NSError errorWithDomain:NSOSStatusErrorDomain code:kAudioUnitErr_FailedInitialization userInfo:nil];
        }
        // Notify superclass that initialization was not successful
        self.renderResourcesAllocated = NO;
        
        return NO;
    }
    _inputBus.allocateRenderResources(self.maximumFramesToRender);
#if TINY_WANTS_SIDECHAIN
    _sidechainBus.allocateRenderResources(self.maximumFramesToRender);
#endif
    _kernel.setMusicalContextBlock(self.musicalContextBlock);
    _kernel.setTransportStateBlock(self.transportStateBlock);
    _kernel.initialize(inputChannelCount, outputChannelCount, _outputBus.format.sampleRate);
    _processHelper = std::make_unique<AUProcessHelper>(_kernel, inputChannelCount, outputChannelCount);
    [self startDispatchTimer];
    return [super allocateRenderResourcesAndReturnError:outError];
}

// Deallocate resources allocated in allocateRenderResourcesAndReturnError:
// Subclassers should call the superclass implementation.
- (void)deallocateRenderResources {
    [self stopDispatchTimer];
    
    // Deallocate your resources.
    _kernel.deInitialize();
    
    [super deallocateRenderResources];
}

#pragma mark - AUAudioUnit (AUAudioUnitImplementation)

// Block which subclassers must provide to implement rendering.
- (AUInternalRenderBlock)internalRenderBlock {
    /*
     Capture in locals to avoid ObjC member lookups. If "self" is captured in
     render, we're doing it wrong.
     */
    // Specify captured objects are mutable.
    __block DSPKernel *kernel = &_kernel;
    __block std::unique_ptr<AUProcessHelper> &processHelper = _processHelper;
    __block BufferedInputBus *input = &_inputBus;
    
#if TINY_WANTS_SIDECHAIN
    __block BufferedInputBus *sidechain = &_sidechainBus;
#endif
    
    return ^AUAudioUnitStatus(AudioUnitRenderActionFlags                 *actionFlags,
                              const AudioTimeStamp                       *timestamp,
                              AVAudioFrameCount                           frameCount,
                              NSInteger                                   outputBusNumber,
                              AudioBufferList                            *outputData,
                              const AURenderEvent                        *realtimeEventListHead,
                              AURenderPullInputBlock __unsafe_unretained pullInputBlock) {
        
        AudioUnitRenderActionFlags pullFlags = 0;
        
        if (frameCount > kernel->maximumFramesToRender()) {
            return kAudioUnitErr_TooManyFramesToProcess;
        }
        
        // Input
        {
            AUAudioUnitStatus err = input->pullInput(&pullFlags, timestamp, frameCount, 0, pullInputBlock);
            
            if (err != 0) { return err; }
        }
        AudioBufferList *inAudioBufferList = input->mutableAudioBufferList;
        AudioBufferList *sidechainAudioBufferList = nil;
        
#if TINY_WANTS_SIDECHAIN
        // Sidechain
        {
            AUAudioUnitStatus err = sidechain->pullInput(&pullFlags, timestamp, frameCount, 1, pullInputBlock);
            
            if (err != 0) { return err; }
        }
        sidechainAudioBufferList = sidechain->mutableAudioBufferList;
#endif
        
        /*
         Important:
         If the caller passed non-null output pointers (outputData->mBuffers[x].mData), use those.
         
         If the caller passed null output buffer pointers, process in memory owned by the Audio Unit
         and modify the (outputData->mBuffers[x].mData) pointers to point to this owned memory.
         The Audio Unit is responsible for preserving the validity of this memory until the next call to render,
         or deallocateRenderResources is called.
         
         If your algorithm cannot process in-place, you will need to preallocate an output buffer
         and use it here.
         
         See the description of the canProcessInPlace property.
         */
        
        // If passed null output buffer pointers, process in-place in the input buffer.
        AudioBufferList *outAudioBufferList = outputData;
        if (outAudioBufferList->mBuffers[0].mData == nullptr) {
            for (UInt32 i = 0; i < outAudioBufferList->mNumberBuffers; ++i) {
                outAudioBufferList->mBuffers[i].mData = inAudioBufferList->mBuffers[i].mData;
            }
        }
        
        processHelper->processWithEvents(inAudioBufferList,
                                         sidechainAudioBufferList,
                                         outAudioBufferList,
                                         timestamp,
                                         frameCount,
                                         realtimeEventListHead);
        return noErr;
    };
}

// MARK: - Full State

-(NSDictionary<NSString *,id> *)fullState {
    return [super fullState];
}

- (void)setFullState:(NSDictionary<NSString *,id> *)fullState {
    [super setFullState:fullState];
}

// MARK: - Timer

- (void)startDispatchTimer {
    // See: https://stackoverflow.com/questions/21563825/timer-in-another-thread-in-objective-c
    if (_dispatchTimer != nil) {
        [self stopDispatchTimer];
    }

    _dispatchTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0));
    double interval = 1 / 30.0;
    dispatch_time_t startTime = dispatch_time(DISPATCH_TIME_NOW, 0);
    uint64_t intervalTime = (uint64_t)(interval * NSEC_PER_SEC);
    dispatch_source_set_timer(_dispatchTimer, startTime, intervalTime, 0);
    
    __weak Auv3_AUAudioUnit *weakSelf = self;
    __block DSPKernel *kernel = &_kernel;

    // Attach the block you want to run on the timer fire
    dispatch_source_set_event_handler(_dispatchTimer, ^{
        //
        Auv3_AUAudioUnit *strongSelf = weakSelf;
        if (strongSelf == nil) { return; }
        
        // Latency reporting
        const auto latency_secs = kernel->latency_ms() / 1000;
        
        if (weakSelf.latency != latency_secs) {
            [weakSelf willChangeValueForKey:@"latency"];
            strongSelf->_latency = latency_secs;
            [weakSelf didChangeValueForKey:@"latency"];
        }
    });

    // Start the timer
    dispatch_resume(_dispatchTimer);
}

- (void)stopDispatchTimer {
    if (_dispatchTimer) {
        dispatch_cancel(_dispatchTimer);
        _dispatchTimer = nil;
    }
}

@end
