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
            executor.queue.push([cb=std::move(on_confirm)] {
                cb(true);
            });
        }];
        UIAlertAction* noAction = [UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel
                                                         handler:^(UIAlertAction* action) {
            executor.queue.push([cb=std::move(on_confirm)] {
                cb(false);
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
            executor.queue.push([cb=std::move(on_text), text] {
                cb(text);
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

auto Platform_dialogs::open_file(const std::string& title, const std::string& default_path, std::function<void(std::optional<std::string>)> on_open, Execution_context executor) -> void
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
                    const auto p = std::string{[path UTF8String]};
                    executor.queue.push([cb=std::move(on_open), p] {
                        cb(p);
                    });
                    [url stopAccessingSecurityScopedResource]; // We need to revisit this.
                }
            } else {
                executor.queue.push([cb=std::move(on_open)] {
                    cb(std::nullopt);
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
