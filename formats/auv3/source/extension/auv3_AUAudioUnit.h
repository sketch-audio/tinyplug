#import <AudioToolbox/AudioToolbox.h>
#import <AVFoundation/AVFoundation.h>

#include "tinyplug/tinyplug.h"

@interface Auv3_AUAudioUnit : AUAudioUnit
- (void)setupParameterTree;
- (tiny::Ui_receiver)makeReceiver;
@end
