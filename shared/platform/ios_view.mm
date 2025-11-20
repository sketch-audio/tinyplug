#include "platform.h"

#if PLATFORM_IOS
#include <chrono>
#include <unordered_map>
#include <unordered_set>

#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "platform_view.h"
#include "window_context.h"

#if __has_feature(objc_arc)
static_assert(false, "This is a non-ARC file");
#endif

@interface IosView : UIView
- (id)initWithDelegate:(std::shared_ptr<tiny::View_delegate>)delegate;
- (void)startDisplayLink;
- (void)stopDisplayLink;
@end

@implementation IosView {
    std::shared_ptr<tiny::View_delegate> _delegate;
    CADisplayLink* _displayLink;
    
    tiny::User_interaction _interaction;
    struct Pointer_data {
        tiny::Pointer pointer;
        std::optional<tiny::Coords> pos_down;
        std::optional<tiny::Coords> pos_last;
        bool pending_gesture;
        bool started_drag;
    };
    
    std::unordered_map<UITouch*, Pointer_data> _active_pointers;
    std::unordered_map<UITouch*, CGPoint> _activeTouches;
}

- (id)initWithDelegate:(std::shared_ptr<tiny::View_delegate>)delegate {
    _delegate = delegate;
    const auto size = _delegate->get_size();
    self = [super initWithFrame:CGRectMake(0, 0, size.w, size.h)];
    if (self) {
        //
        self.multipleTouchEnabled = YES;

        //
        UITapGestureRecognizer *singleTap = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(handleSingleTap:)];
        singleTap.delaysTouchesEnded = false;
        singleTap.cancelsTouchesInView = false;
        [self addGestureRecognizer:singleTap];
        [singleTap release];

        UITapGestureRecognizer *doubleTap = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(handleDoubleTap:)];
        doubleTap.numberOfTapsRequired = 2;
        doubleTap.delaysTouchesEnded = false;
        doubleTap.cancelsTouchesInView = false;
        [self addGestureRecognizer:doubleTap];
        [doubleTap release];

        UILongPressGestureRecognizer *longPress = [[UILongPressGestureRecognizer alloc] initWithTarget:self action:@selector(handleLongPress:)];
        longPress.delaysTouchesEnded = false;
        longPress.cancelsTouchesInView = false;
        [self addGestureRecognizer:longPress];
        [longPress release];
    }
    return self;
}

- (void)startDisplayLink{
    [self stopDisplayLink];
    _displayLink = [CADisplayLink displayLinkWithTarget:self selector:@selector(onDisplayLink:)];
    [_displayLink addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
}

- (void)stopDisplayLink{
    if (_displayLink) {
        [_displayLink invalidate];
        _displayLink = nil;
    }
}

- (void)dealloc {
    [self stopDisplayLink];
    [super dealloc];
}

- (void)onDisplayLink:(CADisplayLink *)sender {
    [self drawRect:{}]; // Invalidate the view.
}

- (void)drawRect:(CGRect)rect {
    const auto time_now = tiny::System_clock::now();
    
    _interaction.pointers.clear();
    for (const auto& [touch, pointer_data] : _active_pointers) {
        _interaction.pointers.push_back(pointer_data.pointer);
    }

    _delegate->draw(_interaction, time_now);
    
    // Remove terminated pointers. (Drag_end, Click, Double_click, Right_click)
    // I think we could move this to touchesEnded/touchesCancelled
    std::erase_if(_active_pointers, [](auto const& pair) {
        const auto& pointer = pair.second.pointer;
        return std::holds_alternative<tiny::Drag_end>(pointer.state) ||
               std::holds_alternative<tiny::Click>(pointer.state) ||
               std::holds_alternative<tiny::Double_click>(pointer.state) ||
               std::holds_alternative<tiny::Right_click>(pointer.state);
    });
    
    for (auto& [touch, pointer_data] : _active_pointers) {
        tiny::try_set(pointer_data.pointer.state, tiny::Consumed{});
        pointer_data.pending_gesture = false;
    }
}

- (void)handleSingleTap:(UITapGestureRecognizer *)gesture {
    if (gesture.state == UIGestureRecognizerStateEnded) {
        // for touch in gesture touches (have to go by index and get location)
        // find closest tagged interaction
        // try set that interaction
        NSUInteger touches = [gesture numberOfTouches];
        for (NSUInteger i = 0; i < touches; ++i) {
            CGPoint location = [gesture locationOfTouch:i inView:self];
            // find the closest pointer in _active_pointes
            tiny::Coords loc{location.x, location.y};
            UITouch* closest_touch = nullptr;
            float closest_dist = std::numeric_limits<float>::max();
            for (const auto& [touch, pointer_data] : _active_pointers) {
                if (const auto last_pos = pointer_data.pos_last) {
                    float dx = last_pos->x - loc.x;
                    float dy = last_pos->y - loc.y;
                    float dist = dx * dx + dy * dy;
                    if (dist < closest_dist) {
                        closest_dist = dist;
                        closest_touch = touch;
                    }
                }
            }
            if (closest_touch) {
                auto it = _active_pointers.find(closest_touch);
                if (it != _active_pointers.end()) {
                    auto& pointer_data = it->second;
                    tiny::try_set(pointer_data.pointer.state, tiny::Click{loc});
                    pointer_data.pending_gesture = true;
                    pointer_data.pos_down = std::nullopt;
                    pointer_data.started_drag = false;
                }
            }
        }
    }
}

- (void)handleDoubleTap:(UITapGestureRecognizer *)gesture {
    if (gesture.state == UIGestureRecognizerStateEnded) {
        // for touch in gesture touches (have to go by index and get location)
        // find closest tagged interaction
        // try set that interaction
        NSUInteger touches = [gesture numberOfTouches];
        for (NSUInteger i = 0; i < touches; ++i) {
            CGPoint location = [gesture locationOfTouch:i inView:self];
            // find the closest pointer in _active_pointes
            tiny::Coords loc{location.x, location.y};
            UITouch* closest_touch = nullptr;
            float closest_dist = std::numeric_limits<float>::max();
            for (const auto& [touch, pointer_data] : _active_pointers) {
                if (const auto last_pos = pointer_data.pos_last) {
                    float dx = last_pos->x - loc.x;
                    float dy = last_pos->y - loc.y;
                    float dist = dx * dx + dy * dy;
                    if (dist < closest_dist) {
                        closest_dist = dist;
                        closest_touch = touch;
                    }
                }
            }
            if (closest_touch) {
                auto it = _active_pointers.find(closest_touch);
                if (it != _active_pointers.end()) {
                    auto& pointer_data = it->second;
                    tiny::try_set(pointer_data.pointer.state, tiny::Double_click{loc});
                    pointer_data.pending_gesture = true;
                    pointer_data.pos_down = std::nullopt;
                    pointer_data.started_drag = false;
                }
            }
        }
    }
}

- (void)handleLongPress:(UILongPressGestureRecognizer *)gesture {
    if (gesture.state == UIGestureRecognizerStateBegan) {
        // for touch in gesture touches (have to go by index and get location)
        // find closest tagged interaction
        // try set that interaction
        NSUInteger touches = [gesture numberOfTouches];
        for (NSUInteger i = 0; i < touches; ++i) {
            CGPoint location = [gesture locationOfTouch:i inView:self];
            // find the closest pointer in _active_pointes
            tiny::Coords loc{location.x, location.y};
            UITouch* closest_touch = nullptr;
            float closest_dist = std::numeric_limits<float>::max();
            for (const auto& [touch, pointer_data] : _active_pointers) {
                if (const auto last_pos = pointer_data.pos_last) {
                    float dx = last_pos->x - loc.x;
                    float dy = last_pos->y - loc.y;
                    float dist = dx * dx + dy * dy;
                    if (dist < closest_dist) {
                        closest_dist = dist;
                        closest_touch = touch;
                    }
                }
            }
            if (closest_touch) {
                auto it = _active_pointers.find(closest_touch);
                if (it != _active_pointers.end()) {
                    auto& pointer_data = it->second;
                    tiny::try_set(pointer_data.pointer.state, tiny::Right_click{loc});
                    pointer_data.pending_gesture = true;
                    pointer_data.pos_down = std::nullopt;
                    pointer_data.started_drag = false;
                }
            }
        }
    }
}

// MARK: - touches

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [super touchesBegan:touches withEvent:event];
    
    // for touch in touches
    // insert new tagged interaction with state down
    for (UITouch* touch in touches) {
        CGPoint location = [touch locationInView:self];
        const auto tag = (uintptr_t)touch;
        const auto pos = tiny::Coords{location.x, location.y};
        _active_pointers[touch] = Pointer_data{
            .pointer = {.tag = tag, .state = tiny::Down{pos}},
            .pos_down = pos,
            .pos_last = pos,
            .pending_gesture = false,
            .started_drag = false,
        };
        _activeTouches[touch] = location;
    }
}

- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [super touchesMoved:touches withEvent:event];
    
    for (UITouch* touch in touches) {
        CGPoint location = [touch locationInView:self];
        const auto pos = tiny::Coords{location.x, location.y};
        auto it = _active_pointers.find(touch);
        if (it != _active_pointers.end()) {
            auto& pointer_data = it->second;
            if (const auto down_pos = pointer_data.pos_down; down_pos && pointer_data.started_drag) {
                tiny::try_set(pointer_data.pointer.state, tiny::Drag{.fpos = *down_pos, .tpos = pos});
                pointer_data.pos_last = pos;
            }
            else if (const auto down_pos = pointer_data.pos_down){
                tiny::try_set(pointer_data.pointer.state, tiny::Drag_start{.fpos = *down_pos, .tpos = pos});
                pointer_data.pos_last = pos;
                pointer_data.started_drag = true;
            }
        }
        _activeTouches[touch] = location;
    }
}

- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [super touchesEnded:touches withEvent:event];

    for (UITouch* touch in touches) {
        CGPoint location = [touch locationInView:self];
        const auto pos = tiny::Coords{location.x, location.y};
        auto it = _active_pointers.find(touch);
        if (it != _active_pointers.end()) {
            auto& pointer_data = it->second;
            if (const auto down_pos = pointer_data.pos_down; down_pos && pointer_data.started_drag) {
                tiny::try_set(pointer_data.pointer.state, tiny::Drag_end{.fpos = *down_pos, .tpos = pos});
            }
            else if (!pointer_data.pending_gesture) {
                tiny::try_set(pointer_data.pointer.state, tiny::Consumed{});
            }
        }
        _activeTouches.erase(touch); // Gesture should be processed before touch end/cancel.
    }
}

- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [super touchesCancelled:touches withEvent:event];

    for (UITouch* touch in touches) {
        auto it = _active_pointers.find(touch);
        if (it != _active_pointers.end()) {
            auto& pointer_data = it->second;
            tiny::try_set(pointer_data.pointer.state, tiny::Consumed{});
        }
        _activeTouches.erase(touch); // Gesture should be processed before touch end/cancel.
    }
}

- (void)pressesChanged:(NSSet<UIPress *> *)presses withEvent:(UIPressesEvent *)event {
    UIKeyModifierFlags flags = event.modifierFlags;
    _interaction.modifier_keys = {
        .primary = (flags & UIKeyModifierCommand) != 0,
        .alt = (flags & UIKeyModifierAlternate) != 0,
        .shift = (flags & UIKeyModifierShift) != 0,
    };
}

@end

// MARK: - OpenFileHelper

@interface OpenFileHelper : NSObject <UIDocumentPickerDelegate>
@property(nonatomic, copy) void (^completion)(NSArray<NSURL *> *urls);
@end

@implementation OpenFileHelper
- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    if (self.completion) { self.completion(urls); }
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)controller {
    if (self.completion) { self.completion(@[]); }
}
@end

// MARK: - Platform_view

namespace tiny {

Platform_view::Platform_view(std::shared_ptr<View_delegate> delegate, bool owns_view, std::function<void()>) : _delegate{delegate}, _owns_view{owns_view}
{
    UIView* view = [[IosView alloc] initWithDelegate:delegate];

    auto context = std::make_unique<Window_context>();
    context->setup({.native_handle = static_cast<void*>(view)});
    _delegate->assign_context(std::move(context));

    _view = view;
}

Platform_view::~Platform_view() 
{
    if (_owns_view) {
        [(UIView*)_view removeFromSuperview];
        [(UIView*)_view release];
    }
    _view = nullptr;
}

auto Platform_view::on_create() -> void
{

}

auto Platform_view::on_show() -> void
{
    if (auto view = static_cast<IosView*>(_view)) {
        [view startDisplayLink];
    }
}

auto Platform_view::on_hide() -> void
{
    if (auto view = static_cast<IosView*>(_view)) {
        [view stopDisplayLink];
    }
}

auto Platform_view::on_destroy() -> void
{
    _delegate->invalidate_context();
}

auto Platform_view::receive_parent(void* parent) -> void
{
    [(UIView*)parent addSubview:(UIView*)_view];
}

auto Platform_view::resize(int32_t w, int32_t h) -> void
{
    [(UIView*)_view setFrame:CGRectMake(0, 0, w, h)]; // So the context can get the size from the view.
    _delegate->on_resize({w, h});
}

// MARK: - alert

auto Platform_dialogs::message(const std::string& title, const std::string& message, Later<> on_done) -> void
{
    // Copy to locals.
    const auto title_copy = title;
    const auto message_copy = message;

    dispatch_async(dispatch_get_main_queue(), ^{
        UIAlertController* alert = [UIAlertController alertControllerWithTitle:[NSString stringWithUTF8String:title_copy.c_str()]
                                                                       message:[NSString stringWithUTF8String:message_copy.c_str()]
                                                                preferredStyle:UIAlertControllerStyleAlert];
        UIAlertAction* okAction = [UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault
                                                         handler:^(UIAlertAction* action) {
            on_done();
        }];
        [alert addAction:okAction];
        
        // Find the topmost view controller
        UIViewController* rootViewController = [UIApplication sharedApplication].keyWindow.rootViewController;
        UIViewController* topViewController = rootViewController;
        while (topViewController.presentedViewController) {
            topViewController = topViewController.presentedViewController;
        }
        
        [topViewController presentViewController:alert animated:YES completion:nil];
    });
}

auto Platform_dialogs::confirm(const std::string& title, const std::string& message, Later<bool> on_confirm) -> void
{
    // Copy to locals.
    const auto title_copy = title;
    const auto message_copy = message;

    dispatch_async(dispatch_get_main_queue(), ^{
        UIAlertController* alert = [UIAlertController alertControllerWithTitle:[NSString stringWithUTF8String:title_copy.c_str()]
                                                                       message:[NSString stringWithUTF8String:message_copy.c_str()]
                                                                preferredStyle:UIAlertControllerStyleAlert];
        UIAlertAction* yesAction = [UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault
                                                          handler:^(UIAlertAction* action) {
            on_confirm(true);
        }];
        UIAlertAction* noAction = [UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel
                                                         handler:^(UIAlertAction* action) {
            on_confirm(false);
        }];
        [alert addAction:yesAction];
        [alert addAction:noAction];
        
        // Find the topmost view controller
        UIViewController* rootViewController = [UIApplication sharedApplication].keyWindow.rootViewController;
        UIViewController* topViewController = rootViewController;
        while (topViewController.presentedViewController) {
            topViewController = topViewController.presentedViewController;
        }
        
        [topViewController presentViewController:alert animated:YES completion:nil];
    });
}

auto Platform_dialogs::text_input(const std::string& title, const std::string& message, Later<std::string> on_text) -> void
{
    // Copy to locals.
    const auto title_copy = title;
    const auto message_copy = message;

    dispatch_async(dispatch_get_main_queue(), ^{
        UIAlertController* alert = [UIAlertController alertControllerWithTitle:[NSString stringWithUTF8String:title_copy.c_str()]
                                                                       message:[NSString stringWithUTF8String:message_copy.c_str()]
                                                                preferredStyle:UIAlertControllerStyleAlert];
        [alert addTextFieldWithConfigurationHandler:^(UITextField* textField) {
            textField.placeholder = @"";
        }];
        UIAlertAction* okAction = [UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault
                                                         handler:^(UIAlertAction* action) {
            UITextField* textField = alert.textFields.firstObject;
            on_text(std::string([textField.text UTF8String]));
        }];
        UIAlertAction* cancelAction = [UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel
                                                             handler:^(UIAlertAction* action) {
            //...
        }];
        [alert addAction:okAction];
        [alert addAction:cancelAction];
        
        // Find the topmost view controller
        UIViewController* rootViewController = [UIApplication sharedApplication].keyWindow.rootViewController;
        UIViewController* topViewController = rootViewController;
        while (topViewController.presentedViewController) {
            topViewController = topViewController.presentedViewController;
        }
        
        [topViewController presentViewController:alert animated:YES completion:nil];
    });
}

auto Platform_dialogs::open_url(const std::string& url) -> void
{
    // Copy to locals.
    const auto url_copy = url;

    if (const auto nsurl = [NSURL URLWithString:[NSString stringWithUTF8String:url_copy.c_str()]]) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [[UIApplication sharedApplication] openURL:nsurl options:@{} completionHandler:nil];
        });
    }
}

// MARK: - open file

auto Platform_dialogs::open_file(const std::string& title, const std::string& default_path, Later<std::optional<std::string>> on_open) -> void
{
    // Copy to locals.
    const auto title_copy = title;
    const auto default_path_copy = default_path;

    dispatch_async(dispatch_get_main_queue(), ^{
        UIDocumentPickerViewController* documentPicker = [[UIDocumentPickerViewController alloc] initWithDocumentTypes:@[UTTypeItem.identifier]
                                                                                                                inMode:UIDocumentPickerModeImport];
        documentPicker.allowsMultipleSelection = false;

        OpenFileHelper* helper = [[OpenFileHelper alloc] init];
        helper.completion = ^(NSArray<NSURL *> *urls) {
            if (urls.count > 0) {
                NSURL *url = urls.firstObject;
                BOOL ok = [url startAccessingSecurityScopedResource];
                if (ok) {
                    NSString *path = url.path;
                    on_open(std::string(path.UTF8String));
                    [url stopAccessingSecurityScopedResource];
                }
            } else {
                on_open(std::nullopt);
            }
            [helper release];
            [documentPicker release];
        };
        documentPicker.delegate = helper;
        
        // Find the topmost view controller
        UIViewController* rootViewController = [UIApplication sharedApplication].keyWindow.rootViewController;
        UIViewController* topViewController = rootViewController;
        while (topViewController.presentedViewController) {
            topViewController = topViewController.presentedViewController;
        }
        
        [topViewController presentViewController:documentPicker animated:YES completion:nil];
    });
}

} // namespace tiny
#endif
