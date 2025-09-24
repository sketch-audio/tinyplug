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
@property NSMutableDictionary<NSNumber*, id>* observerTokens;
@end

@implementation Auv3_AUAudioUnit {
    // Latency reporting.
    dispatch_source_t _dispatchTimer;
    NSTimeInterval _latency;
    
    // C++ members need to be ivars; they would be copied on access if they were properties.
    bool _parameterTreeSetup;
    tiny::Param_infos<tiny::Param_model> _param_infos;
    DSPKernel _kernel;
    std::shared_ptr<tiny::Plug_editor> _editor;
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
    using namespace tiny;
    
    if (_parameterTreeSetup == false) {
        // Build the tree
        _param_infos = Param_infos<Param_model>{};
        const auto tree = _param_infos.tree();
        
        // Traverse the tree, creating parameters and groups along the way.
        auto make_node = [&](auto&& self_, const Param_node& node) -> AUParameterNode* {
            return std::visit(Inline_visitor{
                [&](const Param_spec& spec) -> AUParameterNode* {
                    return [self makeParameterFor:spec];
                },
                [&](const Param_group& group) -> AUParameterNode* {
                    NSMutableArray<AUParameterNode*>* children = [NSMutableArray array];
                    for (const auto& child : group.nodes) {
                        [children addObject:self_(self_, child)];
                    }
                    NSString* identifier = [NSString stringWithUTF8String:group.string_id];
                    NSString* name = [NSString stringWithUTF8String:group.name];
                    return [AUParameterTree createGroupWithIdentifier:identifier name:name children:children];
                }
            }, node);
        };
        
        // Most likely the root is a group, don't create a named group for it.
        if (const auto* g = std::get_if<Param_group>(&tree)) {
            NSMutableArray<AUParameterNode*>* rootChildren = [NSMutableArray array];
            for (auto const& child : g->nodes) {
                [rootChildren addObject:make_node(make_node, child)];
            }
            AUParameterTree* parameter_tree = [AUParameterTree createTreeWithChildren:rootChildren];
            _parameterTree = parameter_tree;
        }
        else if (const auto* s = std::get_if<Param_spec>(&tree)) {
            AUParameter *parameter = [self makeParameterFor:*s];
            AUParameterTree *parameter_tree = [AUParameterTree createTreeWithChildren:@[parameter]];
            _parameterTree = parameter_tree;
        }
        
        // Send the Parameter default values to the Kernel before setting up the parameter callbacks, so that the defaults set in the Kernel.hpp don't propagate back to the AUParameters via GetParameter
        for (AUParameter *param in _parameterTree.allParameters) {
            _kernel.setParameter(param.address, param.value);
        }
        
        [self setupParameterCallbacks];
        
        _parameterTreeSetup = true;
    }
}

// MARK: - makeReceiver

- (tiny::Ui_receiver)makeReceiver {
    using namespace tiny;
    __weak typeof(self) self_ = self;
    return Ui_receiver{
        .get_knob_value = [self_](uint32_t addr) {
            auto s = self_;
            if (!s) return double{};
            const auto& spec = s->_param_infos.param_for(addr);
            const auto host = s->_kernel.getParameter(addr);
            const auto knob = Value_conv::host_to_knob(host, spec.semantics);
            return knob;
        },
        .pop_event = [self_](Ui_event& event) {
            auto s = self_;
            if (!s) return false;
            return s->_kernel.pop_event(event);
        },
        .action_handler = [self_](const User_action& action) {
            auto s = self_;
            if (!s) return;
            std::visit(Inline_visitor{
                [&](const Action_start& a) {
                    auto current = s->_kernel.getParameter(a.id);
                    auto* auparam = [s->_parameterTree parameterWithAddress:a.id];
                    auto token = (__bridge AUParameterObserverToken)s->_observerTokens[@(auparam.address)];
                    [auparam setValue:current originator:token atHostTime:0 eventType:AUParameterAutomationEventTypeTouch];
                },
                [&](const Set_param& a) {
                    const auto& param = s->_param_infos.param_for(a.id);
                    const auto host_value = Value_conv::knob_to_host(a.value, param.semantics);
                    auto* auparam = [s->_parameterTree parameterWithAddress:a.id];
                    auto token = (__bridge AUParameterObserverToken)s->_observerTokens[@(auparam.address)];
                    [auparam setValue:host_value originator:token atHostTime:0 eventType:AUParameterAutomationEventTypeValue];
                },
                [&](const Action_end& a) {
                    auto current = s->_kernel.getParameter(a.id);
                    auto* auparam = [s->_parameterTree parameterWithAddress:a.id];
                    auto token = (__bridge AUParameterObserverToken)s->_observerTokens[@(auparam.address)];
                    [auparam setValue:current originator:token atHostTime:0 eventType:AUParameterAutomationEventTypeRelease];
                },
            }, action);
        }
    };
}

- (void)setEditor:(std::shared_ptr<tiny::Plug_editor>)editor {
    _editor = editor;
}

// MARK: - makeParameterFor

- (AUParameter*)makeParameterFor:(tiny::Param_spec)spec {
    using namespace tiny;

    NSString* identifier = [NSString stringWithUTF8String:spec.string_id];
    NSString* name = [NSString stringWithUTF8String:spec.name];

    auto flags_for = [](Host_policy policy) -> AudioUnitParameterOptions {
        switch (policy) {
            case Host_policy::automation: return kAudioUnitParameterFlag_IsWritable | kAudioUnitParameterFlag_IsReadable;
            case Host_policy::control: return kAudioUnitParameterFlag_IsReadable;
            case Host_policy::state: return {};
            case Host_policy::interface: return kAudioUnitParameterFlag_OmitFromPresets;
            default: return kAudioUnitParameterFlag_IsWritable | kAudioUnitParameterFlag_IsReadable;
        }
    };

    AUParameter* parameter = std::visit(Inline_visitor{
        [&](const Bool_semantics& b) {
            AUParameter* param = [AUParameterTree createParameterWithIdentifier:identifier
                                                                           name:name
                                                                        address:spec.id
                                                                            min:0
                                                                            max:1
                                                                            unit:kAudioUnitParameterUnit_Boolean
                                                                        unitName:nil
                                                                            flags:flags_for(spec.policy) | kAudioUnitParameterFlag_ValuesHaveStrings
                                                                    valueStrings:nil
                                                            dependentParameters:nil];
            param.value = b.def_val ? 1.f : 0.f;
            return param;
        },
        [&](const List_semantics& l) {
            NSMutableArray<NSString*>* valueStrings = [NSMutableArray array];
            for (const auto& item : l.items) {
                [valueStrings addObject:[NSString stringWithUTF8String:item]];
            }
            AUParameter* param = [AUParameterTree createParameterWithIdentifier:identifier
                                                                           name:name
                                                                        address:spec.id
                                                                            min:0
                                                                            max:static_cast<float>(l.items.size() - 1)
                                                                           unit:kAudioUnitParameterUnit_Indexed
                                                                       unitName:nil
                                                                          flags:flags_for(spec.policy) | kAudioUnitParameterFlag_ValuesHaveStrings
                                                                   valueStrings:valueStrings
                                                            dependentParameters:nil];
            param.value = static_cast<float>(l.def_val);
            return param;
        },
        [&](const Int_semantics& i) {
            AUParameter* param = [AUParameterTree createParameterWithIdentifier:identifier
                                                                           name:name
                                                                        address:spec.id
                                                                            min:static_cast<float>(i.min_val)
                                                                            max:static_cast<float>(i.max_val)
                                                                           unit:kAudioUnitParameterUnit_Indexed
                                                                       unitName:nil
                                                                          flags:flags_for(spec.policy) | kAudioUnitParameterFlag_ValuesHaveStrings
                                                                   valueStrings:nil
                                                            dependentParameters:nil];
            param.value = static_cast<float>(i.def_val);
            return param;
        },
        [&](const Real_semantics& r) {
            AUParameter* param = [AUParameterTree createParameterWithIdentifier:identifier
                                                                           name:name
                                                                        address:spec.id
                                                                            min:0
                                                                            max:1
                                                                           unit:kAudioUnitParameterUnit_Generic
                                                                       unitName:nil
                                                                          flags:flags_for(spec.policy) | kAudioUnitParameterFlag_ValuesHaveStrings
                                                                   valueStrings:nil
                                                            dependentParameters:nil];
            param.value = static_cast<float>(Value_conv::plain_to_host(r.def_val, r));
            return param;
        },
    }, spec.semantics);
    
    return parameter;
}

- (void)setupParameterCallbacks {
    // Make a local pointer to the kernel to avoid capturing self.
    __block tiny::Param_infos<tiny::Param_model> *param_infos = &_param_infos;
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
        const auto& spec = param_infos->param_for(static_cast<uint32_t>(param.address));
        const auto str_value = tiny::Host_formatter::format_string(param.value, spec.semantics);
        return [NSString stringWithUTF8String:str_value.c_str()];
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
    using namespace tiny;
    return Plug_info::can_process_mono ? @[@2, @2, @1, @1] : @[@2, @2];
}

- (NSTimeInterval)latency {
    return _latency;
}

- (NSTimeInterval)tailTime {
    return _kernel.tail_secs();
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

    // go through all parameters
    __block DSPKernel *kernel = &_kernel;
    for (AUParameter *param in _parameterTree.allParameters) {
        AUParameterObserverToken token = [param tokenByAddingParameterObserver:^(AUParameterAddress address, AUValue value) {
            kernel->onHostUpdated(address, value);
        }];
        _observerTokens[@(param.address)] = (__bridge id)token;
    }

    return [super allocateRenderResourcesAndReturnError:outError];
}

// Deallocate resources allocated in allocateRenderResourcesAndReturnError:
// Subclassers should call the superclass implementation.
- (void)deallocateRenderResources {
    [self stopDispatchTimer];
    
    // Deallocate your resources.
    _kernel.deInitialize();

    for (AUParameter *param in self.parameterTree.allParameters) {
        AUParameterObserverToken token = (__bridge AUParameterObserverToken)_observerTokens[@(param.address)];
        if (token) {
            [param removeParameterObserver:token];
        }
    }
    [_observerTokens removeAllObjects];
    
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
    NSMutableDictionary<NSString *, id> *state = [[super fullState] mutableCopy];
    
    using User_params = tiny::Param_infos<tiny::Param_model>;
    const auto num_params = static_cast<int32_t>(User_params::num_params);
    
    // Store number of parameters.
    NSNumber *numParams = [NSNumber numberWithInt:num_params];
    [state setObject:numParams forKey:@"tinyplug-num-params"]; // !!!
    
    // Store editor state.
    if (_editor) {
        const auto edit_state = _editor->save_state();
        
        // Number of edit items.
        const auto num_edit_items = static_cast<int32_t>(edit_state.size());
        NSNumber *numEditItems = [NSNumber numberWithInt:num_edit_items];
        [state setObject:numEditItems forKey:@"tinyplug-num-editor-items"]; // !!!
        
        // Editor state map.
        if (num_edit_items > 0) {
            // Write key-value pairs to NSData
            NSMutableData *data = [NSMutableData data];
            
            // Helpers
            auto write_value = [&](const auto& value) {
                [data appendBytes:&value length:sizeof(value)];
            };
            
            auto write_container = [&](const auto& container) {
                const auto num = static_cast<uint32_t>(container.size());
                [data appendBytes:&num length:sizeof(num)];
                [data appendBytes:container.data() length:sizeof(container[0]) * num];
            };
            
            for (const auto& [key, value] : edit_state) {
                write_container(key);
                const auto tag = tiny::tag_for(value);
                write_value(tag);
                switch (tag) {
                    case tiny::State_tag::bool_: {
                        if (const auto* v = std::get_if<bool>(&value)) {
                            write_value(*v);
                        }
                        break;
                    }
                    case tiny::State_tag::int_: {
                        if (const auto* v = std::get_if<int32_t>(&value)) {
                            write_value(*v);
                        }
                        break;
                    }
                    case tiny::State_tag::double_: {
                        if (const auto* v = std::get_if<double>(&value)) {
                            write_value(*v);
                        }
                        break;
                    }
                    case tiny::State_tag::string_: {
                        if (const auto* v = std::get_if<std::string>(&value)) {
                            write_container(*v);
                        }
                        break;
                    }
                    case tiny::State_tag::bytes_: {
                        if (const auto* v = std::get_if<std::vector<uint8_t>>(&value)) {
                            write_container(*v);
                        }
                        break;
                    }
                }
            }
            
            [state setObject:data forKey:@"tinyplug-editor-state-map"]; //
        }
    }

    return [state copy];
}

- (void)setFullState:(NSDictionary<NSString *,id> *)fullState {
    [super setFullState:fullState];
    
    using User_params = tiny::Param_infos<tiny::Param_model>;
    const auto num_params = static_cast<int32_t>(User_params::num_params);

    //
    id version  = [fullState objectForKey:@"tinyplug-num-params"]; // !!!
    if ([version isKindOfClass:[NSNumber class]]) {
        const auto num_state_params = [version intValue];

        // The base implementation already set parameters contained in state.
        // We just have to set the rest to their defaults.
        if (num_params < num_state_params) {
            using User_params = tiny::Param_infos<tiny::Param_model>;
            for (auto i = num_state_params; i < num_params; ++i) {
                const auto& param = _param_infos.param_for(static_cast<uint32_t>(i));
                const auto def_val = tiny::get_host_default(param);
                [[_parameterTree parameterWithAddress:i] setValue:def_val];
            }
        }
    }
    
    id numEditItems = [fullState objectForKey:@"tinyplug-num-editor-items"]; // !!!
    if ([numEditItems isKindOfClass:[NSNumber class]]) {
        const auto num_edit_items = [numEditItems intValue];

        if (num_edit_items > 0) {
            id data = [fullState objectForKey:@"tinyplug-editor-state-map"]; //

            if ([data isKindOfClass:[NSData class]]) {
                //
                const auto* bytes = (const uint8_t*)[data bytes];
                const auto size = static_cast<size_t>([data length]);
                size_t offset = 0;

                auto read_value = [&](auto& value) {
                    if (offset + sizeof(value) <= size) {
                        std::memcpy(&value, bytes + offset, sizeof(value));
                        offset += sizeof(value);
                    }
                };
                auto read_container = [&](auto& container) {
                    using Element = typename std::decay<decltype(container)>::type::value_type;
                    auto num = uint32_t{};
                    read_value(num);
                    const auto container_size = sizeof(Element) * num;
                    if (offset + container_size <= size) {
                        container.resize(num);
                        std::memcpy(container.data(), bytes + offset, container_size);
                        offset += container_size;
                    }
                };

                auto edit_state = tiny::State_map{};
                for (auto i = 0; i < num_edit_items; ++i) {
                    auto key = std::string{};
                    read_container(key);

                    auto tag = tiny::State_tag{};
                    read_value(tag);

                    auto value = tiny::State_item{};
                    switch (tag) {
                        case tiny::State_tag::bool_: {
                            auto v = bool{};
                            read_value(v);
                            value = v;
                            break;
                        }
                        case tiny::State_tag::int_: {
                            auto v = int32_t{};
                            read_value(v);
                            value = v;
                            break;
                        }
                        case tiny::State_tag::double_: {
                            auto v = double{};
                            read_value(v);
                            value = v;
                            break;
                        }
                        case tiny::State_tag::string_: {
                            auto v = std::string{};
                            read_container(v);
                            value = v;
                            break;
                        }
                        case tiny::State_tag::bytes_: {
                            auto v = std::vector<uint8_t>{};
                            read_container(v);
                            value = v;
                            break;
                        }
                    }
                    edit_state.emplace(std::move(key), std::move(value));
                }

                _editor->load_state(edit_state);
            }
        }
    }
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
        const auto latency_secs = kernel->latency_secs();
        
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
