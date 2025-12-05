#include "../platform_dialogs.h"

#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

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

auto Platform_dialogs::message(const std::string& title, const std::string& message, std::function<void()> on_done, Execution_context executor) -> void
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
            executor.queue.push(on_done);
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

auto Platform_dialogs::confirm(const std::string& title, const std::string& message, std::function<void(bool)> on_confirm, Execution_context executor) -> void
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
            executor.queue.push([on_confirm=std::move(on_confirm)] {
                on_confirm(true);
            });
        }];
        UIAlertAction* noAction = [UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel
                                                         handler:^(UIAlertAction* action) {
            executor.queue.push([on_confirm=std::move(on_confirm)] {
                on_confirm(false);
            });
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

auto Platform_dialogs::text_input(const std::string& title, const std::string& message, std::function<void(std::string)> on_text, Execution_context executor) -> void
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
            executor.queue.push([on_text=std::move(on_text), text] {
                on_text(text);
            });
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

auto Platform_dialogs::open_url(const std::string& url, Execution_context /*ececutor*/) -> void
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

auto Platform_dialogs::open_file(const std::string& title, const std::string& default_path, std::function<void(std::optional<std::string>)> on_open, Execution_context executor) -> void
{
    // Copy to locals.
    const auto title_copy = title;
    const auto default_path_copy = default_path;

    dispatch_async(dispatch_get_main_queue(), ^{
        UIDocumentPickerViewController* documentPicker = [[UIDocumentPickerViewController alloc] initWithDocumentTypes:@[UTTypeItem.identifier]
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

                    executor.launcher.launch([=, on_open=std::move(on_open)] {
                        on_open(temp_str);
                        [[NSFileManager defaultManager] removeItemAtPath:[NSString stringWithUTF8String:temp_str.c_str()] error:nil];
                    });
                }
                else {
                    executor.launcher.launch([on_open=std::move(on_open)] {
                        on_open(std::nullopt);
                    });
                }
            } else {
                executor.launcher.launch([on_open=std::move(on_open)] {
                    on_open(std::nullopt);
                });
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
