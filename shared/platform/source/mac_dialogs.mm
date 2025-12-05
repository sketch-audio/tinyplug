#include "../platform_dialogs.h"

#import <Cocoa/Cocoa.h>

#if __has_feature(objc_arc)
static_assert(false, "This is a non-ARC file");
#endif

namespace tiny {

auto Platform_dialogs::message(const std::string& title, const std::string& message, std::function<void()> on_done, Execution_context executor) -> void
{
    // Copy to locals.
    const auto title_copy = title;
    const auto message_copy = message;

    dispatch_async(dispatch_get_main_queue(), ^{
        NSAlert* alert = [[NSAlert alloc] init];
        [alert setMessageText:[NSString stringWithUTF8String:title_copy.c_str()]];
        [alert setInformativeText:[NSString stringWithUTF8String:message_copy.c_str()]];
        [alert addButtonWithTitle:@"OK"];

        NSWindow* keyWindow = [[NSApplication sharedApplication] keyWindow];
        if (keyWindow != nil) {
            [alert beginSheetModalForWindow:keyWindow completionHandler:^(NSModalResponse /*returnCode*/) {
                executor.queue.push(on_done);
                dispatch_async(dispatch_get_main_queue(), ^{
                    [keyWindow makeKeyAndOrderFront:nil];
                });
            }];
        } 
        else {
            [alert runModal];
            executor.queue.push(on_done);
        };

        [alert release];
    });
}

auto Platform_dialogs::confirm(const std::string& title, const std::string& message, std::function<void(bool)> on_confirm, Execution_context executor) -> void
{
    // Copy to locals.
    const auto title_copy = title;
    const auto message_copy = message;

    dispatch_async(dispatch_get_main_queue(), ^{
        NSAlert* alert = [[NSAlert alloc] init];
        [alert setMessageText:[NSString stringWithUTF8String:title_copy.c_str()]];
        [alert setInformativeText:[NSString stringWithUTF8String:message_copy.c_str()]];
        [alert addButtonWithTitle:@"OK"];
        [alert addButtonWithTitle:@"Cancel"];

        NSWindow* keyWindow = [[NSApplication sharedApplication] keyWindow];
        if (keyWindow != nil) {
            [alert beginSheetModalForWindow:keyWindow completionHandler:^(NSModalResponse returnCode) {
                executor.queue.push([=, cb=std::move(on_confirm)] {
                    cb(returnCode == NSAlertFirstButtonReturn);
                });
                dispatch_async(dispatch_get_main_queue(), ^{
                    [keyWindow makeKeyAndOrderFront:nil];
                });
            }];
        }
        else {
            NSModalResponse response = [alert runModal];
            executor.queue.push([=, cb=std::move(on_confirm)] {
                cb(response == NSAlertFirstButtonReturn);
            });
        }

        [alert release];
    });
}

auto Platform_dialogs::text_input(const std::string& title, const std::string& message, std::function<void(std::string)> on_text, Execution_context executor) -> void
{
    // Copy to locals. 
    const auto title_copy = title;
    const auto message_copy = message;

    dispatch_async(dispatch_get_main_queue(), ^{
        NSAlert* alert = [[NSAlert alloc] init];
        [alert setMessageText:[NSString stringWithUTF8String:title_copy.c_str()]];
        [alert setInformativeText:[NSString stringWithUTF8String:message_copy.c_str()]];
        [alert addButtonWithTitle:@"OK"];
        [alert addButtonWithTitle:@"Cancel"];

        NSTextField* input = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 200, 24)];
        [alert setAccessoryView:input];

        NSWindow* keyWindow = [[NSApplication sharedApplication] keyWindow];
        if (keyWindow != nil) {
            [alert beginSheetModalForWindow:keyWindow completionHandler:^(NSModalResponse returnCode) {
                if (returnCode == NSAlertFirstButtonReturn) {
                    NSString* inputString = [input stringValue];
                    if (inputString) {
                        const auto result = std::string{[inputString UTF8String]};
                        executor.queue.push([=, cb=std::move(on_text)] {
                            cb(result);
                        });
                    }
                }
                dispatch_async(dispatch_get_main_queue(), ^{
                    [keyWindow makeKeyAndOrderFront:nil];
                });
            }];
        } 
        else {
            NSModalResponse response = [alert runModal];
            if (response == NSAlertFirstButtonReturn) {
                NSString* inputString = [input stringValue];
                if (inputString) {
                    const auto result = std::string{[inputString UTF8String]};
                    executor.queue.push([=, cb=std::move(on_text)] {
                        cb(result);
                    });
                }
            }
        }

        [input release];
        [alert release];
    });
}

auto Platform_dialogs::open_url(const std::string& url, Execution_context /*executor*/) -> void
{
    // Copy to locals.
    const auto url_copy = url;

    dispatch_async(dispatch_get_main_queue(), ^{
        NSString* ns_url = [NSString stringWithUTF8String:url_copy.c_str()];
        NSURL* nsurl = [NSURL URLWithString:ns_url];
        if (nsurl) {
            [[NSWorkspace sharedWorkspace] openURL:nsurl];
        }
    });
}

auto Platform_dialogs::open_file(const std::string& title, const std::string& default_path, std::function<void(std::optional<std::string>)> on_open, Execution_context executor) -> void
{
    // Copy to locals.
    const auto title_copy = title;
    const auto default_path_copy = default_path;

    dispatch_async(dispatch_get_main_queue(), ^{
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        [panel setTitle:[NSString stringWithUTF8String:title_copy.c_str()]];
        if (!default_path_copy.empty()) {
            NSString* ns_path = [NSString stringWithUTF8String:default_path_copy.c_str()];
            NSURL* url = [NSURL fileURLWithPath:ns_path];
            if (url) {
                [panel setDirectoryURL:url];
            }
        }
        [panel setCanChooseFiles:YES];
        [panel setCanChooseDirectories:NO];
        [panel setAllowsMultipleSelection:NO];

        NSWindow* keyWindow = [[NSApplication sharedApplication] keyWindow];
        if (keyWindow != nil) {
            [panel beginSheetModalForWindow:keyWindow completionHandler:^(NSModalResponse result) {
                if (result == NSModalResponseOK) {
                    NSURL* selectedURL = [[panel URLs] firstObject];
                    if (selectedURL) {
                        NSString* path = [selectedURL path];
                        const auto result_path = std::string{[path UTF8String]}; 
                        executor.queue.push([=, cb=std::move(on_open)] {
                            cb(result_path);
                        });
                    } else {
                        executor.queue.push([cb=std::move(on_open)] {
                            cb(std::nullopt);
                        });
                    }
                } else {
                    executor.queue.push([cb=std::move(on_open)] {
                        cb(std::nullopt);
                    });
                }
                dispatch_async(dispatch_get_main_queue(), ^{
                    [keyWindow makeKeyAndOrderFront:nil];
                });
            }];
        } 
        else {
            NSModalResponse result = [panel runModal];
            if (result == NSModalResponseOK) {
                NSURL* selectedURL = [[panel URLs] firstObject];
                if (selectedURL) {
                    NSString* path = [selectedURL path];
                    const auto result_path = std::string{[path UTF8String]};
                    executor.queue.push([=, cb=std::move(on_open)] {
                        cb(result_path);
                    });
                } else {
                    executor.queue.push([cb=std::move(on_open)] {
                        cb(std::nullopt);
                    });
                }
            } else {
                executor.queue.push([cb=std::move(on_open)] {
                    cb(std::nullopt);
                });
            }
        }
    });
}

} // namespace tiny