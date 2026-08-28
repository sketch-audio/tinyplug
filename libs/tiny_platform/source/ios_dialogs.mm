#include <tiny_platform/platform_dialogs.hpp>

#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "window_registry.hpp"

#if __has_feature(objc_arc)
static_assert(false, "This is a non-ARC file");
#endif

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

// MARK: - Dialogs

namespace tiny {

namespace {

// The view controller a dialog should be presented from.
//
// Walking the responder chain up from the view that asked gets the right scene
// and the right presented stack, and it works inside an out-of-process AUv3
// extension where the app has no key window of its own. `keyWindow` (deprecated,
// and nil under multi-scene) is only the last resort.
auto top_view_controller(Window_token token) -> UIViewController*
{
    auto* native = Window_registry::resolve(token);
    if (!native) native = Window_registry::sole();

    UIViewController* controller = nil;
    for (UIResponder* responder = static_cast<UIView*>(native); responder != nil; responder = responder.nextResponder) {
        if ([responder isKindOfClass:[UIViewController class]]) {
            controller = static_cast<UIViewController*>(responder);
            break;
        }
    }

    if (!controller) controller = static_cast<UIView*>(native).window.rootViewController;
    if (!controller) controller = [UIApplication sharedApplication].keyWindow.rootViewController;

    while (controller.presentedViewController) {
        controller = controller.presentedViewController;
    }
    return controller;
}

} // namespace

auto Platform_dialogs::message(const std::string& title, const std::string& message, std::function<void()> on_done, Dialog_context ctx) -> void
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
            ctx.tasks.on_main(on_done);
        }];
        [alert addAction:okAction];
        
        [top_view_controller(ctx.window) presentViewController:alert animated:YES completion:nil];
    });
}

auto Platform_dialogs::confirm(const std::string& title, const std::string& message, std::function<void(bool)> on_confirm, Dialog_context ctx) -> void
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
            ctx.tasks.on_main([on_confirm=std::move(on_confirm)] {
                on_confirm(true);
            });
        }];
        UIAlertAction* noAction = [UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel
                                                         handler:^(UIAlertAction* action) {
            ctx.tasks.on_main([on_confirm=std::move(on_confirm)] {
                on_confirm(false);
            });
        }];
        [alert addAction:yesAction];
        [alert addAction:noAction];
        
        [top_view_controller(ctx.window) presentViewController:alert animated:YES completion:nil];
    });
}

auto Platform_dialogs::text_input(const std::string& title, const std::string& message, std::function<void(std::string)> on_text, Dialog_context ctx) -> void
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
            const auto text = std::string{[textField.text UTF8String]};
            ctx.tasks.on_main([on_text=std::move(on_text), text] {
                on_text(text);
            });
        }];
        UIAlertAction* cancelAction = [UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel
                                                             handler:^(UIAlertAction* action) {
            // Answer with nothing rather than dropping the callback.
            ctx.tasks.on_main([on_text=std::move(on_text)] {
                on_text(std::string{});
            });
        }];
        [alert addAction:okAction];
        [alert addAction:cancelAction];
        
        [top_view_controller(ctx.window) presentViewController:alert animated:YES completion:nil];
    });
}

auto Platform_dialogs::open_url(const std::string& url, Dialog_context /*ctx*/) -> void
{
    // Copy to locals.
    const auto url_copy = url;

    if (const auto nsurl = [NSURL URLWithString:[NSString stringWithUTF8String:url_copy.c_str()]]) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [[UIApplication sharedApplication] openURL:nsurl options:@{} completionHandler:nil];
        });
    }
}

auto Platform_dialogs::save_file(const std::string& title, const std::string& default_path, const std::string& name, const std::string& extension, std::function<void(std::optional<std::string>)> on_save, Dialog_context ctx) -> void {
    
    //const auto title_copy = title;
    //const auto default_path_copy = default_path;
    const auto name_copy = name;
    const auto extension_copy = extension;

    ctx.tasks.on_background([=, on_save = std::move(on_save)]() mutable {
        NSString* ns_name = [NSString stringWithUTF8String:name_copy.c_str()];
        NSString* ns_ext = [NSString stringWithUTF8String:extension_copy.c_str()];
        NSString* tempFilename = [NSString stringWithFormat:@"%@.%@", ns_name, ns_ext];
        NSURL* tempURL = [[NSURL fileURLWithPath:NSTemporaryDirectory()] URLByAppendingPathComponent:tempFilename];
        auto tempPathStr = std::string{[tempURL.path UTF8String]};

        on_save(tempPathStr); // Write to temp.

        // 3. Once written, jump to UI thread to present the "Move" dialog
        dispatch_async(dispatch_get_main_queue(), ^{
            if (![[NSFileManager defaultManager] fileExistsAtPath:tempURL.path]) return;

            UIDocumentPickerViewController* picker = [[UIDocumentPickerViewController alloc] initForExportingURLs:@[tempURL] asCopy:NO];

            OpenFileHelper* helper = [[OpenFileHelper alloc] init];
            helper.completion = ^(NSArray<NSURL *> *urls) {
                // Cleanup temp if it wasn't moved (e.g., user cancelled)
                [[NSFileManager defaultManager] removeItemAtURL:tempURL error:nil];
                [helper release];
            };

            picker.delegate = helper;
            [top_view_controller(ctx.window) presentViewController:picker animated:YES completion:nil];
        });
    });
}

// MARK: - open file

// Shared by `open_file`/`choose_dir` -- the document picker only differs in
// which content type it's restricted to (any item vs. folders only).
static auto present_document_picker(NSString* content_type, std::function<void(std::optional<std::string>)> on_open, Dialog_context ctx) -> void
{
    dispatch_async(dispatch_get_main_queue(), ^{
        UIDocumentPickerViewController* documentPicker = [[UIDocumentPickerViewController alloc] initWithDocumentTypes:@[content_type]
                                                                                                                inMode:UIDocumentPickerModeOpen];
        documentPicker.allowsMultipleSelection = false;

        OpenFileHelper* helper = [[OpenFileHelper alloc] init];
        helper.completion = ^(NSArray<NSURL *> *urls) {
            if (urls.count > 0) {
                NSURL *url = urls.firstObject;
                BOOL ok = [url startAccessingSecurityScopedResource];
                if (ok) {
                    NSString *path = url.path;

                    // Copy to NSTemporaryDirectory
                    NSString* temp = [NSTemporaryDirectory() stringByAppendingPathComponent:[url lastPathComponent]];
                    [[NSFileManager defaultManager] copyItemAtPath:path toPath:temp error:nil];

                    [url stopAccessingSecurityScopedResource];
                    
                    const auto temp_str = std::string{[temp UTF8String]};

                    ctx.tasks.on_background([=, on_open=std::move(on_open)] {
                        on_open(temp_str);
                        [[NSFileManager defaultManager] removeItemAtPath:[NSString stringWithUTF8String:temp_str.c_str()] error:nil];
                    });
                }
                else {
                    ctx.tasks.on_background([on_open=std::move(on_open)] {
                        on_open(std::nullopt);
                    });
                }
            } else {
                ctx.tasks.on_background([on_open=std::move(on_open)] {
                    on_open(std::nullopt);
                });
            }
            [helper release];
            [documentPicker release];
        };
        documentPicker.delegate = helper;
        
        [top_view_controller(ctx.window) presentViewController:documentPicker animated:YES completion:nil];
    });
}

auto Platform_dialogs::open_file(const std::string& /*title*/, const std::string& /*default_path*/, std::function<void(std::optional<std::string>)> on_open, Dialog_context ctx) -> void
{
    present_document_picker(UTTypeItem.identifier, std::move(on_open), ctx);
}

auto Platform_dialogs::choose_dir(const std::string& /*title*/, const std::string& /*default_path*/, std::function<void(std::optional<std::string>)> on_choose, Dialog_context ctx) -> void
{
    present_document_picker(UTTypeFolder.identifier, std::move(on_choose), ctx);
}

} // namespace tiny
