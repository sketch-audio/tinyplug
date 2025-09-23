#import <AudioToolbox/AudioToolbox.h>
#import <AVFoundation/AVFoundation.h>

#include "tinyplug/tinyplug.h"
#include "plug_editor.h"

@interface Auv3_AUAudioUnit : AUAudioUnit
- (void)setupParameterTree;
-(tiny::Ui_receiver)makeReceiver;
-(void)setEditor:(std::shared_ptr<tiny::Plug_editor>)editor;
@end
