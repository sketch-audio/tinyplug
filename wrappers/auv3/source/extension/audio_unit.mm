#import "audio_unit.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

#import <AVFoundation/AVFoundation.h>
#import <CoreAudioKit/AUViewController.h>

#import "AUProcessHelper.hpp"
#import "BufferedAudioBus.hpp"
#import "DSPKernel.hpp"

#include <tiny_platform/platform_paths.hpp>
#include "plug_info.hpp"
#include "preset_list.hpp"

#if !__has_feature(objc_arc)
static_assert(false, "ARC must be enabled for this file");
#endif

static auto presets_path() -> std::filesystem::path
{
    using namespace tiny;
    return Platform_paths::shared_writable({Plug_info::company_directory_name, Plug_info::product_directory_name}) / "Presets";
}

@interface Auv3_AUAudioUnit()
@property (nonatomic, readwrite) AUParameterTree *parameterTree;
@property AUAudioUnitBusArray *inputBusArray;
@property AUAudioUnitBusArray *outputBusArray;
@property (nonatomic, readonly) AUAudioUnitBus *outputBus;
@property (nonatomic, readwrite) AUAudioUnitPreset *preset;
- (void)updateReportedLatency;
@end

@implementation Auv3_AUAudioUnit {
    // Latency reporting. Updated on main only; the render thread posts to the kernel's relay.
    NSTimeInterval _latency;
    
    // C++ members need to be ivars; they would be copied on access if they were properties.
    bool _parameterTreeSetup;
    DSPKernel _kernel;
    std::shared_ptr<tiny::plugin::Editor> _editor;
    // Undo history and action queue live on the AU (plug-in lifetime), not in the
    // view, so the editor's Edit_context (built once at construction) stays valid
    // across window open/close and host preset loads are captured with the window closed.
    tiny::Undo_history _undo_history;
    tiny::Action_queue _actions;
    BufferedInputBus _inputBus;
#if TINY_WANTS_SIDECHAIN
    BufferedInputBus _sidechainBus;
#endif
    NSArray<AUAudioUnitBus *> *_inputBuses;
    NSArray<AUAudioUnitPreset*>* _factory_presets;
    
    std::unique_ptr<tiny::State_adapter> _state_adapter;
    
    std::unique_ptr<AUProcessHelper> _processHelper;
    std::unordered_map<AUParameterAddress, AUParameterObserverToken> _observerTokens;
}

@synthesize parameterTree = _parameterTree;

- (instancetype)initWithComponentDescription:(AudioComponentDescription)componentDescription options:(AudioComponentInstantiationOptions)options error:(NSError **)outError {
    self = [super initWithComponentDescription:componentDescription options:options error:outError];
    
    if (self == nil) { return nil; }
    
    [self setupAudioBuses];
    _parameterTreeSetup = false;
    
    _observerTokens = {};
    
    using namespace tiny;
    auto temp = [NSMutableArray arrayWithCapacity:Preset_list::num_presets];
    for (size_t i = {}; i < Preset_list::num_presets; ++i) {
        auto preset = [[AUAudioUnitPreset alloc] init];
        preset.number = static_cast<NSInteger>(i);
        preset.name = [NSString stringWithUTF8String:Preset_list::names[i]];
        [temp addObject:preset];
    }
    _factory_presets = [temp copy];
    
    using namespace params;
    using User_params = Infos<models::Params>;
    //const auto num_params = User_params::num_params;
    
    using Provider = State_adapter::Provider;
    __weak typeof(self) self_ = self;
    _state_adapter = std::make_unique<State_adapter>(Provider{
        .load_model = [self_]() {
            return State_adapter::Load_model{
                .param_tree = &User_params::param_tree(),
                .num_params = User_params::num_params
            };
        },
        .save_model = [self_](){
            auto s = self_;
            if (!s) return State_adapter::Save_model{};
            
            const auto knob_defaults = params::make_defaults<double, User_params>(params::Space::Knob);
            auto values = std::vector<double>(knob_defaults.begin(), knob_defaults.end());
            for (AUParameter *param in s->_parameterTree.allParameters) {
                const auto addr = static_cast<uint32_t>(param.address);
                const auto& spec = User_params::param_spec(addr);
                const auto host = param.value;
                const auto knob = Value_helper::host_to_knob(host, spec.semantics);
                values[param.address] = knob;
            }
            
            auto editor_state = s->_editor->save_state();
            
            return State_adapter::Save_model{
                .version = 1,
                .param_tree = &User_params::param_tree(),
                .param_values = values,
                .editor_state = editor_state
            };
        }
    });

    // Latency notifications can't be posted from the render thread, and can't be posted
    // from the view either — the editor may never be opened. The render thread posts; the
    // relay delivers on main. Scoped to the AU, not to render resources, so a proposal made
    // in the last block before `deallocateRenderResources` still reaches the host.
    __weak Auv3_AUAudioUnit* weak_self = self;
    _kernel.start_relay(tiny::Relay::Spec{
        .execute = [weak_self]() {
            Auv3_AUAudioUnit* strong_self = weak_self;
            if (strong_self == nil) return;
            [strong_self updateReportedLatency];
        },
        .interval = 0.1, // Seconds.
    });

    return self;
}

- (void)dealloc {
    // Before anything else: a delivery must not observe a half-torn-down AU.
    _kernel.stop_relay();
}

// Peek, never accept: the accept belongs to the `latency` getter, because the host reading
// the property is the only thing that means it adopted the value. Compared against the ivar
// rather than `self.latency`, which would call that getter and advance the handshake.
// Main thread only — KVO observers run on the posting thread.
- (void)updateReportedLatency {
    const auto latency_secs = _kernel.peek_latency_secs();
    if (_latency == latency_secs) { return; }

    [self willChangeValueForKey:@"latency"];
    _latency = latency_secs;
    [self didChangeValueForKey:@"latency"];
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
        const auto tree = params::Infos<models::Params>::param_tree();
        
        // Traverse the tree, creating parameters and groups along the way.
        auto make_node = [&](auto&& self_, const params::Node& node) -> AUParameterNode* {
            return std::visit(Inline_visitor{
                [&](const params::Spec& spec) -> AUParameterNode* {
                    return [self makeParameterFor:spec];
                },
                [&](const params::Group& group) -> AUParameterNode* {
                    NSMutableArray<AUParameterNode*>* children = [NSMutableArray array];
                    for (const auto& child : group.nodes) {
                        [children addObject:self_(self_, child)];
                    }
                    NSString* identifier = [NSString stringWithUTF8String:group.identifier.c_str()];
                    NSString* name = [NSString stringWithUTF8String:group.name.c_str()];
                    return [AUParameterTree createGroupWithIdentifier:identifier name:name children:children];
                }
            }, node);
        };
        
        // Most likely the root is a group, don't create a named group for it.
        if (const auto* g = std::get_if<params::Group>(&tree)) {
            NSMutableArray<AUParameterNode*>* rootChildren = [NSMutableArray array];
            for (auto const& child : g->nodes) {
                [rootChildren addObject:make_node(make_node, child)];
            }
            AUParameterTree* parameter_tree = [AUParameterTree createTreeWithChildren:rootChildren];
            _parameterTree = parameter_tree;
        }
        else if (const auto* s = std::get_if<params::Spec>(&tree)) {
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

- (tiny::Undo_history*)undoHistory {
    return &_undo_history;
}

- (tiny::Action_queue*)actions {
    return &_actions;
}

- (tiny::State_adapter*)stateAdapter {
    return _state_adapter.get();
}

// MARK: - makeReceiver

- (tiny::Ui_receiver)makeReceiver {
    using namespace tiny;
    using namespace params;
    __weak typeof(self) self_ = self;
    return Ui_receiver{
        .get_param = [self_](uint32_t addr) {
            auto s = self_;
            if (!s) return double{};
            const auto& spec = params::Infos<models::Params>::param_spec(addr);
            const auto host = s->_kernel.getParameter(addr);
            const auto knob = Value_helper::host_to_knob(host, spec.semantics);
            return knob;
        },
        .read_meters = [self_](std::span<meters::Sample> out) {
            if (auto s = self_) s->_kernel.read_meters(out);
        },
        .action_handler = [self_](const User_action& action) {
            auto s = self_;
            if (!s) return;
            std::visit(Inline_visitor{
                [&](const Action_start& a) {
                    auto current = s->_kernel.getParameter(a.address);
                    auto* auparam = [s->_parameterTree parameterWithAddress:a.address];
                    auto it = s->_observerTokens.find(auparam.address);
                    auto* token = it != s->_observerTokens.end() ? it->second : nil;
                    [auparam setValue:current originator:token atHostTime:0 eventType:AUParameterAutomationEventTypeTouch];
                },
                [&](const Set_param& a) {
                    const auto& param = params::Infos<models::Params>::param_spec(a.address);
                    const auto host_value = Value_helper::knob_to_host(a.value, param.semantics);
                    auto* auparam = [s->_parameterTree parameterWithAddress:a.address];
                    auto it = s->_observerTokens.find(auparam.address);
                    auto* token = it != s->_observerTokens.end() ? it->second : nil;
                    [auparam setValue:host_value originator:token atHostTime:0 eventType:AUParameterAutomationEventTypeValue];
                },
                [&](const Action_end& a) {
                    auto current = s->_kernel.getParameter(a.address);
                    auto* auparam = [s->_parameterTree parameterWithAddress:a.address];
                    auto it = s->_observerTokens.find(auparam.address);
                    auto* token = it != s->_observerTokens.end() ? it->second : nil;
                    [auparam setValue:current originator:token atHostTime:0 eventType:AUParameterAutomationEventTypeRelease];
                },
                [](const auto&) {}
            }, action);
        }
    };
}

- (void)setEditor:(std::shared_ptr<tiny::plugin::Editor>)editor {
    _editor = editor;
}

#if TINY_HAS_WORKER
- (void)bindEditorToWorker {
    if (!_editor) return;
    DSPKernel* kernel = &_kernel;
    tiny::try_bind_worker(*_editor, tiny::Worker_editor_actor{
        [kernel](const auto& m) { return kernel->_worker_from_edit.push(m); }
    });
}

- (void)drainWorkerToEditor {
    if (!_editor) return;
    tiny::try_drain_worker_to_editor(*_editor, _kernel._worker_to_edit);
}
#endif

// MARK: - makeParameterFor

- (AUParameter*)makeParameterFor:(const tiny::params::Spec&)spec {
    using namespace tiny;
    using namespace params;

    NSString* identifier = [NSString stringWithUTF8String:spec.identity.identifier.c_str()];
    NSString* name = [NSString stringWithUTF8String:spec.name.c_str()];

    auto flags_for = [](params::Policy policy) -> AudioUnitParameterOptions {
        switch (policy) {
            case params::Policy::Automation: return kAudioUnitParameterFlag_IsWritable | kAudioUnitParameterFlag_IsReadable;
            case params::Policy::Control: return kAudioUnitParameterFlag_IsReadable;
            case params::Policy::Hidden: return {};
            case params::Policy::Interface: return kAudioUnitParameterFlag_OmitFromPresets;
            default: return kAudioUnitParameterFlag_IsWritable | kAudioUnitParameterFlag_IsReadable;
        }
    };

    AUParameter* parameter = std::visit(Inline_visitor{
        [&](const params::Semantics::Bool& b) {
            AUParameter* param = [AUParameterTree createParameterWithIdentifier:identifier
                                                                           name:name
                                                                        address:spec.identity.address
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
        [&](const params::Semantics::List& l) {
            NSMutableArray<NSString*>* valueStrings = [NSMutableArray array];
            for (const auto& item : l.items) {
                [valueStrings addObject:[NSString stringWithUTF8String:std::string{item}.c_str()]];
            }
            AUParameter* param = [AUParameterTree createParameterWithIdentifier:identifier
                                                                           name:name
                                                                        address:spec.identity.address
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
        [&](const params::Semantics::Int& i) {
            AUParameter* param = [AUParameterTree createParameterWithIdentifier:identifier
                                                                           name:name
                                                                        address:spec.identity.address
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
        [&](const params::Semantics::Real& r) {
            AUParameter* param = [AUParameterTree createParameterWithIdentifier:identifier
                                                                           name:name
                                                                        address:spec.identity.address
                                                                            min:0
                                                                            max:1
                                                                           unit:kAudioUnitParameterUnit_Generic
                                                                       unitName:nil
                                                                          flags:flags_for(spec.policy) | kAudioUnitParameterFlag_ValuesHaveStrings
                                                                   valueStrings:nil
                                                            dependentParameters:nil];
            param.value = static_cast<float>(Value_helper::plain_to_host(r.def_val, r));
            return param;
        },
        [&](const params::Semantics::Fixed& f) {
            AUParameter* param = [AUParameterTree createParameterWithIdentifier:identifier
                                                                           name:name
                                                                        address:spec.identity.address
                                                                            min:static_cast<float>(f.min_val)
                                                                            max:static_cast<float>(f.max_val)
                                                                           unit:kAudioUnitParameterUnit_Generic
                                                                       unitName:nil
                                                                          flags:flags_for(spec.policy) | kAudioUnitParameterFlag_ValuesHaveStrings
                                                                   valueStrings:nil
                                                            dependentParameters:nil];
            param.value = static_cast<float>(f.def_val);
            return param;
        }
    }, spec.semantics);
    
    return parameter;
}

- (void)setupParameterCallbacks {
    using namespace tiny;
    using namespace params;
    
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
        const auto& spec = params::Infos<models::Params>::param_spec(static_cast<uint32_t>(param.address));
        const auto str_value = Host_formatter::to_string(value, spec.semantics);
        return [NSString stringWithUTF8String:str_value.c_str()];
    };
    
    _parameterTree.implementorValueFromStringCallback = ^(AUParameter *param, NSString *string) {
        const auto addr = static_cast<uint32_t>(param.address);
        const auto& spec = params::Infos<models::Params>::param_spec(addr);
        const auto str = std::string{[string UTF8String]};
        
        if (const auto plain = Host_formatter::to_value(str, spec.semantics)) {
            const auto host = Value_helper::plain_to_host(*plain, spec.semantics);
            return static_cast<float>(host);
        }
        return 0.f;
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

- (void)setRenderingOffline:(BOOL)renderingOffline {
    [super setRenderingOffline:renderingOffline];
    _kernel.setOffline(renderingOffline);
}

- (NSArray<NSNumber *> *)channelCapabilities {
    using namespace tiny;
    return Plug_info::can_process_mono ? @[@2, @2, @1, @1] : @[@2, @2];
}

- (NSTimeInterval)latency {
    // The host is reading, so it has adopted the value — this is the accept half of the
    // handshake, and the kernel may swap on its next render. `_latency` was already set to
    // the peeked value when the pump posted the KVO change below.
    _kernel.accept_latency();
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

    // New Logic versions appear to offer a wildcard 0x0 configuration along with 1024x768 and 1366x1024.
    for (NSUInteger i = 0; i < availableViewConfigurations.count; ++i) {
        AUAudioUnitViewConfiguration* config = availableViewConfigurations[i];
        if (config.width <= 0. && config.height <= 0.) {
            return [NSIndexSet indexSetWithIndex:i];
        }
    }

    // Otherwise, accept all.
    return [NSIndexSet indexSetWithIndexesInRange:NSMakeRange(0, availableViewConfigurations.count)];
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

    // `configure` may have moved our latency. We are already on the main thread here, so
    // notify directly rather than waiting for the relay's next tick.
    [self updateReportedLatency];

    // Since we're immediate in the gui we might be able to get rid of these observer tokens.
    __block DSPKernel *kernel = &_kernel;
    for (AUParameter *param in _parameterTree.allParameters) {
        AUParameterObserverToken token = [param tokenByAddingParameterObserver:^(AUParameterAddress address, AUValue value) {
            kernel->onHostUpdated(address, value);
        }];
        _observerTokens[param.address] = token;
    }

    return [super allocateRenderResourcesAndReturnError:outError];
}

// The host is about to feed us discontinuous audio (seek, loop jump, un-mute).
- (void)reset {
    _kernel.clear();
    [super reset];
}

// Deallocate resources allocated in allocateRenderResourcesAndReturnError:
// Subclassers should call the superclass implementation.
- (void)deallocateRenderResources {
    // Deallocate your resources.
    _kernel.deInitialize();

    for (AUParameter *param in self.parameterTree.allParameters) {
        auto it = _observerTokens.find(param.address);
        if (it != _observerTokens.end()) {
            AUParameterObserverToken token = it->second;
            [param removeParameterObserver:token];
        }
    }
    //[_observerTokens removeAllObjects];
    
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
        {
            AUAudioUnitStatus err = sidechain->pullInput(&pullFlags, timestamp, frameCount, 1, pullInputBlock);

            if (err == 0) { sidechainAudioBufferList = sidechain->mutableAudioBufferList; }
        }
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

// MARK: - State helpers

- (void)addParamValues:(const tiny::Maybe_values<double>&)maybe_values toDictionary:(NSMutableDictionary<NSString *, id> *)state {
    using namespace tiny;
    using namespace params;
    
    const auto num_params = static_cast<int32_t>(maybe_values.size());
    auto numParamsEntry = [NSNumber numberWithInt:num_params];
    [state setObject:numParamsEntry forKey:@(State_rules::Auv3::num_params)];
    
    // Serialize the parameter values to our optional key.
    if (num_params <= 0) return;
    
    auto data = [NSMutableData data];
    
    auto write_value = [&](const auto& value) {
        [data appendBytes:&value length: sizeof(value)];
    };
    
    using User_params = params::Infos<models::Params>;
    
    for (auto i = 0; i < num_params; ++i) {
        const auto value = maybe_values[i];
        if (value.has_value()) {
            const auto& spec = User_params::param_spec(i);
            const auto host = Value_helper::knob_to_host(*value, spec.semantics);
            const auto to_write = static_cast<float>(host);
            write_value(to_write);
        }
        else {
            write_value(State_rules::no_value);
        }
    }
    [state setObject:data forKey:@(State_rules::Auv3::values_from_preset)];
}

- (void)addEditorState:(const tiny::State_map&)edit_state toDictionary:(NSMutableDictionary<NSString *, id> *)state {
    using namespace tiny;
    
    // Number of edit items.
    const auto num_edit_items = static_cast<int32_t>(edit_state.size());
    NSNumber *numEditItems = [NSNumber numberWithInt:num_edit_items];
    [state setObject:numEditItems forKey:@(State_rules::Auv3::num_editor_items)];
    
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
                case tiny::State_tag::Bool: {
                    if (const auto* v = std::get_if<bool>(&value)) {
                        write_value(*v);
                    }
                    break;
                }
                case tiny::State_tag::Int: {
                    if (const auto* v = std::get_if<int32_t>(&value)) {
                        write_value(*v);
                    }
                    break;
                }
                case tiny::State_tag::Double: {
                    if (const auto* v = std::get_if<double>(&value)) {
                        write_value(*v);
                    }
                    break;
                }
                case tiny::State_tag::String: {
                    if (const auto* v = std::get_if<std::string>(&value)) {
                        write_container(*v);
                    }
                    break;
                }
                default:
                    break;
            }
        }
        
        [state setObject:data forKey:@(State_rules::Auv3::editor_state_map)];
    }
}

// MARK: - Full State

-(NSDictionary<NSString *,id> *)fullState {
    using namespace tiny;
    
    // Grab the base implementation.
    NSMutableDictionary<NSString *,id> *state = [[super fullState] mutableCopy]; // auto would deduce as `id`
    
    // Store number of parameters (the parameter values are in the base implementation).
    using User_params = params::Infos<tiny::models::Params>;
    auto numParamsEntry = [NSNumber numberWithInt:static_cast<int32_t>(User_params::num_params)];
    [state setObject:numParamsEntry forKey:@(State_rules::Auv3::num_params)];
    
    // Store editor state.
    if (_editor) {
        const auto edit_state = _editor->save_state();
        [self addEditorState:edit_state toDictionary:state];
    }

    return [state copy];
}

- (void)setFullState:(NSDictionary<NSString *,id> *)fullState {
    using namespace tiny;

    if (fullState == nil) return;

    using User_params = tiny::params::Infos<tiny::models::Params>;
    const auto num_params = static_cast<int32_t>(User_params::num_params);

    // Snapshot all current param values in knob space (mirrors makeReceiver's get_param).
    auto snapshot_knob_params = [&]() {
        auto out = std::array<double, User_params::num_params>{};
        for (auto i = decltype(num_params){}; i < num_params; ++i) {
            const auto& spec = User_params::param_spec(static_cast<uint32_t>(i));
            const auto host = _kernel.getParameter(static_cast<uint32_t>(i));
            out[i] = params::Value_helper::host_to_knob(host, spec.semantics);
        }
        return out;
    };

    // Pre-load snapshot for host-load undo capture (before the base applies values).
    const auto before = snapshot_knob_params();

    [super setFullState:fullState]; // Call base.

    // Clamped: the count comes from the host's plist and indexes param_spec below.
    const auto num_stored_params = [&]() {
        id numParamsEntry = [fullState objectForKey:@(State_rules::Auv3::num_params)];
        if ([numParamsEntry isKindOfClass:[NSNumber class]]) {
            return std::clamp([numParamsEntry intValue], int32_t{0}, num_params);
        }
        return int32_t{0};
    }();
    
    // Is this a preset or regular state?
    id values_from_preset = [fullState objectForKey:@(State_rules::Auv3::values_from_preset)];
    if (values_from_preset != nil && [values_from_preset isKindOfClass:[NSData class]]) {
        auto data = (NSData*)values_from_preset;
        
        //
        const auto* bytes = static_cast<const uint8_t*>([data bytes]);
        const auto size = static_cast<size_t>([data length]);
        size_t offset = 0;

        auto read_value = [&](auto& value) {
            if (offset + sizeof(value) <= size) {
                std::memcpy(&value, bytes + offset, sizeof(value));
                offset += sizeof(value);
                return true;
            }
            return false;
        };
        
        for (auto i = 0; i < num_stored_params; ++i) {
            const auto& param = User_params::param_spec(static_cast<uint32_t>(i));
            auto host = float{};
            if (!read_value(host)) break; // Truncated chunk; keep what we applied.
            if (!State_rules::is_persistent(param) || host == State_rules::no_value) continue;
            [[_parameterTree parameterWithAddress:i] setValue:host];
        }
    }
    
    // The base implementation already set parameters contained in state.
    // We just have to set the rest to their defaults.
    if (num_params > num_stored_params) {
        for (auto i = num_stored_params; i < num_params; ++i) {
            const auto& param = User_params::param_spec(static_cast<uint32_t>(i));
            const auto def_val = params::Value_helper::default_value(param, params::Space::Host);
            [[_parameterTree parameterWithAddress:i] setValue:def_val];
        }
    }
    
    // Set interface parameters to their default values.
    for (auto i = decltype(num_params){}; i < num_params; ++i) {
        const auto& param = User_params::param_spec(static_cast<uint32_t>(i));
        if (param.policy == params::Policy::Interface) {
            const auto def_val = params::Value_helper::default_value(param, params::Space::Host);
            [[_parameterTree parameterWithAddress:i] setValue:def_val];
        }
    }

    // Record the host load as one coalesced undo step (works editor open or closed).
    // The editor notify() is dispatched at the end, after the editor state loads, so
    // a preset's name/marker can fold into this step. Stash the snapshot until then.
    const auto host_after = snapshot_knob_params();
    auto host_changes = std::vector<Set_param>{};
    _undo_history.push_host_load(before, host_after, host_changes);

    id numEditItems = [fullState objectForKey:@(State_rules::Auv3::num_editor_items)];
    if ([numEditItems isKindOfClass:[NSNumber class]]) {
        const auto num_edit_items = [numEditItems intValue];

        if (num_edit_items > 0) {
            id data = [fullState objectForKey:@(State_rules::Auv3::editor_state_map)];

            if ([data isKindOfClass:[NSData class]]) {
                //
                const auto* bytes = (const uint8_t*)[data bytes];
                const auto size = static_cast<size_t>([data length]);
                size_t offset = 0;

                auto read_value = [&](auto& value) {
                    if (offset + sizeof(value) > size) return false;
                    std::memcpy(&value, bytes + offset, sizeof(value));
                    offset += sizeof(value);
                    return true;
                };
                auto read_container = [&](auto& container) {
                    using Element = typename std::decay<decltype(container)>::type::value_type;
                    auto num = uint32_t{};
                    if (!read_value(num)) return false;
                    const auto container_size = sizeof(Element) * num;
                    if (offset + container_size > size) return false;
                    container.resize(num);
                    std::memcpy(container.data(), bytes + offset, container_size);
                    offset += container_size;
                    return true;
                };

                // Reject the whole map on any bad read: a partial parse misaligns the
                // stream and would apply junk as if the load had succeeded.
                auto edit_state = tiny::State_map{};
                auto ok = true;
                for (auto i = 0; i < num_edit_items && ok; ++i) {
                    auto key = std::string{};
                    if (!read_container(key)) { ok = false; break; }

                    auto tag = tiny::State_tag{};
                    if (!read_value(tag)) { ok = false; break; }

                    auto value = tiny::State_item{};
                    switch (tag) {
                        case tiny::State_tag::Bool: {
                            auto v = bool{};
                            ok = read_value(v);
                            value = v;
                            break;
                        }
                        case tiny::State_tag::Int: {
                            auto v = int32_t{};
                            ok = read_value(v);
                            value = v;
                            break;
                        }
                        case tiny::State_tag::Double: {
                            auto v = double{};
                            ok = read_value(v);
                            value = v;
                            break;
                        }
                        case tiny::State_tag::String: {
                            auto v = std::string{};
                            ok = read_container(v);
                            value = std::move(v);
                            break;
                        }
                        default:
                            ok = false;
                            break;
                    }
                    if (!ok) break;
                    edit_state.emplace(std::move(key), std::move(value));
                }

                if (ok) _editor->load_state(edit_state);
            }
        }
    }

    // Notify the editor of the host load synchronously (works editor open or closed),
    // letting it fold its marker params into the load's single undo step via add_param.
    if (_editor) {
        auto add_param = [self](uint32_t addr, double knob) {
            using namespace params;
            auto* auparam = [self->_parameterTree parameterWithAddress:addr];
            if (!auparam) return;
            const auto& spec = User_params::param_spec(addr);
            const auto from = Value_helper::host_to_knob(auparam.value, spec.semantics);
            const auto host = Value_helper::knob_to_host(knob, spec.semantics);
            [auparam setValue:static_cast<AUValue>(host)];
            self->_undo_history.amend_host_load(addr, from, knob);
        };
        _editor->notify(Host_event{Host_preset_loaded{
            .changes = host_changes,
            .params = host_after,
            .add_param = add_param,
        }});
    }
}

// MARK: - Presets

- (NSArray<AUAudioUnitPreset *> *)factoryPresets {
    return _factory_presets;
}

- (AUAudioUnitPreset *)currentPreset {
    return _preset;
}

- (NSDictionary<NSString *,id> *)presetDictFor:(const std::string&)path {
    using namespace tiny;
    
    auto file = std::ifstream{path};
    
    if (file.is_open()) {
        try {
            using Json = nlohmann::ordered_json;
            auto json = Json{};
            file >> json;
            
            const auto param_values = _state_adapter->param_values(json);
            const auto editor_state = _state_adapter->editor_state(json);
            
            auto dict = [[NSMutableDictionary alloc] init];
            
            // Add minimal keys:
            [dict setObject:[NSNumber numberWithInt:0] forKey:@(kAUPresetVersionKey)]; //
            [dict setObject:[NSNumber numberWithInt:Plug_info::Auv2::type] forKey:@(kAUPresetTypeKey)];
            [dict setObject:[NSNumber numberWithInt:Plug_info::Auv2::subtype] forKey:@(kAUPresetSubtypeKey)];
            [dict setObject:[NSNumber numberWithInt:Plug_info::manufacturer_code] forKey:@(kAUPresetManufacturerKey)];
            //[dict setObject:[NSData data] forKey:@(kAUPresetDataKey)]; // We're writing the parameter values ourself in the preset.
            
            [self addParamValues:param_values toDictionary:dict];
            [self addEditorState:editor_state toDictionary:dict];
            
            return dict;
        }
        catch (...) {
            //
        }
    }
    
    return nil;
}

- (void)setCurrentPreset:(AUAudioUnitPreset *)currentPreset {
    using namespace tiny;
    
    if (currentPreset == nil) {
        _preset = nil;
        return;
    }
    
    if (currentPreset.number >= 0) {
        // Factory preset.
        const auto bundle_resources = Platform_paths::format_readable({/*bundle id*/});
        const auto filename = std::string{[currentPreset.name UTF8String]} + "." + Plug_info::Presets::extension;
        
        const auto path = bundle_resources / filename;
        
        if (!std::filesystem::exists(path) || !std::filesystem::is_regular_file(path)) return;
        
        // Load
        auto dict = [self presetDictFor:path];
        [self setFullState:dict];
        
        _preset = currentPreset;
    }
    else {
        // User preset.
        auto dict = [self presetStateFor:currentPreset error:nil];
        [self setFullState:dict];
        _preset = currentPreset;
    }
}

- (BOOL)supportsUserPresets {
    return true;
}

- (NSArray<AUAudioUnitPreset *> *)userPresets {
    using namespace tiny;
    
    const auto presets_writable = presets_path();
    if (!std::filesystem::exists(presets_writable) || !std::filesystem::is_directory(presets_writable)) {
        return @[];
    }
    
    const auto target_ext = std::string{"."} + std::string{Plug_info::Presets::extension};
    
    auto preset_names = std::vector<std::string>{};
    for (const auto& entry : std::filesystem::directory_iterator(presets_writable)) {
        if (entry.is_regular_file() && entry.path().extension() == target_ext) {
            preset_names.push_back(entry.path().stem().string());
        }
    }
    
    std::ranges::sort(preset_names);
    
    auto au_presets = [NSMutableArray array];
    auto number = -1;
    for (auto& name : preset_names) {
        auto au_preset = [[AUAudioUnitPreset alloc] init];
        au_preset.name = [NSString stringWithUTF8String:name.c_str()];
        au_preset.number = number;
        [au_presets addObject:au_preset];
        number -= 1;
    }
    
    return [au_presets copy];
}

- (BOOL)saveUserPreset:(AUAudioUnitPreset *)userPreset error:(NSError *__autoreleasing  _Nullable *)outError {
    using namespace tiny;
    
    const auto presets_writable = presets_path();
    const auto filename = std::string{[userPreset.name UTF8String]} + "." + Plug_info::Presets::extension;
    const auto path = presets_writable / filename;
    
    auto ec = std::error_code{};

    if (!std::filesystem::exists(presets_writable)) {
        std::filesystem::create_directories(presets_writable, ec);
        if (ec) {
            if (outError) {
                *outError = [NSError errorWithDomain:NSCocoaErrorDomain
                                                code:NSFileWriteNoPermissionError
                                            userInfo:@{NSLocalizedDescriptionKey: @"Could not create presets directory."}];
            }
            return NO;
        }
    }
    
    const auto name = std::string{[userPreset.name UTF8String]};
    const auto preset_json = _state_adapter->preset_state({{"preset-name", name}});
    
    // 4. Write to file
    auto file = std::ofstream{path};
    if (file.is_open()) {
        file << preset_json.dump(4);
        file.close();
        return YES;
    } else {
        if (outError) {
            *outError = [NSError errorWithDomain:NSCocoaErrorDomain
                                            code:NSFileWriteUnknownError
                                        userInfo:@{NSLocalizedDescriptionKey: @"Failed to open file for writing."}];
        }
        return NO;
    }
    
    return true;
}

- (BOOL)deleteUserPreset:(AUAudioUnitPreset *)userPreset error:(NSError *__autoreleasing  _Nullable *)outError {
    using namespace tiny;

    const auto presets_writable = presets_path();
    const auto filename = std::string{[userPreset.name UTF8String]} + "." + Plug_info::Presets::extension;
    const auto path = presets_writable / filename;

    auto ec = std::error_code{};
    
    // 2. Check if it exists first
    if (!std::filesystem::exists(path, ec)) {
        if (outError) {
            *outError = [NSError errorWithDomain:NSCocoaErrorDomain
                                            code:NSFileNoSuchFileError
                                        userInfo:@{NSLocalizedDescriptionKey: @"Preset file not found."}];
        }
        return NO;
    }

    // 3. Attempt to delete
    if (std::filesystem::remove(path, ec)) {
        return YES;
    } else {
        // 4. Handle failure (e.g., permissions or file locked)
        if (outError) {
            *outError = [NSError errorWithDomain:NSCocoaErrorDomain
                                            code:NSFileWriteNoPermissionError
                                        userInfo:@{NSLocalizedDescriptionKey: @(ec.message().c_str())}];
        }
        return NO;
    }
}

- (NSDictionary<NSString *,id> *)presetStateFor:(AUAudioUnitPreset *)userPreset error:(NSError *__autoreleasing  _Nullable *)outError {
    using namespace tiny;
    
    // Obtain the presets path.
    const auto presets_writable = presets_path();
    const auto filename = std::string{[userPreset.name UTF8String]} + "." + Plug_info::Presets::extension;
    const auto path = presets_writable / filename;
    
    if (std::filesystem::exists(path) && std::filesystem::is_regular_file(path)) {
        return [self presetDictFor:path];
    }
    
    return nil;
}

@end
