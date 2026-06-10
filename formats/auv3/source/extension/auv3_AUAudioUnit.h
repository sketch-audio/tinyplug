#import <AudioToolbox/AudioToolbox.h>
#import <AVFoundation/AVFoundation.h>

#include "tinyplug/tinyplug.hpp"
#include "editor.hpp"

@interface Auv3_AUAudioUnit : AUAudioUnit
- (void)setupParameterTree;
-(tiny::Ui_receiver)makeReceiver;
-(void)setEditor:(std::shared_ptr<tiny::plugin::Editor>)editor;
#if TINY_HAS_WORKER
-(void)bindEditorToWorker;
-(void)drainWorkerToEditor;
#endif
@end
