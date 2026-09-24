#import <AudioToolbox/AudioToolbox.h>
#import <AVFoundation/AVFoundation.h>

#include <tinyplug/tinyplug.hpp>
#include <tiny_plugin.hpp>

@interface Auv3_AUAudioUnit : AUAudioUnit
- (void)setupParameterTree;
-(tiny::Ui_receiver)makeReceiver;
-(tiny::Undo_history*)undoHistory;
-(tiny::Action_queue*)actions;
-(tiny::Task_manager*)tasks;
-(tiny::State_adapter*)stateAdapter;
-(void)setEditor:(std::shared_ptr<tiny::User_editor>)editor;
#if TINY_HAS_STATE
-(tiny::state::Editor_actor<tiny::models::Resolved::State>)stateActor;
#endif
#if TINY_HAS_NOTES_IN
-(tiny::Note_sender)noteSender;
#endif
#if TINY_HAS_WORKER
-(void)bindEditorToWorker;
-(void)drainWorkerToEditor;
#endif
@end
