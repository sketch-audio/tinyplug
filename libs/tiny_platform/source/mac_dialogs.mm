#include <tiny_platform/platform_dialogs.hpp>

#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "window_registry.hpp"

#if __has_feature(objc_arc)
static_assert(false, "This is a non-ARC file");
#endif

namespace tiny {

namespace {

// How many times we'll wait for an existing sheet to clear before giving up and
// presenting anyway. Bounded so a host that leaves a sheet up forever can't leak
// observers; each wait is event-driven, not a spin.
constexpr auto max_sheet_waits = 8;

// The window a dialog should hang off, in preference order:
//
//   1. the view that asked, via its token — the whole point of the exercise;
//   2. the only view this binary has open, if the caller hasn't been migrated
//      (unambiguous by construction, so it can't pick the wrong instance);
//   3. whatever the host has key or main.
//
// nil is a real answer, not a failure: an out-of-process AUv3 view service has no
// windows of its own.
auto host_window(Window_token token) -> NSWindow*
{
    auto* native = Window_registry::resolve(token);
    if (!native) native = Window_registry::sole();

    if (auto* view = static_cast<NSView*>(native)) {
        if (auto* window = [view window]) return window;
    }

    if (auto* key = [NSApp keyWindow]) return key;
    return [NSApp mainWindow];
}

// Run `begin` once `host` has no sheet attached.
//
// A window can only show one sheet at a time, and asking while one is up is how
// dialogs go missing. Two ways we get here: the host put a sheet up (Logic's
// bounce progress, a save prompt), or we did — NSSavePanel and NSAlert call their
// completion handler while the sheet is still on screen, so a chained
// "save then confirm" lands on a window that is still busy.
void when_sheet_free(NSWindow* host, void (^begin)(NSWindow*), int waits_left)
{
    if (!host.attachedSheet || waits_left <= 0) {
        begin(host);
        return;
    }

    auto* center = [NSNotificationCenter defaultCenter];
    __block id observer = [center addObserverForName:NSWindowDidEndSheetNotification
                                              object:host
                                               queue:[NSOperationQueue mainQueue]
                                          usingBlock:^(NSNotification*) {
        [[NSNotificationCenter defaultCenter] removeObserver:observer];
        // One more turn of the run loop so the dismissal finishes before we ask
        // for the slot — `attachedSheet` can still be set inside this notification.
        dispatch_async(dispatch_get_main_queue(), ^{
            when_sheet_free(host, begin, waits_left - 1);
        });
    }];
}

// Present an alert and hand the response to `done` exactly once. Takes ownership
// of `alert`.
void run_alert(NSAlert* alert, Window_token token, void (^done)(NSModalResponse))
{
    auto* host = host_window(token);

    if (host) {
        when_sheet_free(host, ^(NSWindow* window) {
            [alert beginSheetModalForWindow:window completionHandler:^(NSModalResponse response) {
                done(response);
                dispatch_async(dispatch_get_main_queue(), ^{
                    [window makeKeyAndOrderFront:nil];
                });
                [alert release];
            }];
        }, max_sheet_waits);
        return;
    }

    // No window to hang off. App-modal is the only shape NSAlert supports here,
    // so this is the one place we still block — activate first or it comes up
    // behind the host and looks like nothing happened.
    [NSApp activateIgnoringOtherApps:YES];
    done([alert runModal]);
    [alert release];
}

// Same, for the file panels. `panel` is autoreleased by its own factory, so the
// block capture is what keeps it alive across the sheet.
void run_panel(NSSavePanel* panel, Window_token token, void (^done)(NSModalResponse))
{
    auto* host = host_window(token);

    if (host) {
        when_sheet_free(host, ^(NSWindow* window) {
            [panel beginSheetModalForWindow:window completionHandler:^(NSModalResponse response) {
                done(response);
                dispatch_async(dispatch_get_main_queue(), ^{
                    [window makeKeyAndOrderFront:nil];
                });
            }];
        }, max_sheet_waits);
        return;
    }

    [NSApp activateIgnoringOtherApps:YES];
    done([panel runModal]);
}

auto to_string(NSString* value) -> std::string
{
    const auto* utf8 = [value UTF8String];
    return utf8 ? std::string{utf8} : std::string{};
}

auto to_ns(const std::string& value) -> NSString*
{
    return [NSString stringWithUTF8String:value.c_str()];
}

} // namespace

auto Platform_dialogs::message(const std::string& title, const std::string& message, std::function<void()> on_done, Dialog_context ctx) -> void
{
    // Copy to locals.
    const auto title_copy = title;
    const auto message_copy = message;

    dispatch_async(dispatch_get_main_queue(), ^{
        NSAlert* alert = [[NSAlert alloc] init];
        [alert setMessageText:to_ns(title_copy)];
        [alert setInformativeText:to_ns(message_copy)];
        [alert addButtonWithTitle:@"OK"];

        run_alert(alert, ctx.window, ^(NSModalResponse) {
            ctx.tasks.on_main(on_done);
        });
    });
}

auto Platform_dialogs::confirm(const std::string& title, const std::string& message, std::function<void(bool)> on_confirm, Dialog_context ctx) -> void
{
    // Copy to locals.
    const auto title_copy = title;
    const auto message_copy = message;

    dispatch_async(dispatch_get_main_queue(), ^{
        NSAlert* alert = [[NSAlert alloc] init];
        [alert setMessageText:to_ns(title_copy)];
        [alert setInformativeText:to_ns(message_copy)];
        [alert addButtonWithTitle:@"OK"];
        [alert addButtonWithTitle:@"Cancel"];

        run_alert(alert, ctx.window, ^(NSModalResponse response) {
            const auto confirmed = (response == NSAlertFirstButtonReturn);
            ctx.tasks.on_main([=, on_confirm=std::move(on_confirm)] {
                on_confirm(confirmed);
            });
        });
    });
}

auto Platform_dialogs::text_input(const std::string& title, const std::string& message, std::function<void(std::string)> on_text, Dialog_context ctx) -> void
{
    // Copy to locals.
    const auto title_copy = title;
    const auto message_copy = message;

    dispatch_async(dispatch_get_main_queue(), ^{
        NSAlert* alert = [[NSAlert alloc] init];
        [alert setMessageText:to_ns(title_copy)];
        [alert setInformativeText:to_ns(message_copy)];
        [alert addButtonWithTitle:@"OK"];
        [alert addButtonWithTitle:@"Cancel"];

        NSTextField* input = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 200, 24)];
        [alert setAccessoryView:input];
        [alert.window setInitialFirstResponder:input];

        run_alert(alert, ctx.window, ^(NSModalResponse response) {
            // Cancel reports an empty string rather than dropping the callback:
            // a caller left waiting on a dialog that will never answer is worse
            // than one told "nothing".
            const auto result = (response == NSAlertFirstButtonReturn) ? to_string([input stringValue]) : std::string{};
            [input release]; // Held past the alert's own retain so the read above is unambiguous.
            ctx.tasks.on_main([=, on_text=std::move(on_text)] {
                on_text(result);
            });
        });
    });
}

auto Platform_dialogs::open_url(const std::string& url, Dialog_context /*ctx*/) -> void
{
    // Copy to locals.
    const auto url_copy = url;

    dispatch_async(dispatch_get_main_queue(), ^{
        NSURL* nsurl = [NSURL URLWithString:to_ns(url_copy)];
        if (nsurl) {
            [[NSWorkspace sharedWorkspace] openURL:nsurl];
        }
    });
}

auto Platform_dialogs::save_file(const std::string& title, const std::string& default_path, const std::string& name, const std::string& extension, std::function<void(std::optional<std::string>)> on_save, Dialog_context ctx) -> void
{
    // Copy to locals.
    const auto title_copy = title;
    const auto default_path_copy = default_path;
    const auto name_copy = name;
    const auto extension_copy = extension;

    dispatch_async(dispatch_get_main_queue(), ^{
        NSSavePanel* panel = [NSSavePanel savePanel];
        [panel setTitle:to_ns(title_copy)];
        if (!default_path_copy.empty()) {
            NSURL* url = [NSURL fileURLWithPath:to_ns(default_path_copy)];
            if (url) {
                [panel setDirectoryURL:url];
            }
        }

        if (!name_copy.empty()) {
            [panel setNameFieldStringValue:to_ns(name_copy)];
        }

        if (!extension_copy.empty()) {
            UTType* type = [UTType typeWithFilenameExtension:to_ns(extension_copy)];
            if (type) {
                [panel setAllowedContentTypes:@[type]];
            }
        }

        [panel setCanCreateDirectories:YES];
        [panel setAllowsOtherFileTypes:NO];
        [panel setExtensionHidden:NO];
        [panel setCanSelectHiddenExtension:YES];

        run_panel(panel, ctx.window, ^(NSModalResponse response) {
            auto path = std::optional<std::string>{};
            if (response == NSModalResponseOK) {
                if (NSURL* selected = [panel URL]) {
                    path = to_string([selected path]);
                }
            }
            ctx.tasks.on_background([=, on_save=std::move(on_save)] {
                on_save(path);
            });
        });
    });
}

// Shared by `open_file`/`choose_dir` -- an NSOpenPanel restricted to either
// files or directories, dispatched the same way either way.
static auto run_open_panel(const std::string& title, const std::string& default_path, std::function<void(std::optional<std::string>)> on_open, Dialog_context ctx, bool choose_directory) -> void
{
    // Copy to locals.
    const auto title_copy = title;
    const auto default_path_copy = default_path;

    dispatch_async(dispatch_get_main_queue(), ^{
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        [panel setTitle:to_ns(title_copy)];
        if (!default_path_copy.empty()) {
            NSURL* url = [NSURL fileURLWithPath:to_ns(default_path_copy)];
            if (url) {
                [panel setDirectoryURL:url];
            }
        }
        [panel setCanChooseFiles:!choose_directory];
        [panel setCanChooseDirectories:choose_directory];
        [panel setAllowsMultipleSelection:NO];

        run_panel(panel, ctx.window, ^(NSModalResponse response) {
            auto path = std::optional<std::string>{};
            if (response == NSModalResponseOK) {
                if (NSURL* selected = [[panel URLs] firstObject]) {
                    path = to_string([selected path]);
                }
            }
            ctx.tasks.on_background([=, on_open=std::move(on_open)] {
                on_open(path);
            });
        });
    });
}

auto Platform_dialogs::open_file(const std::string& title, const std::string& default_path, std::function<void(std::optional<std::string>)> on_open, Dialog_context ctx) -> void
{
    run_open_panel(title, default_path, std::move(on_open), ctx, false);
}

auto Platform_dialogs::choose_dir(const std::string& title, const std::string& default_path, std::function<void(std::optional<std::string>)> on_choose, Dialog_context ctx) -> void
{
    run_open_panel(title, default_path, std::move(on_choose), ctx, true);
}

} // namespace tiny
